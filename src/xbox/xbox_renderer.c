#ifdef XBOX_PB_RENDERER

#include "xbox_renderer.h"
#include "asset_cache.h"
#include "matrix_math.h"
#include "text_utils.h"
#include <hal/video.h>
#include <pbkit/pbkit.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"

#ifndef NV097_SET_BLEND_OP
#define NV097_SET_BLEND_OP 0x0000034c
#endif

#ifndef NV097_SET_BLEND_OP_V_FUNC_ADD
#define NV097_SET_BLEND_OP_V_FUNC_ADD 0x1
#endif

#ifndef NV097_SET_BEGIN_END_OP
#define NV097_SET_BEGIN_END_OP 0x000017fc
#endif

#ifndef NV097_SET_BEGIN_END_OP_V_QUADS
#define NV097_SET_BEGIN_END_OP_V_QUADS 0x8
#endif

#ifndef NV097_SET_BEGIN_END_OP_V_END
#define NV097_SET_BEGIN_END_OP_V_END 0x0
#endif

#ifndef NV097_SET_COLOR
#define NV097_SET_COLOR 0x00001818
#endif

#ifndef NV097_SET_VERTEX_DATA3F_V
#define NV097_SET_VERTEX_DATA3F_V 0x00001d00
#endif

#ifndef NV097_SET_SCISSOR_HORIZONTAL
#define NV097_SET_SCISSOR_HORIZONTAL 0x00000200
#endif

#ifndef NV097_SET_SCISSOR_VERTICAL
#define NV097_SET_SCISSOR_VERTICAL   0x00000204
#endif

// Texture registers Stage 0
#define NV097_SET_TEXTURE_OFFSET_0 0x00001b00
#define NV097_SET_TEXTURE_FORMAT_0 0x00001b04

#define DYNAMIC_TPAG_OFFSET_BASE 0xD0000000u

PbTexture* PbTexture_Create(int w, int h, int format) {
    PbTexture* tex = (PbTexture*)malloc(sizeof(PbTexture));
    if (!tex) return NULL;

    size_t size = w * h * 4; // Assuming 32-bit (8888)
    
    // Allocate memory the GPU can actually see
    // Parameters: size, min_addr, max_addr, alignment, protection
    tex->pixels = MmAllocateContiguousMemoryEx(size, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);

    if (!tex->pixels) {
        free(tex);
        return NULL;
    }

    tex->width = w;
    tex->height = h;
    tex->pitch = w * 4;
    tex->format = format;
    tex->blendMode = BLENDMODE_NONE;

    return tex;
}

void PbTexture_Update(PbTexture* tex, void* src_pixels) {
    if (!tex || !tex->pixels || !src_pixels) return;
    
    // Copy CPU data (from STB_Image) into GPU-visible RAM
    memcpy(tex->pixels, src_pixels, tex->width * tex->height * 4);
}

void PbTexture_Destroy(PbTexture* tex) {
    if (!tex) return;

    if (tex->pixels) {
        // Correct way to free GPU memory in nxdk
        MmFreeContiguousMemory(tex->pixels);
    }

    free(tex);
}

void PbTexture_SetBlendMode(PbTexture* tex, int blendMode) {
    tex->blendMode = blendMode;
}

void Pbkit_SetBlendMode(int mode) {
    // 0x304 is NV097_SET_BLEND_ENABLE
    uint32_t *p = pb_begin();
    p[0] = (1 << 18) | NV097_SET_BLEND_ENABLE; // Command header: 1 value to this register
    p[1] = (mode == 0 ? 0 : 1);
    p += 2;
    pb_end(p);

    if (mode == 0) return;

    uint32_t sfactor, dfactor;

    switch(mode) {
        case 1: // BLENDMODE_BLEND
            sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
            dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA;
            break;
        case 2: // BLENDMODE_ADD
            sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA;
            dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE;
            break;
        case 3: // BLENDMODE_MOD
            sfactor = NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO;
            dfactor = NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_COLOR;
            break;
        default: return;
    }

    p = pb_begin();
    // Pushing 3 registers starting at SFACTOR (SFACTOR, DFACTOR, BLEND_OP)
    // SFACTOR = 0x344
    p[0] = (3 << 18) | NV097_SET_BLEND_FUNC_SFACTOR; 
    p[1] = sfactor;
    p[2] = dfactor;
    p[3] = NV097_SET_BLEND_OP_V_FUNC_ADD; 
    p += 4;
    pb_end(p);  
}

static void xbox_set_scissor(int x, int y, int width, int height) {
    uint32_t *p = pb_begin();
    
    // Command header: 1 value to HORIZONTAL (0x200)
    p[0] = (1 << 18) | NV097_SET_SCISSOR_HORIZONTAL;
    // Bits 31-16: Width, Bits 15-0: X-offset
    p[1] = (width << 16) | (x & 0xFFFF);
    
    // Command header: 1 value to VERTICAL (0x204)
    p[2] = (1 << 18) | NV097_SET_SCISSOR_VERTICAL;
    // Bits 31-16: Height, Bits 15-0: Y-offset
    p[3] = (height << 16) | (y & 0xFFFF);
    
    p += 4;
    pb_end(p);
}

static void transformWorldToView(MainRenderer* render, float wx, float wy, float* vx, float* vy) {
    float lx = wx - render->currentViewX;
    float ly = wy - render->currentViewY;

    if (render->currentViewAngle != 0.0f) {
        float cx = render->currentViewW / 2.0f;
        float cy = render->currentViewH / 2.0f;
        
        lx -= cx;
        ly -= cy;
        
        float angleRad = -render->currentViewAngle * ((float) M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);
        
        float nx = lx * cosA - ly * sinA;
        float ny = lx * sinA + ly * cosA;
        
        lx = nx + cx;
        ly = ny + cy;
    }

    *vx = lx * (render->currentPortW / render->currentViewW);
    *vy = ly * (render->currentPortH / render->currentViewH);
}

static inline uint32_t pack_u32(uint8_t b3, uint8_t b2, uint8_t b1, uint8_t b0) {
    return ((uint32_t)b3 << 24) | ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b0;
}

// Matches the layout of standard 2D vertices (similar to SDL_Vertex)
typedef struct {
    float x, y;
    DWORD color; // A8R8G8B8 format
    float u, v;  // Texture coordinates
} pb_Vertex2D;

// Helper to safely bypass C strict aliasing when pushing floats to the push buffer
static inline uint32_t f2u(float f) {
    union { float f; uint32_t u; } val;
    val.f = f;
    return val.u;
}

void pb_render_geometry(const pb_Vertex2D *vertices, int num_vertices, const int *indices, int num_indices) {
    if (!vertices || num_vertices == 0) return;

    uint32_t *p;

    // 1. Tell the GPU we are about to start drawing Triangles
    p = pb_begin();
    pb_push(p++, NV097_SET_BEGIN_END, 1);
    *(p++) = NV097_SET_BEGIN_END_OP_TRIANGLES;
    pb_end(p);

    // 2. Push the geometry inline
    int count = indices ? num_indices : num_vertices;

    for (int i = 0; i < count; i++) {
        int v_idx = indices ? indices[i] : i;
        const pb_Vertex2D *v = &vertices[v_idx];

        p = pb_begin();

        // -- WRITE ATTRIBUTES --
        // Note: Writing to Attribute 0 (Position) tells the GPU to assemble the vertex.
        // Therefore, we MUST write UVs and Colors FIRST, and Position LAST.

        // Attribute 9: Texture Coordinates (2 floats)
        pb_push(p++, NV097_INLINE_ARRAY + 9*4, 2);
        *(p++) = f2u(v->u);
        *(p++) = f2u(v->v);

        // Attribute 3: Diffuse Color (1 DWORD)
        pb_push(p++, NV097_INLINE_ARRAY + 3*4, 1);
        *(p++) = v->color;

        // Attribute 0: Position (4 floats: X, Y, Z, RHW) -> THIS TRIGGERS THE VERTEX
        // Setting Z=0.0f and RHW=1.0f tells the GPU this is pre-transformed 2D screen space.
        pb_push(p++, NV097_INLINE_ARRAY + 0*4, 4);
        *(p++) = f2u(v->x);
        *(p++) = f2u(v->y);
        *(p++) = f2u(0.0f); // Z
        *(p++) = f2u(1.0f); // RHW (1.0 = screen space)

        pb_end(p);
    }

    // 3. End primitive drawing
    p = pb_begin();
    pb_push(p++, NV097_SET_BEGIN_END, 1);
    *(p++) = NV097_SET_BEGIN_END_OP_END;
    pb_end(p);
}

static void emitQuad(MainRenderer* render, PbTexture* tex,
                     float x[4], float y[4], float u[4], float v[4], 
                     float r[4], float g[4], float b[4], float a[4]) {
    
    pb_Vertex2D verts[4];
    
    for (int i = 0; i < 4; i++) {
        float vx, vy;
        transformWorldToView(render, x[i], y[i], &vx, &vy);
        
        verts[i].x = vx;
        verts[i].y = vy;
        verts[i].u = u[i];
        verts[i].v = v[i];

        uint8_t rr = r[0] * 255.0f;
        uint8_t gg = g[0] * 255.0f;
        uint8_t bb = b[0] * 255.0f;
        uint8_t aa = a[0] * 255.0f;
        
        verts[i].color = pack_u32(aa, rr, gg, bb);
    }

    int indices[6] = {0, 1, 2, 2, 3, 0};
    pb_render_geometry(verts, 4, indices, 6);
}

static void emitColoredQuad(MainRenderer* render, PbTexture* tex, float x[4], float y[4], float u[4], float v[4], float r, float g, float b, float a) {
    float rc[4] = {r, r, r, r};
    float gc[4] = {g, g, g, g};
    float bc[4] = {b, b, b, b};
    float ac[4] = {a, a, a, a};
    emitQuad(render, tex, x, y, u, v, rc, gc, bc, ac);
}

void print_array(int parray[], int size)
{
    int i;
    // Loop to print the elements of the array
    for(i = 0; i < size - 1; i++)
    {
        printf("%d, ", parray[i]);
    }
    // Printing the last element
    printf("%d ", parray[i]);
    printf("\n");
}

static void evictOldest(MainRenderer* render) {
    // // The oldest texture is always at index 0
    // uint32_t oldestId = render->renderTexturesUsedTracker[0];

    // if (render->renderTextures[oldestId] != nullptr) {
    //     PbTexture_Destroy(render->renderTextures[oldestId]);
    //     render->renderTextures[oldestId] = nullptr; 
    // }

    // // Shift everything left to remove the hole at index 0
    // for (int i = 0; i < render->textureCount - 1; i++) {
    //     render->renderTexturesUsedTracker[i] = render->renderTexturesUsedTracker[i + 1];
    // }
    // // Set the last slot to a placeholder (e.g., a dummy value) 
    // // so it doesn't look like a valid pageId until updated
    // render->renderTexturesUsedTracker[render->textureCount - 1] = 0xFFFFFFFF; 
}

static void updateLRU(MainRenderer* render, uint32_t pageId) {
    // // 1. Find if pageId is already in the tracker and remove it
    // int foundIdx = -1;
    // for (int i = 0; i < render->textureCount; i++) {
    //     if (render->renderTexturesUsedTracker[i] == pageId) {
    //         foundIdx = i;
    //         break;
    //     }
    // }

    // // 2. Shift items left to close the gap if it was found
    // int startIdx = (foundIdx != -1) ? foundIdx : 0;
    // for (int i = startIdx; i < render->textureCount - 1; i++) {
    //     render->renderTexturesUsedTracker[i] = render->renderTexturesUsedTracker[i + 1];
    // }

    // // 3. Put the current page at the very end (Most Recently Used)
    // render->renderTexturesUsedTracker[render->textureCount - 1] = pageId;
}
#define ENSURE_TEXTURE_LOADED_MAX_LRU_REMOVE 8

// Lazy-load a texture on demand when it's first needed
static void ensureTextureLoaded(MainRenderer* render, DataWin* dw, uint32_t pageId) {
    if (pageId >= render->textureCount) return;
    if (render->renderTextures[pageId] != nullptr) return;  // Already loaded
    
    Texture* txtr = &dw->txtr.textures[pageId];
    int w, h, channels;
    
    uint8_t* pngData = txtr->blobData;
    uint32_t pngSize = txtr->blobSize;
    
    // If blob data wasn't preloaded, try to load from asset cache
    if (pngData == nullptr && render->assetCache != nullptr) {
        AssetCacheEntry entry = AssetCache_getTextureBlobData(render->assetCache, txtr->blobOffset, txtr->blobSize);
        pngData = (uint8_t*) entry.data;
        pngSize = (uint32_t) entry.size;
    }
    
    if (pngData == nullptr) {
        fprintf(stderr, "XBOX:  TXTR page %u has no data and no cache available\n", pageId);
        return;
    }
    
    uint8_t* pixels = nullptr;

    unsigned int i = 0;
    while (i < ENSURE_TEXTURE_LOADED_MAX_LRU_REMOVE && pixels == nullptr) {
        debugPrint("XBOX:  Trying to load TXTR page %u from memory...\n", pageId);
        pixels = stbi_load_from_memory(pngData, (int) pngSize, &w, &h, &channels, 4);
        
        if (pixels == nullptr) {
            fprintf(stderr, "XBOX:  Failed to decode TXTR page %u\n", pageId);
            evictOldest(render);
            // debugPrint("LRU freed: %s\n", freed ? "true" : "false");
            XBMemStat();
        }

        i++;
    }

    if (i == ENSURE_TEXTURE_LOADED_MAX_LRU_REMOVE) {
        fprintf(stderr, "Loading retries for TXTR page %u hit max of %u, giving up and loading whiteTexture.\n", pageId, ENSURE_TEXTURE_LOADED_MAX_LRU_REMOVE);
        render->renderTextures[pageId] = render->whiteTexture;
        updateLRU(render, pageId);
        return;
    }

    updateLRU(render, pageId);

    render->textureWidths[pageId] = w;
    render->textureHeights[pageId] = h;

    PbTexture* tex = safeCalloc(1, sizeof(PbTexture));
    tex->width = w;
    tex->height = h;
    tex->pitch = w * 4;
    tex->format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8; // Assuming 32-bit PNG

    // ALLOCATE CONTIGUOUS MEMORY FOR THE GPU
    tex->pixels = MmAllocateContiguousMemoryEx(w * h * 4, 0, 0x03FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);

    if (tex->pixels) {
        // Copy the loaded image data to the GPU memory
        // Note: For optimal performance on Xbox, you should 'swizzle' this data using XGSwizzleRect
        memcpy(tex->pixels, pixels, w * h * 4); 
    }

    render->renderTextures[pageId] = tex;
    stbi_image_free(pixels);
    fprintf(stderr, "XBOX:  Lazy-loaded TXTR page %u (%dx%d)\n", pageId, w, h);
}

static void renderInit(Renderer* renderer, DataWin* dataWin) {
    MainRenderer* render = (MainRenderer*) renderer;
    renderer->dataWin = dataWin;

    render->textureCount = dataWin->txtr.count;
    render->renderTextures = safeCalloc(render->textureCount, sizeof(PbTexture*));
    render->textureWidths = safeCalloc(render->textureCount, sizeof(int32_t));
    render->textureHeights = safeCalloc(render->textureCount, sizeof(int32_t));
    render->renderTexturesUsedTracker = safeCalloc(render->textureCount, sizeof(uint32_t));

    // Don't load textures here - they will be loaded on-demand when first used

    render->whiteTexture = PbTexture_Create(1, 1, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8);
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    PbTexture_Update(render->whiteTexture, whitePixel);
    PbTexture_SetBlendMode(render->whiteTexture, BLENDMODE_BLEND);

    render->originalTexturePageCount = render->textureCount;
    render->originalTpagCount = dataWin->tpag.count;
    render->originalSpriteCount = dataWin->sprt.count;
}

static void renderDestroy(Renderer* renderer) {
    MainRenderer* render = (MainRenderer*) renderer;

    if (render->fboTexture) PbTexture_Destroy(render->fboTexture);
    if (render->whiteTexture) PbTexture_Destroy(render->whiteTexture);

    for (uint32_t i = 0; render->textureCount > i; i++) {
        if (render->renderTextures[i]) PbTexture_Destroy(render->renderTextures[i]);
    }

    free(render->renderTextures);
    free(render->textureWidths);
    free(render->textureHeights);
    free(render->renderTexturesUsedTracker);
    free(render);
}

static void renderBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    MainRenderer* render = (MainRenderer*) renderer;

    render->windowW = windowW;
    render->windowH = windowH;
    render->gameW = gameW;
    render->gameH = gameH;

    if (gameW != render->fboWidth || gameH != render->fboHeight) {
        if (render->fboTexture) PbTexture_Destroy(render->fboTexture);
        render->fboTexture = PbTexture_Create(gameW, gameH, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8);
        render->fboWidth = gameW;
        render->fboHeight = gameH;
        fprintf(stderr, "XBOX:  FBO resized to %dx%d\n", gameW, gameH);
    }

    pb_wait_for_vbl();
    pb_target_back_buffer(); // Basic setup

    // If you want to render to your FBO texture instead of the screen:
    // pb_set_color_buffer(render->fboTexture->pixels, render->fboWidth, render->fboHeight, render->fboTexture->pitch);
    
    // Reset Viewport and Scissor (Clips) to full screen
    pb_set_viewport(0, 0, windowW, windowH, 0, 65535); 
    xbox_set_scissor(0, 0, windowW, windowH);
}

static void renderBeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    MainRenderer* render = (MainRenderer*) renderer;

    render->currentViewX = (float)viewX;
    render->currentViewY = (float)viewY;
    render->currentViewW = (float)viewW;
    render->currentViewH = (float)viewH;
    render->currentPortX = (float)portX;
    render->currentPortY = (float)portY;
    render->currentPortW = (float)portW;
    render->currentPortH = (float)portH;
    render->currentViewAngle = viewAngle;

    // Set the Hardware Viewport
    // pb_set_viewport params: x, y, width, height, minZ, maxZ
    pb_set_viewport(portX, portY, portW, portH, 0, 65535);

    // Set Hardware Scissor (Clip Rect)
    xbox_set_scissor(portX, portY, portW, portH);
}

static void renderEndView(Renderer* renderer) {
    MainRenderer* render = (MainRenderer*) renderer;
    // Reset scissor to full screen
    xbox_set_scissor(0, 0, render->windowW, render->windowH);
}

static void renderEndFrame(Renderer* renderer) {
    MainRenderer* render = (MainRenderer*) renderer;

    // 1. If you were rendering to an FBO, you'd now switch back to the screen
    // pb_target_back_buffer();

    // 2. Perform the SDL_RenderCopy equivalent (Blit FBO to Screen)
    // You can call your internal 'emitQuad' function here using fboTexture
    float x[4] = {0, render->windowW, render->windowW, 0};
    float y[4] = {0, 0, render->windowH, render->windowH};
    float u[4] = {0, 1, 1, 0};
    float v[4] = {0, 0, 1, 1};
    float c[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // White/Full Alpha
    
    // Custom blit:
    // emitQuad(render, render->fboTexture, x, y, u, v, c, c, c, c);

    // 3. Finalize and Flip
    while (pb_busy());    // Ensure all commands are processed
    pb_finished();   // The actual SwapBuffers/Present
}

static void renderRendererFlush(Renderer* renderer) {
    (void) renderer;
}

static void renderDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    MainRenderer* render = (MainRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || render->textureCount <= (uint32_t) pageId) return;

    // Lazy-load texture on demand
    ensureTextureLoaded(render, dw, pageId);
    
    PbTexture* tex = render->renderTextures[pageId];
    if (!tex) return;

    float texW = (float)render->textureWidths[pageId];
    float texH = (float)render->textureHeights[pageId];

    float u0 = (float) tpag->sourceX / texW;
    float v0 = (float) tpag->sourceY / texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / texH;

    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->sourceWidth;
    float localY1 = localY0 + (float) tpag->sourceHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    float xs[4], ys[4];
    Matrix4f_transformPoint(&transform, localX0, localY0, &xs[0], &ys[0]); // top-left
    Matrix4f_transformPoint(&transform, localX1, localY0, &xs[1], &ys[1]); // top-right
    Matrix4f_transformPoint(&transform, localX1, localY1, &xs[2], &ys[2]); // bottom-right
    Matrix4f_transformPoint(&transform, localX0, localY1, &xs[3], &ys[3]); // bottom-left

    float us[4] = {u0, u1, u1, u0};
    float vs[4] = {v0, v0, v1, v1};

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    emitColoredQuad(render, tex, xs, ys, us, vs, r, g, b, alpha);
}

static void renderDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, uint32_t color, float alpha) {
    MainRenderer* render = (MainRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || render->textureCount <= (uint32_t) pageId) return;

    // Lazy-load texture on demand
    ensureTextureLoaded(render, dw, pageId);
    
    PbTexture* tex = render->renderTextures[pageId];
    if (!tex) return;

    float texW = (float)render->textureWidths[pageId];
    float texH = (float)render->textureHeights[pageId];

    float u0 = (float) (tpag->sourceX + srcOffX) / texW;
    float v0 = (float) (tpag->sourceY + srcOffY) / texH;
    float u1 = (float) (tpag->sourceX + srcOffX + srcW) / texW;
    float v1 = (float) (tpag->sourceY + srcOffY + srcH) / texH;

    float xs[4] = { x, x + srcW * xscale, x + srcW * xscale, x };
    float ys[4] = { y, y, y + srcH * yscale, y + srcH * yscale };
    float us[4] = { u0, u1, u1, u0 };
    float vs[4] = { v0, v0, v1, v1 };

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    emitColoredQuad(render, tex, xs, ys, us, vs, r, g, b, alpha);
}

static void renderDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    MainRenderer* render = (MainRenderer*) renderer;

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
            emitColoredQuad(render, render->whiteTexture, tx[i], ty[i], us, vs, r, g, b, alpha);
        }
    } else {
        float xs[4] = {x1, x2 + 1, x2 + 1, x1};
        float ys[4] = {y1, y1, y2 + 1, y2 + 1};
        emitColoredQuad(render, render->whiteTexture, xs, ys, us, vs, r, g, b, alpha);
    }
}
static void renderDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    renderer->vtable->drawLineColor(renderer, x1, y1, x2, y2, width, color, color, alpha);
}

static void renderDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    MainRenderer* render = (MainRenderer*) renderer;

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

    emitQuad(render, render->whiteTexture, xs, ys, us, vs, rc, gc, bc, ac);
}

static void renderDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg) {
    MainRenderer* render = (MainRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dw->font.fonts[fontIndex];
    int32_t fontTpagIndex = DataWin_resolveTPAG(dw, font->textureOffset);
    if (0 > fontTpagIndex) return;

    TexturePageItem* fontTpag = &dw->tpag.items[fontTpagIndex];
    int16_t pageId = fontTpag->texturePageId;
    if (0 > pageId || render->textureCount <= (uint32_t) pageId) return;

    // Lazy-load texture on demand
    ensureTextureLoaded(render, dw, pageId);
    
    PbTexture* tex = render->renderTextures[pageId];
    if (!tex) return;

    float texW = (float)render->textureWidths[pageId];
    float texH = (float)render->textureHeights[pageId];

    uint32_t color = renderer->drawColor;
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    char* processed = TextUtils_preprocessGmlText(text);
    int32_t textLen = (int32_t) strlen(processed);
    int32_t lineCount = TextUtils_countLines(processed, textLen);

    float totalHeight = (float) lineCount * (float) font->emSize;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    float cursorY = valignOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        int32_t pos = 0;

        while (lineLen > pos) {
            uint16_t ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;
            
            if (glyph->sourceWidth == 0 || glyph->sourceHeight == 0) {
                cursorX += glyph->shift;
                continue;
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

            emitColoredQuad(render, tex, xs, ys, us, vs, r, g, b, renderer->drawAlpha);

            cursorX += glyph->shift;
            if (lineLen > pos) {
                int32_t savedPos = pos;
                uint16_t nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
                pos = savedPos;
                cursorX += TextUtils_getKerningOffset(glyph, nextCh);
            }
        }

        cursorY += (float) font->emSize;
        if (textLen > lineEnd) lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
        else lineStart = lineEnd;
    }
    free(processed);
}


static uint32_t findOrAllocTexturePageSlot(MainRenderer* render) {
    for (uint32_t i = render->originalTexturePageCount; render->textureCount > i; i++) {
        if (render->renderTextures[i] == nullptr) return i;
    }
    uint32_t newPageId = render->textureCount++;
    render->renderTextures = safeRealloc(render->renderTextures, render->textureCount * sizeof(PbTexture*));
    render->textureWidths = safeRealloc(render->textureWidths, render->textureCount * sizeof(int32_t));
    render->textureHeights = safeRealloc(render->textureHeights, render->textureCount * sizeof(int32_t));
    render->renderTextures[newPageId] = nullptr;
    return newPageId;
}

static uint32_t findOrAllocTpagSlot(DataWin* dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count++;
    dw->tpag.items = safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;
    return newIndex;
}

static uint32_t findOrAllocSpriteSlot(DataWin* dw, uint32_t originalSpriteCount) {
    for (uint32_t i = originalSpriteCount; dw->sprt.count > i; i++) {
        if (dw->sprt.sprites[i].textureCount == 0) return i;
    }
    uint32_t newIndex = dw->sprt.count++;
    dw->sprt.sprites = safeRealloc(dw->sprt.sprites, dw->sprt.count * sizeof(Sprite));
    memset(&dw->sprt.sprites[newIndex], 0, sizeof(Sprite));
    return newIndex;
}

// TODO: implement renderCreateSpriteFromSurface
static int32_t renderCreateSpriteFromSurface(Renderer* renderer, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    // MainRenderer* render = (MainRenderer*) renderer;
    // DataWin* dw = renderer->dataWin;

    // if (0 >= w || 0 >= h || !render->fboTexture) return -1;

    // uint8_t* pixels = safeMalloc((size_t) w * (size_t) h * 4);
    
    // SDL_Rect rect = { x, y, w, h };
    // if (SDL_RenderReadPixels(render->renderRenderer, &rect, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8, pixels, w * 4) != 0) {
    //     free(pixels);
    //     return -1;
    // }

    // PbTexture* newTex = PbTexture_Create(w, h, NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8B8G8R8);
    // PbTexture_Update(newTex, nullptr, pixels, w * 4);
    // PbTexture_SetBlendMode(newTex, BLENDMODE_BLEND);
    
    // // SDL_SetTextureScaleMode(newTex, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);

    // free(pixels);

    // uint32_t pageId = findOrAllocTexturePageSlot(render);
    // render->renderTextures[pageId] = newTex;
    // render->textureWidths[pageId] = w;
    // render->textureHeights[pageId] = h;

    // uint32_t tpagIndex = findOrAllocTpagSlot(dw, render->originalTpagCount);
    // TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    // tpag->sourceX = 0; tpag->sourceY = 0;
    // tpag->sourceWidth = (uint16_t) w; tpag->sourceHeight = (uint16_t) h;
    // tpag->targetX = 0; tpag->targetY = 0;
    // tpag->targetWidth = (uint16_t) w; tpag->targetHeight = (uint16_t) h;
    // tpag->boundingWidth = (uint16_t) w; tpag->boundingHeight = (uint16_t) h;
    // tpag->texturePageId = (int16_t) pageId;

    // uint32_t fakeOffset = DYNAMIC_TPAG_OFFSET_BASE + tpagIndex;
    // hmput(dw->tpagOffsetMap, fakeOffset, (int32_t) tpagIndex);

    // uint32_t spriteIndex = findOrAllocSpriteSlot(dw, render->originalSpriteCount);
    // Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    // sprite->name = "dynamic_sprite";
    // sprite->width = (uint32_t) w;
    // sprite->height = (uint32_t) h;
    // sprite->originX = xorig;
    // sprite->originY = yorig;
    // sprite->textureCount = 1;
    // sprite->textureOffsets = safeMalloc(sizeof(uint32_t));
    // sprite->textureOffsets[0] = fakeOffset;
    // sprite->maskCount = 0;
    // sprite->masks = nullptr;

    // fprintf(stderr, "XBOX:  Created dynamic sprite %u (%dx%d) from surface at (%d,%d)\n", spriteIndex, w, h, x, y);
    // return (int32_t) spriteIndex;
}

static void renderDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    MainRenderer* render = (MainRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;
    if (render->originalSpriteCount > (uint32_t) spriteIndex) {
        fprintf(stderr, "XBOX:  Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return;

    repeat(sprite->textureCount, i) {
        uint32_t offset = sprite->textureOffsets[i];
        if (offset >= DYNAMIC_TPAG_OFFSET_BASE) {
            int32_t tpagIdx = DataWin_resolveTPAG(dw, offset);
            if (tpagIdx >= 0) {
                TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
                int16_t pageId = tpag->texturePageId;
                if (pageId >= 0 && render->textureCount > (uint32_t) pageId) {
                    if (render->renderTextures[pageId]) {
                        PbTexture_Destroy(render->renderTextures[pageId]);
                        render->renderTextures[pageId] = nullptr;
                    }
                }
                tpag->texturePageId = -1;
            }
            hmdel(dw->tpagOffsetMap, offset);
        }
    }

    free(sprite->textureOffsets);
    memset(sprite, 0, sizeof(Sprite));
    fprintf(stderr, "XBOX:  Deleted sprite %d\n", spriteIndex);
}

// ===[ Vtable ]===

static RendererVtable renderVtable = {
    .init = renderInit,
    .destroy = renderDestroy,
    .beginFrame = renderBeginFrame,
    .endFrame = renderEndFrame,
    .beginView = renderBeginView,
    .endView = renderEndView,
    .drawSprite = renderDrawSprite,
    .drawSpritePart = renderDrawSpritePart,
    .drawRectangle = renderDrawRectangle,
    .drawLine = renderDrawLine,
    .drawLineColor = renderDrawLineColor,
    .drawText = renderDrawText,
    .flush = renderRendererFlush,
    .createSpriteFromSurface = renderCreateSpriteFromSurface,
    .deleteSprite = renderDeleteSprite,
    .drawTile = nullptr,
};

// ===[ Public API ]===

Renderer* MainRenderer_create() {
    MainRenderer* render = safeCalloc(1, sizeof(MainRenderer));
    render->base.vtable = &renderVtable;
    render->base.drawColor = 0xFFFFFF; // white (BGR)
    render->base.drawAlpha = 1.0f;
    render->base.drawFont = -1;
    render->base.drawHalign = 0;
    render->base.drawValign = 0;
    
    return (Renderer*) render;
}

void MainRenderer_setAssetCache(MainRenderer* renderer, AssetCache* cache) {
    if (renderer) {
        renderer->assetCache = cache;
    }
}

#endif