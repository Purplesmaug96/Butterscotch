#include "sdl_renderer.h"
#include "platformdefs.h"
#include "matrix_math.h"
#include "text_utils.h"
#include "image_decoder.h"
#include "utils.h"
#include "stb_ds.h"

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

static void emitQuad(SDLRenderer* sdl, SDL_Texture* tex,
                     float x[4], float y[4], float u[4], float v[4],
                     float r[4], float g[4], float b[4], float a[4]) {
    SDL_Vertex verts[4];

    for (int i = 0; i < 4; i++) {
        float vx, vy;
        transformWorldToView(sdl, x[i], y[i], &vx, &vy);

        verts[i].position.x = vx;
        verts[i].position.y = vy;
        verts[i].tex_coord.x = u[i];
        verts[i].tex_coord.y = v[i];

		// SDL3 uses float colours, sdl2 doesnt
        verts[i].color.r = r[i];
        verts[i].color.g = g[i];
        verts[i].color.b = b[i];
        verts[i].color.a = a[i];
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
        SDL_SetTextureBlendMode(sdl->whiteTexture, SDL_BLENDMODE_BLEND);
    }

    SDL_SetRenderDrawBlendMode(sdl->renderer, SDL_BLENDMODE_BLEND);

    fprintf(stderr, "SDL: Renderer initialized (%u texture pages)\n", sdl->textureCount);
}

static void sdlDestroy(Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);

    if (sdl->whiteTexture) SDL_DestroyTexture(sdl->whiteTexture);

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

    SDL_SetRenderTarget(sdl->renderer, NULL);
    SDL_SetRenderDrawColor(sdl->renderer, 255, 0, 255, 255);
    SDL_RenderClear(sdl->renderer);
}

static void sdlEndFrameInit(MAYBE_UNUSED Renderer* renderer) {
}

static void sdlEndFrameEnd(MAYBE_UNUSED Renderer* renderer) {
    SDLRenderer* sdl = SDL(renderer);
    SDL_SetRenderTarget(sdl->renderer, NULL);
    SDL_RenderPresent(sdl->renderer);
}

// ===[ View / Camera ]===

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

    SDL_Rect portRect = { portX, portY, portW, portH };
    SDL_SetRenderViewport(sdl->renderer, &portRect);
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

static void sdlDrawRectangleColor(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                                  MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                                  MAYBE_UNUSED float y2, MAYBE_UNUSED uint32_t color1,
                                  MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3,
                                  MAYBE_UNUSED uint32_t color4, MAYBE_UNUSED float alpha,
                                  MAYBE_UNUSED bool outline) {
	sdlDrawRectangle(renderer, x1, y1, x2, y2, color1, alpha, outline);
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

static void sdlDrawTriangle(MAYBE_UNUSED Renderer* renderer, MAYBE_UNUSED float x1,
                            MAYBE_UNUSED float y1, MAYBE_UNUSED float x2,
                            MAYBE_UNUSED float y2, MAYBE_UNUSED float x3,
                            MAYBE_UNUSED float y3, MAYBE_UNUSED uint32_t color1,
                            MAYBE_UNUSED uint32_t color2, MAYBE_UNUSED uint32_t color3,
                            MAYBE_UNUSED float alpha, MAYBE_UNUSED bool outline) {
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

static void sdlGpuSetBlendEnable(Renderer* renderer, bool enable) {
    SDLRenderer* sdl = SDL(renderer);
    SDL_SetRenderDrawBlendMode(sdl->renderer, enable ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
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
    if (sdl->surfaces[surfaceID]) {
        SDL_DestroyTexture(sdl->surfaces[surfaceID]);
        sdl->surfaces[surfaceID] = nullptr;
        sdl->surfaceTexture[surfaceID] = nullptr;
        sdl->surfaceWidth[surfaceID] = 0;
        sdl->surfaceHeight[surfaceID] = 0;
    }
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
