#include "sdl_renderer.h"
#include "platformdefs.h"
#include "matrix_math.h"
#include "text_utils.h"
#include "image_decoder.h"
#include "utils.h"
#include "stb_ds.h"
#include "math_compat.h"

#include <SDL3/SDL_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ===[ Helper ]===

static SDLRenderer* SDL(Renderer* r) {
    return (SDLRenderer*)r;
}

static uint32_t findOrAllocateSurfaceSlot(SDLRenderer* sdl) {
    repeat(sdl->surfaceCount, i) {
        if (sdl->surfaces[i] == nullptr)
            return i;
    }
    uint32_t newIndex = sdl->surfaceCount++;
    sdl->surfaces       = (SDL_Texture**)safeRealloc(sdl->surfaces,       sdl->surfaceCount * sizeof(SDL_Texture*));
    sdl->surfaceTexture = (SDL_Texture**)safeRealloc(sdl->surfaceTexture, sdl->surfaceCount * sizeof(SDL_Texture*));
    sdl->surfaceWidth   = (int32_t*)safeRealloc(sdl->surfaceWidth,        sdl->surfaceCount * sizeof(int32_t));
    sdl->surfaceHeight  = (int32_t*)safeRealloc(sdl->surfaceHeight,       sdl->surfaceCount * sizeof(int32_t));
    sdl->surfaces[newIndex]       = nullptr;
    sdl->surfaceTexture[newIndex] = nullptr;
    sdl->surfaceWidth[newIndex]   = 0;
    sdl->surfaceHeight[newIndex]  = 0;
    return newIndex;
}

static void transformWorldToView(SDLRenderer* sdl, float wx, float wy, float* vx, float* vy) {
    float lx = wx - sdl->currentViewX;
    float ly = wy - sdl->currentViewY;

    if (sdl->currentViewAngle != 0.0f) {
        float cx = sdl->currentViewW / 2.0f;
        float cy = sdl->currentViewH / 2.0f;

        lx -= cx;
        ly -= cy;

        float angleRad = -sdl->currentViewAngle * ((float) M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);

        float nx = lx * cosA - ly * sinA;
        float ny = lx * sinA + ly * cosA;

        lx = nx + cx;
        ly = ny + cy;
    }

    *vx = lx * (sdl->currentPortW / sdl->currentViewW) + (float)sdl->currentPortX;
    *vy = ly * (sdl->currentPortH / sdl->currentViewH) + (float)sdl->currentPortY;
}

static SDL_Texture* CreateWhiteTextureCopy(SDL_Renderer* renderer, SDL_Texture* orig_texture) {
    float w, h;
    if (!SDL_GetTextureSize(orig_texture, &w, &h)) {
        return NULL;
    }

    SDL_Texture* white_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                                   SDL_TEXTUREACCESS_TARGET, (int)w, (int)h);
    if (!white_texture) {
        return NULL;
    }

    SDL_BlendMode maskMode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ZERO, SDL_BLENDOPERATION_ADD
    );

    SDL_Texture* old_target = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, white_texture);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 0);
    SDL_RenderClear(renderer);

    SDL_BlendMode old_blend;
    SDL_GetTextureBlendMode(orig_texture, &old_blend);
    SDL_SetTextureBlendMode(orig_texture, maskMode);

    SDL_RenderTexture(renderer, orig_texture, NULL, NULL);

    SDL_SetTextureBlendMode(orig_texture, old_blend);
    SDL_SetRenderTarget(renderer, old_target);

    SDL_SetTextureBlendMode(white_texture, SDL_BLENDMODE_BLEND);

    SDL_ScaleMode orig_scale_mode;
    if (SDL_GetTextureScaleMode(orig_texture, &orig_scale_mode)) {
        SDL_SetTextureScaleMode(white_texture, orig_scale_mode);
    }

    return white_texture;
}

static void emitQuad(SDLRenderer* sdl, SDL_Texture* tex,
                     float x[4], float y[4], float u[4], float v[4],
                     float r[4], float g[4], float b[4], float a[4]) {
    SDL_Vertex verts[4];

    for (int i = 0; i < 4; i++) {
        transformWorldToView(sdl, x[i], y[i], &verts[i].position.x, &verts[i].position.y);

        verts[i].tex_coord.x = u[i];
        verts[i].tex_coord.y = v[i];

        verts[i].color.r = sdl->fogEnable ? BGR_R(sdl->fogColor) : r[i];
        verts[i].color.g = sdl->fogEnable ? BGR_G(sdl->fogColor) : g[i];
        verts[i].color.b = sdl->fogEnable ? BGR_B(sdl->fogColor) : b[i];
        verts[i].color.a = a[i];
    }

	if (sdl->fogEnable) {
		if (sdl->preFogTex != tex && tex != nullptr) {
			sdl->preFogTex = tex;
			tex = CreateWhiteTextureCopy(sdl->renderer, tex);
			if (sdl->fogTex) SDL_DestroyTexture(sdl->fogTex);
			sdl->fogTex = tex;
		}
		else if (sdl->preFogTex == tex) tex = sdl->fogTex;
	}

    int indices[6] = {0, 1, 2, 2, 3, 0};
    SDL_RenderGeometry(sdl->renderer, tex, verts, 4, indices, 6);
}

static void emitColoredQuad(SDLRenderer* sdl, SDL_Texture* tex, float x[4], float y[4], float u[4], float v[4], float r, float g, float b, float a) {
    float rc[4] = {r, r, r, r};
    float gc[4] = {g, g, g, g};
    float bc[4] = {b, b, b, b};
    float ac[4] = {a, a, a, a};
    emitQuad(sdl, tex, x, y, u, v, rc, gc, bc, ac);
}

static void emitTri(SDLRenderer* sdl, SDL_Texture* tex,
                    float x[3], float y[3], float u[3], float v[3],
                    float r[3], float g[3], float b[3], float a[3]) {
    SDL_Vertex verts[3];

    for (int i = 0; i < 3; i++) {
        transformWorldToView(sdl, x[i], y[i], &verts[i].position.x, &verts[i].position.x);

        verts[i].tex_coord.x = u[i];
        verts[i].tex_coord.y = v[i];

        verts[i].color.r = sdl->fogEnable ? BGR_R(sdl->fogColor) : r[i];
        verts[i].color.g = sdl->fogEnable ? BGR_G(sdl->fogColor) : g[i];
        verts[i].color.b = sdl->fogEnable ? BGR_B(sdl->fogColor) : b[i];
        verts[i].color.a = a[i];
    }

	if (sdl->fogEnable) {
		if (sdl->preFogTex != tex && tex != nullptr) {
			sdl->preFogTex = tex;
			tex = CreateWhiteTextureCopy(sdl->renderer, tex);
			if (sdl->fogTex) SDL_DestroyTexture(sdl->fogTex);
			sdl->fogTex = tex;
		}
		else if (sdl->preFogTex == tex) tex = sdl->fogTex;
	}

    int indices[3] = {0, 1, 2};
    SDL_RenderGeometry(sdl->renderer, tex, verts, 3, indices, 3);
}

// Lazy-load a texture on demand when it's first needed
static bool ensureTextureLoaded(SDLRenderer* sdl, DataWin* dw, uint32_t pageId) {
    if (sdl->textureLoaded[pageId]) return (sdl->textureWidths[pageId] != 0);

    sdl->textureLoaded[pageId] = true;

    Texture* txtr = &dw->txtr.textures[pageId];
    DataWin_loadTxtrIfNeeded(dw, pageId);

    int w, h;
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    uint8_t* pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t)txtr->blobSize, gm2022_5, &w, &h);
    if (pixels == nullptr) {
        fprintf(stderr, "SDL: Failed to decode TXTR page %u\n", pageId);
        return false;
    }
    if (!txtr->mapped) {
        free(txtr->blobData);
        txtr->blobData = nullptr;
    }

    sdl->textureWidths[pageId] = w;
    sdl->textureHeights[pageId] = h;

    sdl->sdlTextures[pageId] = SDL_CreateTexture(
        sdl->renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STATIC, w, h);
    if (sdl->sdlTextures[pageId]) {
        SDL_UpdateTexture(sdl->sdlTextures[pageId], NULL, pixels, w * 4);
        SDL_SetTextureScaleMode(sdl->sdlTextures[pageId], SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(sdl->sdlTextures[pageId], SDL_BLENDMODE_BLEND);

    }

    free(pixels);

    fprintf(stderr, "SDL: Loaded TXTR page %u (%dx%d)\n", pageId, w, h);
    return sdl->sdlTextures[pageId] != nullptr;
}

// ===[ Lifecycle ]===

static void sdlInit(Renderer* renderer, DataWin* dataWin) {
    renderer->dataWin = dataWin;
    SDLRenderer* sdl = SDL(renderer);

    sdl->window = (SDL_Window*)platformGetWindow();
    sdl->renderer = SDL_CreateRenderer(sdl->window, NULL);
    if (!sdl->renderer) {
        fprintf(stderr, "SDL: Failed to create renderer: %s\n", SDL_GetError());
        abort();
    }

    sdl->originalTexturePageCount = dataWin->txtr.count;
    sdl->originalTpagCount = dataWin->tpag.count;
    sdl->originalSpriteCount = dataWin->sprt.count;

    sdl->textureCount = dataWin->txtr.count;
    sdl->sdlTextures   = (SDL_Texture**)safeCalloc(sdl->textureCount, sizeof(SDL_Texture*));
    sdl->textureWidths  = (int32_t*)safeCalloc(sdl->textureCount, sizeof(int32_t));
    sdl->textureHeights = (int32_t*)safeCalloc(sdl->textureCount, sizeof(int32_t));
    sdl->textureLoaded  = (bool*)safeCalloc(sdl->textureCount, sizeof(bool));

    sdl->whiteTexture = SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, 1, 1);
    if (sdl->whiteTexture) {
        uint32_t white = 0xFFFFFFFF;
        SDL_UpdateTexture(sdl->whiteTexture, NULL, &white, sizeof(uint32_t));
        SDL_SetTextureScaleMode(sdl->whiteTexture, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(sdl->whiteTexture, SDL_BLENDMODE_BLEND);
    }

    SDL_SetRenderDrawBlendMode(sdl->renderer, SDL_BLENDMODE_BLEND);

    fprintf(stderr, "SDL: Renderer initialized (%u texture pages)\n", sdl->textureCount);
}

static void sdlDestroy(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);

    if (sdl->whiteTexture) SDL_DestroyTexture(sdl->whiteTexture);

	if (sdl->fogTex) SDL_DestroyTexture(sdl->fogTex);

    if (sdl->sdlTextures) {
        repeat(sdl->textureCount, i) {
            if (sdl->sdlTextures[i]) SDL_DestroyTexture(sdl->sdlTextures[i]);
        }
        free(sdl->sdlTextures);
    }
    free(sdl->textureWidths);
    free(sdl->textureHeights);
    free(sdl->textureLoaded);

    repeat(sdl->surfaceCount, i) {
        if (sdl->surfaces[i]) SDL_DestroyTexture(sdl->surfaces[i]);
        if (sdl->surfaceTexture[i]) SDL_DestroyTexture(sdl->surfaceTexture[i]);
    }
    free(sdl->surfaces);
    free(sdl->surfaceTexture);
    free(sdl->surfaceWidth);
    free(sdl->surfaceHeight);

    if (sdl->renderer) SDL_DestroyRenderer(sdl->renderer);

    free(sdl);
}

// ===[ Frame ]===

static void sdlBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->gameW = gameW;
    sdl->gameH = gameH;
    sdl->windowW = windowW;
    sdl->windowH = windowH;

    // Bind the application surface (matches glBeginFrame)
    int32_t appId = renderer->runner->applicationSurfaceId;
    if (appId >= 0 && (uint32_t) appId < sdl->surfaceCount && sdl->surfaces[appId] != nullptr) {
        SDL_SetRenderTarget(sdl->renderer, sdl->surfaces[appId]);
    } else {
        SDL_SetRenderTarget(sdl->renderer, NULL);
    }

    SDL_Rect fullRect = { 0, 0, gameW, gameH };
    SDL_SetRenderViewport(sdl->renderer, &fullRect);
    sdl->base.CPortX = 0;
    sdl->base.CPortY = 0;
    sdl->base.CPortW = gameW;
    sdl->base.CPortH = gameH;
}

static void sdlEndFrameInit(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);
    if (renderer->runner->usingAppSurface && !renderer->runner->appSurfaceAutoDraw) {
        SDL_SetRenderTarget(sdl->renderer, NULL);
        return;
    }
}

static void sdlEndFrameEnd(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);

    // If using app surface and not auto-draw, the render target was already set to NULL
    // by endFrameInit and GUI drew directly to the window. Just present.
    if (renderer->runner->usingAppSurface && !renderer->runner->appSurfaceAutoDraw) {
        SDL_RenderPresent(sdl->renderer);
        return;
    }

    // Draw app surface to window with letterboxing
    int32_t appId = renderer->runner->applicationSurfaceId;

    SDL_SetRenderTarget(sdl->renderer, NULL);
    SDL_SetRenderViewport(sdl->renderer, NULL);
    SDL_SetRenderClipRect(sdl->renderer, NULL);
    SDL_SetRenderDrawColor(sdl->renderer, 0, 0, 0, 255);
    SDL_RenderClear(sdl->renderer);

    if (appId >= 0 && (uint32_t) appId < sdl->surfaceCount && sdl->surfaces[appId] != nullptr) {
        // Compute letterbox: fit game aspect ratio into window
        float gameAspect = (float) sdl->gameW / (float) sdl->gameH;
        float windowAspect = (float) sdl->windowW / (float) sdl->windowH;
        int32_t drawW, drawH;
        if (windowAspect > gameAspect) {
            drawH = sdl->windowH;
            drawW = (int32_t) ((float) drawH * gameAspect);
        } else {
            drawW = sdl->windowW;
            drawH = (int32_t) ((float) drawW / gameAspect);
        }
        int32_t offX = (sdl->windowW - drawW) / 2;
        int32_t offY = (sdl->windowH - drawH) / 2;

        SDL_FRect dstRect = { (float)offX, (float)offY, (float)drawW, (float)drawH };
        SDL_RenderTexture(sdl->renderer, sdl->surfaces[appId], NULL, &dstRect);
    }

    SDL_RenderPresent(sdl->renderer);
}

// ===[ View / Camera ]===

static void sdlApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix,
                               const Matrix4f* projectionMatrix);

static void sdlBeginView(Renderer* renderer, int32_t viewX, int32_t viewY,
                         int32_t viewW, int32_t viewH,
                         int32_t portX, int32_t portY,
                         int32_t portW, int32_t portH,
                         float viewAngle) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->currentViewX = (float)viewX;
    sdl->currentViewY = (float)viewY;
    sdl->currentViewW = (float)viewW;
    sdl->currentViewH = (float)viewH;
    sdl->currentViewAngle = viewAngle;
    sdl->currentPortX = portX;
    sdl->currentPortY = portY;
    sdl->currentPortW = portW;
    sdl->currentPortH = portH;

    // Set viewport and scissor rect
    SDL_Rect portRect = { portX, portY, portW, portH };
    SDL_SetRenderViewport(sdl->renderer, &portRect);
    SDL_SetRenderClipRect(sdl->renderer, &portRect);

    // Set up camera and projection (matches glBeginView)
    int32_t viewCurrent = 0;
    if (renderer->runner->viewsEnabled) {
        viewCurrent = renderer->runner->viewCurrent;
    }
    RuntimeView* view = &renderer->runner->views[viewCurrent];
    sdl->base.cameraCurrent = view->cameraId;
    GMLCamera* camera = Runner_getCameraById(renderer->runner, sdl->base.cameraCurrent);
    sdlApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
}

static void sdlApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix,
                               const Matrix4f* projectionMatrix) {
    Matrix4f world = renderer->gmlMatrices[MATRIX_WORLD];
    Matrix4f view = *viewMatrix;
    Matrix4f projection = *projectionMatrix;

    Matrix4f worldView;
    Matrix4f_multiply(&worldView, &view, &world);

    Matrix4f worldViewProjection;
    Matrix4f_multiply(&worldViewProjection, &projection, &worldView);

    renderer->gmlMatrices[MATRIX_VIEW] = view;
    renderer->gmlMatrices[MATRIX_PROJECTION] = projection;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW] = worldView;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = worldViewProjection;
}

static void sdlEndView(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);
    SDL_SetRenderClipRect(sdl->renderer, NULL);
}

// ===[ GUI ]===

static void sdlBeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH,
                        int32_t portX, int32_t portY,
                        int32_t portW, int32_t portH,
                        int32_t targetSurfaceId) {
    SDLRenderer* sdl = SDL(renderer);

    // Update view transform parameters for transformWorldToView to use during GUI draws
    sdl->currentViewX = 0.0f;
    sdl->currentViewY = 0.0f;
    sdl->currentViewW = (float) guiW;
    sdl->currentViewH = (float) guiH;
    sdl->currentViewAngle = 0.0f;
    sdl->currentPortX = 0;
    sdl->currentPortY = 0;
    sdl->currentPortW = portW;
    sdl->currentPortH = portH;

    if (targetSurfaceId == RENDER_TARGET_HOST_FRAMEBUFFER) {
        SDL_SetRenderTarget(sdl->renderer, NULL);
        SDL_Rect rect = { 0, 0, portW, portH };
        SDL_SetRenderViewport(sdl->renderer, &rect);
        SDL_SetRenderClipRect(sdl->renderer, &rect);
    } else {
        require(targetSurfaceId >= 0 && (uint32_t) targetSurfaceId < sdl->surfaceCount);
        require(sdl->surfaces[targetSurfaceId] != nullptr);
        SDL_SetRenderTarget(sdl->renderer, sdl->surfaces[targetSurfaceId]);
        SDL_Rect rect = { portX, portY, portW, portH };
        SDL_SetRenderViewport(sdl->renderer, &rect);
        SDL_SetRenderClipRect(sdl->renderer, &rect);
    }

    // Set up GUI camera (matches glBeginGUI)
    sdl->base.cameraCurrent = GUI_CAMERA;
    GMLCamera* camera = &renderer->runner->guiCamera;
    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = guiW;
    camera->viewHeight = guiH;
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;

    Matrix4f projectionMatrix;
    Matrix4f_Orthographic(&projectionMatrix, (float) guiW, (float) guiH, 32000.0, 0.0);

    Matrix4f viewMatrix;
    float x = (float) guiW * 0.5f;
    float y = (float) guiH * 0.5f;
    Matrix4f_identity(&viewMatrix);
    Matrix4f_LookAt(&viewMatrix, x, y, -16000.0, x, y, 16000.0, 0.0, 1.0, 0.0);
    camera->viewMatrix = viewMatrix;
    camera->projectionMatrix = projectionMatrix;

    sdlApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
}

static void sdlSetGuiProjection(Renderer* renderer, int32_t guiW,
                                int32_t guiH, int32_t portW,
                                int32_t portH, bool renderingToUserSurface) {
    SDLRenderer* sdl = SDL(renderer);

    // Update view transform parameters for transformWorldToView
    sdl->currentViewX = 0.0f;
    sdl->currentViewY = 0.0f;
    sdl->currentViewW = (float) guiW;
    sdl->currentViewH = (float) guiH;
    sdl->currentViewAngle = 0.0f;
    sdl->currentPortX = 0;
    sdl->currentPortY = 0;
    sdl->currentPortW = portW;
    sdl->currentPortH = portH;

    sdl->base.cameraCurrent = GUI_CAMERA;
    GMLCamera* camera = &renderer->runner->guiCamera;
    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = guiW;
    camera->viewHeight = guiH;
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;

    Matrix4f projectionMatrix;
    Matrix4f_Orthographic(&projectionMatrix, (float) guiW, (float) guiH, 32000.0, 0.0);
    if (renderingToUserSurface) Matrix4f_flipClipY(&projectionMatrix);

    Matrix4f viewMatrix;
    float x = (float) guiW * 0.5f;
    float y = (float) guiH * 0.5f;
    Matrix4f_identity(&viewMatrix);
    Matrix4f_LookAt(&viewMatrix, x, y, -16000.0, x, y, 16000.0, 0.0, 1.0, 0.0);
    camera->viewMatrix = viewMatrix;
    camera->projectionMatrix = projectionMatrix;

    sdlApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
}

static void sdlEndGUI(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);
    SDL_SetRenderClipRect(sdl->renderer, NULL);
}

// ===[ Drawing: Sprites ]===

static void sdlDrawSprite(Renderer* renderer, int32_t tpagIndex,
                          float x, float y,
                          float originX, float originY,
                          float xscale, float yscale,
                          float angleDeg, uint32_t color,
                          float alpha) {
	SDLRenderer* sdl = SDL(renderer);
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t) pageId) return;

    ensureTextureLoaded(sdl, dw, (uint32_t) pageId);

    SDL_Texture* tex = sdl->sdlTextures[pageId];
    if (!tex) return;

    float texW = (float) sdl->textureWidths[pageId];
    float texH = (float) sdl->textureHeights[pageId];

    float u0 = (float) tpag->sourceX / texW;
    float v0 = (float) tpag->sourceY / texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / texH;

    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->targetWidth;
    float localY1 = localY0 + (float) tpag->targetHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float xs[4], ys[4];
    Matrix4f_transformPoint(&transform, localX0, localY0, &xs[0], &ys[0]);
    Matrix4f_transformPoint(&transform, localX1, localY0, &xs[1], &ys[1]);
    Matrix4f_transformPoint(&transform, localX1, localY1, &xs[2], &ys[2]);
    Matrix4f_transformPoint(&transform, localX0, localY1, &xs[3], &ys[3]);

    float us[4] = {u0, u1, u1, u0};
    float vs[4] = {v0, v0, v1, v1};

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    emitColoredQuad(sdl, tex, xs, ys, us, vs, r, g, b, alpha);
}

static void sdlDrawSpritePart(Renderer* renderer, int32_t tpagIndex,
                              int32_t srcOffX, int32_t srcOffY,
                              int32_t srcW, int32_t srcH,
                              float x, float y,
                              float xscale, float yscale,
                              float angleDeg, float pivotX,
                              float pivotY, uint32_t color,
                              float alpha) {
	SDLRenderer* sdl = SDL(renderer);
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t) pageId) return;

    ensureTextureLoaded(sdl, dw, (uint32_t) pageId);

    SDL_Texture* tex = sdl->sdlTextures[pageId];
    if (!tex) return;

    float texW = (float) sdl->textureWidths[pageId];
    float texH = (float) sdl->textureHeights[pageId];

    float u0 = (float) (tpag->sourceX + srcOffX) / texW;
    float v0 = (float) (tpag->sourceY + srcOffY) / texH;
    float u1 = (float) (tpag->sourceX + srcOffX + srcW) / texW;
    float v1 = (float) (tpag->sourceY + srcOffY + srcH) / texH;

    float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
    if (angleDeg == 0.0f) {
        cx0 = x;                         cy0 = y;
        cx1 = x + (float) srcW * xscale; cy1 = y;
        cx2 = x + (float) srcW * xscale; cy2 = y + (float) srcH * yscale;
        cx3 = x;                         cy3 = y + (float) srcH * yscale;
    } else {
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);
        float qx0 = x,                         qy0 = y;
        float qx1 = x + (float) srcW * xscale, qy1 = y;
        float qx2 = x + (float) srcW * xscale, qy2 = y + (float) srcH * yscale;
        float qx3 = x,                         qy3 = y + (float) srcH * yscale;
        float dx, dy;
        dx = qx0 - pivotX; dy = qy0 - pivotY; cx0 = cosA * dx - sinA * dy + pivotX; cy0 = sinA * dx + cosA * dy + pivotY;
        dx = qx1 - pivotX; dy = qy1 - pivotY; cx1 = cosA * dx - sinA * dy + pivotX; cy1 = sinA * dx + cosA * dy + pivotY;
        dx = qx2 - pivotX; dy = qy2 - pivotY; cx2 = cosA * dx - sinA * dy + pivotX; cy2 = sinA * dx + cosA * dy + pivotY;
        dx = qx3 - pivotX; dy = qy3 - pivotY; cx3 = cosA * dx - sinA * dy + pivotX; cy3 = sinA * dx + cosA * dy + pivotY;
    }

    float xs[4] = {cx0, cx1, cx2, cx3};
    float ys[4] = {cy0, cy1, cy2, cy3};
    float us[4] = {u0, u1, u1, u0};
    float vs[4] = {v0, v0, v1, v1};

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    emitColoredQuad(sdl, tex, xs, ys, us, vs, r, g, b, alpha);
}

static void sdlDrawSpritePos(Renderer* renderer, int32_t tpagIndex,
                             float x1, float y1,
                             float x2, float y2,
                             float x3, float y3,
                             float x4, float y4,
                             float alpha) {
    SDLRenderer* sdl = SDL(renderer);
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t) pageId) return;
    ensureTextureLoaded(sdl, dw, (uint32_t) pageId);
    SDL_Texture* tex = sdl->sdlTextures[pageId];
    if (!tex) return;
    float texW = (float) sdl->textureWidths[pageId];
    float texH = (float) sdl->textureHeights[pageId];
    float u0 = (float) tpag->sourceX / texW;
    float v0 = (float) tpag->sourceY / texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / texH;
    float xs[4] = {x1, x2, x3, x4};
    float ys[4] = {y1, y2, y3, y4};
    float us[4] = {u0, u1, u1, u0};
    float vs[4] = {v0, v0, v1, v1};
    emitColoredQuad(sdl, tex, xs, ys, us, vs, 1.0f, 1.0f, 1.0f, alpha);
}

// ===[ Drawing: Primitives ]===

static void sdlDrawRectangle(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                             MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                             MAYBE_UNUSED float y2, MAYBE_UNUSED uint32_t color,
                             MAYBE_UNUSED float alpha, MAYBE_UNUSED bool outline) {
	SDLRenderer* sdl = SDL(renderer);

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;
    float us[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float vs[4] = {0.5f, 0.5f, 0.5f, 0.5f};

    if (outline) {
        float tx[4][4] = {
            {x1, x2 + 1, x2 + 1, x1}, // top
            {x1, x2 + 1, x2 + 1, x1}, // bottom
            {x1, x1 + 1, x1 + 1, x1}, // left
            {x2, x2 + 1, x2 + 1, x2}  // right
        };
        float ty[4][4] = {
            {y1, y1, y1 + 1, y1 + 1}, // top
            {y2, y2, y2 + 1, y2 + 1}, // bottom
            {y1 + 1, y1 + 1, y2, y2}, // left
            {y1 + 1, y1 + 1, y2, y2}  // right
        };
        for (int i = 0; i < 4; i++) {
            emitColoredQuad(sdl, sdl->whiteTexture, tx[i], ty[i], us, vs, r, g, b, alpha);
        }
    } else {
        float xs[4] = {x1, x2 + 1, x2 + 1, x1};
        float ys[4] = {y1, y1, y2 + 1, y2 + 1};
        emitColoredQuad(sdl, sdl->whiteTexture, xs, ys, us, vs, r, g, b, alpha);
    }
}

static void sdlDrawLineColor(Renderer* renderer, float x1, float y1,
                             float x2, float y2, float width,
                             uint32_t color1, uint32_t color2, float alpha);

static void sdlDrawRectangleColor(Renderer* renderer, float x1,
                                  float y1, float x2,
                                  float y2, uint32_t color1,
                                  uint32_t color2, uint32_t color3,
                                  uint32_t color4, float alpha,
                                  bool outline) {
    SDLRenderer* sdl = SDL(renderer);

    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);
    uint8_t r3 = (uint8_t) BGR_R(color3), g3 = (uint8_t) BGR_G(color3), b3 = (uint8_t) BGR_B(color3);
    uint8_t r4 = (uint8_t) BGR_R(color4), g4 = (uint8_t) BGR_G(color4), b4 = (uint8_t) BGR_B(color4);

    float us[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float vs[4] = {0.5f, 0.5f, 0.5f, 0.5f};

    if (outline) {
        sdlDrawLineColor(renderer, x1, y1, x2, y1, 1.0f, color1, color2, alpha);
        sdlDrawLineColor(renderer, x2, y1, x2, y2, 1.0f, color2, color3, alpha);
        sdlDrawLineColor(renderer, x2, y2, x1, y2, 1.0f, color3, color4, alpha);
        sdlDrawLineColor(renderer, x1, y2, x1, y1, 1.0f, color4, color1, alpha);
    } else {
        float rc[4] = {r1 / 255.0f, r2 / 255.0f, r3 / 255.0f, r4 / 255.0f};
        float gc[4] = {g1 / 255.0f, g2 / 255.0f, g3 / 255.0f, g4 / 255.0f};
        float bc[4] = {b1 / 255.0f, b2 / 255.0f, b3 / 255.0f, b4 / 255.0f};
        float ac[4] = {alpha, alpha, alpha, alpha};
        float xs[4] = {x1, x2 + 1, x2 + 1, x1};
        float ys[4] = {y1, y1, y2 + 1, y2 + 1};
        emitQuad(sdl, sdl->whiteTexture, xs, ys, us, vs, rc, gc, bc, ac);
    }
}

static void sdlDrawLine(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                        MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                        MAYBE_UNUSED float y2, MAYBE_UNUSED float width,
                        MAYBE_UNUSED uint32_t color, MAYBE_UNUSED float alpha) {
	renderer->vtable->drawLineColor(renderer, x1, y1, x2, y2, width, color, color, alpha);
}

static void sdlDrawLineColor(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                             MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                             MAYBE_UNUSED float y2, MAYBE_UNUSED float width,
                             MAYBE_UNUSED uint32_t color1, MAYBE_UNUSED uint32_t color2,
                             MAYBE_UNUSED float alpha) {
	SDLRenderer* sdl = SDL(renderer);

    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    float xs[4] = {x1 + px, x1 - px, x2 - px, x2 + px};
    float ys[4] = {y1 + py, y1 - py, y2 - py, y2 + py};
    float us[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    float vs[4] = {0.5f, 0.5f, 0.5f, 0.5f};

    float r1 = (float) BGR_R(color1) / 255.0f;
    float g1 = (float) BGR_G(color1) / 255.0f;
    float b1 = (float) BGR_B(color1) / 255.0f;

    float r2 = (float) BGR_R(color2) / 255.0f;
    float g2 = (float) BGR_G(color2) / 255.0f;
    float b2 = (float) BGR_B(color2) / 255.0f;

    float rc[4] = {r1, r1, r2, r2};
    float gc[4] = {g1, g1, g2, g2};
    float bc[4] = {b1, b1, b2, b2};
    float ac[4] = {alpha, alpha, alpha, alpha};

    emitQuad(sdl, sdl->whiteTexture, xs, ys, us, vs, rc, gc, bc, ac);
}

static void sdlDrawTriangle(Renderer* renderer, float x1,
                            float y1, float x2,
                            float y2, float x3,
                            float y3, uint32_t color1,
                            uint32_t color2, uint32_t color3,
                            float alpha, bool outline) {
    SDLRenderer* sdl = SDL(renderer);

    if (outline) {
        sdlDrawLineColor(renderer, x1, y1, x2, y2, 1, color1, color2, alpha);
        sdlDrawLineColor(renderer, x2, y2, x3, y3, 1, color2, color3, alpha);
        sdlDrawLineColor(renderer, x3, y3, x1, y1, 1, color3, color1, alpha);
    } else {
        float r1 = (float) BGR_R(color1) / 255.0f;
        float g1 = (float) BGR_G(color1) / 255.0f;
        float b1 = (float) BGR_B(color1) / 255.0f;
        float r2 = (float) BGR_R(color2) / 255.0f;
        float g2 = (float) BGR_G(color2) / 255.0f;
        float b2 = (float) BGR_B(color2) / 255.0f;
        float r3 = (float) BGR_R(color3) / 255.0f;
        float g3 = (float) BGR_G(color3) / 255.0f;
        float b3 = (float) BGR_B(color3) / 255.0f;

        float xs[3] = {x1, x2, x3};
        float ys[3] = {y1, y2, y3};
        float us[3] = {0.5f, 0.5f, 0.5f};
        float vs[3] = {0.5f, 0.5f, 0.5f};
        float rc[3] = {r1, r2, r3};
        float gc[3] = {g1, g2, g3};
        float bc[3] = {b1, b2, b3};
        float ac[3] = {alpha, alpha, alpha};

        emitTri(sdl, sdl->whiteTexture, xs, ys, us, vs, rc, gc, bc, ac);
    }
}

// ===[ Drawing: Text ]===

static void drawText(
    Renderer* renderer, const char* text,
    float x, float y,
    float xscale, float yscale,
    float angleDeg, float lineSeparation,
    int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4,
    float alpha
) {
    SDLRenderer* sdl = SDL(renderer);
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];

    int32_t fontTpagIndex = font->tpagIndex;
    if (0 > fontTpagIndex) return;

    TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];
    int16_t pageId = fontTpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t) pageId) return;

    ensureTextureLoaded(sdl, dw, (uint32_t) pageId);

    SDL_Texture* tex = sdl->sdlTextures[pageId];
    if (!tex) return;

    float texW = (float) sdl->textureWidths[pageId];
    float texH = (float) sdl->textureHeights[pageId];

    PreprocessedText pt = TextUtils_preprocessGmlText(text);
    const char* str = pt.text;
    int32_t textLen = (int32_t) strlen(str);
    int32_t lineCount = TextUtils_countLines(str, textLen);

    float lineStride = (0.0f > lineSeparation)
        ? TextUtils_lineStride(font)
        : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    int32_t c1 = _c1, c2 = _c2, c3 = _c3, c4 = _c4;
    bool needsLerpingOnTheFly = c1 != c2 || c2 != c3 || c3 != c4;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(str[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, str + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        float gradientX = 0.0f;
        int32_t pos = 0;

        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(str + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(str + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    float advance = (float) glyph->shift;
                    float leftFrac = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                    float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                    if (needsLerpingOnTheFly) {
                        c1 = Color_lerp(_c1, _c2, leftFrac);
                        c2 = Color_lerp(_c1, _c2, rightFrac);
                        c3 = Color_lerp(_c4, _c3, rightFrac);
                        c4 = Color_lerp(_c4, _c3, leftFrac);
                    }

                    float u0 = (float) (fontTpag->sourceX + glyph->sourceX) / texW;
                    float v0 = (float) (fontTpag->sourceY + glyph->sourceY) / texH;
                    float u1 = (float) (fontTpag->sourceX + glyph->sourceX + glyph->sourceWidth) / texW;
                    float v1 = (float) (fontTpag->sourceY + glyph->sourceY + glyph->sourceHeight) / texH;

                    float localX0 = cursorX + glyph->offset;
                    float localY0 = cursorY;
                    float localX1 = localX0 + (float) glyph->sourceWidth;
                    float localY1 = localY0 + (float) glyph->sourceHeight;

                    float xs[4], ys[4];
                    Matrix4f_transformPoint(&transform, localX0, localY0, &xs[0], &ys[0]);
                    Matrix4f_transformPoint(&transform, localX1, localY0, &xs[1], &ys[1]);
                    Matrix4f_transformPoint(&transform, localX1, localY1, &xs[2], &ys[2]);
                    Matrix4f_transformPoint(&transform, localX0, localY1, &xs[3], &ys[3]);

                    float us[4] = {u0, u1, u1, u0};
                    float vs[4] = {v0, v0, v1, v1};

                    float rc[4] = {(float) BGR_R(c1), (float) BGR_R(c2), (float) BGR_R(c3), (float) BGR_R(c4)};
                    float gc[4] = {(float) BGR_G(c1), (float) BGR_G(c2), (float) BGR_G(c3), (float) BGR_G(c4)};
                    float bc[4] = {(float) BGR_B(c1), (float) BGR_B(c2), (float) BGR_B(c3), (float) BGR_B(c4)};

                    for (int i = 0; i < 4; i++) { rc[i] /= 255.0f; gc[i] /= 255.0f; bc[i] /= 255.0f; }

                    emitQuad(sdl, tex, xs, ys, us, vs, rc, gc, bc, (float[4]){alpha, alpha, alpha, alpha});
                }

                cursorX += glyph->shift;
                gradientX += glyph->shift;
                if (hasNext) {
                    cursorX += TextUtils_getKerningOffset(glyph, nextCh);
                    gradientX += TextUtils_getKerningOffset(glyph, nextCh);
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        if (textLen > lineEnd) lineStart = TextUtils_skipNewline(str, lineEnd, textLen);
        else lineStart = lineEnd;
    }
    PreprocessedText_free(pt);
}

static void sdlDrawText(Renderer* renderer, const char* text,
                        float x, float y,
                        float xscale, float yscale,
                        float angleDeg, float lineSeparation) {
    drawText(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation,
                    renderer->drawColor, renderer->drawColor,
                    renderer->drawColor, renderer->drawColor,
                    renderer->drawAlpha);
}

static void sdlDrawTextColor(Renderer* renderer, const char* text,
                             float x, float y,
                             float xscale, float yscale,
                             float angleDeg, int32_t c1,
                             int32_t c2, int32_t c3,
                             int32_t c4, float alpha,
                             float lineSeparation) {
    drawText(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation,
                    c1, c2, c3, c4, alpha);
}

// ===[ Flush / Clear ]===

static void sdlFlush(MAYBE_UNUSED Renderer* renderer) {
}

static void sdlClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    SDLRenderer* sdl = SDL(renderer);
    uint8_t r = BGR_R(color);
    uint8_t g = BGR_G(color);
    uint8_t b = BGR_B(color);
    uint8_t a = (uint8_t)(alpha * 255.0f);
    SDL_SetRenderDrawColor(sdl->renderer, r, g, b, a);
    SDL_RenderClear(sdl->renderer);
}

// ===[ Sprite Management ]===

static uint32_t sdlFindOrAllocTexturePageSlot(SDLRenderer* sdl) {
    for (uint32_t i = sdl->originalTexturePageCount; sdl->textureCount > i; i++) {
        if (sdl->sdlTextures[i] == nullptr) return i;
    }
    uint32_t newPageId = sdl->textureCount;
    sdl->textureCount++;
    sdl->sdlTextures     = (SDL_Texture**)safeRealloc(sdl->sdlTextures,     sdl->textureCount * sizeof(SDL_Texture*));
    sdl->textureWidths   = (int32_t*)safeRealloc(sdl->textureWidths,       sdl->textureCount * sizeof(int32_t));
    sdl->textureHeights  = (int32_t*)safeRealloc(sdl->textureHeights,      sdl->textureCount * sizeof(int32_t));
    sdl->textureLoaded   = (bool*)safeRealloc(sdl->textureLoaded,          sdl->textureCount * sizeof(bool));
    sdl->sdlTextures[newPageId]     = nullptr;
    sdl->textureWidths[newPageId]   = 0;
    sdl->textureHeights[newPageId]  = 0;
    sdl->textureLoaded[newPageId]   = false;
    return newPageId;
}

static int32_t sdlCreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID,
                                          int32_t x, int32_t y,
                                          int32_t w, int32_t h,
                                          MAYBE_UNUSED bool removeback, MAYBE_UNUSED bool smooth,
                                          int32_t xorig, int32_t yorig) {
    SDLRenderer* sdl = SDL(renderer);
    DataWin* dw = renderer->dataWin;

    if (w <= 0 || h <= 0) return -1;
    if (surfaceID < 0 || (uint32_t)surfaceID >= sdl->surfaceCount) return -1;
    if (!sdl->surfaces[surfaceID]) return -1;

    // Read pixels from the surface
    SDL_Texture* prevTarget = SDL_GetRenderTarget(sdl->renderer);
    SDL_SetRenderTarget(sdl->renderer, sdl->surfaces[surfaceID]);

    SDL_Rect readRect = {x, y, w, h};
    SDL_Surface* readSurf = SDL_RenderReadPixels(sdl->renderer, &readRect);
    SDL_SetRenderTarget(sdl->renderer, prevTarget);
    if (!readSurf) return -1;

    uint8_t* pixels = (uint8_t*)safeMalloc((size_t)w * (size_t)h * 4);
    if (!pixels) {
        SDL_DestroySurface(readSurf);
        return -1;
    }
    memcpy(pixels, readSurf->pixels, (size_t)w * (size_t)h * 4);
    SDL_DestroySurface(readSurf);

    // Create SDL texture from the captured pixels
    SDL_Texture* newTex = SDL_CreateTexture(sdl->renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!newTex) {
        free(pixels);
        return -1;
    }
    SDL_UpdateTexture(newTex, NULL, pixels, w * 4);
    SDL_SetTextureScaleMode(newTex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(newTex, SDL_BLENDMODE_BLEND);
    free(pixels);

    // Find or allocate texture page slot
    uint32_t pageId = sdlFindOrAllocTexturePageSlot(sdl);
    sdl->sdlTextures[pageId] = newTex;
    sdl->textureWidths[pageId] = w;
    sdl->textureHeights[pageId] = h;
    sdl->textureLoaded[pageId] = true;

    // Find or allocate TPAG slot
    uint32_t tpagIndex;
    for (tpagIndex = sdl->originalTpagCount; dw->tpag.count > tpagIndex; tpagIndex++) {
        if (dw->tpag.items[tpagIndex].texturePageId == -1) break;
    }
    if (tpagIndex >= dw->tpag.count) {
        tpagIndex = dw->tpag.count;
        dw->tpag.count++;
        dw->tpag.items = (TexturePageItem*)safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
        memset(&dw->tpag.items[tpagIndex], 0, sizeof(TexturePageItem));
    }
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t)w;
    tpag->sourceHeight = (uint16_t)h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t)w;
    tpag->targetHeight = (uint16_t)h;
    tpag->boundingWidth = (uint16_t)w;
    tpag->boundingHeight = (uint16_t)h;
    tpag->texturePageId = (int16_t)pageId;

    // Allocate sprite slot
    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, sdl->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    sprite->width = (uint32_t)w;
    sprite->height = (uint32_t)h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t*)safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t)tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    fprintf(stderr, "SDL: Created dynamic sprite %u (%dx%d) from surface %d at (%d,%d)\n", spriteIndex, w, h, surfaceID, x, y);
    return (int32_t)spriteIndex;
}

static void sdlDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    SDLRenderer* sdl = SDL(renderer);
    DataWin* dw = renderer->dataWin;

    if (spriteIndex < 0 || dw->sprt.count <= (uint32_t)spriteIndex) return;

    if (sdl->originalSpriteCount > (uint32_t)spriteIndex) {
        fprintf(stderr, "SDL: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    repeat(sprite->textureCount, i) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t)tpagIdx >= sdl->originalTpagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && sdl->textureCount > (uint32_t)pageId) {
                if (sdl->sdlTextures[pageId]) {
                    SDL_DestroyTexture(sdl->sdlTextures[pageId]);
                    sdl->sdlTextures[pageId] = nullptr;
                }
            }
            tpag->texturePageId = -1;
        }
    }

    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;

    fprintf(stderr, "SDL: Deleted sprite %d\n", spriteIndex);
}

// ===[ Blend / GPU State ]===

static SDL_BlendFactor blendFactorToSDL(int factor) {
    switch (factor) {
        case bm_zero:             return SDL_BLENDFACTOR_ZERO;
        case bm_one:              return SDL_BLENDFACTOR_ONE;
        case bm_src_color:        return SDL_BLENDFACTOR_SRC_COLOR;
        case bm_inv_src_color:    return SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        case bm_src_alpha:        return SDL_BLENDFACTOR_SRC_ALPHA;
        case bm_inv_src_alpha:    return SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        case bm_dest_alpha:       return SDL_BLENDFACTOR_DST_ALPHA;
        case bm_inv_dest_alpha:   return SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
        case bm_dest_color:       return SDL_BLENDFACTOR_DST_COLOR;
        case bm_inv_dest_color:   return SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR;
        case bm_src_alpha_sat:    return SDL_BLENDFACTOR_SRC_ALPHA;
        default:                  return SDL_BLENDFACTOR_SRC_ALPHA;
    }
}

static void sdlApplyBlendMode(SDLRenderer* sdl) {
    SDL_BlendOperation colorOp;
    switch (sdl->currentBlendMode) {
        case bm_reverse_subtract: colorOp = SDL_BLENDOPERATION_REV_SUBTRACT; break;
        case bm_min:              colorOp = SDL_BLENDOPERATION_MINIMUM;      break;
        default:                  colorOp = SDL_BLENDOPERATION_ADD;          break;
    }

    sdl->sdlBlendMode = SDL_ComposeCustomBlendMode(
        blendFactorToSDL(sdl->currentSFactor),
        blendFactorToSDL(sdl->currentDFactor),
        colorOp,
        blendFactorToSDL(sdl->currentSFactorAlpha),
        blendFactorToSDL(sdl->currentDFactorAlpha),
        colorOp);

    SDL_SetRenderDrawBlendMode(sdl->renderer,
        sdl->sdlBlendEnabled ? sdl->sdlBlendMode : SDL_BLENDMODE_NONE);
}

static void sdlBlendModeSetFactors(SDLRenderer* sdl, int32_t mode) {
    switch (mode) {
        default:
        case bm_normal:
            sdl->currentSFactor = bm_src_alpha;
            sdl->currentDFactor = bm_inv_src_alpha;
            break;
        case bm_add:
            sdl->currentSFactor = bm_src_alpha;
            sdl->currentDFactor = bm_one;
            break;
        case bm_subtract:
            sdl->currentSFactor = bm_zero;
            sdl->currentDFactor = bm_inv_src_color;
            break;
        case bm_reverse_subtract:
            sdl->currentSFactor = bm_src_alpha;
            sdl->currentDFactor = bm_one;
            break;
        case bm_min:
            sdl->currentSFactor = bm_one;
            sdl->currentDFactor = bm_one;
            break;
        case bm_max:
            sdl->currentSFactor = bm_src_alpha;
            sdl->currentDFactor = bm_inv_src_color;
            break;
    }
    sdl->currentSFactorAlpha = sdl->currentSFactor;
    sdl->currentDFactorAlpha = sdl->currentDFactor;
}

static BlendFactors sdlGpuGetBlendFactors(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);
    return (BlendFactors){sdl->currentSFactor, sdl->currentDFactor, sdl->currentSFactorAlpha, sdl->currentDFactorAlpha};
}

static int32_t sdlGpuGetBlendMode(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);
    return sdl->currentBlendMode;
}

static void sdlGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    SDLRenderer* sdl = SDL(renderer);
    if (sdl->currentBlendMode == mode) return;
    sdl->currentBlendMode = mode;
    sdlBlendModeSetFactors(sdl, mode);
    sdlApplyBlendMode(sdl);
}

static void sdlGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor,
                                  int32_t sfactor_alpha, int32_t dfactor_alpha) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->currentBlendMode = bm_complex;
    sdl->currentSFactor = sfactor;
    sdl->currentDFactor = dfactor;
    sdl->currentSFactorAlpha = sfactor_alpha;
    sdl->currentDFactorAlpha = dfactor_alpha;
    sdlApplyBlendMode(sdl);
}

static void sdlGpuSetBlendEnable(Renderer* renderer, bool enable) {
    SDLRenderer* sdl = SDL(renderer);
    sdl->sdlBlendEnabled = enable;
    SDL_SetRenderDrawBlendMode(sdl->renderer, enable ? sdl->sdlBlendMode : SDL_BLENDMODE_NONE);
}

static bool sdlGpuGetBlendEnable(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);
    SDL_BlendMode mode;
    SDL_GetRenderDrawBlendMode(sdl->renderer, &mode);
    return mode != SDL_BLENDMODE_NONE;
}

static void sdlGpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    SDLRenderer* sdl = SDL(renderer);
    if (sdl->alphaTestEnable == enable) return;
    sdl->alphaTestEnable = enable;
}

static void sdlGpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    SDLRenderer* sdl = SDL(renderer);
    float refF = (float)ref / 255.0f;
    if (sdl->alphaTestRef == refF) return;
    sdl->alphaTestRef = refF;
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
    if (sdl->fogEnable == enable && sdl->fogColor == color) return;
    sdl->fogEnable = enable;
    sdl->fogColor = color;
}

// ===[ Tile Rendering ]===

static void sdlDrawTiled(
    SDLRenderer* sdl, SDL_Texture* tex,
    float gridX, float gridY,
    float tileW, float tileH,
    bool tileX, bool tileY,
    float roomW, float roomH,
    float quadOffX0, float quadW,
    float quadOffY0, float quadH,
    float u0, float v0, float u1, float v1,
    float r, float g, float b, float alpha
) {
    if (tileW <= 0.0f || tileH <= 0.0f) return;

    float startX, endX, startY, endY;
    if (tileX) {
        startX = fmodf(gridX, tileW);
        if (startX > 0) startX -= tileW;
        endX = roomW;
    } else {
        startX = gridX;
        endX = startX + tileW;
    }
    if (tileY) {
        startY = fmodf(gridY, tileH);
        if (startY > 0) startY -= tileH;
        endY = roomH;
    } else {
        startY = gridY;
        endY = startY + tileH;
    }

    if (startX >= endX || startY >= endY) return;

    int32_t tilesX = (int32_t)((endX - startX) / tileW) + 1;
    int32_t tilesY = (int32_t)((endY - startY) / tileH) + 1;
    if (tilesX <= 0 || tilesY <= 0) return;

    float us[4] = {u0, u1, u1, u0};
    float vs[4] = {v0, v0, v1, v1};

    for (int32_t iy = 0; iy < tilesY; iy++) {
        float dy = startY + (float)iy * tileH;
        if (dy >= endY) break;
        float vy0 = dy + quadOffY0;
        float vy1 = vy0 + quadH;
        for (int32_t ix = 0; ix < tilesX; ix++) {
            float dx = startX + (float)ix * tileW;
            if (dx >= endX) break;
            float vx0 = dx + quadOffX0;
            float vx1 = vx0 + quadW;
            float xs[4] = {vx0, vx1, vx1, vx0};
            float ys[4] = {vy0, vy0, vy1, vy1};
            emitColoredQuad(sdl, tex, xs, ys, us, vs, r, g, b, alpha);
        }
    }
}

static void sdlDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex,
                               float originX, float originY,
                               float x, float y,
                               float xscale, float yscale,
                               bool tileX, bool tileY,
                               float roomW, float roomH,
                               uint32_t color, float alpha) {
    SDLRenderer* sdl = SDL(renderer);
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t)tpagIndex) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t)pageId) return;
    ensureTextureLoaded(sdl, dw, (uint32_t)pageId);
    SDL_Texture* tex = sdl->sdlTextures[pageId];
    if (!tex) return;

    float texW = (float)sdl->textureWidths[pageId];
    float texH = (float)sdl->textureHeights[pageId];
    float u0 = (float)tpag->sourceX / texW;
    float v0 = (float)tpag->sourceY / texH;
    float u1 = (float)(tpag->sourceX + tpag->sourceWidth) / texW;
    float v1 = (float)(tpag->sourceY + tpag->sourceHeight) / texH;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float)tpag->boundingWidth * axScale;
    float tileH = (float)tpag->boundingHeight * ayScale;

    float localX0 = (float)tpag->targetX - originX;
    float localY0 = (float)tpag->targetY - originY;
    float quadOffX0 = originX * axScale + xscale * localX0;
    float quadOffY0 = originY * ayScale + yscale * localY0;
    float quadW = xscale * (float)tpag->targetWidth;
    float quadH = yscale * (float)tpag->targetHeight;

    float r = (float)BGR_R(color) / 255.0f;
    float g = (float)BGR_G(color) / 255.0f;
    float b = (float)BGR_B(color) / 255.0f;

    sdlDrawTiled(sdl, tex,
        x - originX * axScale, y - originY * ayScale,
        tileW, tileH, tileX, tileY, roomW, roomH,
        quadOffX0, quadW, quadOffY0, quadH,
        u0, v0, u1, v1, r, g, b, alpha);
}

static void sdlDrawTiledPart(Renderer* renderer, int32_t tpagIndex,
                             int32_t srcX, int32_t srcY,
                             int32_t srcW, int32_t srcH,
                             float dstX, float dstY,
                             float dstW, float dstH,
                             uint32_t color, float alpha) {
    SDLRenderer* sdl = SDL(renderer);
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t)tpagIndex) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t)pageId) return;
    ensureTextureLoaded(sdl, dw, (uint32_t)pageId);
    SDL_Texture* tex = sdl->sdlTextures[pageId];
    if (!tex) return;

    float texW = (float)sdl->textureWidths[pageId];
    float texH = (float)sdl->textureHeights[pageId];

    float u0 = (float)(tpag->sourceX + srcX) / texW;
    float v0 = (float)(tpag->sourceY + srcY) / texH;
    float u1 = (float)(tpag->sourceX + srcX + srcW) / texW;
    float v1 = (float)(tpag->sourceY + srcY + srcH) / texH;

    float r = (float)BGR_R(color) / 255.0f;
    float g = (float)BGR_G(color) / 255.0f;
    float b = (float)BGR_B(color) / 255.0f;

    sdlDrawTiled(sdl, tex,
        dstX, dstY, (float)srcW, (float)srcH,
        true, true, dstX + dstW, dstY + dstH,
        0.0f, (float)srcW, 0.0f, (float)srcH,
        u0, v0, u1, v1, r, g, b, alpha);
}

// ===[ Surface Functions ]===

static int32_t sdlCreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    SDLRenderer* sdl = SDL(renderer);
    uint32_t idx = findOrAllocateSurfaceSlot(sdl);

    sdl->surfaceTexture[idx] = SDL_CreateTexture(
        sdl->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_TARGET, width, height);
    sdl->surfaces[idx] = sdl->surfaceTexture[idx];
    sdl->surfaceWidth[idx] = width;
    sdl->surfaceHeight[idx] = height;

    fprintf(stderr, "SDL: Created surface %u with size (%dx%d)\n", idx, width, height);
    return (int32_t)idx;
}

static bool sdlSurfaceExists(Renderer* renderer, int32_t surfaceID) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID < 0 || (uint32_t)surfaceID >= sdl->surfaceCount) return false;
    return sdl->surfaces[surfaceID] != nullptr;
}

static bool sdlSetRenderTarget(Renderer* renderer, int32_t surfaceID,
                               MAYBE_UNUSED bool implicitApplicationSurface) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID < 0 || (uint32_t)surfaceID >= sdl->surfaceCount) return false;
    if (sdl->surfaces[surfaceID] == nullptr) return false;
    SDL_SetRenderTarget(sdl->renderer, sdl->surfaces[surfaceID]);
    return true;
}

static int32_t sdlEnsureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
    SDLRenderer* sdl = SDL(renderer);
    int32_t id = renderer->runner->applicationSurfaceId;

    bool needsCreate = (id < 0) || ((uint32_t)id >= sdl->surfaceCount) || (sdl->surfaces[id] == nullptr);
    if (needsCreate) {
        id = sdlCreateSurface(renderer, width, height);
        renderer->runner->applicationSurfaceId = id;
        return id;
    }

    if (sdl->surfaceWidth[id] != width || sdl->surfaceHeight[id] != height) {
        renderer->vtable->surfaceResize(renderer, id, width, height);
    }
    return id;
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

static void sdlDrawSurface(Renderer* renderer, int32_t surfaceID,
                           int32_t srcLeft, int32_t srcTop,
                           int32_t srcWidth, int32_t srcHeight,
                           float x, float y,
                           float xscale, float yscale,
                           float angleDeg, uint32_t color,
                           float alpha) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID < 0 || (uint32_t)surfaceID >= sdl->surfaceCount) return;
    SDL_Texture* tex = sdl->surfaceTexture[surfaceID];
    if (!tex) return;
    int32_t texW = sdl->surfaceWidth[surfaceID];
    int32_t texH = sdl->surfaceHeight[surfaceID];
    if (texW <= 0 || texH <= 0) return;

    if (srcWidth < 0) { srcLeft = 0; srcTop = 0; srcWidth = texW; srcHeight = texH; }

    float u0 = (float)srcLeft / (float)texW;
    float v0 = (float)srcTop / (float)texH;
    float u1 = (float)(srcLeft + srcWidth) / (float)texW;
    float v1 = (float)(srcTop + srcHeight) / (float)texH;

    float angleRad = -angleDeg * ((float)M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float localX1 = (float)srcWidth;
    float localY1 = (float)srcHeight;

    float xs[4], ys[4];
    Matrix4f_transformPoint(&transform, 0.0f, 0.0f, &xs[0], &ys[0]);
    Matrix4f_transformPoint(&transform, localX1, 0.0f, &xs[1], &ys[1]);
    Matrix4f_transformPoint(&transform, localX1, localY1, &xs[2], &ys[2]);
    Matrix4f_transformPoint(&transform, 0.0f, localY1, &xs[3], &ys[3]);

    float us[4] = {u0, u1, u1, u0};
    float vs[4] = {v0, v0, v1, v1};

    float r = (float)BGR_R(color) / 255.0f;
    float g = (float)BGR_G(color) / 255.0f;
    float b = (float)BGR_B(color) / 255.0f;

    emitColoredQuad(sdl, tex, xs, ys, us, vs, r, g, b, alpha);
}

static void sdlDrawSurfaceTiled(Renderer* renderer, int32_t surfaceID,
                                float x, float y,
                                float xscale, float yscale,
                                float roomW, float roomH,
                                uint32_t color, float alpha) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID < 0 || (uint32_t)surfaceID >= sdl->surfaceCount) return;
    SDL_Texture* tex = sdl->surfaceTexture[surfaceID];
    if (!tex) return;
    int32_t texW = sdl->surfaceWidth[surfaceID];
    int32_t texH = sdl->surfaceHeight[surfaceID];
    if (texW <= 0 || texH <= 0) return;

    float tileW = (float)texW * fabsf(xscale);
    float tileH = (float)texH * fabsf(yscale);

    float r = (float)BGR_R(color) / 255.0f;
    float g = (float)BGR_G(color) / 255.0f;
    float b = (float)BGR_B(color) / 255.0f;

    sdlDrawTiled(sdl, tex,
        x, y, tileW, tileH,
        true, true, roomW, roomH,
        0.0f, xscale * (float)texW,
        0.0f, yscale * (float)texH,
        0.0f, 0.0f, 1.0f, 1.0f,
        r, g, b, alpha);
}

static void sdlSurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID < 0 || (uint32_t)surfaceID >= sdl->surfaceCount) return;
    if (sdl->surfaces[surfaceID]) SDL_DestroyTexture(sdl->surfaces[surfaceID]);
    sdl->surfaces[surfaceID] = SDL_CreateTexture(
        sdl->renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_TARGET, width, height);
    sdl->surfaceTexture[surfaceID] = sdl->surfaces[surfaceID];
    sdl->surfaceWidth[surfaceID] = width;
    sdl->surfaceHeight[surfaceID] = height;
}

static void sdlSurfaceFree(Renderer* renderer, int32_t surfaceID) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID < 0 || (uint32_t)surfaceID >= sdl->surfaceCount) return;
    if (surfaceID == renderer->runner->applicationSurfaceId) return;
    if (sdl->surfaces[surfaceID]) {
        SDL_DestroyTexture(sdl->surfaces[surfaceID]);
        sdl->surfaces[surfaceID] = nullptr;
        sdl->surfaceTexture[surfaceID] = nullptr;
        sdl->surfaceWidth[surfaceID] = 0;
        sdl->surfaceHeight[surfaceID] = 0;
    }
}

static void sdlSurfaceCopy(Renderer* renderer, int32_t destSurfaceID,
                           int32_t destX, int32_t destY,
                           int32_t srcSurfaceID, int32_t srcX,
                           int32_t srcY, int32_t srcW,
                           int32_t srcH, bool part) {
    SDLRenderer* sdl = SDL(renderer);
    if (destSurfaceID < 0 || (uint32_t)destSurfaceID >= sdl->surfaceCount) return;
    if (srcSurfaceID < 0 || (uint32_t)srcSurfaceID >= sdl->surfaceCount) return;
    if (!sdl->surfaces[destSurfaceID] || !sdl->surfaceTexture[srcSurfaceID]) return;

    SDL_Texture* srcTex = sdl->surfaceTexture[srcSurfaceID];
    int32_t srcTexW = sdl->surfaceWidth[srcSurfaceID];
    int32_t srcTexH = sdl->surfaceHeight[srcSurfaceID];
    int32_t destW = sdl->surfaceWidth[destSurfaceID];
    int32_t destH = sdl->surfaceHeight[destSurfaceID];

    int32_t sX = part ? srcX : 0;
    int32_t sY = part ? srcY : 0;
    int32_t sW = part ? srcW : srcTexW;
    int32_t sH = part ? srcH : srcTexH;

    float u0 = (float)sX / (float)srcTexW;
    float v0 = (float)sY / (float)srcTexH;
    float u1 = (float)(sX + sW) / (float)srcTexW;
    float v1 = (float)(sY + sH) / (float)srcTexH;

    SDL_Texture* prevTarget = SDL_GetRenderTarget(sdl->renderer);
    SDL_Rect prevViewport;
    SDL_GetRenderViewport(sdl->renderer, &prevViewport);

    float savedVX = sdl->currentViewX;
    float savedVY = sdl->currentViewY;
    float savedVW = sdl->currentViewW;
    float savedVH = sdl->currentViewH;
    float savedVA = sdl->currentViewAngle;
    int32_t savedPX = sdl->currentPortX;
    int32_t savedPY = sdl->currentPortY;
    int32_t savedPW = sdl->currentPortW;
    int32_t savedPH = sdl->currentPortH;

    SDL_SetRenderTarget(sdl->renderer, sdl->surfaces[destSurfaceID]);
    SDL_Rect fullRect = {0, 0, destW, destH};
    SDL_SetRenderViewport(sdl->renderer, &fullRect);

    sdl->currentViewX = 0;
    sdl->currentViewY = 0;
    sdl->currentViewW = (float)destW;
    sdl->currentViewH = (float)destH;
    sdl->currentViewAngle = 0;
    sdl->currentPortX = 0;
    sdl->currentPortY = 0;
    sdl->currentPortW = destW;
    sdl->currentPortH = destH;

    float xs[4] = {(float)destX, (float)(destX + sW), (float)(destX + sW), (float)destX};
    float ys[4] = {(float)destY, (float)destY, (float)(destY + sH), (float)(destY + sH)};
    float us[4] = {u0, u1, u1, u0};
    float vs[4] = {v0, v0, v1, v1};

    emitColoredQuad(sdl, srcTex, xs, ys, us, vs, 1.0f, 1.0f, 1.0f, 1.0f);

    SDL_SetRenderTarget(sdl->renderer, prevTarget);
    SDL_SetRenderViewport(sdl->renderer, &prevViewport);
    sdl->currentViewX = savedVX;
    sdl->currentViewY = savedVY;
    sdl->currentViewW = savedVW;
    sdl->currentViewH = savedVH;
    sdl->currentViewAngle = savedVA;
    sdl->currentPortX = savedPX;
    sdl->currentPortY = savedPY;
    sdl->currentPortW = savedPW;
    sdl->currentPortH = savedPH;
}

static bool sdlSurfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID < 0 || (uint32_t)surfaceID >= sdl->surfaceCount) return false;
    if (!sdl->surfaces[surfaceID]) return false;

    int32_t w = sdl->surfaceWidth[surfaceID];
    int32_t h = sdl->surfaceHeight[surfaceID];
    if (w <= 0 || h <= 0) return false;

    SDL_Texture* prevTarget = SDL_GetRenderTarget(sdl->renderer);
    SDL_SetRenderTarget(sdl->renderer, sdl->surfaces[surfaceID]);

    SDL_Rect fullRect = {0, 0, w, h};
    SDL_Surface* surf = SDL_RenderReadPixels(sdl->renderer, &fullRect);
    if (!surf) {
        SDL_SetRenderTarget(sdl->renderer, prevTarget);
        return false;
    }

    memcpy(outRGBA, surf->pixels, (size_t)w * (size_t)h * 4);
    SDL_DestroySurface(surf);

    SDL_SetRenderTarget(sdl->renderer, prevTarget);
    return true;
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

static uint32_t sdlSpriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
    SDLRenderer* sdl = SDL(renderer);
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return 0;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t) pageId) return 0;
    return (uint32_t)(tpagIndex + 1);
}

static uint32_t sdlSurfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
    SDLRenderer* sdl = SDL(renderer);
    if (surfaceID < 0 || (uint32_t)surfaceID >= sdl->surfaceCount) return 0;
    if (!sdl->surfaceTexture[surfaceID]) return 0;
    return 0x40000000u | (uint32_t)surfaceID;
}

// Resolve a texture handle to its pixel dimensions. Returns false if unresolvable.
static bool sdlResolveTextureHandle(SDLRenderer* sdl, uint32_t texHandle, int32_t* outW, int32_t* outH) {
    if (texHandle == 0) return false;
    if (texHandle & 0x40000000u) {
        uint32_t sid = texHandle & ~0x40000000u;
        if (sid >= sdl->surfaceCount || sdl->surfaceTexture[sid] == nullptr) return false;
        *outW = sdl->surfaceWidth[sid];
        *outH = sdl->surfaceHeight[sid];
        return true;
    }
    int32_t tpagIndex = (int32_t) texHandle - 1;
    if (0 > tpagIndex) return false;
    DataWin* dw = sdl->base.dataWin;
    if (dw->tpag.count <= (uint32_t) tpagIndex) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t) pageId) return false;
    *outW = sdl->textureWidths[pageId];
    *outH = sdl->textureHeights[pageId];
    return true;
}

static float sdlTextureGetTexelWidth(Renderer* renderer, uint32_t texID) {
    SDLRenderer* sdl = SDL(renderer);
    int32_t w = 0, h = 0;
    if (!sdlResolveTextureHandle(sdl, texID, &w, &h) || 0 >= w) return 1.0f;
    return 1.0f / (float) w;
}

static float sdlTextureGetTexelHeight(Renderer* renderer, uint32_t texID) {
    SDLRenderer* sdl = SDL(renderer);
    int32_t w = 0, h = 0;
    if (!sdlResolveTextureHandle(sdl, texID, &w, &h) || 0 >= h) return 1.0f;
    return 1.0f / (float) h;
}

static bool sdlTextureGetUVs(Renderer* renderer, uint32_t texHandle, float* outUVs) {
    SDLRenderer* sdl = SDL(renderer);
    if (texHandle == 0) return false;
    if (texHandle & 0x40000000u) {
        uint32_t sid = texHandle & ~0x40000000u;
        if (sid >= sdl->surfaceCount || !sdl->surfaceTexture[sid]) return false;
        outUVs[0] = 0.0f; outUVs[1] = 0.0f; outUVs[2] = 1.0f; outUVs[3] = 1.0f;
        return true;
    }
    int32_t tpagIndex = (int32_t)texHandle - 1;
    if (0 > tpagIndex) return false;
    DataWin* dw = renderer->dataWin;
    if (dw->tpag.count <= (uint32_t)tpagIndex) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || sdl->textureCount <= (uint32_t)pageId) return false;
    float w = (float)sdl->textureWidths[pageId];
    float h = (float)sdl->textureHeights[pageId];
    if (w <= 0.0f || h <= 0.0f) return false;
    float divW = 1.0f / w;
    float divH = 1.0f / h;
    outUVs[0] = (float)tpag->sourceX * divW;
    outUVs[1] = (float)tpag->sourceY * divH;
    outUVs[2] = outUVs[0] + (float)tpag->sourceWidth * divW;
    outUVs[3] = outUVs[1] + (float)tpag->sourceHeight * divH;
    return true;
}

static void sdlTextureSetStage(Renderer* renderer, int32_t slot, uint32_t texHandle) {
    SDLRenderer* sdl = SDL(renderer);
    if (slot < 0) {
        fprintf(stderr, "SDL: Invalid Texture Stage\n");
        return;
    }
    if (slot > MAX_TEXTURE_STAGES) {
        fprintf(stderr, "SDL: Texture Stage Higher Than Max\n");
        return;
    }
    SDL_Texture* tex = nullptr;
    if (texHandle != 0) {
        if (texHandle & 0x40000000u) {
            uint32_t sid = texHandle & ~0x40000000u;
            if (sid < sdl->surfaceCount) tex = sdl->surfaceTexture[sid];
        } else {
            int32_t tpagIndex = (int32_t)texHandle - 1;
            if (tpagIndex >= 0) {
                DataWin* dw = renderer->dataWin;
                if (dw->tpag.count > (uint32_t)tpagIndex) {
                    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
                    int16_t pageId = tpag->texturePageId;
                    if (pageId >= 0 && sdl->textureCount > (uint32_t)pageId)
                        tex = sdl->sdlTextures[pageId];
                }
            }
        }
    }
    sdl->textureStages[slot] = tex;
}

// ===[ Shader Queries ]===

static bool sdlShaderIsCompiled(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED int32_t shader) {
    return false;
}

static bool sdlShadersSupported(void) {
    return false;
}

// ===[ Matrix ]===

static void sdlSetMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) {
    renderer->gmlMatrices[matrixType] = matrix;

    Matrix4f world = renderer->gmlMatrices[MATRIX_WORLD];
    Matrix4f view = renderer->gmlMatrices[MATRIX_VIEW];
    Matrix4f projection = renderer->gmlMatrices[MATRIX_PROJECTION];

    Matrix4f worldView;
    Matrix4f_multiply(&worldView, &view, &world);

    Matrix4f worldViewProjection;
    Matrix4f_multiply(&worldViewProjection, &projection, &worldView);

    renderer->gmlMatrices[MATRIX_WORLD_VIEW] = worldView;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = worldViewProjection;
}

// ===[ Vtable ]===

static RendererVtable sdlVtable;

// ===[ Public API ]===

bool SDLRenderer_ensureTextureLoaded(SDLRenderer* sdl, uint32_t pageId) {
    if (pageId >= sdl->textureCount) return false;
    return ensureTextureLoaded(sdl, sdl->base.dataWin, pageId);
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
	sdl->preFogTex       = nullptr;
	sdl->fogTex          = nullptr;

    sdl->windowW = 0;
    sdl->windowH = 0;
    sdl->gameW   = 0;
    sdl->gameH   = 0;

    sdl->textureCount          = 0;
    sdl->sdlTextures           = nullptr;
    sdl->textureWidths         = nullptr;
    sdl->textureHeights        = nullptr;
    sdl->textureLoaded         = nullptr;

    memset(sdl->textureStages, 0, sizeof(sdl->textureStages));

    sdl->originalTexturePageCount = 0;
    sdl->originalTpagCount        = 0;
    sdl->originalSpriteCount      = 0;
    sdl->surfaces                 = nullptr;
    sdl->surfaceTexture           = nullptr;
    sdl->surfaceWidth             = nullptr;
    sdl->surfaceHeight            = nullptr;
    sdl->surfaceCount             = 0;

    sdl->currentBlendMode     = 0;
    sdl->currentSFactor       = bm_src_alpha;
    sdl->currentDFactor       = bm_inv_src_alpha;
    sdl->currentSFactorAlpha  = bm_src_alpha;
    sdl->currentDFactorAlpha  = bm_inv_src_alpha;
    sdl->sdlBlendMode         = SDL_BLENDMODE_BLEND;
    sdl->sdlBlendEnabled      = true;

    return (Renderer*)sdl;
}
