#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct AssetCache_ AssetCache;

typedef struct {
    const uint8_t* data;
    size_t size;
} AssetCacheEntry;

// Create an asset cache with the given maximum size in bytes
AssetCache* AssetCache_create(size_t maxSize, const char* dataWinPath);

// Get texture blob data from cache (or load from disc if not cached)
AssetCacheEntry AssetCache_getTextureBlobData(AssetCache* cache, uint64_t blobOffset, uint32_t blobSize);

// Get audio blob data from cache (or load from disc if not cached)
AssetCacheEntry AssetCache_getAudioBlobData(AssetCache* cache, uint64_t blobOffset, uint32_t blobSize);

// Destroy cache and free all resources
void AssetCache_destroy(AssetCache* cache);
