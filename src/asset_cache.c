#include "asset_cache.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "stb_ds.h"

typedef struct CacheEntry_ {
    uint64_t blobOffset;
    uint32_t blobSize;
    uint8_t* data;
    struct CacheEntry_* prev;  // LRU doubly-linked list
    struct CacheEntry_* next;
} CacheEntry;

typedef struct AssetCache_ {
    CacheEntry* entries;  // Hash map (using stb_ds)
    CacheEntry* lru_head;  // Most recently used
    CacheEntry* lru_tail;  // Least recently used
    size_t maxSize;
    size_t currentSize;
    FILE* dataWinFile;
} AssetCache;

static uint64_t cache_key(uint64_t blobOffset) {
    return blobOffset;
}

static void lru_move_to_head(AssetCache* cache, CacheEntry* entry) {
    if (entry == cache->lru_head) return;
    
    // Remove from current position
    if (entry->prev) entry->prev->next = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
    if (entry == cache->lru_tail) cache->lru_tail = entry->prev;
    
    // Add to head
    if (cache->lru_head) cache->lru_head->prev = entry;
    entry->next = cache->lru_head;
    entry->prev = nullptr;
    cache->lru_head = entry;
    
    if (!cache->lru_tail) cache->lru_tail = entry;
}

static void lru_remove_tail(AssetCache* cache) {
    if (!cache->lru_tail) return;
    
    CacheEntry* tail = cache->lru_tail;
    cache->lru_tail = tail->prev;
    if (cache->lru_tail) {
        cache->lru_tail->next = nullptr;
    } else {
        cache->lru_head = nullptr;
    }
    
    cache->currentSize -= tail->blobSize;
    free(tail->data);
    free(tail);
}

AssetCache* AssetCache_create(size_t maxSize, const char* dataWinPath) {
    AssetCache* cache = calloc(1, sizeof(AssetCache));
    if (!cache) return nullptr;
    
    cache->dataWinFile = fopen(dataWinPath, "rb");
    if (!cache->dataWinFile) {
        free(cache);
        return nullptr;
    }
    
    cache->maxSize = maxSize;
    cache->currentSize = 0;
    
    return cache;
}

static AssetCacheEntry load_from_file(AssetCache* cache, uint64_t blobOffset, uint32_t blobSize) {
    // Sanity check - blob shouldn't be larger than 50MB
    if (blobSize > 50 * 1024 * 1024) {
        fprintf(stderr, "AssetCache: Blob size %u is suspiciously large, returning nullptr\n", blobSize);
        return (AssetCacheEntry) { nullptr, 0 };
    }
    
    uint8_t* data = malloc(blobSize);
    if (!data) {
        fprintf(stderr, "AssetCache: Failed to allocate %u bytes for blob at offset 0x%llx\n", blobSize, (unsigned long long)blobOffset);
        return (AssetCacheEntry) { nullptr, 0 };
    }
    
    if (fseek(cache->dataWinFile, (long)blobOffset, SEEK_SET) != 0) {
        fprintf(stderr, "AssetCache: Failed to seek to offset 0x%llx\n", (unsigned long long)blobOffset);
        free(data);
        return (AssetCacheEntry) { nullptr, 0 };
    }
    
    size_t bytesRead = fread(data, 1, blobSize, cache->dataWinFile);
    if (bytesRead != blobSize) {
        fprintf(stderr, "AssetCache: Read %zu bytes but expected %u from offset 0x%llx\n", bytesRead, blobSize, (unsigned long long)blobOffset);
        free(data);
        return (AssetCacheEntry) { nullptr, 0 };
    }
    
    return (AssetCacheEntry) { data, blobSize };
}

static AssetCacheEntry cache_get_or_load(AssetCache* cache, uint64_t blobOffset, uint32_t blobSize) {
    if (!cache || !cache->dataWinFile) {
        return (AssetCacheEntry) { nullptr, 0 };
    }
    
    // Look for entry in hash map
    CacheEntry* entry = nullptr;
    for (size_t i = 0; i < arrlen(cache->entries); i++) {
        if (cache->entries[i].blobOffset == blobOffset) {
            entry = &cache->entries[i];
            break;
        }
    }
    
    if (entry) {
        // Move to head of LRU list
        lru_move_to_head(cache, entry);
        return (AssetCacheEntry) { entry->data, entry->blobSize };
    }
    
    // Load from file
    AssetCacheEntry loaded = load_from_file(cache, blobOffset, blobSize);
    if (!loaded.data) {
        return loaded;  // Failed to load
    }
    
    // Make room if needed
    while (cache->currentSize + blobSize > cache->maxSize && cache->lru_tail) {
        lru_remove_tail(cache);
    }
    
    // Add to cache
    arrput(cache->entries, ((CacheEntry) {
        .blobOffset = blobOffset,
        .blobSize = blobSize,
        .data = (uint8_t*)loaded.data,
        .prev = nullptr,
        .next = nullptr
    }));
    
    CacheEntry* newEntry = &cache->entries[arrlen(cache->entries) - 1];
    cache->currentSize += blobSize;
    lru_move_to_head(cache, newEntry);
    
    return (AssetCacheEntry) { loaded.data, blobSize };
}

AssetCacheEntry AssetCache_getTextureBlobData(AssetCache* cache, uint64_t blobOffset, uint32_t blobSize) {
    return cache_get_or_load(cache, blobOffset, blobSize);
}

AssetCacheEntry AssetCache_getAudioBlobData(AssetCache* cache, uint64_t blobOffset, uint32_t blobSize) {
    return cache_get_or_load(cache, blobOffset, blobSize);
}

void AssetCache_destroy(AssetCache* cache) {
    if (!cache) return;
    
    if (cache->dataWinFile) {
        fclose(cache->dataWinFile);
    }
    
    for (size_t i = 0; i < arrlen(cache->entries); i++) {
        free(cache->entries[i].data);
    }
    arrfree(cache->entries);
    
    free(cache);
}
