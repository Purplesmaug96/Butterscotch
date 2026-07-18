#ifndef _BS_SDL_RENDERER_H_
#define _BS_SDL_RENDERER_H_

#include "common.h"
#include "renderer.h"
#include "runner.h"

#include <SDL3/SDL.h>

// Exposed in the header so platform-specific code (main.c) can access FBO fields for screenshots.
typedef struct {
    Renderer base; // Must be first field for struct embedding

	SDL_Window* window;
	SDL_Renderer* renderer;

    bool alphaTestEnable;
    float alphaTestRef;
    bool colorWriteR, colorWriteG, colorWriteB, colorWriteA;
    bool fogEnable;
    uint32_t fogColor; // BGR

    SDL_Texture** sdlTextures;       // one SDL texture per TXTR page
    int32_t* textureWidths;   // needed for UV normalization
    int32_t* textureHeights;
    bool* textureLoaded;      // lazy loading: true once PNG decoded and uploaded
    uint32_t textureCount;

    SDL_Texture* whiteTexture; // 1x1 white pixel for drawing primitives (rectangles, lines, etc.)

    int32_t windowW; // stored from beginFrame for endFrame blit
    int32_t windowH;
    int32_t gameW; // game width (matches the application_surface size)
    int32_t gameH; // game height (matches the application_surface size)

    SDL_Texture* textureStages[8]; // for texture_set_stage

    // Original counts from data.win (dynamic slots start at these indices)
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
    SDL_Texture** surfaces;
    SDL_Texture** surfaceTexture;
    int32_t* surfaceWidth;
    int32_t* surfaceHeight;
    uint32_t surfaceCount;

    // Blending mode + factors
    int32_t currentBlendMode;
    int32_t currentSFactor;
    int32_t currentDFactor;
    int32_t currentSFactorAlpha;
    int32_t currentDFactorAlpha;

    // Current view / port (set by sdlBeginView)
    float currentViewX;
    float currentViewY;
    float currentViewW;
    float currentViewH;
    float currentViewAngle;
    int32_t currentPortX;
    int32_t currentPortY;
    int32_t currentPortW;
    int32_t currentPortH;
} SDLRenderer;

bool SDLRenderer_ensureTextureLoaded(SDLRenderer* sdl, uint32_t pageId);
Renderer* SDLRenderer_create(void);

#endif /* _BS_SDL_RENDERER_H_ */
