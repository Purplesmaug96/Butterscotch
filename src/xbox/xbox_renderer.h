#pragma once

#include "renderer.h"

typedef struct AssetCache_ AssetCache;

typedef struct {
    void* pixels;      // Must be allocated with MmAllocateContiguousMemoryEx
    int width;
    int height;
    int pitch;
    int format;      // e.g., NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8
    int blendMode;
} PbTexture;

typedef struct {
    Renderer base;

    PbTexture** renderTextures;
    int32_t* textureWidths;
    int32_t* textureHeights;
    uint32_t* renderTexturesUsedTracker;
    uint32_t textureCount;


    PbTexture* whiteTexture;

    PbTexture* fboTexture;
    int32_t fboWidth;
    int32_t fboHeight;
    int32_t windowW;
    int32_t windowH;
    int32_t gameW;
    int32_t gameH;

    // View state
    float currentViewAngle;
    float currentViewX, currentViewY;
    float currentViewW, currentViewH;
    float currentPortX, currentPortY;
    float currentPortW, currentPortH;

    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;

    AssetCache* assetCache;
} MainRenderer;

enum {
    BLENDMODE_NONE = 0,
    BLENDMODE_BLEND,
    BLENDMODE_ADD,
    BLENDMODE_MOD
};

Renderer* MainRenderer_create();
void MainRenderer_setAssetCache(MainRenderer* renderer, AssetCache* cache);