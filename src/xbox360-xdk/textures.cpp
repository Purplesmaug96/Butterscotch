// xbox360-xdk: TEMPORARY D3D9-side texture backend
// This replaces the OpenGL-only PS3 streaming code with D3D9 texture page
// uploading, so the 360 backend can render TXTR/CLUT palettes.
//
// NOTE: This file is intentionally self-contained until the real
// independent texture system is factored out.

#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The Xbox360 D3D9 renderer owns the actual GPU texture cache.
// This helper provides the same *data* services as ps3_textures.c
// but in a way compatible with the D3D9 pipeline.

#define PAGE_HEADER_SIZE 12 // u16 w, u16 h, u32 pixelOffset, u32 pixelDataSize

typedef struct {
	uint16_t width;
	uint16_t height;
	uint32_t pixelOffset;
	uint32_t pixelDataSize;
} PageInfo;

static FILE* gFp;
static uint16_t gClutCount;
static uint16_t gPageCount;
static uint16_t* gTpagClutMap; // [tpagCount]
static PageInfo* gPageInfo;	   // [pageCount]
static long gPixelBlockBase;
static bool gInitialized;
static uint32_t gTpagCount;

static inline uint16_t readU16BE(const uint8_t* p) {
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t readU32BE(const uint8_t* p) {
	return ((uint32_t)p[0] << 24) |
		   ((uint32_t)p[1] << 16) |
		   ((uint32_t)p[2] << 8) |
		   (uint32_t)p[3];
}

// Public API expected by the temporary integration code.
// The D3D9 renderer will call these.

bool Xbox360Textures_init(const char* texturesBinPath) {
	if (gInitialized) {
		return true;
	}

	gFp = fopen(texturesBinPath, "rb");
	if (gFp == NULL) {
		fprintf(stderr, "Xbox360Textures: cannot open %s\n", texturesBinPath);
		return false;
	}

	uint8_t headerBuf[7];
	if (fread(headerBuf, 1, 7, gFp) != 7) {
		goto fail;
	}
	if (headerBuf[0] != 0) {
		fprintf(stderr, "Xbox360Textures: unsupported version %u\n", headerBuf[0]);
		goto fail;
	}

	gClutCount = readU16BE(headerBuf + 1);
	gPageCount = readU16BE(headerBuf + 3);
	gTpagCount = readU16BE(headerBuf + 5);

	// TPAG -> CLUT row map
	size_t mapBytes = (size_t)gTpagCount * 2;
	uint8_t* mapBuf = (uint8_t*)malloc(mapBytes);
	if (!mapBuf) {
		goto fail;
	}
	if (fread(mapBuf, 1, mapBytes, gFp) != mapBytes) {
		free(mapBuf);
		goto fail;
	}

	gTpagClutMap = (uint16_t*)malloc(gTpagCount * sizeof(uint16_t));
	if (!gTpagClutMap) {
		free(mapBuf);
		goto fail;
	}
	for (uint32_t i = 0; i < gTpagCount; i++) {
		gTpagClutMap[i] = readU16BE(mapBuf + i * 2);
	}
	free(mapBuf);

	// Page header table
	size_t headerBytes = (size_t)gPageCount * PAGE_HEADER_SIZE;
	uint8_t* hdrBuf = (uint8_t*)malloc(headerBytes);
	if (!hdrBuf) {
		goto fail;
	}
	if (fread(hdrBuf, 1, headerBytes, gFp) != headerBytes) {
		free(hdrBuf);
		goto fail;
	}

	gPageInfo = (PageInfo*)malloc(gPageCount * sizeof(PageInfo));
	if (!gPageInfo) {
		free(hdrBuf);
		goto fail;
	}
	for (uint32_t i = 0; i < gPageCount; i++) {
		const uint8_t* p = hdrBuf + i * PAGE_HEADER_SIZE;
		gPageInfo[i].width = readU16BE(p + 0);
		gPageInfo[i].height = readU16BE(p + 2);
		gPageInfo[i].pixelOffset = readU32BE(p + 4);
		gPageInfo[i].pixelDataSize = readU32BE(p + 8);
	}
	free(hdrBuf);

	// Skip CLUT atlas payload in the file: this temp backend does not create a GPU CLUT texture.
	// The D3D9 renderer already uses RGBA direct textures for TXTR pages in its current path.
	// If you need CLUT sampling, extend the integration to decode CLUT8/4 and upload as a texture.
	//
	// The layout matches ps3_textures.c which reads the CLUT atlas immediately after the header.
	// To reach pixel block base, we just seek past the same region.
	size_t clutBytes = (size_t)gClutCount * 256 * 4;
	if (fseek(gFp, (long)clutBytes, SEEK_CUR) != 0) {
		goto fail;
	}

	// Pixel block starts after: CLUT payload + headers
	gPixelBlockBase = ftell(gFp);

	gInitialized = true;
	return true;

fail:
	if (gFp) {
		fclose(gFp);
	}
	gFp = NULL;
	free(gTpagClutMap);
	gTpagClutMap = NULL;
	free(gPageInfo);
	gPageInfo = NULL;
	gClutCount = 0;
	gPageCount = 0;
	gTpagCount = 0;
	gPixelBlockBase = 0;
	gInitialized = false;
	return false;
}

void Xbox360Textures_free(void) {
	if (!gInitialized) {
		return;
	}
	if (gFp) {
		fclose(gFp);
	}
	gFp = NULL;
	free(gTpagClutMap);
	gTpagClutMap = NULL;
	free(gPageInfo);
	gPageInfo = NULL;
	gClutCount = 0;
	gPageCount = 0;
	gTpagCount = 0;
	gPixelBlockBase = 0;
	gInitialized = false;
}

uint32_t Xbox360Textures_getPageCount(void) {
	return gInitialized ? (uint32_t)gPageCount : 0;
}

// Loads an uncompressed RGBA page payload.
// Returns malloc'd buffer in outPixels (caller frees).
bool Xbox360Textures_loadPage(uint32_t pageId, int* outW, int* outH, uint8_t** outPixels) {
	if (!gInitialized || pageId >= gPageCount || !outW || !outH || !outPixels) {
		return false;
	}

	const PageInfo* h = &gPageInfo[pageId];
	if (h->width == 0 || h->height == 0 || h->pixelDataSize == 0) {
		return false;
	}

	uint8_t* buf = (uint8_t*)malloc(h->pixelDataSize);
	if (!buf) {
		return false;
	}

	if (fseek(gFp, gPixelBlockBase + (long)h->pixelOffset, SEEK_SET) != 0) {
		free(buf);
		return false;
	}

	if (fread(buf, 1, h->pixelDataSize, gFp) != h->pixelDataSize) {
		free(buf);
		return false;
	}

	*outW = (int)h->width;
	*outH = (int)h->height;
	*outPixels = buf;
	return true;
}

// Returns the palette V coordinate for a TPAG, matching ps3_textures.c semantics.
float Xbox360Textures_getTpagPaletteV(int32_t tpagIndex) {
	if (!gInitialized) {
		return -1.0f;
	}
	if (tpagIndex < 0 || (uint32_t)tpagIndex >= gTpagCount) {
		return -1.0f;
	}

	uint16_t row = gTpagClutMap[tpagIndex];
	if (row == 0xFFFF || row >= gClutCount) {
		return -1.0f;
	}
	return ((float)row + 0.5f) / (float)gClutCount;
}
