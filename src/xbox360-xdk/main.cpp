// Butterscotch Xbox 360 — XDK Entry Point
// Uses official Xbox 360 SDK: D3D9, XAudio2, XInputGetState

#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// DbgPrint is a C-linkage kernel function — declare it explicitly since
// we compile .c files as C++ and removed extern "C" wrappers.
extern "C" ULONG __cdecl DbgPrint(const char* format, ...);

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "runner.h"
#include "runner_keyboard.h"
#include "vm.h"
#include "data_win.h"
#include "json_reader.h"
#include "utils.h"
#include "stb_ds.h"

#include "d3d9_renderer.h"

#ifdef USE_XAUDIO2_AUDIO
#include "xaudio2_audio.h"
#else
#include "noop_audio_system.h"
#endif

#include "xdk_file_system.h"
#include "debug_font/debug_font.h"
#include "stb_image.h"

// ===[ POSIX clock polyfill implementation ]===
double _xdk_monotonic_ms(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart * 1000.0;
}

// Screen dimensions (720p native)
#define SCREEN_WIDTH  1280
#define SCREEN_HEIGHT 720

static bool xdkGetWindowSize(int32_t* outW, int32_t* outH) {
    if (outW) *outW = SCREEN_WIDTH;
    if (outH) *outH = SCREEN_HEIGHT;
    return true;
}

static void xdkSetWindowSize(int32_t width, int32_t height) {
    diagLog("Butterscotch: window_set_size ignored on fixed %dx%d backbuffer requested=%dx%d", SCREEN_WIDTH, SCREEN_HEIGHT, width, height);
}

static HANDLE gDiagLog = INVALID_HANDLE_VALUE;
static FILE* gDiagFile = NULL;
static bool gDiagTriedFallback = false;

// Log ring buffer — captures the last N lines of _diagLog output
// so the fatal-error screen can display them to the user.
#define FATAL_LOG_LINES     512
#define FATAL_LOG_LINE_LEN  256
static char gFatalLogLines[FATAL_LOG_LINES][FATAL_LOG_LINE_LEN];
static volatile int gFatalLogHead  = 0;   // next slot to write
static volatile int gFatalLogCount = 0;   // total lines stored (capped at FATAL_LOG_LINES)

static char gLastParseChunk[5] = "NONE";
static int gLastParseChunkIndex = -1;
static int gLastParseChunkTotal = 0;

struct LoadingVertex {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
};

typedef struct LoadingScreen {
    IDirect3DDevice9* dev;
    IDirect3DTexture9* splashTex;
    IDirect3DTexture9* fontTex;
    IDirect3DTexture9* whiteTex;
    IDirect3DVertexShader9* vertexShader;
    IDirect3DPixelShader9* pixelShader;
    IDirect3DVertexDeclaration9* vertexDecl;
    int splashW;
    int splashH;
    bool available;
    char stage[128];
} LoadingScreen;

static LoadingScreen gLoadingScreen;
static LoadingScreen gDiagOverlayScreen;

static bool gDiagOverlayVisible = false;
static bool gDiagOverlayComboWasDown = false;
static float gDiagOverlayFps = 0.0f;
static float gDiagOverlayDtMs = 0.0f;
static int gDiagOverlaySteps = 0;
static uint32_t gDiagOverlayFrameCount = 0;
static double gDiagOverlayWindowStart = 0.0;
static SIZE_T gDiagTotalPhys = 0;
static SIZE_T gDiagAvailPhys = 0;
static SIZE_T gDiagTotalVirtual = 0;
static SIZE_T gDiagAvailVirtual = 0;
static int gDiagControllerConnected = 0;
static int gDiagSpeedCapRemoved = 0;
static uint32_t gDiagRoomAgeFrames = 0;
static uint32_t gDiagRoomTransitionHolds = 0;

static bool diagOpenPath(const char* path, bool overwrite) {
    FILE* f = fopen(path, overwrite ? "wb" : "ab");
    if (f) {
        if (gDiagFile) fclose(gDiagFile);
        if (gDiagLog != INVALID_HANDLE_VALUE) {
            CloseHandle(gDiagLog);
            gDiagLog = INVALID_HANDLE_VALUE;
        }
        gDiagFile = f;
        return true;
    }
    int crtErr = errno;

    HANDLE h = CreateFileA(path,
                           GENERIC_WRITE,
                           FILE_SHARE_READ,
                           NULL,
                           overwrite ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DbgPrint("Butterscotch: open log failed at %s errno=%d gle=%lu\n", path, crtErr, GetLastError());
        return false;
    }
    if (!overwrite) SetFilePointer(h, 0, NULL, FILE_END);
    if (gDiagFile) {
        fclose(gDiagFile);
        gDiagFile = NULL;
    }
    if (gDiagLog != INVALID_HANDLE_VALUE) CloseHandle(gDiagLog);
    gDiagLog = h;
    return true;
}

static void _diagLog(FILE* file, const char* fmt, va_list args) {
    char line[1024];
    _vsnprintf(line, sizeof(line) - 2, fmt, args);
    line[sizeof(line) - 2] = '\0';

    size_t len = strlen(line);
    if (len == 0 || line[len - 1] != '\n') {
        line[len++] = '\n';
        line[len] = '\0';
    }

    DbgPrint("%s", line);
    if (file) {
        fputs(line, file);
        fflush(file);
    }
    if (gDiagLog != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(gDiagLog, line, (DWORD)strlen(line), &written, NULL);
        FlushFileBuffers(gDiagLog);
    }

    // Append to the in-memory log ring buffer for fatal error display.
    // Strip the trailing newline so the display lines look clean.
    size_t copyLen = len;
    if (copyLen > 0 && line[copyLen - 1] == '\n') copyLen--;
    if (copyLen >= FATAL_LOG_LINE_LEN) copyLen = FATAL_LOG_LINE_LEN - 1;
    memcpy(gFatalLogLines[gFatalLogHead], line, copyLen);
    gFatalLogLines[gFatalLogHead][copyLen] = '\0';

    gFatalLogHead = (gFatalLogHead + 1) % FATAL_LOG_LINES;
    if (gFatalLogCount < FATAL_LOG_LINES) gFatalLogCount++;
}

void diagLog(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	_diagLog(gDiagFile, fmt, args);
	va_end(args);
}

void fdiagLog(FILE* file, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	if (file == stdout || file == stderr) {
		_diagLog(gDiagFile, fmt, args);
	}
	else {
		vfprintf(file, fmt, args);
	}
	va_end(args);
}

static void diagOpenLog(void) {
    if (gDiagLog != INVALID_HANDLE_VALUE || gDiagFile || gDiagTriedFallback) return;
    gDiagTriedFallback = true;

    static const char* paths[] = {
        "butterscotch:\\butterscotch.log",
        "game:\\butterscotch.log",
        "d:\\butterscotch.log",
        "hdd:\\butterscotch.log",
        "cache:\\butterscotch.log",
        "uda:\\butterscotch.log",
        "uda:/butterscotch.log",
        "usb0:\\butterscotch.log",
        NULL,
    };
    for (int i = 0; paths[i]; i++) {
        if (diagOpenPath(paths[i], true)) {
            diagLog("Butterscotch: log opened at %s", paths[i]);
            return;
        }
    }
    DbgPrint("Butterscotch: WARNING: no writable log path found\n");
}

static void diagOpenNextToDataWin(const char* dataWinPath) {
    if (gDiagFile || gDiagLog != INVALID_HANDLE_VALUE) {
        diagLog("Butterscotch: keeping existing log while data.win is at %s", dataWinPath);
        return;
    }

    char logPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen >= sizeof(logPath) - 32) return;
        memcpy(logPath, dataWinPath, dirLen);
        strcpy(logPath + dirLen, "butterscotch.log");
    } else {
        strcpy(logPath, "butterscotch.log");
    }

    if (diagOpenPath(logPath, true)) {
        diagLog("Butterscotch: logging to %s", logPath);
    } else {
        diagLog("Butterscotch: WARNING: failed to open log next to data.win at %s gle=%lu", logPath, GetLastError());
    }
}

static void loadingSetVertex(LoadingVertex* v, float x, float y, float u, float vv,
                             float r, float g, float b, float a) {
    v->x = x - 0.5f;
    v->y = y - 0.5f;
    v->z = 0.0f;
    v->w = 1.0f;
    v->u = u;
    v->v = vv;
    v->r = r;
    v->g = g;
    v->b = b;
    v->a = a;
}

static void loadingApplyState(LoadingScreen* ls) {
    IDirect3DDevice9* dev = ls->dev;
    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = SCREEN_WIDTH;
    vp.Height = SCREEN_HEIGHT;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dev->SetVertexShader(ls->vertexShader);
    dev->SetPixelShader(ls->pixelShader);
    dev->SetVertexDeclaration(ls->vertexDecl);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_VIEWPORTENABLE, FALSE);
    for (DWORD sampler = 0; sampler < 8; sampler++) {
        dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
}

static IDirect3DTexture9* loadingCreateTextureFromRgba(IDirect3DDevice9* dev, const uint8_t* pixels, int w, int h) {
    IDirect3DTexture9* tex = NULL;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL)) || !tex) {
        return NULL;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0))) {
        tex->Release();
        return NULL;
    }

    for (int y = 0; y < h; y++) {
        const uint8_t* src = pixels + y * w * 4;
        DWORD* dst = (DWORD*)((uint8_t*)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < w; x++) {
            uint8_t r = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t b = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            if (a == 0) { r = 0; g = 0; b = 0; }
            dst[x] = D3DCOLOR_ARGB(a, r, g, b);
        }
    }

    tex->UnlockRect(0);
    return tex;
}

static IDirect3DTexture9* loadingLoadPng(IDirect3DDevice9* dev, const char* path, int* outW, int* outH) {
    int w = 0;
    int h = 0;
    int channels = 0;
    uint8_t* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) return NULL;
    IDirect3DTexture9* tex = loadingCreateTextureFromRgba(dev, pixels, w, h);
    stbi_image_free(pixels);
    if (tex) {
        *outW = w;
        *outH = h;
    }
    return tex;
}

static IDirect3DTexture9* loadingCreateFontTexture(IDirect3DDevice9* dev) {
    uint8_t* rgba = (uint8_t*)malloc(DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H * 4);
    if (!rgba) return NULL;
    for (int i = 0; i < DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H; i++) {
        uint8_t a = debugFontPixels[i];
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = a;
    }
    IDirect3DTexture9* tex = loadingCreateTextureFromRgba(dev, rgba, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H);
    free(rgba);
    return tex;
}

static bool loadingInit(LoadingScreen* ls, IDirect3DDevice9* dev, const char* dataWinPath) {
    memset(ls, 0, sizeof(*ls));
    ls->dev = dev;
    strcpy(ls->stage, "Starting");

    static const char* vs =
        "struct VS_IN  { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
        "struct VS_OUT { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
        "VS_OUT main(VS_IN i) { VS_OUT o; o.Pos = i.Pos; o.Tex = i.Tex; o.Col = i.Col; return o; }\n";
    static const char* ps =
        "sampler2D s0 : register(s0) = sampler_state { MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };\n"
        "struct PS_IN { float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
        "float4 main(PS_IN i) : COLOR0 { return tex2D(s0, i.Tex) * i.Col; }\n";

    ID3DXBuffer* code = NULL;
    ID3DXBuffer* err = NULL;
    HRESULT hr = D3DXCompileShader(vs, (UINT)strlen(vs), NULL, NULL, "main", "vs_2_0", 0, &code, &err, NULL);
    if (FAILED(hr)) {
        if (err) err->Release();
        diagLog("LOAD: vertex shader compile failed hr=0x%08X", hr);
        return false;
    }
    dev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &ls->vertexShader);
    code->Release();

    hr = D3DXCompileShader(ps, (UINT)strlen(ps), NULL, NULL, "main", "ps_2_0", 0, &code, &err, NULL);
    if (FAILED(hr)) {
        if (err) err->Release();
        diagLog("LOAD: pixel shader compile failed hr=0x%08X", hr);
        return false;
    }
    dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ls->pixelShader);
    code->Release();

    static const D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        D3DDECL_END()
    };
    if (FAILED(dev->CreateVertexDeclaration(decl, &ls->vertexDecl))) {
        diagLog("LOAD: vertex declaration failed");
        return false;
    }

    ls->fontTex = loadingCreateFontTexture(dev);
    if (!ls->fontTex) diagLog("LOAD: debug font texture failed");
    {
        uint8_t whitePixel[4] = { 255, 255, 255, 255 };
        ls->whiteTex = loadingCreateTextureFromRgba(dev, whitePixel, 1, 1);
    }

    char splashPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen < sizeof(splashPath) - 16) {
            memcpy(splashPath, dataWinPath, dirLen);
            strcpy(splashPath + dirLen, "splash.png");
            ls->splashTex = loadingLoadPng(dev, splashPath, &ls->splashW, &ls->splashH);
        }
    }
    diagLog("LOAD: splash %s", ls->splashTex ? "loaded" : "not found");

    ls->available = (ls->vertexShader && ls->pixelShader && ls->vertexDecl);
    return ls->available;
}

static void loadingDestroy(LoadingScreen* ls) {
    if (ls->dev) {
        ls->dev->SetTexture(0, NULL);
        ls->dev->SetVertexShader(NULL);
        ls->dev->SetPixelShader(NULL);
        ls->dev->SetVertexDeclaration(NULL);
    }
    if (ls->splashTex) ls->splashTex->Release();
    if (ls->fontTex) ls->fontTex->Release();
    if (ls->whiteTex) ls->whiteTex->Release();
    if (ls->vertexShader) ls->vertexShader->Release();
    if (ls->pixelShader) ls->pixelShader->Release();
    if (ls->vertexDecl) ls->vertexDecl->Release();
    memset(ls, 0, sizeof(*ls));
}

static void loadingDrawQuad(LoadingScreen* ls, IDirect3DTexture9* tex,
                            float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1,
                            float r, float g, float b, float a) {
    LoadingVertex verts[4];
    loadingSetVertex(&verts[0], x0, y0, u0, v0, r, g, b, a);
    loadingSetVertex(&verts[1], x1, y0, u1, v0, r, g, b, a);
    loadingSetVertex(&verts[2], x1, y1, u1, v1, r, g, b, a);
    loadingSetVertex(&verts[3], x0, y1, u0, v1, r, g, b, a);
    ls->dev->SetTexture(0, tex ? tex : ls->whiteTex);
    ls->dev->DrawPrimitiveUP(D3DPT_QUADLIST, 1, verts, sizeof(LoadingVertex));
}

static void loadingDrawText(LoadingScreen* ls, const char* text, float x, float y, float scale,
                            float r, float g, float b, float a) {
    if (!ls->fontTex || !text) return;
    float penX = x;
    for (const char* p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n') {
            penX = x;
            y += (float)DEBUGFONT_LINE_HEIGHT * scale;
            continue;
        }
        if (ch < DEBUGFONT_FIRST_CP || ch > DEBUGFONT_LAST_CP) ch = '?';
        const DebugFontGlyphEntry* glyph = &debugFontGlyphs[ch - DEBUGFONT_FIRST_CP];
        float gx0 = penX + (float)glyph->xoffset * scale;
        float gy0 = y + (float)glyph->yoffset * scale;
        float gx1 = gx0 + (float)glyph->w * scale;
        float gy1 = gy0 + (float)glyph->h * scale;
        float u0 = ((float)glyph->x + 0.5f) / (float)DEBUGFONT_ATLAS_W;
        float v0 = ((float)glyph->y + 0.5f) / (float)DEBUGFONT_ATLAS_H;
        float u1 = ((float)glyph->x + (float)glyph->w - 0.5f) / (float)DEBUGFONT_ATLAS_W;
        float v1 = ((float)glyph->y + (float)glyph->h - 0.5f) / (float)DEBUGFONT_ATLAS_H;
        loadingDrawQuad(ls, ls->fontTex, gx0, gy0, gx1, gy1, u0, v0, u1, v1, r, g, b, a);
        penX += (float)glyph->xadvance * scale;
    }
}

static float loadingTextWidth(const char* text, float scale) {
    float w = 0.0f;
    if (!text) return w;
    for (const char* p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < DEBUGFONT_FIRST_CP || ch > DEBUGFONT_LAST_CP) ch = '?';
        w += (float)debugFontGlyphs[ch - DEBUGFONT_FIRST_CP].xadvance * scale;
    }
    return w;
}

static void loadingDraw(LoadingScreen* ls, float progress, const char* stage) {
    if (!ls || !ls->available) return;
    if (stage && stage[0]) {
        _snprintf(ls->stage, sizeof(ls->stage) - 1, "%s", stage);
        ls->stage[sizeof(ls->stage) - 1] = '\0';
    }
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    IDirect3DDevice9* dev = ls->dev;
    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (FAILED(dev->BeginScene())) return;
    loadingApplyState(ls);

    if (ls->splashTex && ls->splashW > 0 && ls->splashH > 0) {
        float scaleX = (float)SCREEN_WIDTH / (float)ls->splashW;
        float scaleY = (float)SCREEN_HEIGHT / (float)ls->splashH;
        float scale = (scaleX < scaleY) ? scaleX : scaleY;
        float w = (float)ls->splashW * scale;
        float h = (float)ls->splashH * scale;
        float x = ((float)SCREEN_WIDTH - w) * 0.5f;
        float y = ((float)SCREEN_HEIGHT - h) * 0.5f;
        loadingDrawQuad(ls, ls->splashTex, x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    float barW = 720.0f;
    float barH = 18.0f;
    float barX = ((float)SCREEN_WIDTH - barW) * 0.5f;
    float barY = (float)SCREEN_HEIGHT - 96.0f;
    loadingDrawQuad(ls, NULL, barX - 3.0f, barY - 3.0f, barX + barW + 3.0f, barY + barH + 3.0f,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.70f);
    loadingDrawQuad(ls, NULL, barX, barY, barX + barW, barY + barH,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.12f, 0.12f, 0.12f, 0.95f);
    loadingDrawQuad(ls, NULL, barX, barY, barX + barW * progress, barY + barH,
                    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.73f, 0.18f, 1.0f);

    float textScale = 0.42f;
    float textW = loadingTextWidth(ls->stage, textScale);
    loadingDrawText(ls, ls->stage, ((float)SCREEN_WIDTH - textW) * 0.5f, barY + 30.0f,
                    textScale, 1.0f, 1.0f, 1.0f, 0.92f);

    dev->EndScene();
    dev->Present(NULL, NULL, NULL, NULL);
}

// Draws a full-screen fatal error screen using the log ring buffer.
// Can be called even after loading has ended; falls back gracefully if
// the loading screen resources have been destroyed by trying the diag
// overlay screen, or simply skips rendering if neither is available.
static void drawFatalErrorScreen(LoadingScreen* ls) {
    if (!ls || !ls->available) {
        // Fall back to the diag overlay screen if the primary loading screen
        // has been destroyed (e.g. after loading completed).
        ls = &gDiagOverlayScreen;
    }
    if (!ls || !ls->available) return;

    IDirect3DDevice9* dev = ls->dev;
    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (FAILED(dev->BeginScene())) return;
    loadingApplyState(ls);

    const float scale = 0.36f;
    const float lineH = (float)DEBUGFONT_LINE_HEIGHT * scale + 2.0f;
    const float marginX = 14.0f;
    const float marginY = 12.0f;
    float y = marginY;

    // Title bar
    loadingDrawText(ls, "FATAL ERROR!!!", marginX, y, 0.48f,
                    1.0f, 0.2f, 0.2f, 1.0f);
    y += (float)DEBUGFONT_LINE_HEIGHT * 0.48f + 6.0f;

    // Draw a separator line
    loadingDrawQuad(ls, NULL, marginX, y, (float)SCREEN_WIDTH - marginX, y + 1.0f,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.6f, 0.2f, 0.2f, 0.8f);
    y += 8.0f;

    // Determine the set of lines to display — the most recent ones that fit.
    int count = gFatalLogCount;
    int head = gFatalLogHead;

    // How many lines fit on screen?
    int maxLines = (int)((SCREEN_HEIGHT - y - 10.0f) / lineH);
    if (maxLines <= 0) maxLines = 1;
    if (count > maxLines) count = maxLines;

    // Walk backwards from head to get the last 'count' lines.
    int startIdx = (head - count + FATAL_LOG_LINES) % FATAL_LOG_LINES;
    for (int i = 0; i < count; i++) {
        int idx = (startIdx + i) % FATAL_LOG_LINES;
        const char* line = gFatalLogLines[idx];
        if (line[0] == '\0') continue;

        // Color-code lines: FATAL / BS: prefix in red/orange
        float r = 0.85f, g = 0.85f, b = 0.85f; // default light grey
        if (strstr(line, "FATAL") || strstr(line, "ERROR")) {
            r = 1.0f; g = 0.3f; b = 0.3f;
        } else if (strstr(line, "BS:")) {
            r = 0.7f; g = 0.8f; b = 1.0f;
        }

        loadingDrawText(ls, line, marginX, y, scale, r, g, b, 0.92f);
        y += lineH;
    }

    // Bottom hint
    y = (float)SCREEN_HEIGHT - 30.0f;
    loadingDrawText(ls, "Console hung — check log above",
                    marginX, y, 0.42f, 0.6f, 0.6f, 0.6f, 0.8f);

    dev->EndScene();
    dev->Present(NULL, NULL, NULL, NULL);
}

static bool diagOverlayInit(IDirect3DDevice9* dev, const char* dataWinPath) {
    bool ok = loadingInit(&gDiagOverlayScreen, dev, dataWinPath);
    if (gDiagOverlayScreen.splashTex) {
        gDiagOverlayScreen.splashTex->Release();
        gDiagOverlayScreen.splashTex = NULL;
        gDiagOverlayScreen.splashW = 0;
        gDiagOverlayScreen.splashH = 0;
    }
    return ok && gDiagOverlayScreen.fontTex && gDiagOverlayScreen.whiteTex;
}

static void diagOverlayDrawLine(const char* text, float* y, float scale, float r, float g, float b, float a) {
    loadingDrawText(&gDiagOverlayScreen, text, 22.0f, *y, scale, r, g, b, a);
    *y += (float)DEBUGFONT_LINE_HEIGHT * scale + 3.0f;
}

static float diagBytesToMb(SIZE_T bytes) {
    return (float)((double)bytes / (1024.0 * 1024.0));
}

static void diagOverlayPollSystem(void) {
    MEMORYSTATUS ms;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    gDiagTotalPhys = ms.dwTotalPhys;
    gDiagAvailPhys = ms.dwAvailPhys;
    gDiagTotalVirtual = ms.dwTotalVirtual;
    gDiagAvailVirtual = ms.dwAvailVirtual;
}

static void diagOverlayDraw(Runner* runner, Renderer* renderer, int32_t frameW, int32_t frameH) {
    if (!gDiagOverlayVisible || !runner || !gDiagOverlayScreen.available) return;

    if (renderer && renderer->vtable && renderer->vtable->flush) {
        renderer->vtable->flush(renderer);
    }

    loadingApplyState(&gDiagOverlayScreen);

    const char* roomName = "(none)";
    int32_t roomIndex = -1;
    uint32_t roomSpeed = 0;
    uint32_t roomW = 0;
    uint32_t roomH = 0;
    if (runner->currentRoom) {
        roomName = runner->currentRoom->name ? runner->currentRoom->name : "(null)";
        roomIndex = runner->currentRoomIndex;
        roomSpeed = runner->currentRoom->speed;
        roomW = runner->currentRoom->width;
        roomH = runner->currentRoom->height;
    }

    const float x0 = 12.0f;
    const float y0 = 12.0f;
    const float x1 = 560.0f;
    const float y1 = 222.0f;
    loadingDrawQuad(&gDiagOverlayScreen, NULL, x0, y0, x1, y1,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.72f);
    loadingDrawQuad(&gDiagOverlayScreen, NULL, x0, y0, x1, y0 + 3.0f,
                    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.73f, 0.18f, 0.95f);

    char line[256];
    float y = y0 + 12.0f;
    diagOverlayDrawLine("Butterscotch Debug Overlay (LB+RB)", &y, 0.42f, 1.0f, 0.90f, 0.45f, 1.0f);

    _snprintf(line, sizeof(line) - 1, "FPS: %.1f  dt: %.2fms  steps: %d  speed: %u", gDiagOverlayFps, gDiagOverlayDtMs, gDiagOverlaySteps, roomSpeed);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 1.0f, 1.0f, 1.0f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "Room %d: %s", roomIndex, roomName);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 1.0f, 1.0f, 1.0f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "Room: %ux%u  inst: %d  pending: %d", roomW, roomH, (int32_t)arrlen(runner->instances), runner->pendingRoom);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.82f, 0.92f, 1.0f, 0.95f);

    SIZE_T usedPhys = gDiagTotalPhys > gDiagAvailPhys ? (gDiagTotalPhys - gDiagAvailPhys) : 0;
    _snprintf(line, sizeof(line) - 1, "RAM: %.1f/%.1f MB  free: %.1f MB",
              diagBytesToMb(usedPhys), diagBytesToMb(gDiagTotalPhys),
              diagBytesToMb(gDiagAvailPhys));
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.75f, 1.0f, 0.75f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "Virt free: %.1f MB  pad: %d  fast: %d  roomAge: %u hold: %u",
              diagBytesToMb(gDiagAvailVirtual), gDiagControllerConnected, gDiagSpeedCapRemoved,
              gDiagRoomAgeFrames, gDiagRoomTransitionHolds);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.75f, 1.0f, 0.75f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "Game: %dx%d  frame: %dx%d  app: %dx%d", SCREEN_WIDTH, SCREEN_HEIGHT, frameW, frameH, runner->applicationWidth, runner->applicationHeight);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.82f, 0.92f, 1.0f, 0.95f);

    _snprintf(line, sizeof(line) - 1, "GUI %dx%d:  surf: auto=%d keep=%d id=%d", runner->guiWidth, runner->guiHeight,
              runner->appSurfaceAutoDraw ? 1 : 0, runner->appSurfaceKeepWindowSize ? 1 : 0, runner->applicationSurfaceId);
    line[sizeof(line) - 1] = '\0';
    diagOverlayDrawLine(line, &y, 0.36f, 0.82f, 0.92f, 1.0f, 0.95f);
}

extern "C" void Butterscotch_xdkHang() {
	while (true) {Sleep(1000);}
	diagLog("Butterscotch: FATAL Somehow the end of Butterscotch_xdkHang was reached???");
	drawFatalErrorScreen(&gLoadingScreen);
}

extern "C" void Butterscotch_xdkExit(int errcode, const char* file, int line) {
    diagOpenLog();
    diagLog("Butterscotch: FATAL exit with errcode %d at %s:%d lastChunk=%s index=%d/%d", errcode, file ? file : "(null)", line, gLastParseChunk, gLastParseChunkIndex, gLastParseChunkTotal);
	drawFatalErrorScreen(&gLoadingScreen);
    Butterscotch_xdkHang();
}

extern "C" void Butterscotch_xdkAbort(const char* file, int line) {
    diagOpenLog();
    diagLog("Butterscotch: FATAL abort at %s:%d lastChunk=%s index=%d/%d", file ? file : "(null)", line, gLastParseChunk, gLastParseChunkIndex, gLastParseChunkTotal);
	drawFatalErrorScreen(&gLoadingScreen);
    Butterscotch_xdkHang();
}

extern "C" void Butterscotch_xdkDataWinTrace(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    diagLog("DW: %s", line);
}

extern "C" void Butterscotch_xdkDiagTrace(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    diagLog("%s", line);
}

static void dataWinParseProgress(const char* chunkName, int chunkIndex, int totalChunks, DataWin* dataWin, void* userData) {
    (void)dataWin;
    memcpy(gLastParseChunk, chunkName, 4);
    gLastParseChunk[4] = '\0';
    gLastParseChunkIndex = chunkIndex;
    gLastParseChunkTotal = totalChunks;
    diagLog("PARSE chunk %d/%d %.4s", chunkIndex + 1, totalChunks, chunkName);
    LoadingScreen* loading = (LoadingScreen*)userData;
    if (loading && loading->available) {
        char stage[128];
        _snprintf(stage, sizeof(stage) - 1, "Loading data.win: %.4s %d/%d", chunkName, chunkIndex + 1, totalChunks);
        stage[sizeof(stage) - 1] = '\0';
        float progress = (totalChunks > 0) ? ((float)(chunkIndex + 1) / (float)totalChunks) : 0.0f;
        loadingDraw(loading, progress * 0.82f, stage);
    }
}

static DataWin* parseDataWinGuarded(const char* dataWinPath, DataWinParserOptions parseOpts) {
    DataWin* dataWin = NULL;
    unsigned int exceptionCode = 0;
    __try {
        dataWin = DataWin_parse(dataWinPath, parseOpts);
    } __except (exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        diagLog("Butterscotch: FATAL exception 0x%08X during DataWin_parse lastChunk=%s index=%d/%d", exceptionCode, gLastParseChunk, gLastParseChunkIndex, gLastParseChunkTotal);
        dataWin = NULL;
    }
    return dataWin;
}

static bool initFirstRoomGuarded(Runner* runner) {
    unsigned int exceptionCode = 0;
    __try {
        Runner_initFirstRoom(runner);
    } __except (exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        const char* roomName = "(none)";
        int roomIndex = -1;
        if (runner && runner->currentRoom) {
            roomName = runner->currentRoom->name ? runner->currentRoom->name : "(null)";
            roomIndex = runner->currentRoomIndex;
        }
        diagLog("Butterscotch: FATAL exception 0x%08X during Runner_initFirstRoom room=%d name=%s",
            exceptionCode, roomIndex, roomName);
        return false;
    }
    return true;
}

// ===[ Controller Mapping ]===

typedef struct {
    WORD xpadButton;
    int32_t gmlKey;
} XpadMapping;

static XpadMapping* xpadMappings = NULL;
static int xpadMappingCount = 0;
static WORD prevButtons = 0;
static BYTE prevLeftTrigger = 0;
static BYTE prevRightTrigger = 0;
static bool gamepadApiEnabled = false;

static void setupDefaultMappings(void) {
    static XpadMapping defaults[] = {
        { XINPUT_GAMEPAD_DPAD_UP,    38 },  // VK_UP
        { XINPUT_GAMEPAD_DPAD_DOWN,  40 },  // VK_DOWN
        { XINPUT_GAMEPAD_DPAD_LEFT,  37 },  // VK_LEFT
        { XINPUT_GAMEPAD_DPAD_RIGHT, 39 },  // VK_RIGHT
        { XINPUT_GAMEPAD_A,          90 },  // 'Z' key (confirm/action — Pizza Tower, Undertale, etc.)
        { XINPUT_GAMEPAD_B,          88 },  // 'X' key (cancel/dash — Pizza Tower, Undertale, etc.)
        { XINPUT_GAMEPAD_X,          17 },  // VK_CONTROL
        { XINPUT_GAMEPAD_Y,          16 },  // VK_SHIFT
        { XINPUT_GAMEPAD_START,      27 },  // VK_ESCAPE (menu)
        { XINPUT_GAMEPAD_BACK,       27 },  // VK_ESCAPE
    };
    xpadMappingCount = sizeof(defaults) / sizeof(XpadMapping);
    xpadMappings = (XpadMapping*)malloc(sizeof(defaults));
    memcpy(xpadMappings, defaults, sizeof(defaults));
}

static const char* osTypeName(YoYoOperatingSystem osType) {
    switch (osType) {
        case OS_WINDOWS: return "windows";
        case OS_XBOX360: return "xbox360";
        case OS_XBOXONE: return "xboxone";
        case OS_SWITCH: return "switch";
        case OS_PS4: return "ps4";
        case OS_PS3: return "ps3";
        default: return "unknown";
    }
}

static bool parseOsTypeName(const char* text, YoYoOperatingSystem* out) {
    if (!text || !out) return false;
    if (_stricmp(text, "windows") == 0 || _stricmp(text, "win32") == 0) {
        *out = OS_WINDOWS;
        return true;
    }
    if (_stricmp(text, "xbox360") == 0 || _stricmp(text, "x360") == 0) {
        *out = OS_XBOX360;
        return true;
    }
    if (_stricmp(text, "xboxone") == 0 || _stricmp(text, "xbone") == 0 || _stricmp(text, "xbox") == 0) {
        *out = OS_XBOXONE;
        return true;
    }
    if (_stricmp(text, "switch") == 0) {
        *out = OS_SWITCH;
        return true;
    }
    if (_stricmp(text, "ps4") == 0) {
        *out = OS_PS4;
        return true;
    }
    if (_stricmp(text, "ps3") == 0) {
        *out = OS_PS3;
        return true;
    }
    return false;
}

static float normalizeStickAxis(SHORT value) {
    if (value >= 0) return (float)value / 32767.0f;
    return (float)value / 32768.0f;
}

static void setGamepadButton(GamepadSlot* slot, int index, bool down) {
    if (index < 0 || index >= GP_BUTTON_COUNT) return;
    slot->buttonDown[index] = down;
    slot->buttonValue[index] = down ? 1.0f : 0.0f;
}

static void pollGamepadApi(Runner* runner, const XINPUT_STATE* state, WORD buttons, bool connected) {
    if (!runner || !runner->gamepads) return;

    RunnerGamepad_beginFrame(runner->gamepads);
    GamepadSlot* slot = &runner->gamepads->slots[0];
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));

    if (!connected || !state) {
        slot->connected = false;
        return;
    }

    static bool loggedGamepadConnected = false;
    slot->connected = true;
    slot->jid = 0;
    strncpy(slot->description, "Xbox 360 Controller", sizeof(slot->description) - 1);
    slot->description[sizeof(slot->description) - 1] = '\0';
    strncpy(slot->guid, "xinput-xbox360", sizeof(slot->guid) - 1);
    slot->guid[sizeof(slot->guid) - 1] = '\0';

    setGamepadButton(slot, 0,  (buttons & XINPUT_GAMEPAD_A) != 0);
    setGamepadButton(slot, 1,  (buttons & XINPUT_GAMEPAD_B) != 0);
    setGamepadButton(slot, 2,  (buttons & XINPUT_GAMEPAD_X) != 0);
    setGamepadButton(slot, 3,  (buttons & XINPUT_GAMEPAD_Y) != 0);
    setGamepadButton(slot, 4,  (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
    setGamepadButton(slot, 5,  (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
    setGamepadButton(slot, 8,  (buttons & XINPUT_GAMEPAD_BACK) != 0);
    setGamepadButton(slot, 9,  (buttons & XINPUT_GAMEPAD_START) != 0);
    setGamepadButton(slot, 10, (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0);
    setGamepadButton(slot, 11, (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0);
    setGamepadButton(slot, 12, (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0);
    setGamepadButton(slot, 13, (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
    setGamepadButton(slot, 14, (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0);
    setGamepadButton(slot, 15, (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0);

    slot->buttonValue[6] = (float)state->Gamepad.bLeftTrigger / 255.0f;
    slot->buttonValue[7] = (float)state->Gamepad.bRightTrigger / 255.0f;
    slot->buttonDown[6] = slot->buttonValue[6] >= slot->triggerThreshold;
    slot->buttonDown[7] = slot->buttonValue[7] >= slot->triggerThreshold;

    slot->axisValue[0] = normalizeStickAxis(state->Gamepad.sThumbLX);
    slot->axisValue[1] = -normalizeStickAxis(state->Gamepad.sThumbLY);
    slot->axisValue[2] = normalizeStickAxis(state->Gamepad.sThumbRX);
    slot->axisValue[3] = -normalizeStickAxis(state->Gamepad.sThumbRY);

    for (int i = 0; i < GP_BUTTON_COUNT; i++) {
        bool wasDown = slot->buttonDownPrev[i];
        if (slot->buttonDown[i] && !wasDown) slot->buttonPressed[i] = true;
        if (!slot->buttonDown[i] && wasDown) slot->buttonReleased[i] = true;
    }
    runner->gamepads->connectedCount = 1;
    if (!loggedGamepadConnected) {
        diagLog("Butterscotch: gamepad API slot0 connected desc=%s guid=%s", slot->description, slot->guid);
        loggedGamepadConnected = true;
    }
}

Renderer* renderer;

extern float _offx;

static void drawRunnerFrame(Runner* runner, Renderer* renderer, int32_t gameW, int32_t gameH) {
    int32_t frameW = gameW;
    int32_t frameH = gameH;
    if (runner && !runner->appSurfaceKeepWindowSize && !runner->appSurfaceAutoDraw && runner->currentRoom &&
        runner->currentRoom->width > 0 && runner->currentRoom->height > 0 &&
        runner->currentRoom->width < (uint32_t)frameW && runner->currentRoom->height < (uint32_t)frameH) {
        frameW = (int32_t)runner->currentRoom->width;
        frameH = (int32_t)runner->currentRoom->height;
    }

    if (frameH <= 0) frameH = 1;
    // Use uniform scaling to fit the game frame into the 720p backbuffer while
    // preserving aspect ratio. The renderScale is used by setGameTargetTransform
    // for rendering directly to the backbuffer (non-app-surface path).
    // When using an application surface, the final composition in
    // setWindowSurfaceTransform also uses uniform scaling with letterboxing.
    float scaleX = (float)SCREEN_WIDTH / (float)frameW;
    float scaleY = (float)SCREEN_HEIGHT / (float)frameH;
    float displayScale = (scaleX < scaleY) ? scaleX : scaleY;
	((D3D9Renderer*)renderer)->renderScale = displayScale;
	((D3D9Renderer*)renderer)->renderOffsetX = (SCREEN_WIDTH - (frameW * displayScale)) * 0.5f;
	((D3D9Renderer*)renderer)->renderOffsetY = (SCREEN_HEIGHT - (frameH * displayScale)) * 0.5f;
	((D3D9Renderer*)renderer)->renderingToApplicationSurface = false;
    Runner_drawPre(runner, SCREEN_WIDTH, SCREEN_HEIGHT);
    Runner_beginFrame(runner, gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT);
	Runner_drawViews(runner, frameW, frameH, false);
    renderer->vtable->endFrameInit(renderer);
    Runner_drawPost(runner, SCREEN_WIDTH, SCREEN_HEIGHT);
    Runner_drawGUI(runner, SCREEN_WIDTH, SCREEN_HEIGHT, frameW, frameH);
    diagOverlayDraw(runner, renderer, frameW, frameH);
    renderer->vtable->endFrameEnd(renderer);
}

// Define the internal kernel string structure needed for Object Manager APIs
typedef struct _STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} STRING, *PSTRING;

// Explicitly declare the kernel exports since they are omitted from standard XTL headers
extern "C" {
    LONG __stdcall ObDeleteSymbolicLink(PSTRING SymbolicLinkName);
    LONG __stdcall ObCreateSymbolicLink(PSTRING SymbolicLinkName, PSTRING DeviceName);
}

// bool RemapGameDevice(const char* inputPath) {
//     if (!inputPath || inputPath[0] == '\0') return false;

//     // 1. Skip any leading slashes (handles both '/' and '\')
//     const char* cleanPath = inputPath;
//     while (*cleanPath == '/' || *cleanPath == '\\') {
//         cleanPath++;
//     }

//     // 2. Format the absolute device target path
//     char absoluteDevicePath[MAX_PATH];
//     int written = snprintf(absoluteDevicePath, MAX_PATH, "\\Device\\Harddisk0\\Partition1\\%s", cleanPath);
//     if (written < 0 || written >= MAX_PATH) {
//         return false;
//     }

//     // 3. Normalize all forward slashes to backward slashes for the Object Manager
//     for (int i = 0; absoluteDevicePath[i] != '\0'; i++) {
//         if (absoluteDevicePath[i] == '/') {
//             absoluteDevicePath[i] = '\\';
//         }
//     }

//     // 4. Setup Symbolic Link Target Name
//     STRING symLinkName;
//     symLinkName.Buffer = (PCHAR)"\\??\\game:";
//     symLinkName.Length = (USHORT)strlen(symLinkName.Buffer);
//     symLinkName.MaximumLength = symLinkName.Length + 1;

//     // 5. Delete the default OS mapping
//     ObDeleteSymbolicLink(&symLinkName);

//     // 6. Bind to our new sanitized subdirectory path
//     STRING deviceTarget;
//     deviceTarget.Buffer = absoluteDevicePath;
//     deviceTarget.Length = (USHORT)strlen(deviceTarget.Buffer);
//     deviceTarget.MaximumLength = deviceTarget.Length + 1;

//     LONG status = ObCreateSymbolicLink(&symLinkName, &deviceTarget);
//     return (status >= 0);
// }

// Creates the butterscotch:\ symbolic link to the HDD root.
// MUST be called exactly once at startup — Xenia/Xbox OS will NOT allow
// a symlink to be deleted and recreated pointing at a different path
// without a reboot.  If a game_change needs to load data.win from a
// subdirectory, we construct the full absolute path (e.g.
// butterscotch:\chapter3\data.win) instead of trying to remap the drive.
static bool gDeviceLinkCreated = false;

bool CreateCustomDeviceLink(void) {
    if (gDeviceLinkCreated) {
        diagLog("Butterscotch: device link already created, skipping");
        return true;
    }

    STRING customSymLinkName;
    customSymLinkName.Buffer = (PCHAR)"\\??\\butterscotch:";
    customSymLinkName.Length = (USHORT)strlen(customSymLinkName.Buffer);
    customSymLinkName.MaximumLength = customSymLinkName.Length + 1;

    STRING finalDeviceTarget;
    finalDeviceTarget.Buffer = (PCHAR)"\\Device\\Harddisk0\\Partition1";
    finalDeviceTarget.Length = (USHORT)strlen(finalDeviceTarget.Buffer);
    finalDeviceTarget.MaximumLength = finalDeviceTarget.Length + 1;

    // Delete any stale link first, then create fresh
    ObDeleteSymbolicLink(&customSymLinkName);
    LONG status = ObCreateSymbolicLink(&customSymLinkName, &finalDeviceTarget);

    if (status < 0) {
        diagLog("Butterscotch: FATAL: ObCreateSymbolicLink failed status=%ld", status);
        return false;
    }

    gDeviceLinkCreated = true;
    diagLog("Butterscotch: device link butterscotch:\\ -> \\Device\\Harddisk0\\Partition1 created");
    return true;
}

char* gameSubPath = NULL;
char* gameRootPath = NULL;

void ListButterscotchDirectory() {
    WIN32_FIND_DATAA findData;

	char* d = (char*)safeMalloc(MAX_PATH);

	snprintf(d, MAX_PATH - 1, "butterscotch:\\%s*", gameSubPath);

    // Look for everything in the root of the custom device link
    HANDLE hFind = FindFirstFileA("butterscotch:\\*", &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        diagLog("Butterscotch: Cannot list directory. FindFirstFileA failed with error: %d", GetLastError());
        return;
    }

    diagLog("--- Listing butterscotch:\\%s contents ---", gameSubPath ? gameSubPath : "");
    do {
        // Skip the standard relative directory dots "." and ".."
        if (strcmp(findData.cFileName, ".") == 0 || strcmp(findData.cFileName, "..") == 0) {
            continue;
        }

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            diagLog("  [DIR]  %s", findData.cFileName);
        } else {
            diagLog("  [FILE] %s (%d bytes)", findData.cFileName, findData.nFileSizeLow);
        }
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
    diagLog("---------------------------------------");
}

// ===[ Main Entry Point ]===

int32_t* gGameW = NULL;
int32_t* gGameH = NULL;

// Buffer used to carry the pre-computed data.win path from the game_change
// restart code (bottom of main()) back to the data.win search loop (top of
// main()).  Must be file-scope static so recursive calls to main() share it.
static char gNextDataWinPath[512] = "";
VOID __cdecl main() {
    diagOpenLog();

    diagLog("Butterscotch: Built at %s: %s", __DATE__, __TIME__);

    // Create the butterscotch:\ symlink ONCE — it always points to the HDD root.
    // On game_change we construct the full absolute path (e.g.
    // butterscotch:\chapter3\data.win) rather than trying to remap the link.
    if (!CreateCustomDeviceLink()) {
        diagLog("Butterscotch: FATAL: Failed to create device link");
        drawFatalErrorScreen(&gLoadingScreen);
        Butterscotch_xdkHang();
    }

    ListButterscotchDirectory();

    // Close diagnostic log handles
    if (gDiagFile) {
        diagLog("Butterscotch: Closing log file");
        fclose(gDiagFile);
        gDiagFile = NULL;
    }
    if (gDiagLog != INVALID_HANDLE_VALUE) {
        diagLog("Butterscotch: Closing log handle");
        CloseHandle(gDiagLog);
        gDiagLog = INVALID_HANDLE_VALUE;
    }

    diagOpenLog();

	if (!gameRootPath) {
		gameRootPath = "\\Device\\Harddisk0\\Partition1";
	}

    diagLog("Butterscotch: Built at %s: %s", __DATE__, __TIME__);
    diagLog("Butterscotch: (01) main() entered");

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) { diagLog("Butterscotch: FATAL: D3D create failed"); return; }
    diagLog("Butterscotch: (02) D3D9 created");

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.BackBufferWidth        = SCREEN_WIDTH;
    d3dpp.BackBufferHeight       = SCREEN_HEIGHT;
    d3dpp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    d3dpp.FrontBufferFormat      = D3DFMT_LE_X8R8G8B8;
    d3dpp.MultiSampleType        = D3DMULTISAMPLE_NONE;
    d3dpp.BackBufferCount        = 1;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;
    d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    IDirect3DDevice9* pd3dDevice = NULL;
    HRESULT hr = pD3D->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                    &d3dpp, &pd3dDevice);
    if (FAILED(hr)) {
        diagLog("Butterscotch: FATAL: CreateDevice failed hr=0x%08X", hr);
        return;
    }
    diagLog("Butterscotch: (03) D3D device created");


    // ===[ Locate data.win ]===
    // On Xbox 360, game content is at butterscotch:\ (DVD/HDD) or d:\ (dev kit)
    const char* dataWinPath = NULL;

    // On a game_change restart the exact path was pre-computed — check it first.
    // We copy to a local buffer so we can clear gNextDataWinPath without losing dataWinPath.
    char localNextDataWinPath[512] = "";
    if (gNextDataWinPath[0] != '\0') {
        strncpy(localNextDataWinPath, gNextDataWinPath, sizeof(localNextDataWinPath) - 1);
        localNextDataWinPath[sizeof(localNextDataWinPath) - 1] = '\0';
        gNextDataWinPath[0] = '\0'; // consume immediately

        diagLog("Butterscotch: (03b) trying pre-computed game_change path %s", localNextDataWinPath);
        FILE* testFile = fopen(localNextDataWinPath, "rb");
        if (testFile) {
            fclose(testFile);
            dataWinPath = localNextDataWinPath;
            diagOpenNextToDataWin(dataWinPath);
            ListButterscotchDirectory();
            diagLog("Butterscotch: (05) found data.win at %s", dataWinPath);
        } else {
            diagLog("Butterscotch: pre-computed path not found, falling through to search");
        }
    }

    if (!dataWinPath) {
        // Try multiple paths. Use fopen() for detection since it goes through
        // the Xbox CRT which handles butterscotch:\ paths reliably on both hardware and emulators.
        static const char* searchPaths[] = {
            "butterscotch:\\data.win",
            "butterscotch:\\butterscotch\\data.win",
            "game:\\data.win",
            "game:\\butterscotch\\data.win",
            "d:\\data.win",
            "d:\\butterscotch\\data.win",
            "butterscotch:\\game.unx",
            "butterscotch:\\butterscotch\\game.unx",
            "game:\\game.unx",
            "game:\\butterscotch\\game.unx",
            "d:\\game.unx",
            "d:\\butterscotch\\game.unx",
            NULL,
        };

        diagLog("Butterscotch: (04) searching for data.win");
        for (int i = 0; searchPaths[i]; i++) {
            diagLog("Butterscotch: try %s", searchPaths[i]);

            FILE* testFile = fopen(searchPaths[i], "rb");
            if (testFile) {
                fclose(testFile);
                dataWinPath = searchPaths[i];
                diagOpenNextToDataWin(dataWinPath);
                ListButterscotchDirectory();
                diagLog("Butterscotch: (05) found data.win at %s", dataWinPath);
                break;
            }
        }
    }

    if (!dataWinPath) {
        diagLog("Butterscotch: FATAL: data.win not found");
		bool _loadingOk = loadingInit(&gLoadingScreen, pd3dDevice, dataWinPath);
		if (_loadingOk) {
			diagLog("Butterscotch: NOTE: had to init loading screen early to display log/error");
			drawFatalErrorScreen(&gLoadingScreen);
		}
		else {
			diagLog("Butterscotch: NOTE: failed to init loading screen to display log/error");
		}
        Butterscotch_xdkHang();
    }

    bool loadingOk = loadingInit(&gLoadingScreen, pd3dDevice, dataWinPath);
    if (loadingOk) loadingDraw(&gLoadingScreen, 0.02f, "Starting Butterscotch");

    diagLog("Butterscotch: (06) parsing data.win");
    if (loadingOk) loadingDraw(&gLoadingScreen, 0.05f, "Opening data.win");

    DataWinParserOptions parseOpts;
    memset(&parseOpts, 0, sizeof(parseOpts));
    parseOpts.parseGen8 = true;  parseOpts.parseOptn = true;  parseOpts.parseLang = true;
    parseOpts.parseExtn = true;  parseOpts.parseSond = true;  parseOpts.parseAgrp = true;
    parseOpts.parseSprt = true;  parseOpts.parseBgnd = true;  parseOpts.parsePath = true;
    parseOpts.parseScpt = true;  parseOpts.parseGlob = true;  parseOpts.parseShdr = true;
    parseOpts.parseFont = true;  parseOpts.parseTmln = true;  parseOpts.parseObjt = true;
    parseOpts.parseRoom = true;  parseOpts.parseTpag = true;  parseOpts.parseCode = true;
    parseOpts.parseVari = true;  parseOpts.parseFunc = true;  parseOpts.parseStrg = true;
    parseOpts.parseTxtr = true;  parseOpts.parseAudo = true;
    parseOpts.skipLoadingPreciseMasksForNonPreciseSprites = true;
    parseOpts.progressCallback = dataWinParseProgress;
    parseOpts.progressCallbackUserData = loadingOk ? &gLoadingScreen : NULL;
    DataWin* dataWin = parseDataWinGuarded(dataWinPath, parseOpts);

    if (!dataWin) {
        diagLog("Butterscotch: FATAL: DataWin_parse returned NULL");
		drawFatalErrorScreen(&gLoadingScreen);
        Butterscotch_xdkHang();
    }
    diagLog("Butterscotch: (07) data.win parsed OK");
    if (loadingOk) {
        loadingDraw(&gLoadingScreen, 1.0f, "data.win loaded");
        loadingDestroy(&gLoadingScreen);
        loadingOk = false;
    }
    if (diagOverlayInit(pd3dDevice, dataWinPath)) {
        diagLog("DIAG: overlay renderer ready; toggle with LB+RB");
    } else {
        diagLog("DIAG: overlay renderer unavailable");
    }

    diagLog("Butterscotch: game=%s", dataWin->gen8.displayName ? dataWin->gen8.displayName : "Unknown");

    // ===[ Load CONFIG.JSN (optional) ]===
    char configPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        memcpy(configPath, dataWinPath, dirLen);
        sprintf(configPath + dirLen, "CONFIG.JSN");
    } else {
        strcpy(configPath, "CONFIG.JSN");
    }

    JsonValue* configRoot = NULL;
    HANDLE hConfig = CreateFileA(configPath, GENERIC_READ, FILE_SHARE_READ,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hConfig != INVALID_HANDLE_VALUE) {
        DWORD configSize = GetFileSize(hConfig, NULL);
        char* configText = (char*)malloc(configSize + 1);
        DWORD bytesRead;
        ReadFile(hConfig, configText, configSize, &bytesRead, NULL);
        CloseHandle(hConfig);
        configText[bytesRead] = '\0';
        configRoot = JsonReader_parse(configText);
        free(configText);
        diagLog("Butterscotch: Loaded CONFIG.JSN");
    }

    // ===[ Create Subsystems ]===
    diagLog("Butterscotch: (08) creating subsystems");
    XdkFileSystem* xdkFs = XdkFileSystem_create(dataWinPath);
    FileSystem* fileSystem = (FileSystem*)xdkFs;

    diagLog("Butterscotch: (09) creating renderer");
    renderer = D3D9Renderer_create(pd3dDevice);

	#ifdef USE_XAUDIO2_AUDIO
    diagLog("Butterscotch: (10) creating audio");
    XdkAudioSystem* xdkAudio = XdkAudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*)xdkAudio;
	#else
	diagLog("Butterscotch: (10) creating audio (noop)");
	NoopAudioSystem* noopAudio = NoopAudioSystem_create();
	AudioSystem* audioSystem = (AudioSystem*)noopAudio;
	#endif

    diagLog("Butterscotch: (11) creating VM");
    VMContext* vm = VM_create(dataWin);
    diagLog("Butterscotch: (12) creating runner");
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem);
    runner->getWindowSize = xdkGetWindowSize;
    runner->setWindowSize = xdkSetWindowSize;
    runner->osType = OS_WINDOWS;
    diagLog("Butterscotch: runner created with renderer/audio initialized");

    // Parse CONFIG.JSN options
	// Commented because:
	// Z:\hdd\Butterscotch\src\xbox360-xdk\main.cpp(1018) : error C3861: 'JsonReader_getObject': identifier not found
	// Z:\hdd\Butterscotch\src\xbox360-xdk\main.cpp(1029) : error C3861: 'JsonReader_getObject': identifier not found
	// Z:\hdd\Butterscotch\src\xbox360-xdk\main.cpp(1042) : error C3861: 'JsonReader_getObject': identifier not found
	// Z:\hdd\Butterscotch\src\xbox360-xdk\main.cpp(1047) : error C3861: 'JsonReader_getObjectKey': identifier not found
	// Z:\hdd\Butterscotch\src\xbox360-xdk\main.cpp(1048) : error C3861: 'JsonReader_getObjectValue': identifier not found
	// Z:\hdd\Butterscotch\src\xbox360-xdk\main.cpp(1054) : error C3861: 'JsonReader_getObject': identifier not found
	// Z:\hdd\Butterscotch\src\xbox360-xdk\main.cpp(1059) : error C3861: 'JsonReader_getObject': identifier not found
	// Z:\hdd\Butterscotch\src\xbox360-xdk\main.cpp(1107) : error C3861: 'JsonReader_getObject': identifier not found
    // if (configRoot) {
    //     JsonValue* osTypeVal = JsonReader_getObject(configRoot, "osType");
    //     if (osTypeVal && JsonReader_isString(osTypeVal)) {
    //         YoYoOperatingSystem configuredOsType;
    //         const char* osText = JsonReader_getString(osTypeVal);
    //         if (parseOsTypeName(osText, &configuredOsType)) {
    //             runner->osType = configuredOsType;
    //         } else {
    //             diagLog("CONFIG.JSN: unknown osType '%s', keeping %s", osText, osTypeName(runner->osType));
    //         }
    //     }

    //     JsonValue* disabledArr = JsonReader_getObject(configRoot, "disabledObjects");
    //     if (disabledArr && JsonReader_isArray(disabledArr)) {
    //         sh_new_strdup(runner->disabledObjects);
    //         int count = JsonReader_arrayLength(disabledArr);
    //         for (int i = 0; i < count; i++) {
    //             JsonValue* elem = JsonReader_getArrayElement(disabledArr, i);
    //             if (elem && JsonReader_isString(elem)) {
    //                 const char* name = JsonReader_getString(elem);
    //                 shput(runner->disabledObjects, name, 1);
    //             }
    //         }
    //     }

    //     JsonValue* mappingsObj = JsonReader_getObject(configRoot, "controllerMappings");
    //     if (mappingsObj && JsonReader_isObject(mappingsObj)) {
    //         xpadMappingCount = JsonReader_objectLength(mappingsObj);
    //         xpadMappings = (XpadMapping*)malloc(sizeof(XpadMapping) * xpadMappingCount);
    //         for (int i = 0; i < xpadMappingCount; i++) {
    //             const char* btnStr = JsonReader_getObjectKey(mappingsObj, i);
    //             JsonValue* gmlVal = JsonReader_getObjectValue(mappingsObj, i);
    //             xpadMappings[i].xpadButton = (WORD)atoi(btnStr);
    //             xpadMappings[i].gmlKey = (int32_t)JsonReader_getInt(gmlVal);
    //         }
    //     }

    //     JsonValue* gamepadApiVal = JsonReader_getObject(configRoot, "gamepadApi");
    //     if (gamepadApiVal) {
    //         if (JsonReader_isBool(gamepadApiVal)) {
    //             gamepadApiEnabled = JsonReader_getBool(gamepadApiVal);
    //         } else if (JsonReader_isObject(gamepadApiVal)) {
    //             JsonValue* enabledVal = JsonReader_getObject(gamepadApiVal, "enabled");
    //             if (enabledVal) gamepadApiEnabled = JsonReader_getBool(enabledVal);
    //         }
    //     }
    // }

    if (!xpadMappings) setupDefaultMappings();
    diagLog("Butterscotch: osType=%s (%d)", osTypeName(runner->osType), (int)runner->osType);
    diagLog("Butterscotch: gamepadApi=%s", gamepadApiEnabled ? "enabled" : "disabled");

    diagLog("Butterscotch: (13) audio OK");
    diagLog("Butterscotch: (14) renderer already initialized by Runner_create");
    diagLog("Butterscotch: (15) renderer OK");

    // Initialize first room
    diagLog("Butterscotch: (16) init first room");
    if (dataWin->gen8.roomOrderCount > 0) {
        int32_t firstRoomIndex = dataWin->gen8.roomOrder[0];
        if (firstRoomIndex >= 0 && dataWin->room.count > (uint32_t)firstRoomIndex) {
            Room* firstRoom = &dataWin->room.rooms[firstRoomIndex];
            diagLog("Butterscotch: first room idx=%d name=%s size=%ux%u objects=%u layers=%u tiles=%u",
                firstRoomIndex,
                firstRoom->name ? firstRoom->name : "(null)",
                firstRoom->width,
                firstRoom->height,
                firstRoom->gameObjectCount,
                firstRoom->layerCount,
                firstRoom->tileCount);
        } else {
            diagLog("Butterscotch: first room index out of range idx=%d roomCount=%u",
                firstRoomIndex, dataWin->room.count);
        }
    } else {
        diagLog("Butterscotch: no room order entries");
    }
    if (!initFirstRoomGuarded(runner)) {
		diagLog("Butterscotch: FATAL: !initFirstRoomGuarded(runner)");
		drawFatalErrorScreen(&gLoadingScreen);
        Butterscotch_xdkHang();
    }
    diagLog("Butterscotch: (17) first room OK");

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t)gen8->defaultWindowWidth;
    int32_t gameH = (int32_t)gen8->defaultWindowHeight;
	gGameW = &gameW;
	gGameH = &gameH;
    diagLog("Butterscotch: gameW=%d gameH=%d screenW=%d screenH=%d", gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);

    // Parse deferDrawToAfterAllSteps
    bool deferDraw = false;
    // if (configRoot) {
    //     JsonValue* deferVal = JsonReader_getObject(configRoot, "deferDrawToAfterAllSteps");
    //     if (deferVal) deferDraw = JsonReader_getBool(deferVal);
    // }

    diagLog("Butterscotch: (18) entering main loop");

    // ===[ Main Loop ]===
    LARGE_INTEGER freq, lastTime, currentTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);
    LARGE_INTEGER startTime = lastTime;
    LARGE_INTEGER lastHeartbeatTime = lastTime;
    LARGE_INTEGER lastDiagSystemPollTime = lastTime;
    double accumulator = 0.0;
    uint32_t heartbeatFrame = 0;
    int32_t lastRoomId = runner->currentRoomIndex;
    diagOverlayPollSystem();

    while (!runner->shouldExit) {
		if (runner->pendingWorkingDirectory != nullptr && runner->pendingLaunchParameters != nullptr) {
            // Break from the game loop, we'll handle this later
            break;
        }

        QueryPerformanceCounter(&currentTime);
        double deltaTime = (double)(currentTime.QuadPart - lastTime.QuadPart) / (double)freq.QuadPart;
        lastTime = currentTime;

        // ===[ Poll Controller ]===
        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(state));
        DWORD xinputResult = XInputGetState(0, &state);
        WORD buttons = 0;
        bool controllerConnected = xinputResult == ERROR_SUCCESS;
        gDiagControllerConnected = controllerConnected ? 1 : 0;
        if (controllerConnected) {
            buttons = state.Gamepad.wButtons;

            // Left thumbstick as dpad
            #define STICK_DEADZONE 16384
            SHORT lx = state.Gamepad.sThumbLX;
            SHORT ly = state.Gamepad.sThumbLY;
            if (lx < -STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (lx >  STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (ly >  STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_UP;
            if (ly < -STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        if (gamepadApiEnabled) {
            pollGamepadApi(runner, &state, buttons, controllerConnected);
        }

        bool diagComboDown = controllerConnected &&
            ((buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0) &&
            ((buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
        if (diagComboDown && !gDiagOverlayComboWasDown) {
            gDiagOverlayVisible = !gDiagOverlayVisible;
            diagLog("DIAG: overlay %s", gDiagOverlayVisible ? "on" : "off");
        }
        gDiagOverlayComboWasDown = diagComboDown;

        for (int i = 0; i < xpadMappingCount; i++) {
            WORD mask = xpadMappings[i].xpadButton;
            int32_t gmlKey = xpadMappings[i].gmlKey;

            bool wasPressed = (prevButtons & mask) != 0;
            bool isPressed = (buttons & mask) != 0;

            if (isPressed && !wasPressed)
                RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
            else if (!isPressed && wasPressed)
                RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
        }
        prevButtons = buttons;
        prevRightTrigger = controllerConnected ? state.Gamepad.bRightTrigger : 0;

        bool speedCapRemoved = prevRightTrigger > 128;
        gDiagSpeedCapRemoved = speedCapRemoved ? 1 : 0;

        // ===[ Frame Pacing ]===
        uint32_t roomSpeed = runner->currentRoom->speed;
        double targetFrameTime = (roomSpeed > 0) ? (1.0 / roomSpeed) : (1.0 / 60.0);

        if (deltaTime > targetFrameTime * 2.0) {
            accumulator = targetFrameTime;
            diagLog("TIMING: dropped catch-up dt=%.3f target=%.3f room=%d",
                deltaTime, targetFrameTime, runner->currentRoom ? runner->currentRoomIndex : -1);
        } else {
            accumulator += deltaTime;
            double maxAccumulator = targetFrameTime * 2.0;
            if (accumulator > maxAccumulator) accumulator = maxAccumulator;
        }
        if (speedCapRemoved && targetFrameTime > accumulator) accumulator = targetFrameTime;

        int gameFramesRan = 0;
        bool heldRoomTransitionFrame = false;
        while (accumulator >= targetFrameTime) {
            if (gameFramesRan > 0)
                RunnerKeyboard_beginFrame(runner->keyboard);

            Runner_step(runner);
            bool roomChangedThisStep = false;
            if (runner->currentRoom && runner->currentRoomIndex != lastRoomId) {
                lastRoomId = runner->currentRoomIndex;
                gDiagRoomAgeFrames = 0;
                roomChangedThisStep = true;
                diagLog("ROOM_CHANGED id=%d name=%s", lastRoomId, runner->currentRoom->name ? runner->currentRoom->name : "(null)");
                #ifdef USE_XAUDIO2_AUDIO
				XdkAudioSystem_onRoomChanged(runner->audioSystem, lastRoomId, runner->currentRoom->name);
				#endif
            }

            if (!deferDraw) {
                if (roomChangedThisStep && runner->appSurfaceKeepWindowSize) {
                    heldRoomTransitionFrame = true;
                    gDiagRoomTransitionHolds++;
                } else {
                    drawRunnerFrame(runner, renderer, gameW, gameH);
                }
            }

            accumulator -= targetFrameTime;
            gameFramesRan++;
        }

        // Deferred draw: render once after all catch-up steps
        if (deferDraw && gameFramesRan > 0 && !(heldRoomTransitionFrame && runner->appSurfaceKeepWindowSize)) {
            drawRunnerFrame(runner, renderer, gameW, gameH);
        }

        // Update audio
        if (runner->audioSystem) {
            float dt = (float)deltaTime;
            if (dt < 0.0f) dt = 0.0f;
            if (dt > 0.1f) dt = 0.1f;
            runner->audioSystem->vtable->update(runner->audioSystem, dt);
        }

        if (gameFramesRan > 0) {
            gDiagRoomAgeFrames += (uint32_t)gameFramesRan;
            double elapsedForDiag = (double)(currentTime.QuadPart - startTime.QuadPart) / (double)freq.QuadPart;
            if (gDiagOverlayWindowStart <= 0.0) gDiagOverlayWindowStart = elapsedForDiag;
            gDiagOverlayFrameCount += (uint32_t)gameFramesRan;
            gDiagOverlayDtMs = (float)(deltaTime * 1000.0);
            gDiagOverlaySteps = gameFramesRan;
            double diagElapsed = elapsedForDiag - gDiagOverlayWindowStart;
            if (diagElapsed >= 0.5) {
                gDiagOverlayFps = diagElapsed > 0.0 ? (float)((double)gDiagOverlayFrameCount / diagElapsed) : 0.0f;
                gDiagOverlayFrameCount = 0;
                gDiagOverlayWindowStart = elapsedForDiag;
            }
            double sysPollElapsed = (double)(currentTime.QuadPart - lastDiagSystemPollTime.QuadPart) / (double)freq.QuadPart;
            if (sysPollElapsed >= 1.0) {
                diagOverlayPollSystem();
                lastDiagSystemPollTime = currentTime;
            }

            heartbeatFrame += (uint32_t) gameFramesRan;
            if ((heartbeatFrame % 120) < (uint32_t) gameFramesRan) {
                double elapsed = (double)(currentTime.QuadPart - startTime.QuadPart) / (double)freq.QuadPart;
                double hbElapsed = (double)(currentTime.QuadPart - lastHeartbeatTime.QuadPart) / (double)freq.QuadPart;
                double hbFps = hbElapsed > 0.0 ? 120.0 / hbElapsed : 0.0;
                lastHeartbeatTime = currentTime;
                diagLog("HB frame %u t=%.3f dt120=%.3f fps=%.2f room=%d speed=%u pending=%d inst=%d", heartbeatFrame, elapsed, hbElapsed, hbFps, runner->currentRoom ? runner->currentRoomIndex : -1, runner->currentRoom ? runner->currentRoom->speed : 0, runner->pendingRoom, (int32_t) arrlen(runner->instances));
            }
            RunnerKeyboard_beginFrame(runner->keyboard);
        }
    }

    char* nextWorkingDirectory = nullptr;
    char* nextLaunchParameters = nullptr;
    if (runner->pendingWorkingDirectory != nullptr && runner->pendingLaunchParameters != nullptr) {
        diagLog("Butterscotch: game_change requested, exiting main loop to restart with new working directory '%s' and launch parameters '%s'", runner->pendingWorkingDirectory, runner->pendingLaunchParameters);
        // Snapshot any pending game_change request before we tear the runner down
        nextWorkingDirectory = (char*)safeMalloc(strlen(runner->pendingWorkingDirectory) + 1);
        nextLaunchParameters = (char*)safeMalloc(strlen(runner->pendingLaunchParameters) + 1);
        strcpy(nextWorkingDirectory, runner->pendingWorkingDirectory);
        strcpy(nextLaunchParameters, runner->pendingLaunchParameters);
        free(runner->pendingWorkingDirectory);
        free(runner->pendingLaunchParameters);
        runner->pendingWorkingDirectory = nullptr;
        runner->pendingLaunchParameters = nullptr;
    }

    // ===[ Cleanup ]===
    // Free subsystems in reverse creation order.
    // Destroy audio before Runner_free since runner owns the audioSystem pointer reference.
    if (runner->audioSystem) {
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = NULL;
    }
    Runner_free(runner);
    VM_free(vm);
    XdkFileSystem_destroy(fileSystem);
    renderer->vtable->destroy(renderer);
    if (configRoot) JsonReader_free(configRoot);
    loadingDestroy(&gDiagOverlayScreen);
    DataWin_free(dataWin);
    pd3dDevice->Release();
    pD3D->Release();

    free(xpadMappings);

    diagLog("Butterscotch: game_change end nextWD=%p nextLP=%p dataWinPath=%s",
        nextWorkingDirectory, nextLaunchParameters, dataWinPath);


    // game_change was called: compute the new data.win path and restart.
    // Instead of attempting to remap the butterscotch:\ symlink (which
    // Xenia / the Xbox OS will NOT allow without a reboot), we construct
    // the *full* absolute path, e.g.  butterscotch:\chapter3\data.win,
    // using the previous data.win directory as the base.
    if (nextWorkingDirectory != nullptr && nextLaunchParameters != nullptr) {
        // Derive the directory of the *current* dataWinPath.  This is the
        // base that the GML game_change working directory is relative to.
        char currentDataWinDir[512];
        const char* lastSlash = strrchr(dataWinPath, '\\');
        if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
        if (lastSlash) {
            size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
            if (dirLen >= sizeof(currentDataWinDir)) dirLen = sizeof(currentDataWinDir) - 1;
            memcpy(currentDataWinDir, dataWinPath, dirLen);
            currentDataWinDir[dirLen] = '\0';
        } else {
            strcpy(currentDataWinDir, "butterscotch:\\");
        }

        // Build the full new data.win path by combining the base directory
        // and the working directory from game_change.
        char newDataWinPath[512];
        const char* wd = nextWorkingDirectory;
        // Normalise leading slash on the working directory (it typically
        // comes in as "/chapter3" — strip the leading / or \).
        while (*wd == '/' || *wd == '\\') wd++;

        if (wd[0] == '\0') {
            // game_change with an empty / root working directory — look
            // for data.win in the same place as before.
            snprintf(newDataWinPath, sizeof(newDataWinPath), "%sdata.win", currentDataWinDir);
        } else {
            snprintf(newDataWinPath, sizeof(newDataWinPath), "%s%s\\data.win", currentDataWinDir, wd);
        }
        // Normalise slashes
        for (char* p = newDataWinPath; *p; p++) {
            if (*p == '/') *p = '\\';
        }

        free(nextWorkingDirectory);
        free(nextLaunchParameters);

        diagLog("Butterscotch: game_change -> new data.win path: %s", newDataWinPath);

        // Close diagnostic log handles before restart
        if (gDiagFile) {
            fclose(gDiagFile);
            gDiagFile = NULL;
        }
        if (gDiagLog != INVALID_HANDLE_VALUE) {
            CloseHandle(gDiagLog);
            gDiagLog = INVALID_HANDLE_VALUE;
        }

        // Store the computed path in the file-scope gNextDataWinPath so the
        // data.win search at the top of main() can find it on the next call.
        strncpy(gNextDataWinPath, newDataWinPath, sizeof(gNextDataWinPath) - 1);
        gNextDataWinPath[sizeof(gNextDataWinPath) - 1] = '\0';

        // Recursively re-enter main().  The symlink is already set up;
        // CreateCustomDeviceLink will skip re-creation.
        main();
        return;
    }

    // Close diagnostic log handles
    if (gDiagFile) {
        diagLog("Butterscotch: Closing log file");
        fclose(gDiagFile);
        gDiagFile = NULL;
    }
    if (gDiagLog != INVALID_HANDLE_VALUE) {
        diagLog("Butterscotch: Closing log handle");
        CloseHandle(gDiagLog);
        gDiagLog = INVALID_HANDLE_VALUE;
    }
}