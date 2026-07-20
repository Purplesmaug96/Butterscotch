#ifndef _BS_TEXTURES_H_
#define _BS_TEXTURES_H_

#include <stdbool.h>
#include <stdint.h>

// Loader for the Xbox360 textures.bin/atlas.bin/clut bundles produced by ButterscotchPreprocessor.
// This replaces the old TXTR-based texture loading with indexed palette-based textures,
// saving significant memory compared to storing full RGBA textures.

bool Xbox360Textures_init(const char* texturesBinPath, const char* atlasBinPath, const char* clut8Path);
void Xbox360Textures_free();

// Total number of texture pages from data.win (original TXTR count).
uint32_t Xbox360Textures_getPageCount();

// Returns true if a TPAG index has a valid atlas entry.
bool Xbox360Textures_hasTpagMapping(int32_t tpagIndex);

// Get the atlas ID, position, and CLUT index for a given TPAG.
bool Xbox360Textures_getTpagAtlasInfo(int32_t tpagIndex, int* outAtlasId, int* outAtlasX, int* outAtlasY,
									  int* outWidth, int* outHeight, int* outClutIndex, int* outBpp);

// Loads an atlas page's indexed pixel data and expands it to RGBA using the CLUT.
// The caller MUST free *outPixels with free() after uploading to the GPU.
// Returns false if the page is missing/empty.
bool Xbox360Textures_loadPage(int32_t tpagIndex, int* outW, int* outH, uint8_t** outPixels);

// Load an atlas by its atlas ID (for direct atlas usage, e.g. tiles).
// The caller MUST free *outPixels with free() after uploading to the GPU.
bool Xbox360Textures_loadAtlasById(int atlasId, int* outW, int* outH, uint8_t** outPixels);

// Get the number of atlas pages available.
uint32_t Xbox360Textures_getAtlasCount();

#endif /* _BS_TEXTURES_H_ */