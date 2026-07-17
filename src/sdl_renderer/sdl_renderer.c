#include "sdl_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"
#include "utils.h"
#include "stb_ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===[ Helper ]===

static SDLRenderer* SDL(Renderer* r) {
    return (SDLRenderer*)r;
}

// ===[ Lifecycle ]===

static void sdlInit(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED DataWin* dataWin) {
}

static void sdlDestroy(MAYBE_UNUSED Renderer* renderer) {
}

// ===[ Frame ]===

static void sdlBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->gameW = gameW;
    sdl->gameH = gameH;
    sdl->windowW = windowW;
    sdl->windowH = windowH;
}

static void sdlEndFrameInit(MAYBE_UNUSED Renderer* renderer) {
}

static void sdlEndFrameEnd(MAYBE_UNUSED Renderer* renderer) {
}

// ===[ View / Camera ]===

static void sdlBeginView(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t viewX, MAYBE_UNUSED int32_t viewY,
                         MAYBE_UNUSED int32_t viewW, MAYBE_UNUSED int32_t viewH,
                         MAYBE_UNUSED int32_t portX, MAYBE_UNUSED int32_t portY,
                         MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH,
                         MAYBE_UNUSED float viewAngle) {
}

static void sdlEndView(MAYBE_UNUSED Renderer* renderer) {
}

static void sdlApplyProjection(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED const Matrix4f* viewMatrix,
                               MAYBE_UNUSED const Matrix4f* projectionMatrix) {
}

// ===[ GUI ]===

static void sdlBeginGUI(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t guiW, MAYBE_UNUSED int32_t guiH,
                        MAYBE_UNUSED int32_t portX, MAYBE_UNUSED int32_t portY,
                        MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH,
                        MAYBE_UNUSED int32_t targetSurfaceId) {
}

static void sdlSetGuiProjection(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t guiW,
                                MAYBE_UNUSED int32_t guiH, MAYBE_UNUSED int32_t portW,
                                MAYBE_UNUSED int32_t portH, MAYBE_UNUSED bool renderingToUserSurface) {
}

static void sdlEndGUI(MAYBE_UNUSED Renderer* renderer) {
}

// ===[ Drawing: Sprites ]===

static void sdlDrawSprite(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t tpagIndex,
                          MAYBE_UNUSED float x, MAYBE_UNUSED float y,
                          MAYBE_UNUSED float originX, MAYBE_UNUSED float originY,
                          MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
                          MAYBE_UNUSED float angleDeg, MAYBE_UNUSED uint32_t color,
                          MAYBE_UNUSED float alpha) {
}

static void sdlDrawSpritePart(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t tpagIndex,
                              MAYBE_UNUSED int32_t srcOffX, MAYBE_UNUSED int32_t srcOffY,
                              MAYBE_UNUSED int32_t srcW, MAYBE_UNUSED int32_t srcH,
                              MAYBE_UNUSED float x, MAYBE_UNUSED float y,
                              MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
                              MAYBE_UNUSED float angleDeg, MAYBE_UNUSED float pivotX,
                              MAYBE_UNUSED float pivotY, MAYBE_UNUSED uint32_t color,
                              MAYBE_UNUSED float alpha) {
}

static void sdlDrawSpritePos(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t tpagIndex,
                             MAYBE_UNUSED float x1, MAYBE_UNUSED float y1,
                             MAYBE_UNUSED float x2, MAYBE_UNUSED float y2,
                             MAYBE_UNUSED float x3, MAYBE_UNUSED float y3,
                             MAYBE_UNUSED float x4, MAYBE_UNUSED float y4,
                             MAYBE_UNUSED float alpha) {
}

// ===[ Drawing: Primitives ]===

static void sdlDrawRectangle(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                             MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                             MAYBE_UNUSED float y2, MAYBE_UNUSED uint32_t color,
                             MAYBE_UNUSED float alpha, MAYBE_UNUSED bool outline) {
}

static void sdlDrawRectangleColor(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                                  MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                                  MAYBE_UNUSED float y2, MAYBE_UNUSED uint32_t color1,
                                  MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3,
                                  MAYBE_UNUSED uint32_t color4, MAYBE_UNUSED float alpha,
                                  MAYBE_UNUSED bool outline) {
}

static void sdlDrawLine(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                        MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                        MAYBE_UNUSED float y2, MAYBE_UNUSED float width,
                        MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {
}

static void sdlDrawLineColor(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                             MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                             MAYBE_UNUSED float y2, MAYBE_UNUSED float width,
                             MAYBE_UNUSED uint32_t color1, MAYBE_UNUSED uint32_t color2,
                             MAYBE_UNUSED float alpha) {
}

static void sdlDrawTriangle(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                            MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                            MAYBE_UNUSED float y2, MAYBE_UNUSED float x3,
                            MAYBE_UNUSED float y3, MAYBE_UNUSED uint32_t color1,
                            MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3,
                            MAYBE_UNUSED float alpha, MAYBE_UNUSED bool outline) {
}

// ===[ Drawing: Text ]===

static void sdlDrawText(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED const char* text,
                        MAYBE_UNUSED float x, MAYBE_UNUSED float y,
                        MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
                        MAYBE_UNUSED float angleDeg, MAYBE_UNUSED float lineSeparation) {
}

static void sdlDrawTextColor(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED const char* text,
                             MAYBE_UNUSED float x, MAYBE_UNUSED float y,
                             MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
                             MAYBE_UNUSED float angleDeg, MAYBE_UNUSED int32_t c1,
                             MAYBE_UNUSED int32_t c2, MAYBE_UNUSED int32_t c3,
                             MAYBE_UNUSED int32_t c4, MAYBE_UNUSED float alpha,
                             MAYBE_UNUSED float lineSeparation) {
}

// ===[ Flush / Clear ]===

static void sdlFlush(MAYBE_UNUSED Renderer* renderer) {
}

static void sdlClearScreen(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED uint32_t color,
                           MAYBE_UNUSED float alpha) {
}

// ===[ Sprite Management ]===

static int32_t sdlCreateSpriteFromSurface(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID,
                                          MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y,
                                          MAYBE_UNUSED int32_t w, MAYBE_UNUSED int32_t h,
                                          MAYBE_UNUSED bool removeback, MAYBE_UNUSED bool smooth,
                                          MAYBE_UNUSED int32_t xorig, MAYBE_UNUSED int32_t yorig) {
    return -1;
}

static void sdlDeleteSprite(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t spriteIndex) {
}

// ===[ Blend / GPU State ]===

static BlendFactors sdlGpuGetBlendFactors(MAYBE_UNUSED Renderer* renderer) {
    return (BlendFactors){0, 0, 0, 0};
}

static int32_t sdlGpuGetBlendMode(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);
    return sdl->currentBlendMode;
}

static void sdlGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->currentBlendMode = mode;
}

static void sdlGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor,
                                  int32_t sfactor_alpha, int32_t dfactor_alpha) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->currentSFactor = sfactor;
    sdl->currentDFactor = dfactor;
    sdl->currentSFactorAlpha = sfactor_alpha;
    sdl->currentDFactorAlpha = dfactor_alpha;
}

static void sdlGpuSetBlendEnable(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED bool enable) {
}

static bool sdlGpuGetBlendEnable(MAYBE_UNUSED Renderer* renderer) {
    return true;
}

static void sdlGpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->alphaTestEnable = enable;
}

static void sdlGpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->alphaTestRef = (float)ref / 255.0f;
}

static void sdlGpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->colorWriteR = red;
    sdl->colorWriteG = green;
    sdl->colorWriteB = blue;
    sdl->colorWriteA = alpha;
}

static void sdlGpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
    SDLRenderer* sdl = SDL(renderer);
    *red = sdl->colorWriteR;
    *green = sdl->colorWriteG;
    *blue = sdl->colorWriteB;
    *alpha = sdl->colorWriteA;
}

static void sdlGpuSetFog(Renderer* renderer, bool enable, uint32_t color) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->fogEnable = enable;
    sdl->fogColor = color;
}

// ===[ Tile Rendering ]===

static void sdlDrawSpriteTiled(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t tpagIndex,
                               MAYBE_UNUSED float originX, MAYBE_UNUSED float originY,
                               MAYBE_UNUSED float x, MAYBE_UNUSED float y,
                               MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
                               MAYBE_UNUSED bool tileX, MAYBE_UNUSED bool tileY,
                               MAYBE_UNUSED float roomW, MAYBE_UNUSED float roomH,
                               MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {
}

static void sdlDrawTiledPart(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t tpagIndex,
                             MAYBE_UNUSED int32_t srcX, MAYBE_UNUSED int32_t srcY,
                             MAYBE_UNUSED int32_t srcW, MAYBE_UNUSED int32_t srcH,
                             MAYBE_UNUSED float dstX, MAYBE_UNUSED float dstY,
                             MAYBE_UNUSED float dstW, MAYBE_UNUSED float dstH,
                             MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {
}

// ===[ Surface Functions ]===

static int32_t sdlCreateSurface(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t width,
                                MAYBE_UNUSED int32_t height) {
    return -1;
}

static bool sdlSurfaceExists(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID) {
    return false;
}

static bool sdlSetRenderTarget(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID,
                               MAYBE_UNUSED bool implicitApplicationSurface) {
    return false;
}

static int32_t sdlEnsureApplicationSurface(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t width,
                                           MAYBE_UNUSED int32_t height) {
    return -1;
}

static float sdlGetSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID >= 0 && surfaceID < (int32_t)sdl->surfaceCount && sdl->surfaceWidth) {
        return (float)sdl->surfaceWidth[surfaceID];
    }
    return 0.0f;
}

static float sdlGetSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID >= 0 && surfaceID < (int32_t)sdl->surfaceCount && sdl->surfaceHeight) {
        return (float)sdl->surfaceHeight[surfaceID];
    }
    return 0.0f;
}

static void sdlDrawSurface(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID,
                           MAYBE_UNUSED int32_t srcLeft, MAYBE_UNUSED int32_t srcTop,
                           MAYBE_UNUSED int32_t srcWidth, MAYBE_UNUSED int32_t srcHeight,
                           MAYBE_UNUSED float x, MAYBE_UNUSED float y,
                           MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
                           MAYBE_UNUSED float angleDeg, MAYBE_UNUSED uint32_t color,
                           MAYBE_UNUSED float alpha) {
}

static void sdlDrawSurfaceTiled(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID,
                                MAYBE_UNUSED float x, MAYBE_UNUSED float y,
                                MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
                                MAYBE_UNUSED float roomW, MAYBE_UNUSED float roomH,
                                MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {
}

static void sdlSurfaceResize(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID,
                             MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) {
}

static void sdlSurfaceFree(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID) {
}

static void sdlSurfaceCopy(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t destSurfaceID,
                           MAYBE_UNUSED int32_t destX, MAYBE_UNUSED int32_t destY,
                           MAYBE_UNUSED int32_t srcSurfaceID, MAYBE_UNUSED int32_t srcX,
                           MAYBE_UNUSED int32_t srcY, MAYBE_UNUSED int32_t srcW,
                           MAYBE_UNUSED int32_t srcH, MAYBE_UNUSED bool part) {
}

static bool sdlSurfaceGetPixels(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID,
                                MAYBE_UNUSED uint8_t* outRGBA) {
    return false;
}

// ===[ Shader Functions ]===

static void sdlGpuSetShader(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex) {
}

static void sdlGpuResetShader(MAYBE_UNUSED Renderer* renderer) {
}

static int32_t sdlShaderGetUniform(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex,
                                   MAYBE_UNUSED char* uniform) {
    return -1;
}

static int32_t sdlShaderGetSamplerIndex(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shaderIndex,
                                        MAYBE_UNUSED char* uniform) {
    return -1;
}

static void sdlShaderSetUniformF(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t handle,
                                 MAYBE_UNUSED int32_t count, MAYBE_UNUSED float value1,
                                 MAYBE_UNUSED float value2, MAYBE_UNUSED float value3,
                                 MAYBE_UNUSED float value4) {
}

static void sdlShaderSetUniformFArray(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t handle,
                                      MAYBE_UNUSED float* values, MAYBE_UNUSED uint32_t count) {
}

static void sdlShaderSetUniformI(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t handle,
                                 MAYBE_UNUSED int32_t count, MAYBE_UNUSED int32_t value1,
                                 MAYBE_UNUSED int32_t value2, MAYBE_UNUSED int32_t value3,
                                 MAYBE_UNUSED int32_t value4) {
}

// ===[ Texture Access ]===

static uint32_t sdlSpriteGetTexture(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t tpagIndex) {
    return 0;
}

static uint32_t sdlSurfaceGetTexture(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t surfaceID) {
    return 0;
}

static float sdlTextureGetTexelWidth(Renderer* renderer, uint32_t texID) {
    SDLRenderer* sdl = SDL(renderer);
    if (texID < sdl->textureCount && sdl->textureWidths) {
        return (float)sdl->textureWidths[texID];
    }
    return 0.0f;
}

static float sdlTextureGetTexelHeight(Renderer* renderer, uint32_t texID) {
    SDLRenderer* sdl = SDL(renderer);
    if (texID < sdl->textureCount && sdl->textureHeights) {
        return (float)sdl->textureHeights[texID];
    }
    return 0.0f;
}

static bool sdlTextureGetUVs(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED uint32_t texID,
                             MAYBE_UNUSED float* outUVs) {
    return false;
}

static void sdlTextureSetStage(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t slot,
                               MAYBE_UNUSED uint32_t texID) {
}

// ===[ Shader Queries ]===

static bool sdlShaderIsCompiled(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shader) {
    return false;
}

static bool sdlShadersSupported(void) {
    return false;
}

// ===[ Matrix ]===

static void sdlSetMatrix(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t matrixType,
                         MAYBE_UNUSED Matrix4f matrix) {
}

// ===[ Vtable ]===

static RendererVtable sdlVtable;

// ===[ Public API ]===

bool SDLRenderer_ensureTextureLoaded(MAYBE_UNUSED SDLRenderer* sdl, MAYBE_UNUSED uint32_t pageId) {
    return false;
}

Renderer* SDLRenderer_create(void) {
    SDLRenderer* sdl = (SDLRenderer*)safeCalloc(1, sizeof(SDLRenderer));
    sdl->base.vtable = &sdlVtable;

    sdlVtable.init                        = sdlInit;
    sdlVtable.destroy                     = sdlDestroy;
    sdlVtable.beginFrame                  = sdlBeginFrame;
    sdlVtable.endFrameInit                = sdlEndFrameInit;
    sdlVtable.endFrameEnd                 = sdlEndFrameEnd;
    sdlVtable.beginView                   = sdlBeginView;
    sdlVtable.endView                     = sdlEndView;
    sdlVtable.applyProjection             = sdlApplyProjection;
    sdlVtable.beginGUI                    = sdlBeginGUI;
    sdlVtable.setGuiProjection            = sdlSetGuiProjection;
    sdlVtable.endGUI                      = sdlEndGUI;
    sdlVtable.drawSprite                  = sdlDrawSprite;
    sdlVtable.drawSpritePart              = sdlDrawSpritePart;
    sdlVtable.drawSpritePos               = sdlDrawSpritePos;
    sdlVtable.drawRectangle               = sdlDrawRectangle;
    sdlVtable.drawRectangleColor          = sdlDrawRectangleColor;
    sdlVtable.drawLine                    = sdlDrawLine;
    sdlVtable.drawLineColor               = sdlDrawLineColor;
    sdlVtable.drawTriangle                = sdlDrawTriangle;
    sdlVtable.drawText                    = sdlDrawText;
    sdlVtable.drawTextColor               = sdlDrawTextColor;
    sdlVtable.flush                       = sdlFlush;
    sdlVtable.clearScreen                 = sdlClearScreen;
    sdlVtable.createSpriteFromSurface     = sdlCreateSpriteFromSurface;
    sdlVtable.deleteSprite                = sdlDeleteSprite;
    sdlVtable.gpuGetBlendFactors          = sdlGpuGetBlendFactors;
    sdlVtable.gpuGetBlendMode             = sdlGpuGetBlendMode;
    sdlVtable.gpuSetBlendMode             = sdlGpuSetBlendMode;
    sdlVtable.gpuSetBlendModeExt          = sdlGpuSetBlendModeExt;
    sdlVtable.gpuSetBlendEnable           = sdlGpuSetBlendEnable;
    sdlVtable.gpuGetBlendEnable           = sdlGpuGetBlendEnable;
    sdlVtable.gpuSetAlphaTestEnable       = sdlGpuSetAlphaTestEnable;
    sdlVtable.gpuSetAlphaTestRef          = sdlGpuSetAlphaTestRef;
    sdlVtable.gpuSetColorWriteEnable      = sdlGpuSetColorWriteEnable;
    sdlVtable.gpuGetColorWriteEnable      = sdlGpuGetColorWriteEnable;
    sdlVtable.gpuSetFog                   = sdlGpuSetFog;
    sdlVtable.drawTile                    = nullptr;
    sdlVtable.drawSpriteTiled             = sdlDrawSpriteTiled;
    sdlVtable.drawTiledPart               = sdlDrawTiledPart;
    sdlVtable.createSurface               = sdlCreateSurface;
    sdlVtable.surfaceExists               = sdlSurfaceExists;
    sdlVtable.setRenderTarget             = sdlSetRenderTarget;
    sdlVtable.ensureApplicationSurface    = sdlEnsureApplicationSurface;
    sdlVtable.getSurfaceWidth             = sdlGetSurfaceWidth;
    sdlVtable.getSurfaceHeight            = sdlGetSurfaceHeight;
    sdlVtable.drawSurface                 = sdlDrawSurface;
    sdlVtable.drawSurfaceTiled            = sdlDrawSurfaceTiled;
    sdlVtable.surfaceResize               = sdlSurfaceResize;
    sdlVtable.surfaceFree                 = sdlSurfaceFree;
    sdlVtable.surfaceCopy                 = sdlSurfaceCopy;
    sdlVtable.surfaceGetPixels            = sdlSurfaceGetPixels;
    sdlVtable.gpuSetShader                = sdlGpuSetShader;
    sdlVtable.gpuResetShader              = sdlGpuResetShader;
    sdlVtable.shaderGetUniform            = sdlShaderGetUniform;
    sdlVtable.shaderGetSamplerIndex       = sdlShaderGetSamplerIndex;
    sdlVtable.shaderSetUniformF           = sdlShaderSetUniformF;
    sdlVtable.shaderSetUniformFArray      = sdlShaderSetUniformFArray;
    sdlVtable.shaderSetUniformI           = sdlShaderSetUniformI;
    sdlVtable.spriteGetTexture            = sdlSpriteGetTexture;
    sdlVtable.surfaceGetTexture           = sdlSurfaceGetTexture;
    sdlVtable.textureGetTexelWidth        = sdlTextureGetTexelWidth;
    sdlVtable.textureGetTexelHeight       = sdlTextureGetTexelHeight;
    sdlVtable.textureGetUVs               = sdlTextureGetUVs;
    sdlVtable.textureSetStage             = sdlTextureSetStage;
    sdlVtable.shaderIsCompiled            = sdlShaderIsCompiled;
    sdlVtable.shadersSupported            = sdlShadersSupported;
    sdlVtable.setMatrix                   = sdlSetMatrix;

    sdl->base.drawColor       = 0xFFFFFF;
    sdl->base.drawAlpha       = 1.0f;
    sdl->base.drawFont        = -1;
    sdl->base.drawHalign      = 0;
    sdl->base.drawValign      = 0;
    sdl->base.circlePrecision = 24;
    sdl->base.currentShader   = -1;
    sdl->base.cameraCurrent   = 0;

    sdl->alphaTestEnable = false;
    sdl->alphaTestRef    = 0.0f;
    sdl->colorWriteR     = true;
    sdl->colorWriteG     = true;
    sdl->colorWriteB     = true;
    sdl->colorWriteA     = true;
    sdl->fogEnable       = false;
    sdl->fogColor        = 0;

    sdl->windowW = 0;
    sdl->windowH = 0;
    sdl->gameW   = 0;
    sdl->gameH   = 0;

    sdl->textureCount          = 0;
    sdl->sdlTextures           = nullptr;
    sdl->textureWidths         = nullptr;
    sdl->textureHeights        = nullptr;
    sdl->textureLoaded         = nullptr;

    sdl->originalTexturePageCount = 0;
    sdl->originalTpagCount        = 0;
    sdl->originalSpriteCount      = 0;
    sdl->surfaces                 = nullptr;
    sdl->surfaceTexture           = nullptr;
    sdl->surfaceWidth             = nullptr;
    sdl->surfaceHeight            = nullptr;
    sdl->surfaceCount             = 0;

    sdl->currentBlendMode     = 0;
    sdl->currentSFactor       = 0;
    sdl->currentDFactor       = 0;
    sdl->currentSFactorAlpha  = 0;
    sdl->currentDFactorAlpha  = 0;

    return (Renderer*)sdl;
}
