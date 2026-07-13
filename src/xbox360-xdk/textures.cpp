// Xbox360 texture backend using the preprocessor's TEXTURES.BIN / ATLAS.BIN / CLUT format.
//
// The preprocessor repacks all texture pages into indexed-palette atlases (TEXTURES.BIN),
// with metadata in ATLAS.BIN mapping each TPAG to a sub-rect + CLUT index.
// CLUT8.BIN contains 256-entry RGBA palettes (one per merged color group).
//
// Benefits over the old TXTR-in-data.win approach:
// - Indexed pixel data: 1 byte/pixel (8bpp) or 0.5 bytes/pixel (4bpp) vs 4 bytes/pixel RGBA
// - CLUT stored compactly (256 x 4 bytes = 1KB per palette)
// - Data streamed from disk on demand; not all textures need to be resident at once
// - Memory savings: ~4-8x reduction in backing storage for texture data

#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "textures.h"

// Forward declarations from the D3D9 renderer/main
extern "C" void Butterscotch_xdkDiagTrace(const char* fmt, ...);

// ---------- TEXTURES.BIN format ----------
// Each atlas has a 128-byte header followed by pixel data:
//   offset 0:  version (uint8, must be 0)
//   offset 1:  width (uint16 BE)
//   offset 3:  height (uint16 BE)
//   offset 5:  bpp (uint8: 4 or 8)
//   offset 6:  pixelDataSize (uint32 BE)
//   offset 10: compressionType (uint8: 0=uncompressed, 1=RLE)
//   offset 11-127: padding

#define ATLAS_HEADER_SIZE 128

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint32_t pixelDataSize;
    uint8_t  compressionType; // 0 = uncompressed, 1 = RLE
    uint32_t fileOffset;      // offset in TEXTURES.BIN where the pixel data starts
} AtlasInfo;

// ---------- ATLAS.BIN format ----------
//   offset 0:  version (uint8, must be 0)
//   offset 1:  tpagEntryCount (uint16 BE)
//   offset 3:  tileEntryCount (uint16 BE)
//   offset 5:  atlasCount (uint16 BE)
//   offset 7:  atlas offset table (atlasCount x uint32 BE)
//   Then tpagEntryCount TPAG entries, each 21 bytes:
//     atlasId(u16), atlasX(u16), atlasY(u16), width(u16), height(u16),
//     cropOffsetX(u16), cropOffsetY(u16), croppedWidth(u16), croppedHeight(u16),
//     clutIndex(u16), bpp(u8)
//   Then tileEntryCount tile entries (not used by this loader)

#define TPAG_ENTRY_SIZE 21

typedef struct {
    uint16_t atlasId;
    uint16_t atlasX;
    uint16_t atlasY;
    uint16_t width;
    uint16_t height;
    uint16_t cropOffsetX;
    uint16_t cropOffsetY;
    uint16_t croppedWidth;
    uint16_t croppedHeight;
    uint16_t clutIndex; // 0xFFFF if unmapped
    uint8_t  bpp;
} TpagEntry;

// ---------- CLUT format ----------
// CLUT8.BIN contains 256-entry RGBA palettes.
// Each entry is 4 bytes (RGBA bytes, stored as uint32 LE).
// Entries are sorted by CLUT group ID.

#define CLUT8_PALETTE_SIZE 256
#define CLUT8_COLOR_BYTES  4
#define CLUT8_ENTRY_BYTES  (CLUT8_PALETTE_SIZE * CLUT8_COLOR_BYTES) // 1024

// ---------- Global state ----------

static FILE*    gTexturesFile;   // TEXTURES.BIN handle
static FILE*    gAtlasFile;      // ATLAS.BIN handle
static uint8_t* gClut8Data;      // all CLUT8 entries concatenated (for random access by clutIndex)
static uint32_t gClut8Count;     // number of CLUT8 entries
static bool     gInitialized = false;

static uint32_t gAtlasCount;     // number of atlases
static uint32_t gTpagCount;      // number of TPAG entries
static AtlasInfo* gAtlasInfo;    // [atlasCount]
static TpagEntry* gTpagEntries;  // [tpagCount]

// ---------- Little-endian helpers ----------
// The preprocessor's ByteWriter writes all multi-byte integers in little-endian order
// (see src/preprocessor/byte_writer.c).

static inline uint16_t readU16LE(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint32_t)p[1] << 8));
}

static inline uint32_t readU32LE(const uint8_t* p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// ---------- RLE decompression (mirrors preprocessor's RLE) ----------
// RLE format: pairs of [runLength(u8), value(u8)]
// runLength 1..255, value is the index to repeat.

static uint8_t* rleDecompress(const uint8_t* rleData, size_t rleSize, size_t expectedOutputSize) {
    if (!rleData || rleSize == 0 || expectedOutputSize == 0) return NULL;

    uint8_t* out = (uint8_t*)malloc(expectedOutputSize);
    if (!out) return NULL;

    size_t outPos = 0;
    size_t inPos = 0;

    while (inPos + 1 <= rleSize && outPos < expectedOutputSize) {
        uint8_t runLen = rleData[inPos++];
        uint8_t value = rleData[inPos++];

        size_t copyLen = (size_t)runLen;
        if (copyLen > expectedOutputSize - outPos) {
            copyLen = expectedOutputSize - outPos;
        }
        memset(out + outPos, value, copyLen);
        outPos += copyLen;
    }

    // If output is smaller than expected, pad with zeros
    if (outPos < expectedOutputSize) {
        memset(out + outPos, 0, expectedOutputSize - outPos);
    }

    return out;
}

// ---------- Initialize from TEXTURES.BIN, ATLAS.BIN, CLUT8.BIN ----------

bool Xbox360Textures_init(const char* texturesBinPath, const char* atlasBinPath, const char* clut8Path) {
    if (gInitialized) return true;

    // Open TEXTURES.BIN and parse atlas headers
    gTexturesFile = fopen(texturesBinPath, "rb");
    if (!gTexturesFile) {
        Butterscotch_xdkDiagTrace("Xbox360Textures: cannot open %s", texturesBinPath ? texturesBinPath : "(null)");
        Xbox360Textures_free();
        return false;
    }

    // Read through TEXTURES.BIN to count atlases and record their offsets
    // Each atlas starts with ATLAS_HEADER_SIZE bytes.
    uint32_t maxAtlases = 4096; // sanity limit
    gAtlasInfo = (AtlasInfo*)malloc(maxAtlases * sizeof(AtlasInfo));
    if (!gAtlasInfo) {
        Xbox360Textures_free();
        return false;
    }

    gAtlasCount = 0;
    while (true) {
        uint8_t hdr[ATLAS_HEADER_SIZE];
        size_t got = fread(hdr, 1, ATLAS_HEADER_SIZE, gTexturesFile);
        if (got != ATLAS_HEADER_SIZE) break; // EOF or error

        if (hdr[0] != 0) {
            Butterscotch_xdkDiagTrace("Xbox360Textures: unsupported atlas version %u at atlas %u", hdr[0], gAtlasCount);
            Xbox360Textures_free();
            return false;
        }

        AtlasInfo* ai = &gAtlasInfo[gAtlasCount];
        ai->width           = readU16LE(hdr + 1);
        ai->height          = readU16LE(hdr + 3);
        ai->bpp             = hdr[5];
        ai->pixelDataSize   = readU32LE(hdr + 6);
        ai->compressionType = hdr[10];
        ai->fileOffset      = (uint32_t)ftell(gTexturesFile); // pixel data starts right after header

        if (ai->bpp != 4 && ai->bpp != 8) {
            Butterscotch_xdkDiagTrace("Xbox360Textures: atlas %u invalid bpp=%u", gAtlasCount, ai->bpp);
            Xbox360Textures_free();
            return false;
        }

        // Skip pixel data
        if (fseek(gTexturesFile, (long)ai->pixelDataSize, SEEK_CUR) != 0) {
            Xbox360Textures_free();
            return false;
        }

        gAtlasCount++;
        if (gAtlasCount >= maxAtlases) break;
    }

    Butterscotch_xdkDiagTrace("Xbox360Textures: TEXTURES.BIN: %u atlases", gAtlasCount);

    // Open and parse ATLAS.BIN
    gAtlasFile = fopen(atlasBinPath, "rb");
    if (!gAtlasFile) {
        Butterscotch_xdkDiagTrace("Xbox360Textures: cannot open %s", atlasBinPath ? atlasBinPath : "(null)");
        Xbox360Textures_free();
        return false;
    }

    // Parse ATLAS.BIN header
    {
        uint8_t ab_hdr[7];
        if (fread(ab_hdr, 1, 7, gAtlasFile) != 7) {
            Xbox360Textures_free();
            return false;
        }
        if (ab_hdr[0] != 0) {
            Butterscotch_xdkDiagTrace("Xbox360Textures: unsupported ATLAS.BIN version %u", ab_hdr[0]);
            Xbox360Textures_free();
            return false;
        }

        uint16_t numTpagEntries = readU16LE(ab_hdr + 1);
        uint16_t numTileEntries = readU16LE(ab_hdr + 3);
        uint16_t numAtlases2    = readU16LE(ab_hdr + 5);

        if (numTpagEntries == 0) {
            Butterscotch_xdkDiagTrace("Xbox360Textures: ATLAS.BIN has no TPAG entries");
            Xbox360Textures_free();
            return false;
        }

        // Read atlas offset table (numAtlases2 x uint32) - skip
        for (uint16_t i = 0; i < numAtlases2; i++) {
            uint8_t offBuf[4];
            if (fread(offBuf, 1, 4, gAtlasFile) != 4) {
                Xbox360Textures_free();
                return false;
            }
        }

        // Read TPAG entries
        gTpagCount = (uint32_t)numTpagEntries;
        gTpagEntries = (TpagEntry*)malloc(gTpagCount * sizeof(TpagEntry));
        if (!gTpagEntries) {
            Xbox360Textures_free();
            return false;
        }

        for (uint32_t i = 0; i < gTpagCount; i++) {
            uint8_t entryBuf[TPAG_ENTRY_SIZE];
            if (fread(entryBuf, 1, TPAG_ENTRY_SIZE, gAtlasFile) != TPAG_ENTRY_SIZE) {
                Xbox360Textures_free();
                return false;
            }

            TpagEntry* te = &gTpagEntries[i];
            te->atlasId       = readU16LE(entryBuf + 0);
            te->atlasX        = readU16LE(entryBuf + 2);
            te->atlasY        = readU16LE(entryBuf + 4);
            te->width         = readU16LE(entryBuf + 6);
            te->height        = readU16LE(entryBuf + 8);
            te->cropOffsetX   = readU16LE(entryBuf + 10);
            te->cropOffsetY   = readU16LE(entryBuf + 12);
            te->croppedWidth  = readU16LE(entryBuf + 14);
            te->croppedHeight = readU16LE(entryBuf + 16);
            te->clutIndex     = readU16LE(entryBuf + 18);
            te->bpp           = entryBuf[20];
        }

        (void)numTileEntries; // Not used by this loader
    }

    Butterscotch_xdkDiagTrace("Xbox360Textures: ATLAS.BIN: %u TPAG entries, %u atlases", gTpagCount, gAtlasCount);

    // Load CLUT8.BIN
    if (clut8Path && clut8Path[0]) {
        FILE* clutFile = fopen(clut8Path, "rb");
        if (clutFile) {
            fseek(clutFile, 0, SEEK_END);
            long clutSize = ftell(clutFile);
            fseek(clutFile, 0, SEEK_SET);

            if (clutSize > 0 && (clutSize % CLUT8_ENTRY_BYTES) == 0) {
                gClut8Count = (uint32_t)(clutSize / CLUT8_ENTRY_BYTES);
                gClut8Data = (uint8_t*)malloc((size_t)clutSize);
                if (gClut8Data) {
                    if (fread(gClut8Data, 1, (size_t)clutSize, clutFile) == (size_t)clutSize) {
                        Butterscotch_xdkDiagTrace("Xbox360Textures: CLUT8.BIN: %u entries loaded", gClut8Count);
                    } else {
                        free(gClut8Data);
                        gClut8Data = NULL;
                        gClut8Count = 0;
                        Butterscotch_xdkDiagTrace("Xbox360Textures: failed to read CLUT8.BIN");
                    }
                }
            } else {
                Butterscotch_xdkDiagTrace("Xbox360Textures: CLUT8.BIN invalid size %ld", clutSize);
            }
            fclose(clutFile);
        } else {
            Butterscotch_xdkDiagTrace("Xbox360Textures: CLUT8.BIN not found at %s", clut8Path);
        }
    }

    if (!gClut8Data || gClut8Count == 0) {
        Butterscotch_xdkDiagTrace("Xbox360Textures: WARNING: no CLUT data loaded; palette expansion will fail");
    }

    gInitialized = true;
    Butterscotch_xdkDiagTrace("Xbox360Textures: initialized OK (%u atlases, %u TPAGs, %u CLUT8 entries)",
                              gAtlasCount, gTpagCount, gClut8Count);
    return true;
}

void Xbox360Textures_free(void) {
    gInitialized = false;
    if (gTexturesFile) { fclose(gTexturesFile); gTexturesFile = NULL; }
    if (gAtlasFile)    { fclose(gAtlasFile);    gAtlasFile    = NULL; }
    free(gClut8Data);   gClut8Data   = NULL;
    free(gAtlasInfo);   gAtlasInfo   = NULL;
    free(gTpagEntries); gTpagEntries = NULL;
    gClut8Count  = 0;
    gAtlasCount  = 0;
    gTpagCount   = 0;
}

uint32_t Xbox360Textures_getPageCount(void) {
    return gInitialized ? gTpagCount : 0;
}

uint32_t Xbox360Textures_getAtlasCount(void) {
    return gInitialized ? gAtlasCount : 0;
}

bool Xbox360Textures_hasTpagMapping(int32_t tpagIndex) {
    if (!gInitialized || tpagIndex < 0 || (uint32_t)tpagIndex >= gTpagCount) return false;
    return gTpagEntries[tpagIndex].atlasId != 0xFFFF;
}

bool Xbox360Textures_getTpagAtlasInfo(int32_t tpagIndex,
                                       int* outAtlasId, int* outAtlasX, int* outAtlasY,
                                       int* outWidth, int* outHeight,
                                       int* outClutIndex, int* outBpp) {
    if (!gInitialized || tpagIndex < 0 || (uint32_t)tpagIndex >= gTpagCount) return false;
    const TpagEntry* te = &gTpagEntries[tpagIndex];
    if (te->atlasId == 0xFFFF) return false;
    if (te->atlasId >= gAtlasCount) return false;

    if (outAtlasId)   *outAtlasId   = (int)te->atlasId;
    if (outAtlasX)    *outAtlasX    = (int)te->atlasX;
    if (outAtlasY)    *outAtlasY    = (int)te->atlasY;
    if (outWidth)     *outWidth     = (int)te->width;
    if (outHeight)    *outHeight    = (int)te->height;
    if (outClutIndex) *outClutIndex = (int)te->clutIndex;
    if (outBpp)       *outBpp       = (int)te->bpp;
    return true;
}

// Internal: Read and decompress atlas pixel data.
// Returns malloc'd buffer of width*height bytes (for 8bpp) or ((width*height+1)/2) bytes (for 4bpp).
// Caller frees with free().
static uint8_t* readAtlasPixelData(uint32_t atlasId, size_t* outSize) {
    if (!gInitialized || atlasId >= gAtlasCount || !gTexturesFile || !outSize) return NULL;
    *outSize = 0;

    const AtlasInfo* ai = &gAtlasInfo[atlasId];

    size_t uncompressedSize;
    if (ai->bpp == 8) {
        uncompressedSize = (size_t)ai->width * (size_t)ai->height;
    } else {
        uncompressedSize = ((size_t)ai->width * (size_t)ai->height + 1) / 2;
    }

    // Read compressed data from file
    if (fseek(gTexturesFile, (long)ai->fileOffset, SEEK_SET) != 0) return NULL;

    uint8_t* compressed = (uint8_t*)malloc(ai->pixelDataSize);
    if (!compressed) return NULL;

    if (fread(compressed, 1, ai->pixelDataSize, gTexturesFile) != ai->pixelDataSize) {
        free(compressed);
        return NULL;
    }

    uint8_t* result = NULL;

    if (ai->compressionType == 1) {
        // RLE compressed
        result = rleDecompress(compressed, (size_t)ai->pixelDataSize, uncompressedSize);
    } else {
        // Uncompressed
        if (uncompressedSize == (size_t)ai->pixelDataSize) {
            result = (uint8_t*)malloc(uncompressedSize);
            if (result) memcpy(result, compressed, uncompressedSize);
        } else {
            // Mismatch, but just try to use what we have
            result = (uint8_t*)malloc(uncompressedSize);
            if (result) {
                size_t copySize = (size_t)ai->pixelDataSize < uncompressedSize ? (size_t)ai->pixelDataSize : uncompressedSize;
                memcpy(result, compressed, copySize);
                if (copySize < uncompressedSize) memset(result + copySize, 0, uncompressedSize - copySize);
            }
        }
    }

    free(compressed);
    *outSize = uncompressedSize;
    return result;
}

// Look up a CLUT8 palette entry by index.
// Returns pointer to 4 RGBA bytes, or NULL if out of range.
static const uint8_t* getClut8Colors(int clutIndex) {
    if (!gClut8Data || clutIndex < 0 || (uint32_t)clutIndex >= gClut8Count) return NULL;
    return gClut8Data + (size_t)clutIndex * CLUT8_ENTRY_BYTES;
}

// Convert a CLUT8 color entry (stored as 32-bit PS2 format RGBA bytes) to RGBA bytes.
static inline void clut8ColorToRgba(const uint8_t* clutEntry, uint8_t* outR, uint8_t* outG, uint8_t* outB, uint8_t* outA) {
    // CLUT8 is stored as 32-bit entries in PS2 RGBA format.
    // The preprocessor writes them via convertARGBtoPS2RGBA() then stores as uint32 LE.
    // On Xbox 360 (big-endian PPC), we need to interpret correctly.
    // The byte layout from the preprocessor is: [R, G, B, A] in memory in little-endian files.
    // On big-endian, when read as bytes, this is still [R, G, B, A].
    *outR = clutEntry[0];
    *outG = clutEntry[1];
    *outB = clutEntry[2];
    *outA = clutEntry[3];
}

// Load an atlas by ID, expand indexed pixels to RGBA using the given CLUT index.
// The caller MUST free *outPixels with free() after uploading to the GPU.
bool Xbox360Textures_loadAtlasById(int atlasId, int* outW, int* outH, uint8_t** outPixels) {
    if (!outW || !outH || !outPixels) return false;
    *outW = 0;
    *outH = 0;
    *outPixels = NULL;

    if (!gInitialized || atlasId < 0 || (uint32_t)atlasId >= gAtlasCount) return false;

    const AtlasInfo* ai = &gAtlasInfo[atlasId];

    // Read indexed pixel data
    size_t indexSize = 0;
    uint8_t* indexData = readAtlasPixelData((uint32_t)atlasId, &indexSize);
    if (!indexData) return false;

    // For the atlas, we can't expand without knowing which CLUT to use (TPAGs in the atlas
    // may have different CLUTs). We'll just return the indexed data in 8bpp or 4bpp form
    // and let the caller handle it. But for convenience, expand to RGBA using a default.
    // Actually, since different TPAGs in the same atlas can use different CLUTs, we need
    // the caller to tell us which CLUT. For now, we return RGBA and require the caller
    // to provide a clutIndex, or we default to using the first TPAG's CLUT.

    // Since we don't have a single CLUT for the whole atlas, let's expand each TPAG
    // sub-rect separately using its own CLUT. This is handled by loadPage().
    // For loadAtlasById, we'll just return the raw indexed bytes with a note.
    // For now, treat this as unsupported - use loadPage instead.

    free(indexData);
    return false;
}

// Load a texture page by TPAG index, expanding from indexed+CLUT to full RGBA.
// The RGBA buffer is w*h*4 bytes and must be freed by the caller.
bool Xbox360Textures_loadPage(int32_t tpagIndex, int* outW, int* outH, uint8_t** outPixels) {
    if (!outW || !outH || !outPixels) return false;
    *outW = 0;
    *outH = 0;
    *outPixels = NULL;

    if (!gInitialized || tpagIndex < 0 || (uint32_t)tpagIndex >= gTpagCount) {
        Butterscotch_xdkDiagTrace("Xbox360Textures: loadPage %d: invalid index", tpagIndex);
        return false;
    }

    const TpagEntry* te = &gTpagEntries[tpagIndex];
    if (te->atlasId == 0xFFFF || te->atlasId >= gAtlasCount) {
        Butterscotch_xdkDiagTrace("Xbox360Textures: loadPage %d: unmapped TPAG", tpagIndex);
        return false;
    }

    const AtlasInfo* ai = &gAtlasInfo[te->atlasId];

    // Read the full atlas indexed pixel data
    size_t indexSize = 0;
    uint8_t* indexData = readAtlasPixelData(te->atlasId, &indexSize);
    if (!indexData) {
        Butterscotch_xdkDiagTrace("Xbox360Textures: loadPage %d: failed to read atlas %u pixel data",
                                  tpagIndex, te->atlasId);
        return false;
    }

    // Determine the dimensions of the sub-rect to extract
    int subW = (int)te->width;
    int subH = (int)te->height;
    int atlasW = (int)ai->width;
    int atlasH = (int)ai->height;

    if (subW <= 0 || subH <= 0 || atlasW <= 0 || atlasH <= 0) {
        free(indexData);
        return false;
    }

    // We want to return RGBA pixels for just the sub-rect (the texture page content).
    // If croppedWidth/croppedHeight are present, use those for the output dimensions.
    int outWidth = subW;
    int outHeight = subH;

    // Allocate RGBA output buffer
    size_t rgbaSize = (size_t)outWidth * (size_t)outHeight * 4;
    uint8_t* rgba = (uint8_t*)malloc(rgbaSize);
    if (!rgba) {
        free(indexData);
        return false;
    }
    memset(rgba, 0, rgbaSize);

    // Get CLUT colors
    const uint8_t* clutColors = NULL;
    if (te->clutIndex != 0xFFFF) {
        clutColors = getClut8Colors((int)te->clutIndex);
    }

    // Expand indexed pixels to RGBA
    if (ai->bpp == 8) {
        // 8bpp: 1 byte per pixel
        for (int y = 0; y < outHeight && y < atlasH - (int)te->atlasY; y++) {
            for (int x = 0; x < outWidth && x < atlasW - (int)te->atlasX; x++) {
                int srcX = (int)te->atlasX + x;
                int srcY = (int)te->atlasY + y;
                uint8_t idx = indexData[srcY * atlasW + srcX];

                uint8_t* dst = rgba + (y * outWidth + x) * 4;
                if (clutColors) {
                    const uint8_t* color = clutColors + (size_t)idx * 4;
                    dst[0] = color[0]; // R
                    dst[1] = color[1]; // G
                    dst[2] = color[2]; // B
                    dst[3] = color[3]; // A
                } else {
                    // No CLUT - treat index as grayscale
                    dst[0] = idx;
                    dst[1] = idx;
                    dst[2] = idx;
                    dst[3] = 255;
                }
            }
        }
    } else {
        // 4bpp: 2 pixels per byte
        for (int y = 0; y < outHeight && y < atlasH - (int)te->atlasY; y++) {
            for (int x = 0; x < outWidth && x < atlasW - (int)te->atlasX; x++) {
                int srcX = (int)te->atlasX + x;
                int srcY = (int)te->atlasY + y;
                size_t byteIdx = (size_t)(srcY * atlasW + srcX);
                uint8_t rawByte = indexData[byteIdx / 2];
                uint8_t idx;
                if (byteIdx % 2 == 0) {
                    idx = rawByte & 0x0F;           // low nibble
                } else {
                    idx = (rawByte >> 4) & 0x0F;      // high nibble
                }

                uint8_t* dst = rgba + (y * outWidth + x) * 4;
                if (clutColors) {
                    const uint8_t* color = clutColors + (size_t)idx * 4;
                    dst[0] = color[0];
                    dst[1] = color[1];
                    dst[2] = color[2];
                    dst[3] = color[3];
                } else {
                    // No CLUT - grayscale
                    uint8_t gray = (idx << 4) | idx;
                    dst[0] = gray;
                    dst[1] = gray;
                    dst[2] = gray;
                    dst[3] = 255;
                }
            }
        }
    }

    free(indexData);

    *outW = outWidth;
    *outH = outHeight;
    *outPixels = rgba;

    Butterscotch_xdkDiagTrace("Xbox360Textures: loadPage %d: atlas=%u sub=%dx%d pos=%d,%d expanded to %dx%d RGBA",
                              tpagIndex, te->atlasId, subW, subH, te->atlasX, te->atlasY, outWidth, outHeight);
    return true;
}