#pragma once

#include "renderer.h"
#include <SDL.h>

#if defined(XBOX_SDL_USE_RENDERGEOMETRY) && !defined(XBOX_SDL_HAS_RENDERGEOMETRY)
#undef XBOX_SDL_USE_RENDERGEOMETRY
#endif

#define XBOX_SDL_PIXELFORMAT SDL_PIXELFORMAT_ABGR4444
#define XBOX_SDL_PIXEL_PITCH (w * 2)

typedef struct AssetCache_ AssetCache;

typedef struct {
    Renderer base;

    SDL_Renderer* sdlRenderer;
    SDL_Window* sdlWindow;

    SDL_Texture** sdlTextures;
    int32_t* textureWidths;
    int32_t* textureHeights;
    uint32_t* sdlTexturesUsedTracker;
    uint32_t textureCount;


    SDL_Texture* whiteTexture;

    SDL_Texture* fboTexture;
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

Renderer* MainRenderer_create(SDL_Window* window, SDL_Renderer* renderer);
void MainRenderer_setAssetCache(MainRenderer* renderer, AssetCache* cache);