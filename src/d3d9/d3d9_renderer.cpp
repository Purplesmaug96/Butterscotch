#ifdef _WIN32
#include <windows.h>
#endif
#include <d3d9.h>
#ifdef PLATFORM_XBOX360_XDK
#include <xtl.h>
#include <d3dx9.h>
#include <xgraphics.h>
// VMX/AltiVec intrinsics for PowerPC 970
#include <ppcintrinsics.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <limits.h>
#include <algorithm>

#include <textures.h>

// Threading strategy:
// - Desktop/Windows: use std::thread/std::mutex/std::condition_variable
// - Xbox 360 XDK: use Win32 API (where available in headers) and/or XTL/xtl threading primitives.
//   Note: Xbox 360 toolchains typically do not ship with the full C++ <thread> implementation.

#ifndef PLATFORM_XBOX360_XDK
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <queue>
#endif

#ifndef PLATFORM_XBOX360_XDK
extern "C" {
#include "data_win.h"
}
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#include "d3d9_renderer.h"
#include "shader_loader.h"

#if !defined(PLATFORM_XBOX360_XDK)

#define __cdecl __attribute__((cdecl))
#define _vsnprintf vsnprintf
#define _snprintf snprintf

#define OutputDebugStringA(msg) printf("%s\n", msg)
#define GetFreeMemMB() 1024.0f

// Xbox 360 XDK Hardware Mappings
#define D3DFMT_LIN_A8R8G8B8 D3DFMT_A8R8G8B8
#ifdef D3D9_USE_16BIT_TEXTURES
#define D3DFMT_LIN_A4R4G4B4 D3DFMT_A4R4G4B4
#endif
#define D3DRESOLVE_RENDERTARGET0 0

// Legacy Windows D3DX Library Stubs
#define D3DX_FILTER_POINT 1
struct ID3DXBuffer {
	virtual HRESULT QueryInterface(const IID&, void**) = 0;
	virtual ULONG AddRef() = 0;
	virtual ULONG Release() = 0;
	virtual void* GetBufferPointer() = 0;
	virtual DWORD GetBufferSize() = 0;
};
inline HRESULT D3DXCompileShader(const char*, UINT, void*, void*, const char*, const char*, DWORD, ID3DXBuffer**, ID3DXBuffer**, void*) { return E_FAIL; }
inline HRESULT D3DXLoadSurfaceFromSurface(void*, void*, void*, void*, void*, void*, DWORD, DWORD) { return E_FAIL; }

static int32_t _gGameW;
static int32_t _gGameH;

static int32_t* gGameW = &_gGameW;
static int32_t* gGameH = &_gGameH;

#else

#ifdef D3DPOOL_MANAGED
#undef D3DPOOL_MANAGED
#endif
#define D3DPOOL_MANAGED D3DPOOL_DEFAULT

// #define D3DFMT_A8R8G3608B8 D3DFMT_LIN_A8R8G8B8

#endif

// Texture format selection for GPU memory reduction.
// Define D3D9_USE_16BIT_TEXTURES at build time to use D3DFMT_A4R4G4B4 (16-bit)
// instead of D3DFMT_A8R8G8B8 (32-bit), halving GPU texture memory usage.
// Staging/system-memory allocations remain 32-bit for upload compatibility.
#ifdef D3D9_USE_16BIT_TEXTURES
#define D3D9_GPU_TEXTURE_FORMAT D3DFMT_A4R4G4B4
#define D3D9_GPU_LINEAR_FORMAT D3DFMT_LIN_A4R4G4B4
#else
#define D3D9_GPU_TEXTURE_FORMAT D3DFMT_A8R8G8B8
#define D3D9_GPU_LINEAR_FORMAT D3DFMT_LIN_A8R8G8B8
#endif

float _offx = 0.0f;

#include "stb_ds.h"

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#ifndef PLATFORM_XBOX360_XDK
extern "C" {
#endif
#include "utils.h"
#include "text_utils.h"
#include "runner.h"
#include "image_decoder.h"
#ifndef PLATFORM_XBOX360_XDK
}
#endif

#ifndef PLATFORM_XBOX360_XDK
unsigned long __cdecl DbgPrint(const char* format, ...) {
	va_list args;
	int result;

	va_start(args, format);
	result = vprintf(format, args);
	va_end(args);

	return result;
}

void Butterscotch_xdkDiagTrace(const char* fmt, ...) {
	va_list args;

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);

	if (fmt[strlen(fmt) - 1] != '\n') {
		printf("\n");
	}
}
#endif

#include "stb_image.h"

#ifdef PLATFORM_XBOX360_XDK
extern "C" {
#endif
unsigned long __cdecl DbgPrint(const char* format, ...);
void Butterscotch_xdkDiagTrace(const char* fmt, ...);
#ifdef PLATFORM_XBOX360_XDK
}
#endif

using namespace std;

// ===[ Vertex Format ]===
// Uses FLOAT4 position (pre-transformed screen coords, z=0, w=1)
// and FLOAT4 color to avoid D3DCOLOR endianness issues on Xbox 360.
struct SpriteVertex {
	float x, y, z, w; // position (screen-space, z=0, w=1)
	float u, v;		  // texcoord
	float r, g, b, a; // color as floats
};

// ===[ HLSL Shader Source ]===
//
// Default vertex shader: transforms screen-space pixel coordinates to clip space
// using a uniform half-resolution (uHalfRes = gameW/2, gameH/2) so it works
// at any resolution without hardcoded dimensions.
// On Xbox 360, D3DRS_VIEWPORTENABLE=FALSE is used instead, so the shader is a
// simple pass-through.

#ifdef PLATFORM_XBOX360_XDK
// Vertex shader: simple pass-through for pre-transformed screen-space vertices.
// Position is already in screen pixels with z=0, w=1.
// With D3DRS_VIEWPORTENABLE=FALSE, the GPU uses these directly.
static const char* g_vsSource =
	"struct VS_IN  { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
	"struct VS_OUT { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
	"VS_OUT main(VS_IN i) {\n"
	"  VS_OUT o;\n"
	"  o.Pos = i.Pos;\n"
	"  o.Tex = i.Tex;\n"
	"  o.Col = i.Col;\n"
	"  return o;\n"
	"}\n";
#else
// Transforms screen-space pixel coordinates to clip space using a uniform
// half-resolution (uHalfRes = gameW/2, gameH/2) so it works at any resolution.
static const char* g_vsSource =
	"uniform float2 uHalfRes;\n"
	"struct VS_IN {\n"
	"    float4 Pos : POSITION;\n"
	"    float2 Tex : TEXCOORD0;\n"
	"    float4 Col : TEXCOORD1;\n"
	"};\n"
	"\n"
	"struct VS_OUT {\n"
	"    float4 Pos : POSITION;\n"
	"    float2 Tex : TEXCOORD0;\n"
	"    float4 Col : TEXCOORD1;\n"
	"};\n"
	"\n"
	"VS_OUT main(VS_IN i) {\n"
	"    VS_OUT o;\n"
	"    o.Pos.x = (i.Pos.x / uHalfRes.x) - 1.0f;\n"
	"    o.Pos.y = 1.0f - (i.Pos.y / uHalfRes.y);\n"
	"    o.Pos.z = 0.0f;\n"
	"    o.Pos.w = 1.0f;\n"
	"    o.Tex = i.Tex;\n"
	"    o.Col = i.Col;\n"
	"    return o;\n"
	"}\n";
#endif

static const char* g_psSource =
	"sampler2D s0 : register(s0) = sampler_state {\n"
	"  MinFilter = POINT; MagFilter = POINT; MipFilter = POINT;\n"
	"  AddressU = CLAMP; AddressV = CLAMP;\n"
	"};\n"
	"uniform float4 uFogColor : register(c0);\n"
	"struct PS_IN { float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
	"float4 main(PS_IN i) : COLOR0 {\n"
	"  float4 c = tex2D(s0, i.Tex) * i.Col;\n"
	"  c.rgb = lerp(c.rgb, uFogColor.rgb, uFogColor.a);\n"
	"  return c;\n"
	"}\n";

// ===[ Helpers ]===

static inline void setVertex(SpriteVertex* sv, float px, float py, float tu, float tv,
							 float cr, float cg, float cb, float ca) {
	sv->x = px - 0.5f;
	sv->y = py - 0.5f;
	sv->z = 0.0f;
	sv->w = 1.0f;
	sv->u = tu;
	sv->v = tv;
	sv->r = cr;
	sv->g = cg;
	sv->b = cb;
	sv->a = ca;
}

static inline float texelStart(float pos, float textureSize) {
	return (pos + 0.5f) / textureSize;
}

static inline float texelEnd(float pos, float size, float textureSize) {
	return (pos + size - 0.5f) / textureSize;
}

static inline IDirect3DDevice9* Dev(D3D9Renderer* dr) {
	return (IDirect3DDevice9*)dr->pd3dDevice;
}

void setShaders(D3D9Renderer* dr, void* pVertexShader, void* pPixelShader) {
	IDirect3DDevice9* dev = Dev(dr);

	if (pVertexShader != dr->boundVertexShader) {
		dev->SetVertexShader((IDirect3DVertexShader9*)pVertexShader);
		dr->boundVertexShader = pVertexShader;
	}
	if (pPixelShader != dr->boundPixelShader) {
		dev->SetPixelShader((IDirect3DPixelShader9*)pPixelShader);
		dr->boundPixelShader = pPixelShader;
	}
}

// D3DRS_VIEWPORTENABLE is a 360-only extension, so this function is simply a no-op on desktop
#ifdef PLATFORM_XBOX360_XDK
void setViewportEnable(D3D9Renderer* dr, bool enableViewport) {
	IDirect3DDevice9* dev = Dev(dr);
	if (enableViewport != dr->boundViewportEnable) {
		dev->SetRenderState(D3DRS_VIEWPORTENABLE, enableViewport);
		dr->boundViewportEnable = enableViewport;
	}
}
#else
static inline void setViewportEnable(MAYBE_UNUSED D3D9Renderer* dr, MAYBE_UNUSED bool enableViewport) {}
#endif

static void d3d9DiagOnce(bool* flag, const char* fmt, ...) {
	if (*flag) {
		return;
	}
	*flag = true;

	char line[512];
	va_list args;
	va_start(args, fmt);
	_vsnprintf(line, sizeof(line) - 1, fmt, args);
	va_end(args);
	line[sizeof(line) - 1] = '\0';
	Butterscotch_xdkDiagTrace("%s", line);
}

static void d3d9DiagLimited(int* counter, int limit, const char* fmt, ...) {
	if (*counter >= limit) {
		return;
	}
	(*counter)++;

	char line[512];
	va_list args;
	va_start(args, fmt);
	_vsnprintf(line, sizeof(line) - 1, fmt, args);
	va_end(args);
	line[sizeof(line) - 1] = '\0';
	Butterscotch_xdkDiagTrace("%s", line);
}

#define D3D9_DIAG_LIMITED(limit, fmt, ...)                                                           \
	do {                                                                                             \
		static int _d3d9_diag_limited_counter_line_##__LINE__ = 0;                                   \
		(void)_d3d9_diag_limited_counter_line_##__LINE__;                                            \
		d3d9DiagLimited(&_d3d9_diag_limited_counter_line_##__LINE__, (limit), (fmt), ##__VA_ARGS__); \
	} while (0)

// Forward declaration for staging texture cleanup
void d3d9ReleaseStagingTexture(void);

// File-level staging texture cache for Xbox 360 texture uploads.
// Defined here and released at renderer destruction via d3d9ReleaseStagingTexture().
#ifdef PLATFORM_XBOX360_XDK
static IDirect3DTexture9* gStagingTex = nullptr;
static int32_t gStagingW = 0;
static int32_t gStagingH = 0;

void d3d9ReleaseStagingTexture(void) {
	if (gStagingTex) {
		gStagingTex->Release();
		gStagingTex = nullptr;
	}
	gStagingW = 0;
	gStagingH = 0;
}
#endif

static DWORD gmlBlendFactorToD3D(int32_t factor) {
	switch (factor) {
	case bm_zero:
		return D3DBLEND_ZERO;
	case bm_one:
		return D3DBLEND_ONE;
	case bm_src_color:
		return D3DBLEND_SRCCOLOR;
	case bm_inv_src_color:
		return D3DBLEND_INVSRCCOLOR;
	case bm_src_alpha:
		return D3DBLEND_SRCALPHA;
	case bm_inv_src_alpha:
		return D3DBLEND_INVSRCALPHA;
	case bm_dest_alpha:
		return D3DBLEND_DESTALPHA;
	case bm_inv_dest_alpha:
		return D3DBLEND_INVDESTALPHA;
	case bm_dest_color:
		return D3DBLEND_DESTCOLOR;
	case bm_inv_dest_color:
		return D3DBLEND_INVDESTCOLOR;
	case bm_src_alpha_sat:
		return D3DBLEND_SRCALPHASAT;
	default:
		return D3DBLEND_ONE;
	}
}

static void d3d9SetNormalBlend(IDirect3DDevice9* dev, D3D9Renderer* dr) {
	if (dr && dr->blendIsNormal) {
		return;
	}
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	if (dr) {
		dr->blendIsNormal = true;
	}
}

static void resetFullBackbufferState(D3D9Renderer* dr) {
	IDirect3DDevice9* dev = Dev(dr);
	D3DVIEWPORT9 vp;
	vp.X = 0;
	vp.Y = 0;
	vp.Width = (DWORD)dr->screenW;
	vp.Height = (DWORD)dr->screenH;
	vp.MinZ = 0.0f;
	vp.MaxZ = 1.0f;
	dev->SetViewport(&vp);
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
}

static void setGameTargetTransform(D3D9Renderer* dr) {
	dr->renderStateDirty = true;
	dr->offsetX = 0.0f;
	dr->offsetY = 0.0f;
	dr->portScaleX = dr->renderScale;
	dr->portScaleY = dr->renderScale;
	dr->portOffsetX = dr->renderOffsetX;
	dr->portOffsetY = dr->renderOffsetY;
}

static void setWindowSurfaceTransform(D3D9Renderer* dr) {
	// Use uniform scale to preserve the application surface's aspect ratio
	// within the fixed 720p backbuffer. This prevents stretching when the
	// app surface aspect differs from the screen aspect (e.g., 4:3 game
	// content or widescreen mod application surface on a 16:9 display).
	dr->renderStateDirty = true;
	float scaleX = (dr->appSurfaceW > 0) ? ((float)dr->screenW / (float)dr->appSurfaceW) : 1.0f;
	float scaleY = (dr->appSurfaceH > 0) ? ((float)dr->screenH / (float)dr->appSurfaceH) : 1.0f;
	float uniformScale = (scaleX < scaleY) ? scaleX : scaleY;
	dr->offsetX = _offx + 0.0f;
	dr->offsetY = 0.0f;
	dr->portScaleX = uniformScale;
	dr->portScaleY = uniformScale;
	dr->portOffsetX = _offx + ((float)dr->screenW - (float)dr->appSurfaceW * uniformScale) * 0.5f;
	dr->portOffsetY = ((float)dr->screenH - (float)dr->appSurfaceH * uniformScale) * 0.5f;
}

static void setApplicationSurfaceTransform(D3D9Renderer* dr) {
	dr->renderStateDirty = true;
	dr->offsetX = _offx + 0.0f;
	dr->offsetY = 0.0f;
	dr->portScaleX = 1.0f;
	dr->portScaleY = 1.0f;
	dr->portOffsetX = _offx + 0.0f;
	dr->portOffsetY = 0.0f;
}

static void releaseApplicationSurface(D3D9Renderer* dr) {
	if (dr->appSurfaceLevel) {
		((IDirect3DSurface9*)dr->appSurfaceLevel)->Release();
		dr->appSurfaceLevel = nullptr;
	}
	if (dr->appRenderTexture) {
		((IDirect3DTexture9*)dr->appRenderTexture)->Release();
		dr->appRenderTexture = nullptr;
	}
	if (dr->appSurfaceTexture) {
		((IDirect3DTexture9*)dr->appSurfaceTexture)->Release();
		dr->appSurfaceTexture = nullptr;
	}
	dr->appSurfaceW = 0;
	dr->appSurfaceH = 0;
	dr->appSurfaceAllocW = 0;
	dr->appSurfaceAllocH = 0;
	dr->appSurfaceResolved = false;
}

static bool bindBackbuffer(D3D9Renderer* dr) {
	IDirect3DDevice9* dev = Dev(dr);
	IDirect3DSurface9* backbuffer = nullptr;
	HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
	if (FAILED(hr) || !backbuffer) {
		Butterscotch_xdkDiagTrace("D3D9: GetBackBuffer failed hr=0x%08X", (unsigned)hr);
		return false;
	}
	hr = dev->SetRenderTarget(0, backbuffer);
	backbuffer->Release();
	if (FAILED(hr)) {
		Butterscotch_xdkDiagTrace("D3D9: SetRenderTarget(backbuffer) failed hr=0x%08X", (unsigned)hr);
		return false;
	}
	return true;
}

static void resolveApplicationSurface(D3D9Renderer* dr) {
	if (!dr->appSurfaceTexture || dr->appSurfaceW <= 0 || dr->appSurfaceH <= 0) {
		return;
	}
	if (dr->appSurfaceResolved) {
		return;
	}

#ifdef PLATFORM_XBOX360_XDK
	IDirect3DDevice9* dev = Dev(dr);
	dev->Resolve(D3DRESOLVE_RENDERTARGET0, nullptr,
				 (IDirect3DBaseTexture9*)dr->appSurfaceTexture,
				 nullptr, 0, 0, nullptr, 1.0f, 0, nullptr);
#endif

	dr->appSurfaceResolved = true;
}

static void applyPointSampling(IDirect3DDevice9* dev, D3D9Renderer* dr) {
	// Skip if already applied this frame (unless forced by render target switch)
	if (dr && dr->samplerStateApplied) {
		return;
	}
	// Xbox 360 only has 4 texture samplers; desktop D3D9 has up to 8.
	// Only loop over the actual hardware limit to avoid unnecessary API calls.
#ifdef PLATFORM_XBOX360_XDK
	const DWORD kMaxSamplers = 4;
#else
	const DWORD kMaxSamplers = 8;
#endif
	for (DWORD sampler = 0; sampler < kMaxSamplers; sampler++) {
		dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
		dev->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	}
	if (dr) {
		dr->samplerStateApplied = true;
	}
}

static void d3d9EnsureSharedRenderState(D3D9Renderer* dr) {
	if (!dr || !dr->pd3dDevice) {
		return;
	}
	if (!dr->renderStateDirty) {
		return;
	}

	IDirect3DDevice9* dev = Dev(dr);

	// Set shared render state that should only be applied once per frame
	// (unless something else dirties it).
	// Use setShaders() to avoid redundant SetVertexShader/SetPixelShader calls.
	setShaders(dr, dr->pVertexShader, dr->pPixelShader);
	dev->SetVertexDeclaration((IDirect3DVertexDeclaration9*)dr->pVertexDecl);

	// Set the uHalfRes uniform for the default vertex shader
	float halfRes[2] = { (float)dr->gameW * 0.5f, (float)dr->gameH * 0.5f };
	dev->SetVertexShaderConstantF(0, halfRes, 1);

	// Alpha blending
	d3d9SetNormalBlend(dev, dr);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE,
						D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
							D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);

	// No depth testing for 2D
	dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

	// Point filtering
	applyPointSampling(dev, dr);

	dr->renderStateDirty = false;
}

// Convert Butterscotch BGR color + alpha to float RGBA
// Uses precomputed reciprocal of 255.0 to avoid 3 FP divides per call.
// The Xbox 360 XDK compiler may not optimize /255.0f to *0.0039215689f,
// so use explicit multiply which is faster on PPC970.
static inline void bgrToFloatColor(uint32_t bgr, float alpha, float* outR, float* outG, float* outB, float* outA) {
#ifdef PLATFORM_XBOX360_XDK
#define kInv255 0.003921568627450980f
	// static const float kInv255 = 1.0f / 255.0f;
	*outR = (float)(bgr & 0xFF) * kInv255;
	*outG = (float)((bgr >> 8) & 0xFF) * kInv255;
	*outB = (float)((bgr >> 16) & 0xFF) * kInv255;
#else
	*outR = (float)(bgr & 0xFF) / 255.0f;
	*outG = (float)((bgr >> 8) & 0xFF) / 255.0f;
	*outB = (float)((bgr >> 16) & 0xFF) / 255.0f;
#endif
	*outA = alpha;
}

#ifdef PLATFORM_XBOX360_XDK
// Xbox 360 tiled texture tile sizes.
// D3DFMT_A8R8G8B8 uses 32-byte pitch tiles with 16 rows = 512 bytes per tile.
// At 4 bytes/pixel, this corresponds to 32x16 pixels per tile.
// Textures uploaded to the tiled GPU format should be allocated on these
// boundaries to avoid partial-tile corruption.
#define X360_TILE_W 32
#define X360_TILE_H 16
#define X360_TILE_BYTES 512

static inline uint32_t alignToTile(uint32_t val, uint32_t tileSize) {
	return (val + tileSize - 1) & ~(tileSize - 1);
}
#endif

// Write a pixel in D3DFMT_A8R8G8B8 format.
// On Xbox 360 the tiled texture path expects the bytes laid out as [B,G,R,A]
// in the temporary upload buffer before the runtime swizzles it into GPU memory.
static inline void writePixelBGRA(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
#ifdef PLATFORM_XBOX360_XDK
	dst[0] = b;
	dst[1] = g;
	dst[2] = r;
	dst[3] = a;
#else
	*(DWORD*)dst = D3DCOLOR_ARGB(a, r, g, b);
#endif
}

static inline void writeLinearPixelARGB(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	*(DWORD*)dst = D3DCOLOR_ARGB(a, r, g, b);
}

static bool uploadRgbaToTexture(IDirect3DDevice9* dev, IDirect3DTexture9* dstTex,
								const uint8_t* pixels, int32_t w, int32_t h) {
	if (!dev || !dstTex || !pixels || w <= 0 || h <= 0) {
		return false;
	}

	// Xbox 360 performance: avoid allocating a new systemmem staging texture
	// per upload. Instead, reuse a single staging texture owned by the
	// renderer/device for the most recently seen size.
#ifdef PLATFORM_XBOX360_XDK
	D3DSURFACE_DESC dstDesc;
	HRESULT hr = dstTex->GetLevelDesc(0, &dstDesc);
	if (FAILED(hr)) {
		return false;
	}

	int32_t targetW = (int32_t)dstDesc.Width;
	int32_t targetH = (int32_t)dstDesc.Height;

	if (targetW <= 0 || targetH <= 0) {
		return false;
	}

	if (!gStagingTex || gStagingW != targetW || gStagingH != targetH) {
		if (gStagingTex) {
			gStagingTex->Release();
			gStagingTex = nullptr;
		}

		HRESULT hrCreate = dev->CreateTexture((UINT)targetW, (UINT)targetH, 1, 0,
											  D3DFMT_LIN_A8R8G8B8, D3DPOOL_SYSTEMMEM, &gStagingTex, nullptr);
		if (FAILED(hrCreate) || !gStagingTex) {
			return false;
		}
		gStagingW = targetW;
		gStagingH = targetH;
	}

	// Fill the cached staging texture.
	D3DLOCKED_RECT lr;
	hr = gStagingTex->LockRect(0, &lr, nullptr, 0);
	if (FAILED(hr)) {
		return false;
	}

	// Clear only the active rows we will write to.
	// This avoids touching the full staging allocation when the requested
	// texture size is smaller than the cached staging size.
	{
		const size_t rowBytes = (size_t)w * 4;
		for (int32_t y = 0; y < h; y++) {
			memset((uint8_t*)lr.pBits + (size_t)y * (size_t)lr.Pitch, 0, rowBytes);
		}
	}

#ifdef PLATFORM_XBOX360_XDK
	// XDK VMX/AltiVec accelerated inner loop for RGBA -> ARGB conversion.
	// Processes 4 pixels (16 bytes) per iteration using __vperm byte permutation.
	// On PPC 970, __vperm has 2-cycle latency vs ~12 scalar byte ops per pixel.
	//
	// The permute control vector maps byte positions from the source (RGBA) to
	// the destination (ARGB). For big-endian memory layout where each 4-byte word
	// is R,G,B,A (byte 0=R, 1=G, 2=B, 3=A), the permute indices to get
	// A,R,G,B per pixel are: {3,0,1,2} per group = {3,0,1,2, 7,4,5,6, 11,8,9,10, 15,12,13,14}
	__declspec(align(16)) static const uint32_t sPermARGB[4] = {
		0x03000102, 0x07040506, 0x0B08090A, 0x0F0C0D0E
	};
	__vector4 vPerm = __lvx(sPermARGB, 0);
	__vector4 vZero = __vzero();

	for (int32_t y = 0; y < h; y++) {
		const uint8_t* src = pixels + y * (size_t)w * 4;
		uint8_t* dst = (uint8_t*)lr.pBits + (size_t)y * (size_t)lr.Pitch;
		int32_t x = 0;

		// Process 4 pixels (16 bytes) per VMX iteration
		for (; x + 3 < w; x += 4) {
			__vector4 v = __lvx(src, x * 4);	// Load 4 RGBA pixels
			__vector4 p = __vperm(v, v, vPerm); // Permute RGBA -> ARGB
			// Note: alpha==0 premultiply optimization is skipped in the SIMD path.
			// The scalar fallback still handles exact A==0 => RGB=0 semantics.
			__stvx(p, dst, x * 4); // Store 4 ARGB pixels
		}

		// Remaining pixels (scalar fallback)
		for (; x < w; x++) {
			uint8_t r = src[x * 4 + 0];
			uint8_t g = src[x * 4 + 1];
			uint8_t b = src[x * 4 + 2];
			uint8_t a = src[x * 4 + 3];
			if (a == 0) {
				r = 0;
				g = 0;
				b = 0;
			}
			writeLinearPixelARGB(dst + x * 4, r, g, b, a);
		}
	}
#else
	// Scalar fallback (desktop or no VMX)
	for (int32_t y = 0; y < h; y++) {
		const uint8_t* src = pixels + y * (size_t)w * 4;
		uint8_t* dst = (uint8_t*)lr.pBits + (size_t)y * (size_t)lr.Pitch;
		for (int32_t x = 0; x < w; x++) {
			uint8_t r = src[x * 4 + 0];
			uint8_t g = src[x * 4 + 1];
			uint8_t b = src[x * 4 + 2];
			uint8_t a = src[x * 4 + 3];
			if (a == 0) {
				r = 0;
				g = 0;
				b = 0;
			}
			writeLinearPixelARGB(dst + x * 4, r, g, b, a);
		}
	}
#endif

	gStagingTex->UnlockRect(0);

	IDirect3DSurface9* stagingSurf = nullptr;
	IDirect3DSurface9* dstSurf = nullptr;

	hr = gStagingTex->GetSurfaceLevel(0, &stagingSurf);
	if (FAILED(hr) || !stagingSurf) {
		return false;
	}

	hr = dstTex->GetSurfaceLevel(0, &dstSurf);
	if (FAILED(hr) || !dstSurf) {
		stagingSurf->Release();
		return false;
	}

	RECT srcRect = { 0, 0, w, h };
	RECT dstRect = { 0, 0, w, h };
	hr = D3DXLoadSurfaceFromSurface(dstSurf, nullptr, &dstRect,
									stagingSurf, nullptr, &srcRect,
									D3DX_FILTER_POINT, 0);

	dstSurf->Release();
	stagingSurf->Release();

	return SUCCEEDED(hr);
#else
	// Desktop/Windows: keep existing per-upload staging texture creation.
	D3DSURFACE_DESC desc;
	HRESULT hr = dstTex->GetLevelDesc(0, &desc);
	if (FAILED(hr)) {
		return false;
	}

	IDirect3DTexture9* stagingTex = nullptr;
	HRESULT hrCreate = dev->CreateTexture(desc.Width, desc.Height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &stagingTex, nullptr);
	if (FAILED(hrCreate) || !stagingTex) {
		return false;
	}

	D3DLOCKED_RECT lr;
	hr = stagingTex->LockRect(0, &lr, nullptr, 0);
	if (FAILED(hr)) {
		stagingTex->Release();
		return false;
	}

	memset(lr.pBits, 0, (size_t)lr.Pitch * desc.Height);
	for (int32_t y = 0; y < h; y++) {
		const uint8_t* src = pixels + y * (size_t)w * 4;
		uint8_t* dst = (uint8_t*)lr.pBits + (size_t)y * (size_t)lr.Pitch;
		for (int32_t x = 0; x < w; x++) {
			uint8_t r = src[x * 4 + 0];
			uint8_t g = src[x * 4 + 1];
			uint8_t b = src[x * 4 + 2];
			uint8_t a = src[x * 4 + 3];
			if (a == 0) {
				r = 0;
				g = 0;
				b = 0;
			}
			writePixelBGRA(dst + x * 4, r, g, b, a);
		}
	}

	stagingTex->UnlockRect(0);

	IDirect3DSurface9* stagingSurf = nullptr;
	IDirect3DSurface9* dstSurf = nullptr;
	hr = stagingTex->GetSurfaceLevel(0, &stagingSurf);
	if (SUCCEEDED(hr)) {
		hr = dstTex->GetSurfaceLevel(0, &dstSurf);
	}
	if (SUCCEEDED(hr)) {
		RECT srcRect = { 0, 0, w, h };
		RECT dstRect = { 0, 0, w, h };
		hr = D3DXLoadSurfaceFromSurface(dstSurf, nullptr, &dstRect,
										stagingSurf, nullptr, &srcRect,
										D3DX_FILTER_POINT, 0);
	}

	if (dstSurf) {
		dstSurf->Release();
	}
	if (stagingSurf) {
		stagingSurf->Release();
	}
	stagingTex->Release();
	return SUCCEEDED(hr);
#endif
}

// ===[ Batch Flush ]===

#ifdef PLATFORM_XBOX360_XDK
static void flushBatch(D3D9Renderer* dr) {
	// Cache batch counts locally to avoid multiple structure dereferences
	const uint32_t quadCount = dr->quadCount;
	const uint32_t triCount = dr->triCount;
	if (quadCount == 0 && triCount == 0) {
		return;
	}

	IDirect3DDevice9* dev = Dev(dr);
	Renderer* renderer = (Renderer*)dr;

	// Default to the base shaders to eliminate nested if/else branching
	void* targetVS = dr->pVertexShader;
	void* targetPS = dr->pPixelShader;
	bool targetViewport = false;

	const int32_t currentShader = renderer->currentShader;
	if (currentShader >= 0 && (uint32_t)currentShader < dr->gmlShaderCount) {
		D3D9GMLShader* shader = &dr->gmlShaders[currentShader];
		if (shader->compiled) {
			targetVS = shader->pVertexShader;
			targetPS = shader->pPixelShader;
			targetViewport = true;
		}
	}

	setShaders(dr, targetVS, targetPS);
	setViewportEnable(dr, targetViewport);

	void* desiredTex;
	int32_t bindIdx;

	if (dr->batchSurfaceTex) {
		// Surface texture override (set by d3d9DrawSurface)
		desiredTex = dr->batchSurfaceTex;
		bindIdx = -2;
		dr->batchSurfaceTex = nullptr;
		// Invalidate currentTextureIndex so the next ensureTexture forces a flush
		dr->currentTextureIndex = -1;
	} else {
		desiredTex = dr->whiteTexture;
		bindIdx = dr->currentTextureIndex;
		if (bindIdx >= 0 && (uint32_t)bindIdx < dr->textureCount) {
			void* tex = dr->textures[bindIdx];
			if (tex) {
				desiredTex = tex;
			}
		}
	}

	if (dr->boundTexturePtr != desiredTex) {
		dev->SetTexture(0, (IDirect3DBaseTexture9*)desiredTex);
		dr->boundTextureIndex = bindIdx;
		dr->boundTexturePtr = desiredTex;
	}

	// D3DPT_QUADLIST is an excellent Xbox 360 hardware extension that bypasses index buffers entirely.
	if (quadCount > 0) {
		dev->DrawPrimitiveUP(D3DPT_QUADLIST, quadCount, dr->vertexData, sizeof(SpriteVertex));
		dr->quadCount = 0;
	}

	// Submit any batched triangles (uses the area after quads in vertex buffer)
	if (triCount > 0) {
		const uint32_t triBase = D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD;
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)triCount,
							 (BYTE*)dr->vertexData + triBase * sizeof(SpriteVertex),
							 sizeof(SpriteVertex));
		dr->triCount = 0;
	}
}
#else
IDirect3DVertexDeclaration9* g_pVertexDecl = nullptr;
static void flushBatch(D3D9Renderer* dr) {
	const uint32_t quadCount = (uint32_t)dr->quadCount;
	const uint32_t triCount = (uint32_t)dr->triCount;
	if (quadCount == 0 && triCount == 0) {
		return;
	}

	// Prevent evicting/releasing a texture page while DXVK is drawing with DrawPrimitiveUP.
	std::mutex* gpuMutex = dr && dr->textureGpuMutex ? (std::mutex*)dr->textureGpuMutex : nullptr;
	std::unique_lock<std::mutex> gpuLock;
	if (gpuMutex) {
		gpuLock = std::unique_lock<std::mutex>(*gpuMutex);
	}

	IDirect3DDevice9* dev = Dev(dr);

	// Bind declaration
	dev->SetVertexDeclaration((IDirect3DVertexDeclaration9*)dr->pVertexDecl);

	// Determine which shaders to use based on current GML shader state
	Renderer* renderer = (Renderer*)dr;
	if (renderer->currentShader >= 0 && (uint32_t)renderer->currentShader < dr->gmlShaderCount) {
		D3D9GMLShader* shader = &dr->gmlShaders[renderer->currentShader];
		if (shader->compiled) {
			dev->SetVertexShader((IDirect3DVertexShader9*)shader->pVertexShader);
			dev->SetPixelShader((IDirect3DPixelShader9*)shader->pPixelShader);
		} else {
			dev->SetVertexShader((IDirect3DVertexShader9*)dr->pVertexShader);
			dev->SetPixelShader((IDirect3DPixelShader9*)dr->pPixelShader);
		}
	} else {
		dev->SetVertexShader((IDirect3DVertexShader9*)dr->pVertexShader);
		dev->SetPixelShader((IDirect3DPixelShader9*)dr->pPixelShader);
	}

	// Bind texture (skip redundant SetTexture calls)
	void* desiredTex = nullptr;
	if (dr->batchSurfaceTex) {
		desiredTex = dr->batchSurfaceTex;
		dr->batchSurfaceTex = nullptr;
		dr->currentTextureIndex = -1;
	} else if (dr->currentTextureIndex >= 0 && (uint32_t)dr->currentTextureIndex < dr->textureCount) {
		desiredTex = dr->textures[dr->currentTextureIndex];
	}
	if (!desiredTex) {
		desiredTex = dr->whiteTexture;
	}

	if (dr->boundTexturePtr != desiredTex) {
		dev->SetTexture(0, (IDirect3DBaseTexture9*)desiredTex);
		dr->boundTextureIndex = dr->currentTextureIndex;
		dr->boundTexturePtr = desiredTex;
	}

	// Submit all quads in a single indexed draw call instead of per-quad TRIANGLESTRIP.
	// Generate a temporary index buffer for quad->triangle conversion.
	// Each quad (4 vertices) maps to 2 triangles (6 indices): {0,1,2, 2,3,0}
	if (quadCount > 0) {
		uint32_t totalIndices = quadCount * 6;
		uint16_t* indices = (uint16_t*)safeMalloc(totalIndices * sizeof(uint16_t));
		if (indices) {
			for (uint32_t i = 0; i < quadCount; i++) {
				uint16_t base = (uint16_t)(i * 4);
				indices[i * 6 + 0] = base + 0;
				indices[i * 6 + 1] = base + 1;
				indices[i * 6 + 2] = base + 2;
				indices[i * 6 + 3] = base + 2;
				indices[i * 6 + 4] = base + 3;
				indices[i * 6 + 5] = base + 0;
			}
			uint32_t totalVerts = quadCount * 4;
			dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, totalVerts, quadCount * 2,
										indices, D3DFMT_INDEX16,
										dr->vertexData, sizeof(SpriteVertex));
			free(indices);
		}
		dr->quadCount = 0;
	}

	// Submit any batched triangles (uses the area after quads in vertex buffer)
	if (triCount > 0) {
		const uint32_t triBase = D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD;
		BYTE* triData = (BYTE*)dr->vertexData + triBase * sizeof(SpriteVertex);
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)triCount, triData, (UINT)sizeof(SpriteVertex));
		dr->triCount = 0;
	}
}

#endif

static bool loadTextureBytes(D3D9Renderer* dr, uint32_t index, const uint8_t* bytes, int byteSize, const char* label);
static bool loadExternalTexturePage(D3D9Renderer* dr, uint32_t index);
static bool d3d9SetRenderTarget(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface);
static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha);

// Texture load states
typedef enum {
	TEX_LOAD_IDLE = 0,		// Not queued, not loaded
	TEX_LOAD_QUEUED = 1,	// Queued for decode (or being decoded)
	TEX_LOAD_DECODED = 2,	// Decoded, ready for GPU upload on render thread
	TEX_LOAD_FAILED = 3,	// Decode failed, don't retry
	TEX_LOAD_UPLOADING = 4, // Being uploaded to GPU (render thread)
} TextureLoadState;

static void releaseTexturePage(D3D9Renderer* dr, uint32_t index) {
#ifndef PLATFORM_XBOX360_XDK
	std::mutex* gpuMutex = dr && dr->textureGpuMutex ? (std::mutex*)dr->textureGpuMutex : nullptr;
	std::unique_lock<std::mutex> gpuLock;
	if (gpuMutex) {
		gpuLock = std::unique_lock<std::mutex>(*gpuMutex);
	}
#endif

	// IMPORTANT: eviction must also free any CPU-side decoded buffer that might
	// be pending for upload. Otherwise, evicting pages while async decode is
	// in-flight can accumulate unbounded RAM.

	if (!dr || index >= dr->textureCount) {
		return;
	}

	// Best-effort free decoded pixels (decode worker ownership transferred
	// to texturePendingRGBA when it completes).
	if (dr->texturePendingRGBA && dr->texturePendingRGBA[index]) {
		stbi_image_free(dr->texturePendingRGBA[index]);
		dr->texturePendingRGBA[index] = nullptr;
	}
	if (dr->texturePendingW) {
		dr->texturePendingW[index] = 0;
	}
	if (dr->texturePendingH) {
		dr->texturePendingH[index] = 0;
	}
	if (dr->texturePendingByteSize) {
		dr->texturePendingByteSize[index] = 0;
	}
	if (dr->textureLoadState) {
		dr->textureLoadState[index] = TEX_LOAD_IDLE;
	}

	// If there's no GPU texture currently resident, nothing more to evict.
	if (!dr->textures || !dr->textures[index]) {
		return;
	}

	if (dr->currentTextureIndex == (int32_t)index) {
		// Eviction must not happen mid-batch. Caller ensures safe point.
		// Just invalidate the binding cache; actual DrawPrimitiveUP must
		// already have been flushed by the time releaseTexturePage runs.
		dr->currentTextureIndex = -1;
		Dev(dr)->SetTexture(0, nullptr);
	}

	if (dr->textures[index] != nullptr) {
		((IDirect3DTexture9*)dr->textures[index])->Release();
	}
	dr->textureBytesUsed -= dr->textureBlobSizes[index];
	dr->textureWidths[index] = 0;
	dr->textureHeights[index] = 0;
	if (dr->textureLastUsedFrame) {
		dr->textureLastUsedFrame[index] = 0;
	}
	if (dr->loadedTexturePages > 0) {
		dr->loadedTexturePages--;
	}
}

static void ensureTextureCacheRoom(D3D9Renderer* dr) {
	// Only evict at safe points (between batches) to avoid releasing
	// textures while DXVK still references them for DrawPrimitiveUP.
	if (!dr || dr->quadCount != 0) {
		return;
	}

	const uint32_t maxLoadedPages = 24;
	const uint32_t maxTextureBytesUsed = (256 * 1024 * 1024); // 256 MB

	while (dr->loadedTexturePages > maxLoadedPages || dr->textureBytesUsed > maxTextureBytesUsed) {
		// Evict the least recently used page (by frameCounter ordering)
		uint32_t victim = UINT_MAX;
		uint32_t bestAge = 0;
		bool haveVictim = false;

		// Track the oldest valid frame for LRU comparison
		uint32_t oldestValidFrame = dr->frameCounter;

		for (uint32_t i = 0; i < dr->textureCount; i++) {
			if (!dr->textures[i]) {
				continue;
			}
			if ((int32_t)i == dr->currentTextureIndex) {
				continue;
			}

			uint32_t last = dr->textureLastUsedFrame ? dr->textureLastUsedFrame[i] : 0;
			// If last==0 or hasn't been used this epoch, treat as very old.
			if (last == 0) {
				// Immediately evict textures that have never been used
				victim = i;
				haveVictim = true;
				break;
			}

			// Track the oldest frame in use
			if (last < oldestValidFrame) {
				oldestValidFrame = last;
			}

			uint32_t age = last;
			if (!haveVictim || age < bestAge) {
				bestAge = age;
				victim = i;
				haveVictim = true;
			}
		}

		if (victim == UINT_MAX) {
			break;
		}
		releaseTexturePage(dr, victim);
	}
}

// ===[ Async Texture Loading System ]===

// Texture loading is asynchronous on desktop builds.
// On Xbox 360 we implement the decoder worker pool using Win32 APIs.
// This keeps the desktop path using std::thread while allowing the XDK
// toolchain to build and execute correctly.

// Maximum number of concurrent decode worker threads. The Xbox 360 path uses
// fewer workers to avoid extra CPU and memory pressure during gameplay.
static const uint32_t kMaxDecodeWorkers = 4;

// A decode work item
struct DecodeWorkItem {
	uint32_t textureIndex;
	const uint8_t* blobData;
	bool ownedBlob;
	int blobSize;
	bool gm2022_5;
};

// Global thread pool state (per renderer)
struct TextureDecodePool {
#ifdef PLATFORM_XBOX360_XDK
	CRITICAL_SECTION mutex;
	HANDLE workEvent;
	DecodeWorkItem* workQueue;
	uint32_t queueHead;
	uint32_t queueTail;
	uint32_t queueCount;
	uint32_t queueCapacity;
	HANDLE workers[kMaxDecodeWorkers];
#else
	std::mutex mutex;
	std::condition_variable cv;
	std::queue<DecodeWorkItem> workQueue;
	std::vector<std::thread> workers;
#endif
	bool shutdown;
	uint32_t numWorkers;
};

#ifdef PLATFORM_XBOX360_XDK
static DWORD WINAPI textureDecodeWorkerThreadProc(LPVOID param);

// Worker thread function: decodes a texture page from blob data
static void textureDecodeWorker(D3D9Renderer* dr) {
	TextureDecodePool* pool = (TextureDecodePool*)dr->textureLoadMutex;
	if (!pool) {
		return;
	}

	while (true) {
		WaitForSingleObject(pool->workEvent, INFINITE);

		while (true) {
			DecodeWorkItem item;
			ZERO_STRUCT(item);
			bool gotItem = false;
			bool shouldExit = false;

			EnterCriticalSection(&pool->mutex);
			if (pool->queueCount > 0) {
				item = pool->workQueue[pool->queueHead];
				pool->queueHead = (pool->queueHead + 1) % pool->queueCapacity;
				pool->queueCount--;
				gotItem = true;
			}
			shouldExit = pool->shutdown && pool->queueCount == 0;
			LeaveCriticalSection(&pool->mutex);

			if (!gotItem) {
				if (shouldExit) {
					return;
				}
				break;
			}

			DataWin* dw = dr->base.dataWin;
			if (!dw) {
				return;
			}

			DataWin_loadTxtrIfNeeded(dw, item.textureIndex);

			int w = 0, h = 0;
			uint8_t* pixels = ImageDecoder_decodeToRgba(item.blobData, (size_t)item.blobSize, item.gm2022_5, &w, &h);

			if (item.ownedBlob && item.blobData) {
				free((void*)item.blobData);
			}

			EnterCriticalSection(&pool->mutex);
			if (pixels && w > 0 && h > 0) {
				dr->texturePendingRGBA[item.textureIndex] = pixels;
				dr->texturePendingW[item.textureIndex] = (uint32_t)w;
				dr->texturePendingH[item.textureIndex] = (uint32_t)h;
				// Cache budget: decoded RGBA bytes, not compressed blob size.
				dr->texturePendingByteSize[item.textureIndex] = (uint32_t)((uint64_t)w * (uint64_t)h * 4ull);
				dr->textureLoadState[item.textureIndex] = TEX_LOAD_DECODED;
			} else {
				dr->textureLoadState[item.textureIndex] = TEX_LOAD_FAILED;
				Butterscotch_xdkDiagTrace("D3D9: async decode failed for texture page %u", item.textureIndex);
			}
			dr->textureDecodeInFlight--;
			LeaveCriticalSection(&pool->mutex);
		}
	}
}

static DWORD WINAPI textureDecodeWorkerThreadProc(LPVOID param) {
	textureDecodeWorker((D3D9Renderer*)param);
	return 0;
}

// Start a decode worker if we're under the concurrency limit
static void maybeStartWorker(D3D9Renderer* dr) {
	TextureDecodePool* pool = (TextureDecodePool*)dr->textureLoadMutex;
	if (!pool) {
		return;
	}

	uint32_t workerIndex = UINT32_MAX;
	EnterCriticalSection(&pool->mutex);
	if (pool->numWorkers < kMaxDecodeWorkers && !pool->shutdown) {
		workerIndex = pool->numWorkers++;
	}
	LeaveCriticalSection(&pool->mutex);

	if (workerIndex != UINT32_MAX) {
		HANDLE thread = CreateThread(nullptr, 0, textureDecodeWorkerThreadProc, dr, 0, nullptr);
		if (thread) {
			pool->workers[workerIndex] = thread;
		} else {
			EnterCriticalSection(&pool->mutex);
			pool->numWorkers--;
			LeaveCriticalSection(&pool->mutex);
		}
	}
}

#else
// Worker thread function: decodes a texture page from blob data
static void textureDecodeWorker(D3D9Renderer* dr) {
	TextureDecodePool* pool = (TextureDecodePool*)dr->textureLoadMutex;
	if (!pool) {
		return;
	}

	while (true) {
		DecodeWorkItem item;
		{
			std::unique_lock<std::mutex> lock(pool->mutex);
			pool->cv.wait(lock, [pool]() {
				return pool->shutdown || !pool->workQueue.empty();
			});

			if (pool->shutdown) {
				return;
			}

			item = pool->workQueue.front();
			pool->workQueue.pop();
		}

		// Decode the texture
		int w = 0, h = 0;
		uint8_t* pixels = ImageDecoder_decodeToRgba(item.blobData, (size_t)item.blobSize, item.gm2022_5, &w, &h);

		if (item.ownedBlob && item.blobData) {
			free((void*)item.blobData);
		}

		// Store result
		{
			std::lock_guard<std::mutex> lock(pool->mutex);
			if (pixels && w > 0 && h > 0) {
				// Store decoded data for render thread to upload
				dr->texturePendingRGBA[item.textureIndex] = pixels;
				dr->texturePendingW[item.textureIndex] = (uint32_t)w;
				dr->texturePendingH[item.textureIndex] = (uint32_t)h;
				// Cache budget: decoded RGBA bytes, not compressed blob size.
				dr->texturePendingByteSize[item.textureIndex] = (uint32_t)((uint64_t)w * (uint64_t)h * 4ull);

				dr->textureLoadState[item.textureIndex] = TEX_LOAD_DECODED;
			} else {
				dr->textureLoadState[item.textureIndex] = TEX_LOAD_FAILED;
				Butterscotch_xdkDiagTrace("D3D9: async decode failed for texture page %u", item.textureIndex);
			}
			dr->textureDecodeInFlight--;
		}
	}
}

// Start a decode worker if we're under the concurrency limit
static void maybeStartWorker(D3D9Renderer* dr) {
	TextureDecodePool* pool = (TextureDecodePool*)dr->textureLoadMutex;
	if (!pool) {
		return;
	}

	std::lock_guard<std::mutex> lock(pool->mutex);
	if (pool->numWorkers < kMaxDecodeWorkers && !pool->shutdown) {
		pool->workers.emplace_back(textureDecodeWorker, dr);
		pool->numWorkers++;
	}
}

#endif

static bool readBytesAt(DataWin* dw, size_t offset, size_t count, uint8_t** outData) {
	if (!dw || !outData || count == 0) {
		if (outData) {
			*outData = nullptr;
		}
		return false;
	}

	*outData = nullptr;
	if (offset > SIZE_MAX - count) {
		return false;
	}
	if (dw->fileSize > 0 && offset + count > dw->fileSize) {
		return false;
	}

	uint8_t* data = (uint8_t*)safeMalloc(count);
	if (!data) {
		return false;
	}

	// Prefer the persistent lazyLoadFile handle to avoid fopen/fclose per call.
	if (dw->lazyLoadFile) {
		long savedPos = ftell(dw->lazyLoadFile);
		if (savedPos < 0 || fseek(dw->lazyLoadFile, (long)offset, SEEK_SET) != 0) {
			free(data);
			return false;
		}

		size_t readCount = fread(data, 1, count, dw->lazyLoadFile);
		if (fseek(dw->lazyLoadFile, savedPos, SEEK_SET) != 0) {
			free(data);
			return false;
		}
		if (readCount != count) {
			free(data);
			return false;
		}

		*outData = data;
		return true;
	}

	// Fallback: open file by path (when lazyLoadFile is not available)
	if (dw->lazyLoadFilePath) {
		FILE* file = fopen(dw->lazyLoadFilePath, "rb");
		if (!file) {
			free(data);
			return false;
		}

		if (fseek(file, (long)offset, SEEK_SET) != 0) {
			fclose(file);
			free(data);
			return false;
		}

		size_t readCount = fread(data, 1, count, file);
		fclose(file);
		if (readCount != count) {
			free(data);
			return false;
		}

		*outData = data;
		return true;
	}

	free(data);
	return false;
}

static bool readTexturePageBytes(D3D9Renderer* dr, uint32_t textureIndex, uint8_t** outBytes, int* outSize) {
	if (!dr || !outBytes || !outSize || textureIndex >= dr->textureCount) {
		return false;
	}

	*outBytes = nullptr;
	*outSize = 0;

	DataWin* dw = dr->base.dataWin;
	if (!dw || textureIndex >= dw->txtr.count) {
		return false;
	}

	Texture* txtr = &dw->txtr.textures[textureIndex];
	if (!txtr) {
		return false;
	}

	if (txtr->blobData && txtr->blobSize > 0) {
		*outBytes = (uint8_t*)txtr->blobData;
		*outSize = (int)txtr->blobSize;
		return true;
	}

	if (txtr->blobOffset > 0 && txtr->blobSize > 0) {
		return readBytesAt(dw, txtr->blobOffset, txtr->blobSize, outBytes);
	}

	return false;
}

// Queue a texture page for async decode
static void queueAsyncDecode(D3D9Renderer* dr, uint32_t textureIndex) {
	if (!dr || textureIndex >= dr->textureCount) {
		return;
	}
	if (dr->textureLoadState[textureIndex] != TEX_LOAD_IDLE) {
		return;
	}

	DataWin* dw = dr->base.dataWin;
	if (!dw || textureIndex >= dw->txtr.count) {
		return;
	}

	Texture* txtr = &dw->txtr.textures[textureIndex];

	// Only queue if we have blob data to decode. Embedded TXTR payloads are read
	// from the backing data.win file on demand so the parser does not need to keep
	// all texture bytes resident in RAM.
	if (txtr->blobSize <= 0 || (txtr->blobOffset == 0 && !txtr->blobData)) {
		// External textures can't be decoded async (file I/O on worker thread is risky)
		// Fall through to synchronous path
		return;
	}

	TextureDecodePool* pool = (TextureDecodePool*)dr->textureLoadMutex;
	if (!pool) {
		return;
	}

	// Mark as queued
	dr->textureLoadState[textureIndex] = TEX_LOAD_QUEUED;
	dr->textureDecodeInFlight++;

	bool gm2022_5 = DataWin_isVersionAtLeast(((Renderer*)dr)->dataWin, 2022, 5, 0, 0);

	uint8_t* blobBytes = nullptr;
	int blobByteSize = 0;
	bool ownBlob = false;
	if (!readTexturePageBytes(dr, textureIndex, &blobBytes, &blobByteSize)) {
		dr->textureLoadState[textureIndex] = TEX_LOAD_IDLE;
		dr->textureDecodeInFlight--;
		return;
	}

	if (blobBytes && blobByteSize > 0 && (!txtr->blobData || txtr->blobSize <= 0)) {
		ownBlob = true;
	}

	DecodeWorkItem item;
	item.textureIndex = textureIndex;
	item.blobData = blobBytes;
	item.ownedBlob = ownBlob;
	item.blobSize = blobByteSize;
	item.gm2022_5 = gm2022_5;

#ifdef PLATFORM_XBOX360_XDK
	EnterCriticalSection(&pool->mutex);
	if (pool->queueCount < pool->queueCapacity) {
		pool->workQueue[pool->queueTail] = item;
		pool->queueTail = (pool->queueTail + 1) % pool->queueCapacity;
		pool->queueCount++;
	} else {
		dr->textureLoadState[textureIndex] = TEX_LOAD_IDLE;
		dr->textureDecodeInFlight--;
		LeaveCriticalSection(&pool->mutex);
		Butterscotch_xdkDiagTrace("D3D9: async decode queue full for texture page %u", textureIndex);
		return;
	}
	LeaveCriticalSection(&pool->mutex);
	SetEvent(pool->workEvent);
#else
	{
		std::lock_guard<std::mutex> lock(pool->mutex);
		pool->workQueue.push(item);
	}
	pool->cv.notify_one();
#endif

	// Ensure we have enough workers
	maybeStartWorker(dr);
}

// Upload a decoded texture to the GPU (must be called on render thread)
static bool uploadDecodedTexture(D3D9Renderer* dr, uint32_t textureIndex) {
	// Upload+eviction touches the GPU-visible texture cache.
	// On Xbox 360, avoid any risk of using a texture while it is being
	// released/evicted in another code path.
	// This function must only run at safe batch boundaries.
	if (dr && dr->quadCount != 0) {
		return false;
	}

	// Safety: if there are any async pages pending/decoded, do not evict in
	// the same frame unless we are at a safe point. (We keep eviction
	// triggered by ensureTextureCacheRoom() only at quadCount==0.)

	if (!dr || textureIndex >= dr->textureCount) {
		return false;
	}
	if (dr->textureLoadState[textureIndex] != TEX_LOAD_DECODED) {
		return false;
	}

	uint8_t* pixels = dr->texturePendingRGBA[textureIndex];
	uint32_t w = dr->texturePendingW[textureIndex];
	uint32_t h = dr->texturePendingH[textureIndex];

	// texturePendingByteSize stores decoded RGBA bytes (w*h*4) after Step 3.
	uint32_t byteSize = dr->texturePendingByteSize[textureIndex];

	if (!pixels || w == 0 || h == 0) {
		dr->textureLoadState[textureIndex] = TEX_LOAD_FAILED;
		return false;
	}

	// Mark as uploading to prevent re-entry
	dr->textureLoadState[textureIndex] = TEX_LOAD_UPLOADING;

	IDirect3DDevice9* dev = Dev(dr);
	IDirect3DTexture9* tex = nullptr;

	HRESULT hr = dev->CreateTexture((int)w, (int)h, 1, 0, D3D9_GPU_TEXTURE_FORMAT, D3DPOOL_MANAGED, &tex, nullptr);
	if (FAILED(hr) || !tex) {
		Butterscotch_xdkDiagTrace("D3D9: async CreateTexture failed page=%u %dx%d hr=0x%08X", textureIndex, w, h, (unsigned)hr);
		stbi_image_free(pixels);
		dr->texturePendingRGBA[textureIndex] = nullptr;
		dr->textureLoadState[textureIndex] = TEX_LOAD_FAILED;
		return false;
	}

	if (!uploadRgbaToTexture(dev, tex, pixels, (int32_t)w, (int32_t)h)) {
		Butterscotch_xdkDiagTrace("D3D9: async upload failed page=%u hr=0x%08X", textureIndex, (unsigned)hr);
		tex->Release();
		stbi_image_free(pixels);
		dr->texturePendingRGBA[textureIndex] = nullptr;
		dr->textureLoadState[textureIndex] = TEX_LOAD_FAILED;
		return false;
	}

	// Free the CPU-side decoded pixels
	stbi_image_free(pixels);
	dr->texturePendingRGBA[textureIndex] = nullptr;

	// Install the new texture (eviction must have been done at a safe point).
	dr->textures[textureIndex] = tex;

	dr->textureWidths[textureIndex] = (int32_t)w;
	dr->textureHeights[textureIndex] = (int32_t)h;
	// Cache budget uses decoded RGBA bytes (w*h*4), not compressed blob size.
	dr->textureBlobSizes[textureIndex] = byteSize;
	dr->textureBytesUsed += byteSize;

	dr->loadedTexturePages++;
	dr->textureLoadState[textureIndex] = TEX_LOAD_IDLE; // Reset to idle (loaded)

	Butterscotch_xdkDiagTrace("D3D9: async loaded texture page %u %dx%d", textureIndex, w, h);
	return true;
}

// Process completed async decodes on the render thread.
// Keep this scanning logic lightweight; full async "push" queues are
// possible but higher risk due to synchronization.
static void processCompletedDecodes(D3D9Renderer* dr) {
	if (!dr || !dr->textureLoadState) {
		return;
	}

	// Adaptive scan: scan up to 64 non-decoded slots, but process any
	// decoded textures found along the way. If many textures finish
	// decoding in a single frame, we keep uploading them without throttling.
	uint32_t emptyChecks = 0;
	const uint32_t maxEmptyChecks = 64;
	// Hard cap on total slots scanned per frame to avoid stalls on huge counts.
	const uint32_t hardScanLimit = (dr->textureCount < 1024) ? dr->textureCount : 1024;
	uint32_t totalScanned = 0;

	while (emptyChecks < maxEmptyChecks && totalScanned < hardScanLimit) {
		uint32_t i = dr->textureDecodedUploadCursor;
		dr->textureDecodedUploadCursor = (dr->textureDecodedUploadCursor + 1) % dr->textureCount;
		totalScanned++;

		if (dr->textureLoadState[i] == TEX_LOAD_DECODED) {
			uploadDecodedTexture(dr, i);
		} else {
			emptyChecks++;
		}
	}
}

// Fast check: returns true only if texture is already GPU-loaded
static inline bool isTextureLoaded(D3D9Renderer* dr, uint32_t textureIndex) {
	return dr->textures[textureIndex] != nullptr;
}

// Priority-ordered async ensure: processes pending uploads in order rather than
// checking every texture every frame. Also handles the first-time synchronous fallback.
static bool ensureTexturePageLoadedAsync(D3D9Renderer* dr, uint32_t textureIndex) {
	if (!dr || textureIndex >= dr->textureCount) {
		return false;
	}

	// Already loaded on GPU - fast path
	if (dr->textures[textureIndex]) {
		if (dr->textureLastUsedFrame) {
			dr->textureLastUsedFrame[textureIndex] = dr->frameCounter;
		}
		return true;
	}

	// Check async state
	uint8_t state = dr->textureLoadState[textureIndex];
	switch (state) {
	case TEX_LOAD_DECODED:
		// Decoded but not uploaded yet - upload now on render thread
		if (uploadDecodedTexture(dr, textureIndex)) {
			if (dr->textureLastUsedFrame) {
				dr->textureLastUsedFrame[textureIndex] = dr->frameCounter;
			}
			return true;
		}
		return false;

	case TEX_LOAD_QUEUED:
	case TEX_LOAD_UPLOADING:
		// Still being decoded or uploaded
		return false;

	case TEX_LOAD_FAILED:
		// Previously failed, don't retry
		return false;

	case TEX_LOAD_IDLE:
	default:
		break;
	}

	// Not queued yet. Try to queue for async decode.
	DataWin* dw = dr->base.dataWin;
	if (!dw || textureIndex >= dw->txtr.count) {
		return false;
	}

	Texture* txtr = &dw->txtr.textures[textureIndex];

	if (txtr->blobSize > 0 && (txtr->blobData || txtr->blobOffset > 0)) {
		queueAsyncDecode(dr, textureIndex);
		return false;
	}

	// External textures (no blob data) - must load synchronously
	if (txtr->present) {
		ensureTextureCacheRoom(dr);
		bool ok = loadExternalTexturePage(dr, textureIndex);
		if (ok) {
			dr->loadedTexturePages++;
			if (dr->textureLastUsedFrame) {
				dr->textureLastUsedFrame[textureIndex] = dr->frameCounter;
			}
			return true;
		}
		dr->textureLoadState[textureIndex] = TEX_LOAD_FAILED;
		return false;
	}

	return false;
}

// Synchronous fallback (for external textures and non-async callers)
static bool ensureTexturePageLoaded(D3D9Renderer* dr, uint32_t textureIndex) {
	if (!dr || textureIndex >= dr->textureCount) {
		return false;
	}
	if (dr->textures[textureIndex]) {
		if (dr->textureLastUsedFrame) {
			dr->textureLastUsedFrame[textureIndex] = dr->frameCounter;
		}
		return true;
	}

#ifdef PLATFORM_XBOX360_XDK
	// TEXTURES.BIN + ATLAS.BIN + CLUT8.BIN fallback for Xbox 360.
	// Uses the preprocessor's indexed-palette texture format which saves significant
	// memory compared to storing full RGBA textures from TXTR chunks.
	//
	// IMPORTANT priority rule: TXTR/preprocessed/embedded content must take precedence.
	// So we only attempt streaming from TEXTURES.BIN when the TXTR page has *not*
	// produced a loaded GPU texture.
	if ((uint32_t)textureIndex < dr->originalTexturePageCount && dr->textures[textureIndex] == nullptr) {
		// Check if this TPAG has a mapping in the atlas
		if (Xbox360Textures_hasTpagMapping((int32_t)textureIndex)) {
			uint8_t* rgba = nullptr;
			int w = 0, h = 0;
			if (Xbox360Textures_loadPage((int32_t)textureIndex, &w, &h, &rgba) && rgba && w > 0 && h > 0) {
				ensureTextureCacheRoom(dr);
				IDirect3DDevice9* dev = Dev(dr);
				IDirect3DTexture9* tex = nullptr;
				HRESULT hr = dev->CreateTexture(w, h, 1, 0, D3D9_GPU_TEXTURE_FORMAT, D3DPOOL_MANAGED, &tex, nullptr);
				if (SUCCEEDED(hr) && tex) {
					if (uploadRgbaToTexture(dev, tex, rgba, w, h)) {
						dr->textures[textureIndex] = tex;
						dr->textureWidths[textureIndex] = w;
						dr->textureHeights[textureIndex] = h;
						// Use the indexed data size for memory tracking, not the expanded RGBA size.
						// This accurately reflects the memory savings from using TEXTURES.BIN.
						// The indexed data is typically 1/4 to 1/8 the size of RGBA.
						int atlasId = -1, clutIndex = -1, bpp = 8;
						Xbox360Textures_getTpagAtlasInfo((int32_t)textureIndex, &atlasId, nullptr, nullptr, nullptr, nullptr, &clutIndex, &bpp);
						uint32_t indexedSize;
						if (bpp == 4) {
							indexedSize = (uint32_t)(((uint64_t)w * (uint64_t)h + 1) / 2);
						} else {
							indexedSize = (uint32_t)((uint64_t)w * (uint64_t)h);
						}
						// Add CLUT size (shared, but attribute a fraction per texture)
						uint32_t clutShare = (clutIndex >= 0) ? 256 : 0; // 256 bytes for palette colors
						dr->textureBlobSizes[textureIndex] = indexedSize + clutShare;
						dr->textureBytesUsed += dr->textureBlobSizes[textureIndex];
						dr->loadedTexturePages++;
						if (dr->textureLastUsedFrame) {
							dr->textureLastUsedFrame[textureIndex] = dr->frameCounter;
						}
						free(rgba);
						return true;
					}
					tex->Release();
				}
				free(rgba);
			}
		}
	}
#endif

async_loop:
	// If async is in progress, process completed decodes (may upload our texture)
	if (dr->textureLoadState[textureIndex] == TEX_LOAD_QUEUED ||
		dr->textureLoadState[textureIndex] == TEX_LOAD_DECODED ||
		dr->textureLoadState[textureIndex] == TEX_LOAD_UPLOADING) {
		processCompletedDecodes(dr);
		if (dr->textures[textureIndex]) {
			if (dr->textureLastUsedFrame) {
				dr->textureLastUsedFrame[textureIndex] = dr->frameCounter;
			}
			return true;
		}
		YIELD();
		goto async_loop;
	}

	// Fall back to synchronous decode for external textures
	DataWin* dw = dr->base.dataWin;
	if (!dw || textureIndex >= dw->txtr.count) {
		return false;
	}

	DataWin_loadTxtrIfNeeded(dw, textureIndex);

	Texture* txtr = &dw->txtr.textures[textureIndex];
	ensureTextureCacheRoom(dr);

	bool ok = false;
	if (txtr->blobSize > 0 && (txtr->blobData || txtr->blobOffset > 0)) {
		uint8_t* blobBytes = nullptr;
		int blobByteSize = 0;
		if (readTexturePageBytes(dr, textureIndex, &blobBytes, &blobByteSize)) {
			ok = loadTextureBytes(dr, textureIndex, blobBytes, blobByteSize, "data.win");
			if (blobBytes && blobBytes != txtr->blobData) {
				free(blobBytes);
			}
		}
	} else if (txtr->present) {
		ok = loadExternalTexturePage(dr, textureIndex);
	}

	if (ok) {
		dr->loadedTexturePages++;
	}
	if (dr->textureLastUsedFrame) {
		dr->textureLastUsedFrame[textureIndex] = dr->frameCounter;
	}

	return ok;
}

extern "C" bool D3D9Renderer_ensureTextureLoaded(D3D9Renderer* dr, uint32_t textureIndex) {
	return ensureTexturePageLoaded(dr, textureIndex);
}

static void ensureTexture(D3D9Renderer* dr, int32_t textureIndex) {
	// Only flush when the bound texture changes.
	if (dr->currentTextureIndex == textureIndex) {
		return;
	}
	flushBatch(dr);
	dr->currentTextureIndex = textureIndex;
}

static SpriteVertex* allocQuad(D3D9Renderer* dr) {
	if (dr->quadCount >= D3D9_MAX_QUADS) {
		flushBatch(dr);
	}
	SpriteVertex* v = (SpriteVertex*)(dr->vertexData + dr->quadCount * D3D9_VERTS_PER_QUAD * sizeof(SpriteVertex));
	dr->quadCount++;
	return v;
}

static SpriteVertex* allocTri(D3D9Renderer* dr) {
	if (dr->quadCount > 0) {
		flushBatch(dr);
	}
	if (dr->triCount >= D3D9_MAX_TRIS) {
		flushBatch(dr);
	}
	uint32_t base = D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD + dr->triCount * D3D9_VERTS_PER_TRI;
	SpriteVertex* v = (SpriteVertex*)(dr->vertexData + base * sizeof(SpriteVertex));
	dr->triCount++;
	return v;
}

static bool readWholeFile(const char* path, uint8_t** outData, int* outSize) {
	if (!path || !outData || !outSize) {
		return false;
	}
	*outData = nullptr;
	*outSize = 0;

	FILE* f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > 64 * 1024 * 1024) {
		fclose(f);
		return false;
	}

	uint8_t* data = (uint8_t*)safeMalloc((size_t)size);
	if (!data) {
		fclose(f);
		return false;
	}
	size_t got = fread(data, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		free(data);
		return false;
	}
	*outData = data;
	*outSize = (int)size;
	return true;
}

static bool loadTextureBytes(D3D9Renderer* dr, uint32_t index, const uint8_t* bytes, int byteSize, const char* label) {
	if (!bytes || byteSize <= 0 || index >= dr->textureCount) {
		return false;
	}

	int w, h;

	bool gm2022_5 = DataWin_isVersionAtLeast(((Renderer*)dr)->dataWin, 2022, 5, 0, 0);
	uint8_t* pixels = ImageDecoder_decodeToRgba(bytes, byteSize, gm2022_5, &w, &h);
	if (!pixels) {
		Butterscotch_xdkDiagTrace("D3D9: failed to decode texture page %u from %s bytes=%d", index, label ? label : "(memory)", byteSize);
		Butterscotch_xdkDiagTrace("D3D9: Free memory: %f", GetFreeMemMB());
		Butterscotch_xdkDiagTrace("D3D9: Failure reason: %s", stbi_failure_reason());
		return false;
	}

	IDirect3DDevice9* dev = Dev(dr);
	IDirect3DTexture9* tex = nullptr;

	HRESULT hr = dev->CreateTexture((int)w, (int)h, 1, 0, D3D9_GPU_TEXTURE_FORMAT, D3DPOOL_MANAGED, &tex, nullptr);
	if (FAILED(hr) || !tex) {
		Butterscotch_xdkDiagTrace("D3D9: CreateTexture failed page=%u %dx%d hr=0x%08X", index, w, h, (unsigned)hr);
		stbi_image_free(pixels);
		return false;
	}

	if (!uploadRgbaToTexture(dev, tex, pixels, w, h)) {
		Butterscotch_xdkDiagTrace("D3D9: texture upload failed page=%u hr=0x%08X", index, (unsigned)hr);
		tex->Release();
		stbi_image_free(pixels);
		return false;
	}
	stbi_image_free(pixels);

	dr->textures[index] = tex;
	dr->textureWidths[index] = w;
	dr->textureHeights[index] = h;
	dr->textureBlobSizes[index] = byteSize;
	dr->textureBytesUsed += byteSize;
	Butterscotch_xdkDiagTrace("D3D9: loaded texture page %u %dx%d from %s", index, w, h, label ? label : "(memory)");
	return true;
}

static bool loadExternalTexturePage(D3D9Renderer* dr, uint32_t index) {
	char path[256];
	const char* formats[] = {
		"butterscotch:\\texture_%u.png",
		"butterscotch:\\texture_%u.PNG",
		"butterscotch:\\texture_page_%u.png",
		"butterscotch:\\texture_page_%u.PNG",
		"butterscotch:\\textures\\texture_%u.png",
		"butterscotch:\\textures\\texture_%u.PNG",
		"butterscotch:\\textures\\texture_page_%u.png",
		"butterscotch:\\textures\\texture_page_%u.PNG",
	};

	for (uint32_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		_snprintf(path, sizeof(path), formats[i], index);
		path[sizeof(path) - 1] = '\0';
		uint8_t* data = nullptr;
		int size = 0;
		if (!readWholeFile(path, &data, &size)) {
			continue;
		}
		bool ok = loadTextureBytes(dr, index, data, size, path);
		free(data);
		if (ok) {
			return true;
		}
	}

	Butterscotch_xdkDiagTrace("D3D9: external texture page %u missing; tried texture_%u.png / texture_page_%u.png", index, index, index);
	return false;
}

// Transform a game-space point to screen-space pixels (with pillarbox centering)
static inline void transformPoint(D3D9Renderer* dr, float inX, float inY, float* outX, float* outY) {
	// Game-space: view → port mapping
	*outX = dr->portOffsetX + (inX - dr->offsetX) * dr->portScaleX;
	*outY = dr->portOffsetY + (inY - dr->offsetY) * dr->portScaleY;
}

static inline int32_t floorInt(float v) {
	int32_t i = (int32_t)v;
	return (v < (float)i) ? i - 1 : i;
}

static inline int32_t ceilInt(float v) {
	int32_t i = (int32_t)v;
	return (v > (float)i) ? i + 1 : i;
}

static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop,
							int32_t srcWidth, int32_t srcHeight, float x, float y,
							float xscale, float yscale, float angleDeg, uint32_t color, float alpha);

// Forward declarations used by d3d9CreateSpriteFromSurface (C++ needs these before first use)
static int32_t d3d9CreateSurface(Renderer* renderer, int32_t width, int32_t height);
static void d3d9SurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY,
							int32_t srcSurfaceID, int32_t srcX, int32_t srcY,
							int32_t srcW, int32_t srcH, bool part);
static bool d3d9SurfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA);
static void d3d9SurfaceFree(Renderer* renderer, int32_t surfaceID);

// ===[ Surface Management Helpers ]===

// Finds a free slot in the surface arrays, or grows them if all slots are in use.
static uint32_t d3d9FindOrAllocateSurfaceSlot(D3D9Renderer* dr) {
	for (uint32_t i = 0; i < dr->surfaceCount; i++) {
		if (dr->surfaces[i] == nullptr) {
			return i;
		}
	}
	// Grow the arrays
	uint32_t newIndex = dr->surfaceCount;
	uint32_t newCount = dr->surfaceCount + 1;
	dr->surfaces = (void**)safeRealloc(dr->surfaces, newCount * sizeof(void*));
	dr->surfaceTexture = (void**)safeRealloc(dr->surfaceTexture, newCount * sizeof(void*));
	dr->surfaceWidth = (int32_t*)safeRealloc(dr->surfaceWidth, newCount * sizeof(int32_t));
	dr->surfaceHeight = (int32_t*)safeRealloc(dr->surfaceHeight, newCount * sizeof(int32_t));
	dr->surfaces[newIndex] = nullptr;
	dr->surfaceTexture[newIndex] = nullptr;
	dr->surfaceWidth[newIndex] = 0;
	dr->surfaceHeight[newIndex] = 0;
	dr->surfaceCount = newCount;
	return newIndex;
}

// Releases a single surface's D3D resources (but does not clear the slot).
static void d3d9ReleaseSurfaceSlot(D3D9Renderer* dr, uint32_t slot) {
	if (slot >= dr->surfaceCount) {
		return;
	}
	if (dr->surfaces[slot]) {
		((IDirect3DSurface9*)dr->surfaces[slot])->Release();
		dr->surfaces[slot] = nullptr;
	}
	if (dr->surfaceTexture[slot]) {
		((IDirect3DTexture9*)dr->surfaceTexture[slot])->Release();
		dr->surfaceTexture[slot] = nullptr;
	}
	dr->surfaceWidth[slot] = 0;
	dr->surfaceHeight[slot] = 0;
}

#ifndef PLATFORM_XBOX360_XDK

#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>

#define FXC_EXE "/hdd/Program Files (x86)/Microsoft DirectX SDK (June 2010)/Utilities/bin/x64/fxc.exe"

HRESULT compileShader(
	const char* source,
	const char* profile, // "vs_2_0" or "ps_2_0"
	void** outBytecode,
	size_t* outSize) {
	// 1. Write the HLSL source string out to a temporary file
	FILE* fSrc = fopen("tmp_shader.hlsl", "w");
	fprintf(fSrc, "%s", source);
	fclose(fSrc);

	// Format the profile flag (e.g., "/Tvs_2_0")
	char profileFlag[32];
	snprintf(profileFlag, sizeof(profileFlag), "/T%s", profile);

	// 2. Fork and exec: "wine fxc.exe /T vs_2_0 /Fo /tmp/shader.dxbc /tmp/shader.hlsl"
	pid_t pid = fork();
	if (pid == 0) {
		// In child process
		execlp("wine", "wine", FXC_EXE,
			   profileFlag,			 // "/Tvs_2_0"
			   "/Fotmp_shader.dxbc", // Output file (No space!)
			   "/Zpr",
			   // "/Xu0_deprecated",        // Force standard DX tokens instead of Xbox 360 physical microcode
			   "tmp_shader.hlsl", // Input file
			   nullptr);
		exit(1); // Exit if exec fails
	}

	// 3. Wait for compiler to finish
	int status;
	waitpid(pid, &status, 0);
	if (status != 0) {
		return E_FAIL;
	}

	// 4. Read back the compiled binary DXBC payload
	FILE* fBin = fopen("tmp_shader.dxbc", "rb");
	if (!fBin) {
		return E_FAIL;
	}

	fseek(fBin, 0, SEEK_END);
	*outSize = ftell(fBin);
	fseek(fBin, 0, SEEK_SET);

	*outBytecode = malloc(*outSize);
	fread(*outBytecode, 1, *outSize, fBin);
	fclose(fBin);

	return S_OK;
}

// IDirect3DVertexDeclaration9* g_pVertexDecl = nullptr;

// Call this once during D3D9 renderer initialization:
void InitVertexDeclaration(IDirect3DDevice9* dev) {
	D3DVERTEXELEMENT9 decl[] = {
		// Stream, Offset, Type, Method, Usage, UsageIndex
		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },	// x, y, z, w
		{ 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, // u, v
		{ 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 }, // r, g, b, a (Mapped to TEXCOORD1)
		D3DDECL_END()
	};
	dev->CreateVertexDeclaration(decl, &g_pVertexDecl);
}

#else

HRESULT compileShader(
	const char* source,
	const char* profile, // "vs_2_0" or "ps_2_0"
	void** outBytecode,
	size_t* outSize) {
	ID3DXBuffer* pErr = nullptr;
	HRESULT hr = D3DXCompileShader(source, (UINT)strlen(source),
								   nullptr, nullptr, "main", profile, 0, (ID3DXBuffer**)outBytecode, &pErr, nullptr);
	return hr;
}

#endif

HRESULT useShaders(IDirect3DDevice9* dev, const char* vsSource, const char* psSource, IDirect3DVertexShader9** pVertexShader, IDirect3DPixelShader9** pPixelShader) {
	ID3DXBuffer* pCode = nullptr;
	ID3DXBuffer* pErr = nullptr;
#ifdef PLATFORM_XBOX360_XDK
	// Use vsSource/psSource directly since on 360 they are g_vsSource/g_psSource (pass-through shaders)
	HRESULT hr = D3DXCompileShader(vsSource, (UINT)strlen(vsSource),
								   nullptr, nullptr, "main", "vs_2_0", 0, &pCode, &pErr, nullptr);
	if (FAILED(hr)) {
		Butterscotch_xdkDiagTrace("D3D9: D3DXCompileShader(VS) failed hr=0x%08X\n", (unsigned)hr);
		return E_FAIL;
	}
	hr = dev->CreateVertexShader((const DWORD*)pCode->GetBufferPointer(),
								 (IDirect3DVertexShader9**)pVertexShader);
	pCode->Release();
	if (FAILED(hr)) {
		Butterscotch_xdkDiagTrace("D3D9: CreateVertexShader failed hr=0x%08X\n", (unsigned)hr);
		return E_FAIL;
	}

	hr = D3DXCompileShader(psSource, (UINT)strlen(psSource),
						   nullptr, nullptr, "main", "ps_2_0", 0, &pCode, &pErr, nullptr);
	if (FAILED(hr)) {
		Butterscotch_xdkDiagTrace("D3D9: D3DXCompileShader(PS) failed hr=0x%08X\n", (unsigned)hr);
		return E_FAIL;
	}
	hr = dev->CreatePixelShader((const DWORD*)pCode->GetBufferPointer(),
								(IDirect3DPixelShader9**)pPixelShader);
	pCode->Release();
	if (FAILED(hr)) {
		Butterscotch_xdkDiagTrace("D3D9: CreatePixelShader failed hr=0x%08X\n", (unsigned)hr);
		return E_FAIL;
	}

	return S_OK;
#else
	void* vsBytecode = nullptr;
	size_t vsBytecodeSize = 0;
	HRESULT hr = compileShader(vsSource, "vs_2_0", &vsBytecode, &vsBytecodeSize);
	if (FAILED(hr)) {
		OutputDebugStringA("VS compile failed: ");
		if (pErr) {
			OutputDebugStringA((const char*)pErr->GetBufferPointer());
		}
		free(vsBytecode);
		return E_FAIL;
	}

	HRESULT hrVS = dev->CreateVertexShader((const DWORD*)vsBytecode, (IDirect3DVertexShader9**)pVertexShader);
	if (FAILED(hrVS) || !pVertexShader) {
		Butterscotch_xdkDiagTrace("D3D9: CreateVertexShader(VS) failed hr=0x%08X\n", (unsigned)hrVS);
		free(vsBytecode);
		return E_FAIL;
	}
	free(vsBytecode);

	void* psBytecode = nullptr;
	size_t psSize = 0;
	hr = compileShader(psSource, "ps_2_0", &psBytecode, &psSize);
	if (FAILED(hr)) {
		OutputDebugStringA("PS compile failed: ");
		if (pErr) {
			OutputDebugStringA((const char*)pErr->GetBufferPointer());
		}
		free(psBytecode);
		return E_FAIL;
	}

	HRESULT hrPS = dev->CreatePixelShader((const DWORD*)psBytecode, (IDirect3DPixelShader9**)pPixelShader);
	if (FAILED(hrPS) || !pPixelShader) {
		Butterscotch_xdkDiagTrace("D3D9: CreatePixelShader(PS) failed hr=0x%08X\n", (unsigned)hrPS);
		free(psBytecode);
		return E_FAIL;
	}
	free(psBytecode);

	return S_OK;
#endif
}

// ===[ Shader Compilation Helpers ]===

// Forward declaration of compileShader
HRESULT compileShader(const char* source, const char* profile, void** outBytecode, size_t* outSize);

// Parses HLSL source to extract uniform declarations and assign register slots.
// Returns the number of uniforms found (capped at D3D9_MAX_SHADER_UNIFORMS).
// uniformDeclarations must be D3D9ShaderUniform[D3D9_MAX_SHADER_UNIFORMS].
static uint32_t parseHLSLUniforms(const char* source, const char* profile, D3D9ShaderUniform* uniforms) {
	if (!source) {
		return 0;
	}

	const char* p = source;
	uint32_t count = 0;
	// Track register assignments starting from c0 for vertex, c0 for pixel
	int nextVertexRegister = 0;
	int nextPixelRegister = 0;
	bool isVertex = (strstr(profile, "vs_") != nullptr);

	while (*p && count < D3D9_MAX_SHADER_UNIFORMS) {
		// Skip whitespace and newlines
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
			p++;
		}
		if (!*p) {
			break;
		}

		// Look for "uniform" keyword
		if (strncmp(p, "uniform", 7) == 0) {
			const char* declStart = p;
			p += 7;
			while (*p == ' ' || *p == '\t') {
				p++;
			}

			// Check type: float, float4, float4x4, int, int4, sampler2D, etc.
			char type[64] = { 0 };
			int ti = 0;
			while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != '\n' && ti < 63) {
				type[ti++] = *p++;
			}
			type[ti] = '\0';

			if (strlen(type) == 0) {
				continue;
			}

			// Skip whitespace after type
			while (*p == ' ' || *p == '\t') {
				p++;
			}

			// Read name (until ;, =, :, space, or newline)
			char name[128] = { 0 };
			int ni = 0;
			while (*p && *p != ';' && *p != '=' && *p != ':' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && ni < 127) {
				name[ni++] = *p++;
			}
			name[ni] = '\0';

			if (strlen(name) == 0 || name[0] == '{') {
				continue;
			}

			// Determine type and register count
			bool isSampler = (strcmp(type, "sampler2D") == 0) || (strcmp(type, "sampler") == 0) || (strcmp(type, "SamplerState") == 0);
			int regCount = 1;

			// Array support: check for [N]
			int arraySize = 1;
			while (*p == ' ' || *p == '\t') {
				p++;
			}
			if (*p == '[') {
				p++;
				char numStr[16] = { 0 };
				int numI = 0;
				while (*p && *p >= '0' && *p <= '9' && numI < 15) {
					numStr[numI++] = *p++;
				}
				if (*p == ']') {
					p++;
				}
				arraySize = atoi(numStr);
				if (arraySize < 1) {
					arraySize = 1;
				}
			}

			if (strcmp(type, "float4x4") == 0 || strcmp(type, "matrix") == 0) {
				regCount = 4 * arraySize; // 4 registers for a 4x4 matrix
			} else if (strcmp(type, "float4") == 0 || strcmp(type, "float") == 0) {
				regCount = (strcmp(type, "float4") == 0) ? (1 * arraySize) : (1 * arraySize);
			} else if (strcmp(type, "int4") == 0 || strcmp(type, "int") == 0) {
				regCount = (strcmp(type, "int4") == 0) ? (1 * arraySize) : (1 * arraySize);
			} else if (isSampler) {
				regCount = 1;
			} else {
				// Unknown type, default to 1
				regCount = 1;
			}

			// Assign register
			uniforms[count].name = strdup(name);
			uniforms[count].isSampler = isSampler;
			uniforms[count].isVertex = isVertex;

			if (isVertex) {
				uniforms[count].registerIndex = nextVertexRegister;
				uniforms[count].registerCount = regCount;
				if (!isSampler) {
					nextVertexRegister += regCount;
				}
			} else {
				uniforms[count].registerIndex = nextPixelRegister;
				uniforms[count].registerCount = regCount;
				if (!isSampler) {
					nextPixelRegister += regCount;
				}
			}
			uniforms[count].samplerSlot = 0;

			count++;
		}

		// Skip to next line or semicolon
		while (*p && *p != '\n' && *p != '\r') {
			p++;
		}
		if (*p == '\r') {
			p++;
		}
		if (*p == '\n') {
			p++;
		}
	}

	return count;
}

static bool compileD3D9Program(D3D9GMLShader* gmlShader, const char* vertexShaderSource, const char* fragmentShaderSource, IDirect3DDevice9* dev, const char* name) {
	if (!vertexShaderSource || !fragmentShaderSource) {
		fprintf(stderr, "D3D9: Shader %s has no HLSL source\n", name ? name : "unknown");
		return false;
	}

	// Compile vertex shader
	void* vsBytecode = nullptr;
	size_t vsBytecodeSize = 0;
	HRESULT hr = compileShader(vertexShaderSource, "vs_2_0", &vsBytecode, &vsBytecodeSize);
	if (FAILED(hr) || !vsBytecode) {
		fprintf(stderr, "D3D9: Failed to compile vertex shader %s\nSource:\n%s\n\n", name ? name : "unknown", vertexShaderSource ? vertexShaderSource : "(null)");
		return false;
	}

	IDirect3DVertexShader9* vs = nullptr;
	hr = dev->CreateVertexShader((const DWORD*)vsBytecode, &vs);
	free(vsBytecode);
	if (FAILED(hr) || !vs) {
		fprintf(stderr, "D3D9: CreateVertexShader failed for %s hr=0x%08X\n", name ? name : "unknown", (unsigned)hr);
		return false;
	}

	// Compile pixel shader
	void* psBytecode = nullptr;
	size_t psBytecodeSize = 0;
	hr = compileShader(fragmentShaderSource, "ps_2_0", &psBytecode, &psBytecodeSize);
	if (FAILED(hr) || !psBytecode) {
		fprintf(stderr, "D3D9: Failed to compile pixel shader %s\nSource:\n%s\n\n", name ? name : "unknown", fragmentShaderSource ? fragmentShaderSource : "(null)");
		vs->Release();
		return false;
	}

	IDirect3DPixelShader9* ps = nullptr;
	hr = dev->CreatePixelShader((const DWORD*)psBytecode, &ps);
	free(psBytecode);
	if (FAILED(hr) || !ps) {
		fprintf(stderr, "D3D9: CreatePixelShader failed for %s hr=0x%08X\n", name ? name : "unknown", (unsigned)hr);
		vs->Release();
		return false;
	}

	gmlShader->pVertexShader = vs;
	gmlShader->pPixelShader = ps;
	gmlShader->compiled = true;

	// Parse uniforms from HLSL source
	gmlShader->uniformCount = parseHLSLUniforms(vertexShaderSource, "vs_2_0", gmlShader->uniforms);
	uint32_t psUniforms = parseHLSLUniforms(fragmentShaderSource, "ps_2_0", gmlShader->uniforms + gmlShader->uniformCount);
	gmlShader->uniformCount += psUniforms;

	// Assign sampler slots sequentially
	uint32_t nextSamplerSlot = 0;
	for (uint32_t i = 0; i < gmlShader->uniformCount; i++) {
		if (gmlShader->uniforms[i].isSampler) {
			gmlShader->uniforms[i].samplerSlot = nextSamplerSlot++;
			// For samplers, set up the device to use that slot
			dev->SetVertexShaderConstantF(gmlShader->uniforms[i].registerIndex, nullptr, 0);
		}
	}

	fprintf(stderr, "D3D9: Shader %s compiled successfully (%u uniforms, %u sampler slots)\n",
			name ? name : "unknown", gmlShader->uniformCount, nextSamplerSlot);
	return true;
}

// Find a uniform by name in a shader
static D3D9ShaderUniform* findShaderUniform(D3D9GMLShader* shader, const char* name) {
	if (!shader || !name) {
		return nullptr;
	}
	for (uint32_t i = 0; i < shader->uniformCount; i++) {
		if (strcmp(shader->uniforms[i].name, name) == 0) {
			return &shader->uniforms[i];
		}
	}
	return nullptr;
}

static void freeD3D9GMLShader(D3D9GMLShader* shader) {
	if (!shader) {
		return;
	}
	if (shader->pVertexShader) {
		((IDirect3DVertexShader9*)shader->pVertexShader)->Release();
		shader->pVertexShader = nullptr;
	}
	if (shader->pPixelShader) {
		((IDirect3DPixelShader9*)shader->pPixelShader)->Release();
		shader->pPixelShader = nullptr;
	}
	for (uint32_t i = 0; i < shader->uniformCount; i++) {
		free(shader->uniforms[i].name);
		shader->uniforms[i].name = nullptr;
	}
	shader->uniformCount = 0;
	shader->compiled = false;
}

// ===[ Vtable Implementations ]===

static void d3d9Init(Renderer* renderer, DataWin* dataWin) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);
	renderer->dataWin = dataWin;

	Matrix4f world;
	Matrix4f_identity(&world);
	renderer->gmlMatrices[MATRIX_WORLD] = world;

#ifndef PLATFORM_XBOX360_XDK
	InitVertexDeclaration(dev);
#endif

	Dev(dr)->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	Dev(dr)->SetRenderState(D3DRS_ZENABLE, FALSE);
	// Dont use setViewportEnable here because dr->boundViewportEnable should already be set to true by D3D9Renderer_create, so it wouldnt do anything
	dev->SetRenderState(D3DRS_VIEWPORTENABLE, TRUE);

	// Allocate CPU vertex staging buffer (shared between quads and triangles)
	dr->vertexData = (uint8_t*)safeMalloc((D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD + D3D9_MAX_TRIS * D3D9_VERTS_PER_TRI) * sizeof(SpriteVertex));

	// // Compile shaders from source
	// ID3DXBuffer* pCode = nullptr;
	// ID3DXBuffer* pErr = nullptr;

	HRESULT hr = useShaders(dev, g_vsSource, g_psSource, (IDirect3DVertexShader9**)&dr->pVertexShader, (IDirect3DPixelShader9**)&dr->pPixelShader);
	if (FAILED(hr)) {
		Butterscotch_xdkDiagTrace("D3D9: useShaders failed hr=0x%08X\n", (unsigned)hr);
		exit(1);
	}

	// Create vertex declaration
	static const D3DVERTEXELEMENT9 decl[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
		D3DDECL_END()
	};
	dev->CreateVertexDeclaration(decl, (IDirect3DVertexDeclaration9**)&dr->pVertexDecl);

	// Create 1x1 white texture for primitives
	IDirect3DTexture9* whiteTex = nullptr;
	HRESULT hrTex = dev->CreateTexture(1, 1, 1, 0, D3D9_GPU_TEXTURE_FORMAT, D3DPOOL_MANAGED, &whiteTex, nullptr);
	if (SUCCEEDED(hrTex) && whiteTex) {
		uint8_t whitePixel[4] = { 255, 255, 255, 255 };
		if (!uploadRgbaToTexture(dev, whiteTex, whitePixel, 1, 1)) {
			fprintf(stderr, "D3D9 Error: Failed to fill 1x1 white texture.\n");
			whiteTex->Release();
			whiteTex = nullptr;
		}
	} else {
		fprintf(stderr, "D3D9 Error: Failed to create white texture! HRESULT: 0x%08x\n",
				static_cast<unsigned int>(hrTex));
	}
	dr->whiteTexture = whiteTex;

	// TXTR pages can be huge in fan builds. Decode/upload them on demand.
	dr->textureCount = dataWin->txtr.count;
	dr->textures = (void**)safeCalloc(dr->textureCount, sizeof(void*));
	dr->textureWidths = (int32_t*)safeCalloc(dr->textureCount, sizeof(int32_t));
	dr->textureHeights = (int32_t*)safeCalloc(dr->textureCount, sizeof(int32_t));
	dr->textureBlobSizes = (uint32_t*)safeCalloc(dr->textureCount, sizeof(uint32_t));
	dr->textureLastUsedFrame = (uint32_t*)safeCalloc(dr->textureCount, sizeof(uint32_t));
	dr->loadedTexturePages = 0;
	dr->frameCounter = 1;
	Butterscotch_xdkDiagTrace("D3D9: texture pages will be loaded lazily count=%u", dr->textureCount);

	// Initialize async texture loading system
	dr->textureLoadState = (uint8_t*)safeCalloc(dr->textureCount, sizeof(uint8_t));
	dr->texturePendingRGBA = (uint8_t**)safeCalloc(dr->textureCount, sizeof(uint8_t*));
	dr->texturePendingW = (uint32_t*)safeCalloc(dr->textureCount, sizeof(uint32_t));
	dr->texturePendingH = (uint32_t*)safeCalloc(dr->textureCount, sizeof(uint32_t));
	dr->texturePendingByteSize = (uint32_t*)safeCalloc(dr->textureCount, sizeof(uint32_t));
	dr->textureDecodeWorkerConcurrency = kMaxDecodeWorkers;
	dr->textureDecodeInFlight = 0;
	dr->textureDecodedUploadCursor = 0;

	// Create the decode pool
	TextureDecodePool* pool = new TextureDecodePool();
	dr->textureLoadMutex = (void*)pool;

#ifdef PLATFORM_XBOX360_XDK
	InitializeCriticalSection(&pool->mutex);
	pool->workEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	pool->workQueue = new DecodeWorkItem[256];
	pool->queueHead = 0;
	pool->queueTail = 0;
	pool->queueCount = 0;
	pool->queueCapacity = 256;
	pool->shutdown = false;
	pool->numWorkers = 0;
	dr->textureLoadCond = nullptr;
	dr->textureGpuMutex = nullptr;
#else
	dr->textureLoadCond = (void*)&pool->cv;
	dr->textureGpuMutex = (void*)new std::mutex();
#endif

	dr->originalTexturePageCount = dataWin->txtr.count;
	dr->originalTpagCount = dataWin->tpag.count;
	dr->originalSpriteCount = dataWin->sprt.count;

	dr->currentTextureIndex = -1;
	dr->quadCount = 0;
	dr->triCount = 0;
	dr->viewportX = 0;
	dr->viewportY = 0;
	dr->viewportW = 0;
	dr->viewportH = 0;

	// Initialize GML shader support — compile lazily on first use
#ifndef D3D9_DISABLE_SHADERS
	dr->gmlShaders = (D3D9GMLShader*)safeCalloc(dataWin->shdr.count, sizeof(D3D9GMLShader));
	dr->gmlShaderCount = dataWin->shdr.count;
	fprintf(stderr, "D3D9: %u Shaders found (will compile on demand)\n", dataWin->shdr.count);
#else
	dr->gmlShaders = nullptr;
	dr->gmlShaderCount = 0;
	fprintf(stderr, "D3D9: GML shaders disabled via D3D9_DISABLE_SHADERS\n");
#endif

	// Initialize dynamic surface arrays (empty)
	dr->surfaces = nullptr;
	dr->surfaceTexture = nullptr;
	dr->surfaceWidth = nullptr;
	dr->surfaceHeight = nullptr;
	dr->surfaceCount = 0;
}

static void d3d9Destroy(Renderer* renderer) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	// Shut down the decode pool
	TextureDecodePool* pool = (TextureDecodePool*)dr->textureLoadMutex;

#ifdef PLATFORM_XBOX360_XDK
	if (pool) {
		EnterCriticalSection(&pool->mutex);
		pool->shutdown = true;
		LeaveCriticalSection(&pool->mutex);
		SetEvent(pool->workEvent);

		for (uint32_t i = 0; i < pool->numWorkers; i++) {
			if (pool->workers[i]) {
				WaitForSingleObject(pool->workers[i], INFINITE);
				CloseHandle(pool->workers[i]);
			}
		}

		CloseHandle(pool->workEvent);
		DeleteCriticalSection(&pool->mutex);
		delete[] pool->workQueue;
		delete pool;
		dr->textureLoadMutex = nullptr;
	}
#else
	// Stop workers first, then release GPU cache mutex.
	if (pool) {
		{
			std::lock_guard<std::mutex> lock(pool->mutex);
			pool->shutdown = true;
		}
		pool->cv.notify_all();
		for (auto& t : pool->workers) {
			if (t.joinable()) {
				t.join();
			}
		}
		delete pool;
		dr->textureLoadMutex = nullptr;
		dr->textureLoadCond = nullptr;
	}

	if (dr->textureGpuMutex) {
		std::mutex* m = (std::mutex*)dr->textureGpuMutex;
		delete m;
		dr->textureGpuMutex = nullptr;
	}
#endif

	// Free any pending decoded buffers

	if (dr->texturePendingRGBA) {
		for (uint32_t i = 0; i < dr->textureCount; i++) {
			if (dr->texturePendingRGBA[i]) {
				stbi_image_free(dr->texturePendingRGBA[i]);
				dr->texturePendingRGBA[i] = nullptr;
			}
		}
	}

	for (uint32_t i = 0; i < dr->textureCount; i++) {
		if (dr->textures[i]) {
			((IDirect3DTexture9*)dr->textures[i])->Release();
		}
	}
	free(dr->textures);
	free(dr->textureWidths);
	free(dr->textureHeights);
	free(dr->textureBlobSizes);
	free(dr->textureLastUsedFrame);
	free(dr->textureLoadState);
	free(dr->texturePendingRGBA);
	free(dr->texturePendingW);
	free(dr->texturePendingH);
	free(dr->texturePendingByteSize);
	free(dr->vertexData);
	if (dr->whiteTexture) {
		((IDirect3DTexture9*)dr->whiteTexture)->Release();
	}
	d3d9ReleaseStagingTexture();
	releaseApplicationSurface(dr);
	if (dr->pVertexShader) {
		((IDirect3DVertexShader9*)dr->pVertexShader)->Release();
	}
	if (dr->pPixelShader) {
		((IDirect3DPixelShader9*)dr->pPixelShader)->Release();
	}
	if (dr->pVertexDecl) {
		((IDirect3DVertexDeclaration9*)dr->pVertexDecl)->Release();
	}

	// Release GML shaders
	for (uint32_t i = 0; i < dr->gmlShaderCount; i++) {
		freeD3D9GMLShader(&dr->gmlShaders[i]);
	}
	free(dr->gmlShaders);

	// Release all dynamic surface resources
	for (uint32_t i = 0; i < dr->surfaceCount; i++) {
		d3d9ReleaseSurfaceSlot(dr, i);
	}
	free(dr->surfaces);
	free(dr->surfaceTexture);
	free(dr->surfaceWidth);
	free(dr->surfaceHeight);

	free(dr);
}

static void d3d9BeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);

	dr->gameW = gameW;
	dr->gameH = gameH;
	dr->screenW = windowW;
	dr->screenH = windowH;
	dr->samplerStateApplied = false;
	dr->blendIsNormal = false;
	dr->frameCounter++;
	if (dr->frameCounter == 0) {
		dr->frameCounter = 1;
	}

	// Process any completed async texture decodes before rendering
	processCompletedDecodes(dr);

	// Fit the game inside the 720p backbuffer. On Xbox 360, 1080p backbuffers
	// can fail to allocate; keep 720p and rely on point sampling for crispness.
	float scaleX = (float)windowW / (float)gameW;
	float scaleY = (float)windowH / (float)gameH;
	float fitScale = (scaleX < scaleY) ? scaleX : scaleY;
	dr->renderScale = fitScale;
	dr->renderOffsetX = ((float)windowW - ((float)gameW * dr->renderScale)) * 0.5f;
	dr->renderOffsetY = ((float)windowH - (float)gameH * dr->renderScale) * 0.5f;

	// Debug: print once
	static bool printedOnce = false;
	if (!printedOnce) {
		DbgPrint("D3D9: renderScale=%d/1000 offsetX=%d offsetY=%d gameW=%d gameH=%d screenW=%d screenH=%d\n",
				 (int)(dr->renderScale * 1000), (int)dr->renderOffsetX, (int)dr->renderOffsetY,
				 gameW, gameH, windowW, windowH);
		printedOnce = true;
	}

	dev->BeginScene();

	if (renderer->runner && renderer->runner->usingAppSurface && dr->appSurfaceLevel) {
		d3d9SetRenderTarget(renderer, APPLICATION_SURFACE_ID, false);
		dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
	} else {
		bindBackbuffer(dr);
		resetFullBackbufferState(dr);
		dr->renderingToApplicationSurface = false;
		setGameTargetTransform(dr);
		dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
	}

	// Apply shared GPU render state once per BeginFrame.
	// Mark dirty and let the cache function early-out if it's already clean.
	// Only apply shared render state when it's marked dirty.
	// Callers which change shaders/rendering setup must set dr->renderStateDirty = true.
	d3d9EnsureSharedRenderState(dr);
}

static void d3d9EndFrame(Renderer* renderer) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	// Dev(dr)->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 255), 1.0f, 0);
	flushBatch(dr);
	Dev(dr)->EndScene();
	Dev(dr)->Present(nullptr, nullptr, nullptr, nullptr);
}

static void d3d9EndFrameInit(Renderer* renderer) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	flushBatch(dr);
	if (renderer->runner && renderer->runner->usingAppSurface && dr->appSurfaceLevel && dr->renderingToApplicationSurface) {
		resolveApplicationSurface(dr);
		bindBackbuffer(dr);
		resetFullBackbufferState(dr);
		dr->renderingToApplicationSurface = false;
		setGameTargetTransform(dr);
		Dev(dr)->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
		dr->samplerStateApplied = false;
		applyPointSampling(Dev(dr), dr);
		if (renderer->runner->appSurfaceAutoDraw) {
			d3d9DrawSurface(renderer, APPLICATION_SURFACE_ID, 0, 0, -1, -1, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFF, 1.0f);
		}
	}
}

static void d3d9EndFrameEnd(Renderer* renderer) {
	d3d9EndFrame(renderer);
}

static void d3d9ApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix, const Matrix4f* projectionMatrix);

static void d3d9BeginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
						  int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);
	(void)viewAngle; // TODO: support rotated views

	float targetScaleX = dr->renderingToApplicationSurface ? 1.0f : dr->renderScale;
	float targetScaleY = dr->renderingToApplicationSurface ? 1.0f : dr->renderScale;
	float targetOffsetX = dr->renderingToApplicationSurface ? 0.0f : dr->renderOffsetX;
	float targetOffsetY = dr->renderingToApplicationSurface ? 0.0f : dr->renderOffsetY;
	int32_t targetW = dr->renderingToApplicationSurface ? dr->gameW : dr->screenW;
	int32_t targetH = dr->renderingToApplicationSurface ? dr->gameH : dr->screenH;

	float screenPortX = targetOffsetX + (float)portX * targetScaleX;
	float screenPortY = targetOffsetY + (float)portY * targetScaleY;
	float screenPortW = (float)portW * targetScaleX;
	float screenPortH = (float)portH * targetScaleY;

	int32_t scLeft = floorInt(screenPortX);
	int32_t scTop = floorInt(screenPortY);
	int32_t scRight = ceilInt(screenPortX + screenPortW);
	int32_t scBottom = ceilInt(screenPortY + screenPortH);
	if (scLeft < 0) {
		scLeft = 0;
	}
	if (scTop < 0) {
		scTop = 0;
	}
	if (scRight > targetW) {
		scRight = targetW;
	}
	if (scBottom > targetH) {
		scBottom = targetH;
	}

	// Set viewport to the screen-space port rectangle for scissor alignment.
	D3DVIEWPORT9 vp;
	vp.X = (DWORD)scLeft;
	vp.Y = (DWORD)scTop;
	vp.Width = max(1U, (DWORD)(scRight - scLeft));
	vp.Height = max(1U, (DWORD)(scBottom - scTop));
	vp.MinZ = 0.0f;
	vp.MaxZ = 1.0f;
	dev->SetViewport(&vp);
	dr->viewportX = scLeft;
	dr->viewportY = scTop;
	dr->viewportW = (int32_t)(scRight - scLeft);
	dr->viewportH = (int32_t)(scBottom - scTop);

	// Enable scissor test to clip rendering to the port rectangle.
	// This prevents sprites from bleeding outside their intended viewport
	// (e.g. characters appearing outside dialogue box borders in Undertale).
	RECT scissor;
	scissor.left = scLeft;
	scissor.top = scTop;
	scissor.right = scRight;
	scissor.bottom = scBottom;
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
	dev->SetScissorRect(&scissor);

	// Store view transform for point mapping
	dr->offsetX = (float)viewX;
	dr->offsetY = (float)viewY;
	dr->portScaleX = screenPortW / (float)viewW;
	dr->portScaleY = screenPortH / (float)viewH;
	dr->portOffsetX = screenPortX;
	dr->portOffsetY = screenPortY;

	// Look up camera for the current view and set up view/projection matrices.
	int32_t viewCurrent = 0;
	if (renderer->runner && renderer->runner->viewsEnabled) {
		viewCurrent = renderer->runner->viewCurrent;
	}
	RuntimeView* view = &renderer->runner->views[viewCurrent];
	renderer->cameraCurrent = view->cameraId;
	GMLCamera* camera = Runner_getCameraById(renderer->runner, renderer->cameraCurrent);
	if (camera != nullptr) {
		d3d9ApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
	}
}

static void d3d9EndView(Renderer* renderer) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	flushBatch(dr);
	Dev(dr)->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
}

static void d3d9ApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix, const Matrix4f* projectionMatrix) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	flushBatch(dr);

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

	// GML shader matrix uniform update: if a GML shader is active, re-upload
	// the new matrices to the GPU so shader sees the updated projection.
	dr->cachedGmlMatricesValid = false;
	int32_t currentShader = renderer->currentShader;
	if (currentShader >= 0 && (uint32_t)currentShader < dr->gmlShaderCount) {
		D3D9GMLShader* shader = &dr->gmlShaders[currentShader];
		if (shader->compiled) {
			D3D9ShaderUniform* gmMatrices = findShaderUniform(shader, "gm_Matrices");
			if (gmMatrices != nullptr) {
				IDirect3DDevice9* dev = Dev(dr);
				for (int m = 0; m < 5; m++) {
					dev->SetVertexShaderConstantF(
						gmMatrices->registerIndex + m * 4,
						renderer->gmlMatrices[m].m,
						4);
					dr->cachedGmlMatrices[m] = renderer->gmlMatrices[m];
				}
				dr->cachedGmlMatricesValid = true;
			}
		}
	}
}

static void d3d9BeginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);

	// Use uniform scale to maintain the GUI's aspect ratio within the port rectangle.
	// This prevents GUI elements (dialogue boxes, etc.) from stretching when the
	// screen aspect differs from the game's native aspect (e.g. 4:3 game on 16:9 display).
	float scaleX = (guiW > 0) ? (float)portW / (float)guiW : 1.0f;
	float scaleY = (guiH > 0) ? (float)portH / (float)guiH : 1.0f;
	float uniformScale = (scaleX < scaleY) ? scaleX : scaleY;
	float offsetX = ((float)portW - (float)guiW * uniformScale) * 0.5f;
	float offsetY = ((float)portH - (float)guiH * uniformScale) * 0.5f;

	int32_t scLeft = portX < 0 ? 0 : portX;
	int32_t scTop = portY < 0 ? 0 : portY;
	int32_t scRight = portX + portW;
	int32_t scBottom = portY + portH;
	if (scRight > dr->screenW) {
		scRight = dr->screenW;
	}
	if (scBottom > dr->screenH) {
		scBottom = dr->screenH;
	}

	RECT scissor;
	scissor.left = scLeft;
	scissor.top = scTop;
	scissor.right = scRight;
	scissor.bottom = scBottom;
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
	dev->SetScissorRect(&scissor);

	// When the GUI dimensions match the port dimensions exactly (full-window coverage),
	// the caller (e.g., Runner_drawPost) expects the existing game-to-screen transform
	// to remain active, not an identity transform. This is critical for widescreen mods
	// that call draw_surface_stretched(application_surface, 0, 0, 854, 480) during
	// Post Draw — the 854×480 dest coordinates need to be mapped through the uniform
	// game-to-screen transform to properly fill the 1280×720 backbuffer.
	// Only override the transform when actual GUI scaling (aspect-ratio-aware
	// letterboxing) is needed, i.e., when guiW != portW or guiH != portH.
	// Set up GUI camera
	renderer->cameraCurrent = GUI_CAMERA;
	GMLCamera* guiCamera = &renderer->runner->guiCamera;
	guiCamera->allocated = true;
	guiCamera->viewX = 0.0f;
	guiCamera->viewY = 0.0f;
	guiCamera->viewWidth = guiW;
	guiCamera->viewHeight = guiH;
	guiCamera->borderX = 0;
	guiCamera->borderY = 0;
	guiCamera->speedX = 0;
	guiCamera->speedY = 0;
	guiCamera->objectId = -1;
	guiCamera->viewAngle = 0;

	Matrix4f projectionMatrix;
	Matrix4f_Orthographic(&projectionMatrix, (float)guiW, (float)guiH, 32000.0, 0.0);

	Matrix4f viewMatrix;
	float cx = (float)guiW * 0.5f;
	float cy = (float)guiH * 0.5f;
	Matrix4f_identity(&viewMatrix);
	Matrix4f_LookAt(&viewMatrix, cx, cy, -16000.0, cx, cy, 16000.0, 0.0, 1.0, 0.0);
	guiCamera->viewMatrix = viewMatrix;
	guiCamera->projectionMatrix = projectionMatrix;

	d3d9ApplyProjection(renderer, &guiCamera->viewMatrix, &guiCamera->projectionMatrix);

	if (guiW == portW && guiH == portH) {
		return;
	}

	dr->offsetX = 0.0f;
	dr->offsetY = 0.0f;
	dr->portScaleX = uniformScale;
	dr->portScaleY = uniformScale;
	dr->portOffsetX = (float)portX + offsetX;
	dr->portOffsetY = (float)portY + offsetY;
}

static void d3d9EndGUI(Renderer* renderer) {
	d3d9EndView(renderer);
}

// ===[ Sprite Drawing ]===

static void d3d9DrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
						   float originX, float originY, float xscale, float yscale,
						   float angleDeg, uint32_t color, float alpha) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	DataWin* dw = renderer->dataWin;

	if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) {
		D3D9_DIAG_LIMITED(64, "D3D9: drawSprite invalid tpag=%d count=%u x=%.2f y=%.2f",
						  tpagIndex, dw->tpag.count, x, y);
		return;
	}

	TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
	int32_t texPageId = tpag->texturePageId;
	if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) {
		D3D9_DIAG_LIMITED(64, "D3D9: drawSprite invalid texPage=%d textureCount=%u tpag=%d",
						  texPageId, dr->textureCount, tpagIndex);
		return;
	}
	// Use async loading - skip if texture not ready yet
	if (!ensureTexturePageLoadedAsync(dr, (uint32_t)texPageId)) {
		return;
	}
	if (!dr->textures[texPageId]) {
		D3D9_DIAG_LIMITED(64, "D3D9: drawSprite null texture page=%d tpag=%d", texPageId, tpagIndex);
		return;
	}

	ensureTexture(dr, texPageId);

	float texW = (float)dr->textureWidths[texPageId];
	float texH = (float)dr->textureHeights[texPageId];
	if (texW <= 0 || texH <= 0) {
		return;
	}

	int roomIndex = renderer->runner ? renderer->runner->currentRoomIndex : -1;
	if (dr->drawPhase == RENDER_PHASE_WORLD && (roomIndex >= 288 || roomIndex < 8)) {
		int limit = roomIndex >= 288 ? 96 : 120;
		if (roomIndex >= 288 || tpag->sourceWidth >= 64 || tpag->sourceHeight >= 64 || x <= 8.0f || y <= 8.0f) {
			D3D9_DIAG_LIMITED(limit,
							  "D3D9WORLD: sprite tpag=%d texPage=%d tex=%dx%d tpagSrc=%d,%d %dx%d target=%d,%d bound=%dx%d dst=%.2f,%.2f origin=%.2f,%.2f scale=%.3f,%.3f angle=%.2f room=%d",
							  tpagIndex, texPageId, (int)texW, (int)texH,
							  tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight,
							  tpag->targetX, tpag->targetY, tpag->boundingWidth, tpag->boundingHeight,
							  x, y, originX, originY, xscale, yscale, angleDeg,
							  roomIndex);
		}
	}

	// UV coordinates on the texture atlas
	float u0 = texelStart((float)tpag->sourceX, texW);
	float v0 = texelStart((float)tpag->sourceY, texH);
	float u1 = texelEnd((float)tpag->sourceX, (float)tpag->sourceWidth, texW);
	float v1 = texelEnd((float)tpag->sourceY, (float)tpag->sourceHeight, texH);

	// Quad corners in local space (before transform)
	// Use targetWidth/Height (draw size in bounding rect), not sourceWidth/Height (texture sample size).
	// They differ when the texture was auto-downscaled by GMS to fit a texture page.
	float localX0 = (float)tpag->targetX - originX;
	float localY0 = (float)tpag->targetY - originY;
	float localX1 = localX0 + (float)tpag->targetWidth;
	float localY1 = localY0 + (float)tpag->targetHeight;

	// Scale
	localX0 *= xscale;
	localY0 *= yscale;
	localX1 *= xscale;
	localY1 *= yscale;

	float cr, cg, cb, ca;
	bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

	// Build 4 corners
	float cx[4], cy[4];
	if (angleDeg != 0.0f) {
		// Micro-opt: avoid recomputing deg->rad scale constant.
		const float kDegToRad = (3.14159265f / 180.0f);
		float rad = -angleDeg * kDegToRad;
		float cosA = cosf(rad);
		float sinA = sinf(rad);

		float lx[4] = { localX0, localX1, localX1, localX0 };
		float ly[4] = { localY0, localY0, localY1, localY1 };
		for (int i = 0; i < 4; i++) {
			cx[i] = lx[i] * cosA - ly[i] * sinA;
			cy[i] = lx[i] * sinA + ly[i] * cosA;
		}
	} else {
		cx[0] = localX0;
		cy[0] = localY0;
		cx[1] = localX1;
		cy[1] = localY0;
		cx[2] = localX1;
		cy[2] = localY1;
		cx[3] = localX0;
		cy[3] = localY1;
	}

	// Transform to screen space
	SpriteVertex* v = allocQuad(dr);
	float sx, sy;
	for (int i = 0; i < 4; i++) {
		transformPoint(dr, x + cx[i], y + cy[i], &sx, &sy);
		v[i].x = sx - 0.5f;
		v[i].y = sy - 0.5f;
		v[i].z = 0.0f;
		v[i].w = 1.0f;
		v[i].r = cr;
		v[i].g = cg;
		v[i].b = cb;
		v[i].a = ca;
	}
	v[0].u = u0;
	v[0].v = v0;
	v[1].u = u1;
	v[1].v = v0;
	v[2].u = u1;
	v[2].v = v1;
	v[3].u = u0;
	v[3].v = v1;
}

static void d3d9DrawSpritePart(Renderer* renderer, int32_t tpagIndex,
							   int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH,
							   float x, float y, float xscale, float yscale,
							   float angleDeg, float pivotX, float pivotY,
							   uint32_t color, float alpha) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	DataWin* dw = renderer->dataWin;

	if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) {
		D3D9_DIAG_LIMITED(64, "D3D9: drawSpritePart invalid tpag=%d count=%u src=%d,%d %dx%d dst=%.2f,%.2f",
						  tpagIndex, dw->tpag.count, srcOffX, srcOffY, srcW, srcH, x, y);
		return;
	}
	TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
	int32_t texPageId = tpag->texturePageId;
	if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) {
		D3D9_DIAG_LIMITED(64, "D3D9: drawSpritePart invalid texPage=%d textureCount=%u tpag=%d",
						  texPageId, dr->textureCount, tpagIndex);
		return;
	}
	// Use async loading - skip if texture not ready yet
	if (!ensureTexturePageLoadedAsync(dr, (uint32_t)texPageId)) {
		return;
	}
	if (!dr->textures[texPageId]) {
		D3D9_DIAG_LIMITED(64, "D3D9: drawSpritePart null texture page=%d tpag=%d", texPageId, tpagIndex);
		return;
	}

	ensureTexture(dr, texPageId);

	float texW = (float)dr->textureWidths[texPageId];
	float texH = (float)dr->textureHeights[texPageId];
	if (texW <= 0 || texH <= 0) {
		return;
	}

	int roomIndex = renderer->runner ? renderer->runner->currentRoomIndex : -1;
	if (dr->drawPhase == RENDER_PHASE_POST || dr->drawPhase == RENDER_PHASE_WORLD || roomIndex >= 288) {
		int limit = roomIndex >= 288 ? 128 : 180;
		if (roomIndex >= 288 || srcW >= 16 || srcH >= 16 || x <= 4.0f || y <= 4.0f) {
			D3D9_DIAG_LIMITED(limit,
							  "D3D9PART: phase=%d tpag=%d texPage=%d tex=%dx%d tpagSrc=%d,%d %dx%d target=%d,%d bound=%dx%d srcOff=%d,%d src=%dx%d dst=%.2f,%.2f scale=%.2f,%.2f room=%d",
							  dr->drawPhase,
							  tpagIndex, texPageId, (int)texW, (int)texH,
							  tpag->sourceX, tpag->sourceY, tpag->sourceWidth, tpag->sourceHeight,
							  tpag->targetX, tpag->targetY, tpag->boundingWidth, tpag->boundingHeight,
							  srcOffX, srcOffY, srcW, srcH,
							  x, y, xscale, yscale,
							  roomIndex);
		}
	}

	float u0 = texelStart((float)(tpag->sourceX + srcOffX), texW);
	float v0 = texelStart((float)(tpag->sourceY + srcOffY), texH);
	float u1 = texelEnd((float)(tpag->sourceX + srcOffX), (float)srcW, texW);
	float v1 = texelEnd((float)(tpag->sourceY + srcOffY), (float)srcH, texH);

	float drawW = (float)srcW * xscale;
	float drawH = (float)srcH * yscale;

	float cr, cg, cb, ca;
	bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

	SpriteVertex* v = allocQuad(dr);
	float qx[4] = { x, x + drawW, x + drawW, x };
	float qy[4] = { y, y, y + drawH, y + drawH };
	float cx[4], cy[4];

	if (angleDeg != 0.0f) {
		float rad = -angleDeg * (3.14159265f / 180.0f);
		float cosA = cosf(rad);
		float sinA = sinf(rad);
		for (int i = 0; i < 4; i++) {
			float dx = qx[i] - pivotX;
			float dy = qy[i] - pivotY;
			cx[i] = cosA * dx - sinA * dy + pivotX;
			cy[i] = sinA * dx + cosA * dy + pivotY;
		}
	} else {
		for (int i = 0; i < 4; i++) {
			cx[i] = qx[i];
			cy[i] = qy[i];
		}
	}

	float sx[4], sy[4];
	for (int i = 0; i < 4; i++) {
		transformPoint(dr, cx[i], cy[i], &sx[i], &sy[i]);
	}

	setVertex(&v[0], sx[0], sy[0], u0, v0, cr, cg, cb, ca);
	setVertex(&v[1], sx[1], sy[1], u1, v0, cr, cg, cb, ca);
	setVertex(&v[2], sx[2], sy[2], u1, v1, cr, cg, cb, ca);
	setVertex(&v[3], sx[3], sy[3], u0, v1, cr, cg, cb, ca);
}

static void d3d9DrawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	DataWin* dw = renderer->dataWin;

	if (0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) {
		D3D9_DIAG_LIMITED(64, "D3D9: drawSpritePos invalid tpag=%d count=%u", tpagIndex, dw->tpag.count);
		return;
	}
	TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
	int32_t texPageId = tpag->texturePageId;
	if (0 > texPageId || (uint32_t)texPageId >= dr->textureCount) {
		D3D9_DIAG_LIMITED(64, "D3D9: drawSpritePos invalid texPage=%d textureCount=%u tpag=%d",
						  texPageId, dr->textureCount, tpagIndex);
		return;
	}
	// Use async loading - skip if texture not ready yet
	if (!ensureTexturePageLoadedAsync(dr, (uint32_t)texPageId)) {
		return;
	}
	if (!dr->textures[texPageId]) {
		D3D9_DIAG_LIMITED(64, "D3D9: drawSprite null texture page=%d tpag=%d", texPageId, tpagIndex);
		return;
	}

	ensureTexture(dr, texPageId);

	float texW = (float)dr->textureWidths[texPageId];
	float texH = (float)dr->textureHeights[texPageId];
	if (texW <= 0 || texH <= 0) {
		return;
	}

	float u0 = texelStart((float)tpag->sourceX, texW);
	float v0 = texelStart((float)tpag->sourceY, texH);
	float u1 = texelEnd((float)tpag->sourceX, (float)tpag->sourceWidth, texW);
	float v1 = texelEnd((float)tpag->sourceY, (float)tpag->sourceHeight, texH);

	float cr, cg, cb, ca;
	bgrToFloatColor(renderer->drawColor, alpha, &cr, &cg, &cb, &ca);

	SpriteVertex* v = allocQuad(dr);
	float sx, sy;
	transformPoint(dr, x1, y1, &sx, &sy);
	setVertex(&v[0], sx, sy, u0, v0, cr, cg, cb, ca);
	transformPoint(dr, x2, y2, &sx, &sy);
	setVertex(&v[1], sx, sy, u1, v0, cr, cg, cb, ca);
	transformPoint(dr, x3, y3, &sx, &sy);
	setVertex(&v[2], sx, sy, u1, v1, cr, cg, cb, ca);
	transformPoint(dr, x4, y4, &sx, &sy);
	setVertex(&v[3], sx, sy, u0, v1, cr, cg, cb, ca);
}

static void d3d9DrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
							  uint32_t color, float alpha, bool outline) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	if (outline) {
		// Draw 4 lines as thin rectangles
		float lw = 1.0f;
		d3d9DrawRectangle(renderer, x1, y1, x2, y1 + lw, color, alpha, false); // top
		d3d9DrawRectangle(renderer, x1, y2 - lw, x2, y2, color, alpha, false); // bottom
		d3d9DrawRectangle(renderer, x1, y1, x1 + lw, y2, color, alpha, false); // left
		d3d9DrawRectangle(renderer, x2 - lw, y1, x2, y2, color, alpha, false); // right
		return;
	}

	ensureTexture(dr, -1); // white texture

	float cr, cg, cb, ca;
	bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);
	SpriteVertex* v = allocQuad(dr);

	float sx0, sy0, sx1, sy1;
	transformPoint(dr, x1, y1, &sx0, &sy0);
	// GML adds +1 to width/height for filled rects (matching GL renderer behavior)
	transformPoint(dr, x2 + 1.0f, y2 + 1.0f, &sx1, &sy1);

	setVertex(&v[0], sx0, sy0, 0, 0, cr, cg, cb, ca);
	setVertex(&v[1], sx1, sy0, 1, 0, cr, cg, cb, ca);
	setVertex(&v[2], sx1, sy1, 1, 1, cr, cg, cb, ca);
	setVertex(&v[3], sx0, sy1, 0, 1, cr, cg, cb, ca);
}

static void d3d9DrawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
						 float width, uint32_t color, float alpha) {
	// Draw line as a thin rotated rectangle
	float dx = x2 - x1;
	float dy = y2 - y1;
	float lenSq = dx * dx + dy * dy;
	if (lenSq < 0.000001f) {
		return;
	}
	float invLen = 1.0f / sqrtf(lenSq);
	float nx = -dy * invLen * width * 0.5f;
	float ny = dx * invLen * width * 0.5f;

	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	ensureTexture(dr, -1);
	float cr, cg, cb, ca;
	bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

	SpriteVertex* v = allocQuad(dr);
	float sx, sy;

	transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy);
	setVertex(&v[0], sx, sy, 0, 0, cr, cg, cb, ca);
	transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy);
	setVertex(&v[1], sx, sy, 1, 0, cr, cg, cb, ca);
	transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy);
	setVertex(&v[2], sx, sy, 1, 1, cr, cg, cb, ca);
	transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy);
	setVertex(&v[3], sx, sy, 0, 1, cr, cg, cb, ca);
}

static void d3d9DrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
							  float width, uint32_t color1, uint32_t color2, float alpha) {
	float dx = x2 - x1;
	float dy = y2 - y1;
	float lenSq = dx * dx + dy * dy;
	if (lenSq < 0.000001f) {
		return;
	}
	float invLen = 1.0f / sqrtf(lenSq);
	float nx = -dy * invLen * width * 0.5f;
	float ny = dx * invLen * width * 0.5f;

	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	ensureTexture(dr, -1);
	float c1r, c1g, c1b, c1a;
	bgrToFloatColor(color1, alpha, &c1r, &c1g, &c1b, &c1a);
	float c2r, c2g, c2b, c2a;
	bgrToFloatColor(color2, alpha, &c2r, &c2g, &c2b, &c2a);

	SpriteVertex* v = allocQuad(dr);
	float sx, sy;

	transformPoint(dr, x1 + nx, y1 + ny, &sx, &sy);
	setVertex(&v[0], sx, sy, 0, 0, c1r, c1g, c1b, c1a);
	transformPoint(dr, x2 + nx, y2 + ny, &sx, &sy);
	setVertex(&v[1], sx, sy, 1, 0, c2r, c2g, c2b, c2a);
	transformPoint(dr, x2 - nx, y2 - ny, &sx, &sy);
	setVertex(&v[2], sx, sy, 1, 1, c2r, c2g, c2b, c2a);
	transformPoint(dr, x1 - nx, y1 - ny, &sx, &sy);
	setVertex(&v[3], sx, sy, 0, 1, c1r, c1g, c1b, c1a);
}

static void d3d9DrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
								   uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4,
								   float alpha, bool outline) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	if (outline) {
		d3d9DrawLineColor(renderer, x1, y1, x2, y1, 1.0f, color1, color2, alpha);
		d3d9DrawLineColor(renderer, x2, y1, x2, y2, 1.0f, color2, color3, alpha);
		d3d9DrawLineColor(renderer, x2, y2, x1, y2, 1.0f, color3, color4, alpha);
		d3d9DrawLineColor(renderer, x1, y2, x1, y1, 1.0f, color4, color1, alpha);
		return;
	}

	ensureTexture(dr, -1); // white texture

	float c1r, c1g, c1b, c1a;
	float c2r, c2g, c2b, c2a;
	float c3r, c3g, c3b, c3a;
	float c4r, c4g, c4b, c4a;
	bgrToFloatColor(color1, alpha, &c1r, &c1g, &c1b, &c1a);
	bgrToFloatColor(color2, alpha, &c2r, &c2g, &c2b, &c2a);
	bgrToFloatColor(color3, alpha, &c3r, &c3g, &c3b, &c3a);
	bgrToFloatColor(color4, alpha, &c4r, &c4g, &c4b, &c4a);

	SpriteVertex* v = allocQuad(dr);

	float sx0, sy0, sx1, sy1;
	transformPoint(dr, x1, y1, &sx0, &sy0);
	// GML adds +1 to width/height for filled rects (matching GL renderer behavior)
	transformPoint(dr, x2 + 1.0f, y2 + 1.0f, &sx1, &sy1);

	// Per-vertex colors: TL=color1, TR=color2, BR=color3, BL=color4
	setVertex(&v[0], sx0, sy0, 0, 0, c1r, c1g, c1b, c1a);
	setVertex(&v[1], sx1, sy0, 1, 0, c2r, c2g, c2b, c2a);
	setVertex(&v[2], sx1, sy1, 1, 1, c3r, c3g, c3b, c3a);
	setVertex(&v[3], sx0, sy1, 0, 1, c4r, c4g, c4b, c4a);
}

static void d3d9DrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha, bool outline) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (outline) {
		d3d9DrawLineColor(renderer, x1, y1, x2, y2, 1.0f, color1, color2, alpha);
		d3d9DrawLineColor(renderer, x2, y2, x3, y3, 1.0f, color2, color3, alpha);
		d3d9DrawLineColor(renderer, x3, y3, x1, y1, 1.0f, color3, color1, alpha);
		return;
	}

	ensureTexture(dr, -1);

	float c1r, c1g, c1b, c1a;
	float c2r, c2g, c2b, c2a;
	float c3r, c3g, c3b, c3a;
	bgrToFloatColor(color1, alpha, &c1r, &c1g, &c1b, &c1a);
	bgrToFloatColor(color2, alpha, &c2r, &c2g, &c2b, &c2a);
	bgrToFloatColor(color3, alpha, &c3r, &c3g, &c3b, &c3a);

	SpriteVertex* v = allocTri(dr);
	float sx, sy;
	transformPoint(dr, x1, y1, &sx, &sy);
	setVertex(&v[0], sx, sy, 0.0f, 0.0f, c1r, c1g, c1b, c1a);
	transformPoint(dr, x2, y2, &sx, &sy);
	setVertex(&v[1], sx, sy, 0.0f, 0.0f, c2r, c2g, c2b, c2a);
	transformPoint(dr, x3, y3, &sx, &sy);
	setVertex(&v[2], sx, sy, 0.0f, 0.0f, c3r, c3g, c3b, c3a);
}

// Internal helper: renders text with per-vertex color support.
// When all four corner colors are identical, uses the fast single-color path.
// When colors differ, interpolates per-vertex colors across each glyph based on its
// position within the line (matching GameMaker's draw_text_color behavior).
static void d3d9DrawTextInternal(Renderer* renderer, const char* text, float x, float y,
								 float xscale, float yscale, float angleDeg,
								 uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
								 float alpha, float lineSeparation) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	DataWin* dw = renderer->dataWin;
	int32_t fontIndex = renderer->drawFont;
	if (0 > fontIndex || (uint32_t)fontIndex >= dw->font.count) {
		return;
	}

	Font* font = &dw->font.fonts[fontIndex];

	// Resolve font state: supports both regular and sprite fonts (matching GL renderer)
	TexturePageItem* fontTpag = nullptr; // single TPAG for regular fonts (nullptr for sprite fonts)
	int16_t fontPageId = -1;			 // texture page ID for regular fonts
	float fontTexW = 0.0f, fontTexH = 0.0f;
	Sprite* spriteFontSprite = nullptr; // source sprite for sprite fonts (nullptr for regular fonts)

	if (!font->isSpriteFont) {
		int32_t fontTpagIndex = font->tpagIndex;
		if (0 > fontTpagIndex) {
			return;
		}

		fontTpag = &dw->tpag.items[fontTpagIndex];
		fontPageId = fontTpag->texturePageId;
		if (0 > fontPageId || dr->textureCount <= (uint32_t)fontPageId) {
			return;
		}
		// Use async loading - skip if texture not ready yet
		if (!ensureTexturePageLoadedAsync(dr, (uint32_t)fontPageId)) {
			return;
		}
		if (!dr->textures[fontPageId]) {
			return;
		}

		fontTexW = (float)dr->textureWidths[fontPageId];
		fontTexH = (float)dr->textureHeights[fontPageId];
		if (fontTexW <= 0 || fontTexH <= 0) {
			return;
		}

		ensureTexture(dr, (int32_t)fontPageId);
	} else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t)font->spriteIndex) {
		spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
	} else {
		return;
	}

	// Check if all colors are the same (fast path — no per-vertex interpolation)
	bool uniformColor = (c1 == c2 && c2 == c3 && c3 == c4);
	float cr = 0;
	float cg = 0;
	float cb = 0;
	float ca = 0;
	if (uniformColor) {
		bgrToFloatColor(c1, alpha, &cr, &cg, &cb, &ca);
	}

	// Preprocess: convert # to \n (and \# to literal #)
	PreprocessedText processedText = TextUtils_preprocessGmlText(text);
	const char* processed = processedText.text;
	int32_t textLen = (int32_t)strlen(processed);

	// Count lines
	int32_t lineCount = TextUtils_countLines(processed, textLen);
	float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

	// Vertical alignment offset
	float totalHeight = (float)lineCount * lineStride;
	float valignOffset = 0;
	if (renderer->drawValign == 1) {
		valignOffset = -totalHeight / 2.0f;
	} else if (renderer->drawValign == 2) {
		valignOffset = -totalHeight;
	}

	float fontScaleX = xscale * font->scaleX;
	float fontScaleY = yscale * font->scaleY;

	// Build rotation transform (if needed)
	// Micro-opt: only compute sin/cos once per draw_text call.
	float cosA = 1.0f, sinA = 0.0f;
	bool hasRotation = (angleDeg != 0.0f);
	if (hasRotation) {
		// Precompute deg->rad scale; avoids one multiply in the trig path.
		const float kDegToRad = (3.14159265f / 180.0f);
		float rad = -angleDeg * kDegToRad;
		cosA = cosf(rad);
		sinA = sinf(rad);
	}

	// Iterate through lines. HTML5 subtracts ascenderOffset from per-line y offset.
	float cursorY = valignOffset - (float)font->ascenderOffset;
	int32_t lineStart = 0;

	for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
		// Find end of current line
		int32_t lineEnd = lineStart;
		while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd])) {
			lineEnd++;
		}
		int32_t lineLen = lineEnd - lineStart;

		// Horizontal alignment offset for this line
		float lineWidth = TextUtils_measureLineWidth(font, processed + lineStart, lineLen);
		float halignOffset = 0;
		if (renderer->drawHalign == 1) {
			halignOffset = -lineWidth / 2.0f;
		} else if (renderer->drawHalign == 2) {
			halignOffset = -lineWidth;
		}

		float cursorX = halignOffset;
		float gradientX = 0.0f; // pixel-position cursor for color interpolation

		// Render each glyph - decode one codepoint ahead for kerning
		int32_t pos = 0;
		uint16_t ch = 0;
		bool hasCh = false;
		if (lineLen > pos) {
			ch = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
			hasCh = true;
		}

		while (hasCh) {
			FontGlyph* glyph = TextUtils_findGlyph(font, ch);

			uint16_t nextCh = 0;
			bool hasNext = (lineLen > pos);
			if (hasNext) {
				nextCh = TextUtils_decodeUtf8(processed + lineStart, lineLen, &pos);
			}

			if (glyph) {
				float advance = (float)glyph->shift;
				bool drawGlyph = (glyph->sourceWidth > 0 && glyph->sourceHeight > 0);

				// Resolve per-vertex colors if non-uniform
				uint32_t colTL = c1, colTR = c2, colBR = c3, colBL = c4;
				if (!uniformColor && lineWidth > 0.0f) {
					float leftFrac = gradientX / lineWidth;
					float rightFrac = (gradientX + advance) / lineWidth;
					colTL = Color_lerp(c1, c2, leftFrac);
					colTR = Color_lerp(c1, c2, rightFrac);
					colBR = Color_lerp(c4, c3, rightFrac);
					colBL = Color_lerp(c4, c3, leftFrac);
				}

				float vTLr, vTLg, vTLb, vTLa;
				float vTRr, vTRg, vTRb, vTRa;
				float vBRr, vBRg, vBRb, vBRa;
				float vBLr, vBLg, vBLb, vBLa;
				if (uniformColor) {
					// Use the pre-computed flat color
					vTLr = vTRr = vBRr = vBLr = cr;
					vTLg = vTRg = vBRg = vBLg = cg;
					vTLb = vTRb = vBRb = vBLb = cb;
					vTLa = vTRa = vBRa = vBLa = ca;
				} else {
					bgrToFloatColor(colTL, alpha, &vTLr, &vTLg, &vTLb, &vTLa);
					bgrToFloatColor(colTR, alpha, &vTRr, &vTRg, &vTRb, &vTRa);
					bgrToFloatColor(colBR, alpha, &vBRr, &vBRg, &vBRb, &vBRa);
					bgrToFloatColor(colBL, alpha, &vBLr, &vBLg, &vBLb, &vBLa);
				}

				if (drawGlyph) {
					// Resolve texture and UVs for this glyph (supports both regular and sprite fonts)
					int32_t glyphPageId = -1;
					float glyphTexW = 0.0f, glyphTexH = 0.0f;
					float gU0, gV0, gU1, gV1;
					float localYOff = cursorY;

					if (!font->isSpriteFont) {
						// Regular font: all glyphs share the same atlas page
						glyphPageId = fontPageId;
						glyphTexW = fontTexW;
						glyphTexH = fontTexH;
						gU0 = texelStart((float)(fontTpag->sourceX + glyph->sourceX), glyphTexW);
						gV0 = texelStart((float)(fontTpag->sourceY + glyph->sourceY), glyphTexH);
						gU1 = texelEnd((float)(fontTpag->sourceX + glyph->sourceX), (float)glyph->sourceWidth, glyphTexW);
						gV1 = texelEnd((float)(fontTpag->sourceY + glyph->sourceY), (float)glyph->sourceHeight, glyphTexH);
					} else {
						// Sprite font: each glyph may be on a different texture page
						int32_t glyphIndex = (int32_t)(glyph - font->glyphs);
						if (0 > glyphIndex || glyphIndex >= (int32_t)spriteFontSprite->textureCount) {
							cursorX += glyph->shift;
							gradientX += glyph->shift;
							goto skip_glyph_draw;
						}
						int32_t glyphTpagIdx = spriteFontSprite->tpagIndices[glyphIndex];
						if (0 > glyphTpagIdx) {
							cursorX += glyph->shift;
							gradientX += glyph->shift;
							goto skip_glyph_draw;
						}
						TexturePageItem* glyphTpag = &dw->tpag.items[glyphTpagIdx];
						glyphPageId = glyphTpag->texturePageId;
						if (0 > glyphPageId || dr->textureCount <= (uint32_t)glyphPageId) {
							cursorX += glyph->shift;
							gradientX += glyph->shift;
							goto skip_glyph_draw;
						}
						// Use async loading - skip glyph if texture not ready yet
						if (!ensureTexturePageLoadedAsync(dr, (uint32_t)glyphPageId)) {
							cursorX += glyph->shift;
							gradientX += glyph->shift;
							goto skip_glyph_draw;
						}
						if (!dr->textures[glyphPageId]) {
							cursorX += glyph->shift;
							gradientX += glyph->shift;
							goto skip_glyph_draw;
						}

						glyphTexW = (float)dr->textureWidths[glyphPageId];
						glyphTexH = (float)dr->textureHeights[glyphPageId];
						if (glyphTexW <= 0 || glyphTexH <= 0) {
							cursorX += glyph->shift;
							gradientX += glyph->shift;
							goto skip_glyph_draw;
						}

						gU0 = (float)glyphTpag->sourceX / glyphTexW;
						gV0 = (float)glyphTpag->sourceY / glyphTexH;
						gU1 = (float)(glyphTpag->sourceX + glyphTpag->sourceWidth) / glyphTexW;
						gV1 = (float)(glyphTpag->sourceY + glyphTpag->sourceHeight) / glyphTexH;

						// Sprite font Y offset includes the glyph's targetY and spriteOriginYAdjust
						localYOff = cursorY + (float)(int32_t)glyphTpag->targetY - (float)font->spriteOriginYAdjust;

						// Switch texture if this glyph uses a different page
						ensureTexture(dr, glyphPageId);
					}

					// Local quad position
					float localX0 = cursorX + glyph->offset;
					float localY0 = localYOff;
					float localX1 = localX0 + (float)glyph->sourceWidth;
					float localY1 = localY0 + (float)glyph->sourceHeight;

					// Scale
					float sx0 = localX0 * fontScaleX;
					float sy0 = localY0 * fontScaleY;
					float sx1 = localX1 * fontScaleX;
					float sy1 = localY1 * fontScaleY;

					// Build 4 corners (with optional rotation)
					float cx[4], cy[4];
					if (hasRotation) {
						float lx[4] = { sx0, sx1, sx1, sx0 };
						float ly[4] = { sy0, sy0, sy1, sy1 };
						for (int i = 0; i < 4; i++) {
							cx[i] = lx[i] * cosA - ly[i] * sinA;
							cy[i] = lx[i] * sinA + ly[i] * cosA;
						}
					} else {
						cx[0] = sx0;
						cy[0] = sy0;
						cx[1] = sx1;
						cy[1] = sy0;
						cx[2] = sx1;
						cy[2] = sy1;
						cx[3] = sx0;
						cy[3] = sy1;
					}

					SpriteVertex* v = allocQuad(dr);
					float screenX, screenY;
					for (int i = 0; i < 4; i++) {
						transformPoint(dr, x + cx[i], y + cy[i], &screenX, &screenY);
						v[i].x = screenX - 0.5f;
						v[i].y = screenY - 0.5f;
						v[i].z = 0.0f;
						v[i].w = 1.0f;
					}
					// Per-vertex colors: TL=0, TR=1, BR=2, BL=3
					v[0].r = vTLr;
					v[0].g = vTLg;
					v[0].b = vTLb;
					v[0].a = vTLa;
					v[1].r = vTRr;
					v[1].g = vTRg;
					v[1].b = vTRb;
					v[1].a = vTRa;
					v[2].r = vBRr;
					v[2].g = vBRg;
					v[2].b = vBRb;
					v[2].a = vBRa;
					v[3].r = vBLr;
					v[3].g = vBLg;
					v[3].b = vBLb;
					v[3].a = vBLa;
					v[0].u = gU0;
					v[0].v = gV0;
					v[1].u = gU1;
					v[1].v = gV0;
					v[2].u = gU1;
					v[2].v = gV1;
					v[3].u = gU0;
					v[3].v = gV1;
				}
			skip_glyph_draw:;

				// Advance cursor (shift + kerning)
				cursorX += glyph->shift;
				gradientX += glyph->shift;
				if (drawGlyph && hasNext) {
					float kern = TextUtils_getKerningOffset(glyph, nextCh);
					cursorX += kern;
					gradientX += kern;
				}
			}

			ch = nextCh;
			hasCh = hasNext;
		}

		cursorY += lineStride;

		// Advance past the newline
		if (textLen > lineEnd) {
			lineStart = TextUtils_skipNewline(processed, lineEnd, textLen);
		} else {
			lineStart = lineEnd;
		}
	}

	PreprocessedText_free(processedText);
}

static void d3d9DrawText(Renderer* renderer, const char* text, float x, float y,
						 float xscale, float yscale, float angleDeg, float lineSeparation) {
	uint32_t col = renderer->drawColor;
	d3d9DrawTextInternal(renderer, text, x, y, xscale, yscale, angleDeg,
						 col, col, col, col, renderer->drawAlpha, lineSeparation);
}

static void d3d9DrawTextColor(Renderer* renderer, const char* text, float x, float y,
							  float xscale, float yscale, float angleDeg,
							  int32_t c1, int32_t c2, int32_t c3, int32_t c4,
							  float alpha, float lineSeparation) {
	d3d9DrawTextInternal(renderer, text, x, y, xscale, yscale, angleDeg,
						 (uint32_t)c1, (uint32_t)c2, (uint32_t)c3, (uint32_t)c4,
						 alpha, lineSeparation);
}

static void d3d9Flush(Renderer* renderer) {
	flushBatch((D3D9Renderer*)renderer);
}

static void d3d9ClearScreen(Renderer* renderer, uint32_t color, float alpha) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	flushBatch(dr);
	uint8_t r = (uint8_t)(color & 0xFF);
	uint8_t g = (uint8_t)((color >> 8) & 0xFF);
	uint8_t b = (uint8_t)((color >> 16) & 0xFF);
	uint8_t a = (uint8_t)(alpha * 255.0f);
	Dev(dr)->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(a, r, g, b), 1.0f, 0);
}

// Dynamic sprite creation parity with the GL backend.
// Reads pixels from `surfaceID`/rect and creates:
// - a dynamic TXTR page texture
// - a dynamic TPAG entry
// - a dynamic sprite slot in dw->sprt
static int32_t d3d9CreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y,
										   int32_t w, int32_t h, bool removeback,
										   bool smooth, int32_t xorig, int32_t yorig) {
	(void)removeback;
	(void)smooth;

	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	DataWin* dw = renderer->dataWin;
	if (!dw) {
		return -1;
	}

	if (0 > w || 0 > h || 0 == w || 0 == h) {
		return -1;
	}

	if (0 > surfaceID) {
		return -1;
	}
	if (surfaceID != APPLICATION_SURFACE_ID && ((uint32_t)surfaceID >= dr->surfaceCount || !dr->surfaces[surfaceID])) {
		return -1;
	}

	// Read pixels from the source surface into CPU RGBA.
	// d3d9SurfaceGetPixels expects outRGBA as RGBA.
	int32_t srcW = w;
	int32_t srcH = h;
	uint8_t* rgba = (uint8_t*)safeMalloc((size_t)srcW * (size_t)srcH * 4);
	if (!rgba) {
		return -1;
	}

	// We can only reuse d3d9SurfaceGetPixels when it copies whole surfaces.
	// Use surface_copy into a temporary staging surface for the sub-rect.
	int32_t tempSurface = d3d9CreateSurface(renderer, srcW, srcH);
	if (0 > tempSurface) {
		free(rgba);
		return -1;
	}

	// Copy the requested rect into tempSurface.
	d3d9SurfaceCopy(renderer,
					tempSurface, 0, 0,
					surfaceID, x, y,
					srcW, srcH,
					false);

	bool ok = d3d9SurfaceGetPixels(renderer, tempSurface, rgba);
	// tempSurface is freed after pixels are read.
	d3d9SurfaceFree(renderer, tempSurface);

	if (!ok) {
		free(rgba);
		return -1;
	}

	// Allocate a dynamic renderer-side texture page index for this sprite.
	// This page is not part of dw->txtr; it only exists in dr->textures/* arrays.
	uint32_t pageId = dr->textureCount;

	// Grow renderer texture arrays.
	dr->textures = (void**)safeRealloc(dr->textures, (dr->textureCount + 1) * sizeof(void*));
	dr->textureWidths = (int32_t*)safeRealloc(dr->textureWidths, (dr->textureCount + 1) * sizeof(int32_t));
	dr->textureHeights = (int32_t*)safeRealloc(dr->textureHeights, (dr->textureCount + 1) * sizeof(int32_t));
	dr->textureLastUsedFrame = (uint32_t*)safeRealloc(dr->textureLastUsedFrame, (dr->textureCount + 1) * sizeof(uint32_t));
	// Also grow async arrays
	dr->textureLoadState = (uint8_t*)safeRealloc(dr->textureLoadState, (dr->textureCount + 1) * sizeof(uint8_t));
	dr->texturePendingRGBA = (uint8_t**)safeRealloc(dr->texturePendingRGBA, (dr->textureCount + 1) * sizeof(uint8_t*));
	dr->texturePendingW = (uint32_t*)safeRealloc(dr->texturePendingW, (dr->textureCount + 1) * sizeof(uint32_t));
	dr->texturePendingH = (uint32_t*)safeRealloc(dr->texturePendingH, (dr->textureCount + 1) * sizeof(uint32_t));
	dr->texturePendingByteSize = (uint32_t*)safeRealloc(dr->texturePendingByteSize, (dr->textureCount + 1) * sizeof(uint32_t));
	// Also grow the blob-size array (used by eviction and destruction)
	dr->textureBlobSizes = (uint32_t*)safeRealloc(dr->textureBlobSizes, (dr->textureCount + 1) * sizeof(uint32_t));

	dr->textureLastUsedFrame[pageId] = 0;
	dr->textures[pageId] = nullptr;
	dr->textureWidths[pageId] = 0;
	dr->textureHeights[pageId] = 0;
	dr->textureBlobSizes[pageId] = 0;
	dr->textureLoadState[pageId] = TEX_LOAD_IDLE;
	dr->texturePendingRGBA[pageId] = nullptr;
	dr->texturePendingW[pageId] = 0;
	dr->texturePendingH[pageId] = 0;
	dr->texturePendingByteSize[pageId] = 0;
	dr->textureCount++;

	// Upload captured pixels into a new D3D texture.
	flushBatch(dr);
	IDirect3DDevice9* dev = Dev(dr);

	IDirect3DTexture9* tex = nullptr;
	HRESULT hr = dev->CreateTexture((UINT)srcW, (UINT)srcH, 1, 0, D3D9_GPU_TEXTURE_FORMAT, D3DPOOL_MANAGED, &tex, nullptr);
	if (FAILED(hr) || !tex) {
		free(rgba);
		return -1;
	}

	if (!uploadRgbaToTexture(dev, tex, rgba, srcW, srcH)) {
		tex->Release();
		free(rgba);
		return -1;
	}

	dr->textures[pageId] = tex;
	dr->textureWidths[pageId] = srcW;
	dr->textureHeights[pageId] = srcH;
	dr->textureBlobSizes[pageId] = (uint32_t)(srcW * srcH * 4);
	dr->textureBytesUsed += dr->textureBlobSizes[pageId];

	free(rgba);

	// Allocate a TPAG slot for this sprite in dw->tpag.
	uint32_t tpagIndex = dw->tpag.count;
	dw->tpag.count++;
	dw->tpag.items = (TexturePageItem*)safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
	memset(&dw->tpag.items[tpagIndex], 0, sizeof(TexturePageItem));

	TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
	tpag->sourceX = 0;
	tpag->sourceY = 0;
	tpag->sourceWidth = (uint16_t)srcW;
	tpag->sourceHeight = (uint16_t)srcH;
	tpag->targetX = 0;
	tpag->targetY = 0;
	tpag->targetWidth = (uint16_t)srcW;
	tpag->targetHeight = (uint16_t)srcH;
	tpag->boundingWidth = (uint16_t)srcW;
	tpag->boundingHeight = (uint16_t)srcH;
	tpag->texturePageId = (int16_t)pageId;

	// Allocate a sprite slot.
	uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, dr->originalSpriteCount);
	Sprite* sprite = &dw->sprt.sprites[spriteIndex];
	sprite->width = (uint32_t)srcW;
	sprite->height = (uint32_t)srcH;
	sprite->originX = xorig;
	sprite->originY = yorig;
	sprite->textureCount = 1;

	sprite->tpagIndices = (int32_t*)safeRealloc(sprite->tpagIndices, sizeof(int32_t));
	sprite->tpagIndices[0] = (int32_t)tpagIndex;

	sprite->maskCount = 0;
	sprite->masks = nullptr;

	return (int32_t)spriteIndex;
}

static void d3d9DeleteSprite(Renderer* renderer, int32_t spriteIndex) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (!dr) {
		return;
	}
	if (0 > spriteIndex) {
		return;
	}

	// This backend overloads sprite IDs returned by createSpriteFromSurface.
	// We tagged dynamic sprites by making their tpagIndices point at a
	// renderer-side dynamic texture page (dr->textureCount+ growth), and
	// by appending a TPAG entry at the end of dw->tpag.
	//
	// Delete must be conservative: never touch original data.win sprites.
	DataWin* dw = renderer->dataWin;
	if (!dw) {
		return;
	}
	if ((uint32_t)spriteIndex >= dw->sprt.count) {
		return;
	}

	Sprite* spr = &dw->sprt.sprites[spriteIndex];
	if (spr->textureCount == 0 || !spr->tpagIndices) {
		return;
	}

	// If textureCount != 1, or tpags don't look like dynamic pages, fall back.
	// (We only support dynamic createSpriteFromSurface for now.)
	if (spr->textureCount != 1) {
		return;
	}

	int32_t tpagIndex = spr->tpagIndices[0];
	if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count) {
		return;
	}

	TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
	int32_t pageId = (int32_t)tpag->texturePageId;
	if (pageId < 0) {
		return;
	}
	if ((uint32_t)pageId >= dr->textureCount) {
		return;
	}

	// Heuristic: treat pages beyond original txtr.count as dynamic.
	// (original texture pages are loaded lazily from dw->txtr)
	if ((uint32_t)pageId < dr->originalTexturePageCount) {
		return;
	}

	// Release dynamic D3D texture page.
	flushBatch(dr);
	if (dr->textures[pageId]) {
		((IDirect3DTexture9*)dr->textures[pageId])->Release();
		dr->textures[pageId] = nullptr;
		dr->textureWidths[pageId] = 0;
		dr->textureHeights[pageId] = 0;
		if (dr->textureLastUsedFrame) {
			dr->textureLastUsedFrame[pageId] = 0;
		}
	}

	// Note: we intentionally do not shrink dw->tpag/dw->sprt arrays.
	// We also do not free spr->tpagIndices because it may have been
	// grown via safeRealloc and is owned by dw.
	//
	// We invalidate the TPAG so Renderer_resolveTPAGIndex won't pick it.
	// This mirrors how other backends effectively orphan deleted sprites.
	tpag->texturePageId = -1;
	spr->tpagIndices[0] = -1;
	spr->textureCount = 0;
}

static BlendFactors d3d9GpuGetBlendFactors(Renderer* renderer) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	BlendFactors factors;
	factors.src = dr->sFactor;
	factors.dst = dr->dFactor;
	factors.srcAlpha = dr->sFactorAlpha;
	factors.dstAlpha = dr->dFactorAlpha;
	return factors;
}

static int32_t d3d9GpuGetBlendMode(Renderer* renderer) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	return dr->blendMode;
}

static void d3d9GpuSetBlendMode(Renderer* renderer, int32_t mode) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (!dr) {
		return;
	}

	// Avoid redundant state updates when mode hasn't changed.
	if (dr->blendMode == mode) {
		return;
	}

	dr->renderStateDirty = true;

	IDirect3DDevice9* dev = Dev(dr);
	flushBatch(dr);

	// Mirror GL's GLCommon_blendModeTo* mapping (src/gl_common/gl_common.c)
	//
	// Note: GLCommon only maps the *simple* GameMaker blend modes
	// (bm_normal/add/subtract/reverse_subtract/min/max) to GL blend equation
	// and blend factors. The factor-like bm_* constants (zero/one/src_color/..)
	// are meant for gpuSetBlendModeExt(), but the task requests that
	// d3d9GpuSetBlendMode handles all bm_* defined in renderer.h.

	//
	// For the factor-only modes, we approximate GLCommon_blendModeTo* behavior
	// by treating them as "normal" blend equation (ADD) with SRC/DEST factors
	// derived from the selected factor and its conventional pairing:
	//   - dst factor = ONE_MINUS_SRC_ALPHA where SRC_ALPHA is implied
	//   - otherwise dst factor = ONE (so factors have an effect).
	//
	// If these factor-only modes are used by the game, this keeps the
	// resulting blend state deterministic and close to how D3D interprets
	// the factors.

	// Mirror GLCommon_blendModeTo* mapping (gl_common.c)
	// mode -> equation + (srcFactor, dstFactor)
	//
	// GLCommon_blendModeToEquation:
	//   bm_normal/bm_add/bm_subtract -> ADD
	//   bm_reverse_subtract          -> REVERSE_SUBTRACT
	//   bm_min                       -> MIN
	//   bm_max                       -> ADD
	//
	// GLCommon_blendModeToSFactor:
	//   bm_normal/bm_add/bm_reverse_subtract/bm_max -> SRC_ALPHA
	//   bm_subtract                                  -> ZERO
	//   bm_min                                        -> ONE
	//
	// GLCommon_blendModeToDFactor:
	//   bm_normal -> ONE_MINUS_SRC_ALPHA
	//   bm_add     -> ONE
	//   bm_subtract -> ONE_MINUS_SRC_COLOR
	//   bm_reverse_subtract -> ONE
	//   bm_min     -> ONE
	//   bm_max     -> ONE_MINUS_SRC_COLOR

	DWORD blendOp = D3DBLENDOP_ADD;
	DWORD srcFactorD3D = D3DBLEND_SRCALPHA;
	DWORD dstFactorD3D = D3DBLEND_INVSRCALPHA;

	// These reflect the non-ext "blend mode" contract used by getBlendFactors().
	// GL sets srcAlpha=srcFactor, dstAlpha=dstFactor for simple modes.
	int32_t sFactor = bm_src_alpha;
	int32_t dFactor = bm_inv_src_alpha;
	int32_t sFactorAlpha = sFactor;
	int32_t dFactorAlpha = dFactor;

	switch (mode) {
	case bm_normal:
		// Cs*As + Cd*(1-As)
		d3d9SetNormalBlend(dev, dr);
		dr->blendMode = mode;
		dr->sFactor = sFactor;
		dr->dFactor = dFactor;
		dr->sFactorAlpha = sFactorAlpha;
		dr->dFactorAlpha = dFactorAlpha;
		return;

	case bm_add:
		dr->blendIsNormal = false;
		blendOp = D3DBLENDOP_ADD;
		srcFactorD3D = D3DBLEND_SRCALPHA; // GLCommon: SRC_ALPHA
		dstFactorD3D = D3DBLEND_ONE;	  // GLCommon: ONE
		sFactor = bm_src_alpha;
		dFactor = bm_one;
		sFactorAlpha = sFactor;
		dFactorAlpha = dFactor;
		break;

	case bm_subtract:
		dr->blendIsNormal = false;
		blendOp = D3DBLENDOP_SUBTRACT;
		srcFactorD3D = D3DBLEND_ZERO;		 // GLCommon: ZERO
		dstFactorD3D = D3DBLEND_INVSRCCOLOR; // GLCommon: ONE_MINUS_SRC_COLOR
		sFactor = bm_zero;
		dFactor = bm_inv_src_color;
		sFactorAlpha = sFactor;
		dFactorAlpha = dFactor;
		break;

	case bm_reverse_subtract:
		dr->blendIsNormal = false;
		blendOp = D3DBLENDOP_REVSUBTRACT; // GLCommon: REVERSE_SUBTRACT
		srcFactorD3D = D3DBLEND_SRCALPHA; // GLCommon: SRC_ALPHA
		dstFactorD3D = D3DBLEND_ONE;	  // GLCommon: ONE
		sFactor = bm_src_alpha;
		dFactor = bm_one;
		sFactorAlpha = sFactor;
		dFactorAlpha = dFactor;
		break;

	case bm_min:
		dr->blendIsNormal = false;
		blendOp = D3DBLENDOP_MIN;	 // GLCommon: MIN
		srcFactorD3D = D3DBLEND_ONE; // GLCommon: ONE
		dstFactorD3D = D3DBLEND_ONE; // GLCommon: ONE
		sFactor = bm_one;
		dFactor = bm_one;
		sFactorAlpha = sFactor;
		dFactorAlpha = dFactor;
		break;

	case bm_max:
		dr->blendIsNormal = false;
		blendOp = D3DBLENDOP_ADD;			 // GLCommon: ADD
		srcFactorD3D = D3DBLEND_SRCALPHA;	 // GLCommon: SRC_ALPHA
		dstFactorD3D = D3DBLEND_INVSRCCOLOR; // GLCommon: ONE_MINUS_SRC_COLOR
		sFactor = bm_src_alpha;
		dFactor = bm_inv_src_color;
		sFactorAlpha = sFactor;
		dFactorAlpha = dFactor;
		break;

	default:
		// Factor-only bm_* values are intended for gpuSetBlendModeExt().
		// For compatibility and determinism, fall back to GLCommon "normal".
		d3d9SetNormalBlend(dev, dr);
		dr->blendMode = bm_normal;
		dr->sFactor = bm_src_alpha;
		dr->dFactor = bm_inv_src_alpha;
		dr->sFactorAlpha = bm_src_alpha;
		dr->dFactorAlpha = bm_inv_src_alpha;
		return;
	}

	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_BLENDOP, blendOp);
	dev->SetRenderState(D3DRS_SRCBLEND, (DWORD)srcFactorD3D);
	dev->SetRenderState(D3DRS_DESTBLEND, (DWORD)dstFactorD3D);

	dr->blendMode = mode;
	dr->sFactor = sFactor;
	dr->dFactor = dFactor;
	dr->sFactorAlpha = sFactorAlpha;
	dr->dFactorAlpha = dFactorAlpha;
}

static void d3d9GpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	dr->renderStateDirty = true;
	dr->blendIsNormal = false;
	IDirect3DDevice9* dev = Dev(dr);
	flushBatch(dr);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	dev->SetRenderState(D3DRS_SRCBLEND, gmlBlendFactorToD3D(sfactor));
	dev->SetRenderState(D3DRS_DESTBLEND, gmlBlendFactorToD3D(dfactor));
	// Set separate alpha blend factors if they differ from the color factors
	if (sfactor_alpha != sfactor || dfactor_alpha != dfactor) {
		dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLENDALPHA, gmlBlendFactorToD3D(sfactor_alpha));
		dev->SetRenderState(D3DRS_DESTBLENDALPHA, gmlBlendFactorToD3D(dfactor_alpha));
	} else {
		dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	}
	dr->blendMode = bm_complex;
	dr->sFactor = sfactor;
	dr->dFactor = dfactor;
	dr->sFactorAlpha = sfactor_alpha;
	dr->dFactorAlpha = dfactor_alpha;
}

static void d3d9GpuSetBlendEnable(Renderer* renderer, bool enable) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	dr->renderStateDirty = true;
	dr->blendIsNormal = false;
	flushBatch(dr);
	Dev(dr)->SetRenderState(D3DRS_ALPHABLENDENABLE, enable ? TRUE : FALSE);
	if (!enable) {
		Dev(dr)->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	}
}

static void d3d9GpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	dr->renderStateDirty = true;
	flushBatch(dr);
	Dev(dr)->SetRenderState(D3DRS_ALPHATESTENABLE, enable ? TRUE : FALSE);
	Dev(dr)->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
}

static void d3d9GpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	dr->renderStateDirty = true;
	flushBatch(dr);
	Dev(dr)->SetRenderState(D3DRS_ALPHAREF, (DWORD)ref);
}

static void d3d9GpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	dr->renderStateDirty = true;
	DWORD mask = 0;
	flushBatch(dr);
	if (red) {
		mask |= D3DCOLORWRITEENABLE_RED;
	}
	if (green) {
		mask |= D3DCOLORWRITEENABLE_GREEN;
	}
	if (blue) {
		mask |= D3DCOLORWRITEENABLE_BLUE;
	}
	if (alpha) {
		mask |= D3DCOLORWRITEENABLE_ALPHA;
	}
	Dev(dr)->SetRenderState(D3DRS_COLORWRITEENABLE, mask);
}
static void d3d9GpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	DWORD mask = 0;
	if (dr) {
		Dev(dr)->GetRenderState(D3DRS_COLORWRITEENABLE, &mask);
	}
	if (red) {
		*red = (mask & D3DCOLORWRITEENABLE_RED) != 0;
	}
	if (green) {
		*green = (mask & D3DCOLORWRITEENABLE_GREEN) != 0;
	}
	if (blue) {
		*blue = (mask & D3DCOLORWRITEENABLE_BLUE) != 0;
	}
	if (alpha) {
		*alpha = (mask & D3DCOLORWRITEENABLE_ALPHA) != 0;
	}
}
static bool d3d9GpuGetBlendEnable(Renderer* renderer) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	flushBatch(dr);
	DWORD enabled = TRUE;
	Dev(dr)->GetRenderState(D3DRS_ALPHABLENDENABLE, &enabled);
	return enabled != FALSE;
}
static void d3d9GpuSetFog(Renderer* renderer, bool enable, uint32_t color) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (!dr || (dr->fogEnable == enable && dr->fogColor == color)) {
		return;
	}
	flushBatch(dr);
	// Fog is implemented via the uFogColor uniform in the default pixel shader.
	// uFogColor.rgb = fog color, uFogColor.a = 1.0 if enabled else 0.0.
	// The pixel shader does: c.rgb = lerp(c.rgb, uFogColor.rgb, uFogColor.a).
	// When disabled, alpha is 0 so no fog is applied.
	dr->fogEnable = enable;
	dr->fogColor = color;

	// Apply fog uniform immediately on the render thread.
	// The uniform is at register c0 in the PS, which is a separate register
	// space from the VS c0 (uHalfRes), so no conflict.
	float fogR = (float)(color & 0xFF) / 255.0f;
	float fogG = (float)((color >> 8) & 0xFF) / 255.0f;
	float fogB = (float)((color >> 16) & 0xFF) / 255.0f;
	float fogA = enable ? 1.0f : 0.0f;
	float fogValues[4] = { fogR, fogG, fogB, fogA };
	Dev(dr)->SetPixelShaderConstantF(0, fogValues, 1);
}

// ===[ Dynamic Surface Functions ]===

static int32_t d3d9CreateSurface(Renderer* renderer, int32_t width, int32_t height) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);
	flushBatch(dr);

	if (width <= 0 || height <= 0) {
		return -1;
	}

	uint32_t slot = d3d9FindOrAllocateSurfaceSlot(dr);

	// Round up to multiple of 8 for hardware alignment (matching ensureApplicationSurface & surfaceResize)
	int32_t allocW = (width + 7) & ~7;
	int32_t allocH = (height + 7) & ~7;

	// Create a render-target-capable texture. GetSurfaceLevel(0) provides the surface directly,
	// avoiding a separate CreateRenderTarget allocation (which comes from a more constrained
	// memory pool on Xbox 360 and can fail even when texture memory is available).
	IDirect3DTexture9* tex = nullptr;
	HRESULT hr = dev->CreateTexture((UINT)allocW, (UINT)allocH, 1, D3DUSAGE_RENDERTARGET,
									D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr);
	if (FAILED(hr) || !tex) {
		Butterscotch_xdkDiagTrace("D3D9: surface_create CreateTexture failed %dx%d (alloc=%dx%d) hr=0x%08X", width, height, allocW, allocH, (unsigned)hr);
		return -1;
	}

	// Get the surface level from the texture itself -- no separate render target allocation needed.
	IDirect3DSurface9* surface = nullptr;
	hr = tex->GetSurfaceLevel(0, &surface);
	if (FAILED(hr) || !surface) {
		Butterscotch_xdkDiagTrace("D3D9: surface_create GetSurfaceLevel failed %dx%d (alloc=%dx%d) hr=0x%08X", width, height, allocW, allocH, (unsigned)hr);
		tex->Release();
		return -1;
	}

	dr->surfaces[slot] = surface;
	dr->surfaceTexture[slot] = tex;
	dr->surfaceWidth[slot] = width;
	dr->surfaceHeight[slot] = height;

	Butterscotch_xdkDiagTrace("D3D9: created surface %u size=%dx%d (alloc=%dx%d)", slot, width, height, allocW, allocH);
	return (int32_t)slot;
}

static bool d3d9SurfaceExists(Renderer* renderer, int32_t surfaceID) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	// The application_surface always "exists" (but is managed separately)
	if (surfaceID == APPLICATION_SURFACE_ID) {
		return true;
	}

	if (surfaceID < 0 || (uint32_t)surfaceID >= dr->surfaceCount) {
		return false;
	}
	return dr->surfaces[surfaceID] != nullptr;
}

static bool d3d9SetRenderTarget(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);
	static int logged = 0;

	dr->renderStateDirty = true;

	int32_t viewCurrent = 0;
	if (renderer->runner && renderer->runner->viewsEnabled) {
		viewCurrent = renderer->runner->viewCurrent;
	}
	RuntimeView* view = &renderer->runner->views[viewCurrent];
	renderer->cameraCurrent = view->cameraId;
	GMLCamera* camera = Runner_getCameraById(renderer->runner, renderer->cameraCurrent);

	if (surfaceID == APPLICATION_SURFACE_ID) {
		if (implicitApplicationSurface && dr->savedViewStateValid) {
			flushBatch(dr);
			HRESULT hr = dev->SetRenderTarget(0, (IDirect3DSurface9*)dr->appSurfaceLevel);
			if (FAILED(hr)) {
				D3D9_DIAG_LIMITED(64, "D3D9: SetRenderTarget(app_surface implicit) failed hr=0x%08X", (unsigned)hr);
				return false;
			}
			D3DVIEWPORT9 vp;
			vp.X = 0;
			vp.Y = 0;
			vp.Width = (DWORD)dr->appSurfaceW;
			vp.Height = (DWORD)dr->appSurfaceH;
			vp.MinZ = 0.0f;
			vp.MaxZ = 1.0f;
			dev->SetViewport(&vp);

			dr->offsetX = dr->savedOffsetX;
			dr->offsetY = dr->savedOffsetY;
			dr->portScaleX = dr->savedPortScaleX;
			dr->portScaleY = dr->savedPortScaleY;
			dr->portOffsetX = dr->savedPortOffsetX;
			dr->portOffsetY = dr->savedPortOffsetY;

			if (camera != nullptr) {
				d3d9ApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
			}

			dr->renderingToApplicationSurface = true;
			dr->appSurfaceResolved = false;
			dr->savedViewStateValid = false;
			return true;
		}

		if (!dr->appSurfaceLevel) {
			return false;
		}
		flushBatch(dr);
		HRESULT hr = dev->SetRenderTarget(0, (IDirect3DSurface9*)dr->appSurfaceLevel);
		if (FAILED(hr)) {
			D3D9_DIAG_LIMITED(64, "D3D9: SetRenderTarget(app_surface) failed hr=0x%08X", (unsigned)hr);
			return false;
		}
		D3DVIEWPORT9 vp;
		vp.X = 0;
		vp.Y = 0;
		vp.Width = (DWORD)dr->appSurfaceW;
		vp.Height = (DWORD)dr->appSurfaceH;
		vp.MinZ = 0.0f;
		vp.MaxZ = 1.0f;
		dev->SetViewport(&vp);
		dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		dr->renderingToApplicationSurface = true;
		dr->appSurfaceResolved = false;
		setApplicationSurfaceTransform(dr);
		if (camera != nullptr) {
			d3d9ApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
		}
		return true;
	}

	if (dr->renderingToApplicationSurface) {
		dr->savedOffsetX = dr->offsetX;
		dr->savedOffsetY = dr->offsetY;
		dr->savedPortScaleX = dr->portScaleX;
		dr->savedPortScaleY = dr->portScaleY;
		dr->savedPortOffsetX = dr->portOffsetX;
		dr->savedPortOffsetY = dr->portOffsetY;
		dr->savedViewStateValid = true;
	}

	if (surfaceID < 0 || (uint32_t)surfaceID >= dr->surfaceCount || !dr->surfaces[surfaceID]) {
		D3D9_DIAG_LIMITED(32, "D3D9: surface_set_target invalid id=%d", surfaceID);
		return false;
	}

	flushBatch(dr);
	HRESULT hr = dev->SetRenderTarget(0, (IDirect3DSurface9*)dr->surfaces[surfaceID]);
	if (FAILED(hr)) {
		D3D9_DIAG_LIMITED(64, "D3D9: SetRenderTarget(surface %d) failed hr=0x%08X", surfaceID, (unsigned)hr);
		return false;
	}

	D3DVIEWPORT9 vp;
	vp.X = 0;
	vp.Y = 0;
	vp.Width = (DWORD)dr->surfaceWidth[surfaceID];
	vp.Height = (DWORD)dr->surfaceHeight[surfaceID];
	vp.MinZ = 0.0f;
	vp.MaxZ = 1.0f;
	dev->SetViewport(&vp);
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

	dr->renderingToApplicationSurface = false;
	dr->offsetX = 0.0f;
	dr->offsetY = 0.0f;
	dr->portScaleX = 1.0f;
	dr->portScaleY = 1.0f;
	dr->portOffsetX = 0.0f;
	dr->portOffsetY = 0.0f;

	// Check if this surface belongs to the current view
	if (view != nullptr && surfaceID == view->surfaceId) {
		if (camera != nullptr) {
			d3d9ApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
		}
	} else {
		// Surface doesn't belong to the current view — use SURFACE_CAMERA
		renderer->cameraCurrent = SURFACE_CAMERA;
		GMLCamera* surfCamera = &renderer->runner->surfaceCamera;
		surfCamera->allocated = true;
		surfCamera->viewX = 0.0f;
		surfCamera->viewY = 0.0f;
		surfCamera->viewWidth = dr->surfaceWidth[surfaceID];
		surfCamera->viewHeight = dr->surfaceHeight[surfaceID];
		surfCamera->borderX = 0;
		surfCamera->borderY = 0;
		surfCamera->speedX = 0;
		surfCamera->speedY = 0;
		surfCamera->objectId = -1;
		surfCamera->viewAngle = 0;
		Runner_updateCameraViewSimple(surfCamera);
		d3d9ApplyProjection(renderer, &surfCamera->viewMatrix, &surfCamera->projectionMatrix);
	}

	return true;
}

static int32_t d3d9EnsureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);
	// The runner tracks the authoritative application surface size via
	// applicationWidth/applicationHeight, which are updated by GML's
	// surface_resize(application_surface, ...) calls. Use those instead of
	// the passed width/height (which come from data.win's defaultWindowWidth/Height)
	// so widescreen mod resizes persist across room transitions.
	Runner* r = renderer->runner;
	if (r != nullptr && r->applicationWidth > 0 && r->applicationHeight > 0 &&
		(r->applicationWidth != width || r->applicationHeight != height)) {
		width = r->applicationWidth;
		height = r->applicationHeight;
	}
	int32_t allocW = (width + 7) & ~7;
	int32_t allocH = (height + 7) & ~7;
	if (dr->appSurfaceTexture && dr->appSurfaceW == width && dr->appSurfaceH == height &&
		dr->appSurfaceAllocW == allocW && dr->appSurfaceAllocH == allocH) {
		return APPLICATION_SURFACE_ID;
	}

	releaseApplicationSurface(dr);

	IDirect3DTexture9* sampleTex = nullptr;

#ifdef PLATFORM_XBOX360_XDK
	HRESULT hr = dev->CreateTexture((UINT)allocW, (UINT)allocH, 1, 0, D3DFMT_A8R8G8B8,
									D3DPOOL_DEFAULT, &sampleTex, nullptr);
#else
	// Create a render-target-capable texture. GetSurfaceLevel(0) on this
	// texture returns a surface that can be used as a render target directly,
	// eliminating the need for a separate CreateRenderTarget + StretchRect
	// resolve step (which fails on DXVK).
	HRESULT hr = dev->CreateTexture((UINT)allocW, (UINT)allocH, 1,
									D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
									D3DPOOL_DEFAULT, &sampleTex, nullptr);
#endif
	if (FAILED(hr) || !sampleTex) {
		Butterscotch_xdkDiagTrace("D3D9: CreateTexture(app) failed %dx%d hr=0x%08X", allocW, allocH, (unsigned)hr);
		return APPLICATION_SURFACE_ID;
	}

	IDirect3DSurface9* surface = nullptr;
#ifdef PLATFORM_XBOX360_XDK
	hr = dev->CreateRenderTarget((UINT)allocW, (UINT)allocH, D3DFMT_A8R8G8B8,
								 D3DMULTISAMPLE_NONE, 0, FALSE, &surface, nullptr);
#else
	// Get the surface level from the render-target texture itself
	hr = sampleTex->GetSurfaceLevel(0, &surface);
	if (FAILED(hr) || !surface) {
		Butterscotch_xdkDiagTrace("D3D9: GetSurfaceLevel(app) failed hr=0x%08X", (unsigned)hr);
		sampleTex->Release();
		return APPLICATION_SURFACE_ID;
	}
#endif

	dr->appSurfaceTexture = sampleTex;
	dr->appRenderTexture = nullptr;
	dr->appSurfaceLevel = surface;
	dr->appSurfaceW = width;
	dr->appSurfaceH = height;
	dr->appSurfaceAllocW = allocW;
	dr->appSurfaceAllocH = allocH;
	dr->appSurfaceResolved = false;
	Butterscotch_xdkDiagTrace("D3D9: application_surface created %dx%d alloc=%dx%d", width, height, allocW, allocH);
	return APPLICATION_SURFACE_ID;
}

static float d3d9GetSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (surfaceID == APPLICATION_SURFACE_ID) {
		return (float)dr->appSurfaceW;
	}
	if (surfaceID >= 0 && (uint32_t)surfaceID < dr->surfaceCount && dr->surfaces[surfaceID]) {
		return (float)dr->surfaceWidth[surfaceID];
	}
	return 0.0f;
}

static float d3d9GetSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (surfaceID == APPLICATION_SURFACE_ID) {
		return (float)dr->appSurfaceH;
	}
	if (surfaceID >= 0 && (uint32_t)surfaceID < dr->surfaceCount && dr->surfaces[surfaceID]) {
		return (float)dr->surfaceHeight[surfaceID];
	}
	return 0.0f;
}

static void d3d9DrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop,
							int32_t srcWidth, int32_t srcHeight, float x, float y,
							float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	// Resolve the texture to draw
	IDirect3DTexture9* drawTex = nullptr;
	int32_t texW = 0, texH = 0;

	if (surfaceID == APPLICATION_SURFACE_ID) {
		if (!dr->appSurfaceTexture) {
			D3D9_DIAG_LIMITED(64, "D3D9: draw_surface no app surface id=%d", surfaceID);
			return;
		}
		drawTex = (IDirect3DTexture9*)dr->appSurfaceTexture;
		texW = dr->appSurfaceAllocW;
		texH = dr->appSurfaceAllocH;
	} else if (surfaceID >= 0 && (uint32_t)surfaceID < dr->surfaceCount && dr->surfaceTexture[surfaceID]) {
		drawTex = (IDirect3DTexture9*)dr->surfaceTexture[surfaceID];
		texW = (dr->surfaceWidth[surfaceID] + 7) & ~7;
		texH = (dr->surfaceHeight[surfaceID] + 7) & ~7;
	} else {
		D3D9_DIAG_LIMITED(128, "D3D9: draw_surface unsupported id=%d src=%d,%d %dx%d",
						  surfaceID, srcLeft, srcTop, srcWidth, srcHeight);
		return;
	}

	if (!drawTex || texW <= 0 || texH <= 0) {
		return;
	}

	// Handle application surface switching from render target mode to draw mode
	if (surfaceID == APPLICATION_SURFACE_ID && dr->renderingToApplicationSurface) {
		static int switchLogged = 0;
		flushBatch(dr);
		resolveApplicationSurface(dr);
		bindBackbuffer(dr);
		resetFullBackbufferState(dr);
		dr->renderingToApplicationSurface = false;
		// When the application surface is drawn manually by GML (appSurfaceAutoDraw=0),
		// the game positions it in game-space coordinates (e.g., draw_surface_stretched
		// to fill the room). Use the game-to-screen transform to map those coordinates
		// to the backbuffer, preserving aspect ratio.
		// For auto-draw, setWindowSurfaceTransform would be used, but in the manual
		// path the GML code handles stretching the app surface to the game frame,
		// and setGameTargetTransform correctly maps the game frame to the screen.
		setGameTargetTransform(dr);
		Dev(dr)->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
		dr->samplerStateApplied = false;
		applyPointSampling(Dev(dr), dr);
	}

	// Resolve multi-sampled application surface to its texture
	if (surfaceID == APPLICATION_SURFACE_ID) {
		resolveApplicationSurface(dr);
	}

	if (srcWidth < 0 || srcHeight < 0) {
		srcLeft = 0;
		srcTop = 0;
		if (surfaceID == APPLICATION_SURFACE_ID) {
			srcWidth = dr->appSurfaceW;
			srcHeight = dr->appSurfaceH;
		} else {
			srcWidth = dr->surfaceWidth[surfaceID];
			srcHeight = dr->surfaceHeight[surfaceID];
		}
	}
	if (srcWidth <= 0 || srcHeight <= 0) {
		return;
	}

	if (dr->quadCount > 0) {
		flushBatch(dr);
	}

	float fTexW = (float)texW;
	float fTexH = (float)texH;
	float u0 = (float)srcLeft / fTexW;
	float v0 = (float)srcTop / fTexH;
	float u1 = (float)(srcLeft + srcWidth) / fTexW;
	float v1 = (float)(srcTop + srcHeight) / fTexH;

	float drawW = (float)srcWidth * xscale;
	float drawH = (float)srcHeight * yscale;
	float qx[4] = { x, x + drawW, x + drawW, x };
	float qy[4] = { y, y, y + drawH, y + drawH };
	float cx[4], cy[4];
	if (angleDeg != 0.0f) {
		float rad = -angleDeg * (3.14159265f / 180.0f);
		float cosA = cosf(rad);
		float sinA = sinf(rad);
		for (int i = 0; i < 4; i++) {
			float dx = qx[i] - x;
			float dy = qy[i] - y;
			cx[i] = cosA * dx - sinA * dy + x;
			cy[i] = sinA * dx + cosA * dy + y;
		}
	} else {
		for (int i = 0; i < 4; i++) {
			cx[i] = qx[i];
			cy[i] = qy[i];
		}
	}

	float cr, cg, cb, ca;
	bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

	// Route through the batch system so the surface quad is drawn via the same
	// pipeline as sprites (shaders, viewport, etc.) and consecutive surface
	// draws with the same texture can be batched into a single DrawPrimitiveUP.
	dr->batchSurfaceTex = drawTex;
	{
		SpriteVertex* v = allocQuad(dr);
		float sx, sy;
		transformPoint(dr, cx[0], cy[0], &sx, &sy);
		setVertex(&v[0], sx, sy, u0, v0, cr, cg, cb, ca);
		transformPoint(dr, cx[1], cy[1], &sx, &sy);
		setVertex(&v[1], sx, sy, u1, v0, cr, cg, cb, ca);
		transformPoint(dr, cx[2], cy[2], &sx, &sy);
		setVertex(&v[2], sx, sy, u1, v1, cr, cg, cb, ca);
		transformPoint(dr, cx[3], cy[3], &sx, &sy);
		setVertex(&v[3], sx, sy, u0, v1, cr, cg, cb, ca);
	}
	// Invalidate so the next ensureTexture always flushes (avoids
	// mixing the surface quad with subsequent sprites in the same batch).
	dr->currentTextureIndex = -1;
}

extern int32_t* gGameW;
extern int32_t* gGameH;

static void d3d9SurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);

	if (surfaceID == APPLICATION_SURFACE_ID) {
		Butterscotch_xdkDiagTrace("D3D9: application_surface resize requested %dx%d old=%dx%d room=%d",
								  width, height, dr->appSurfaceW, dr->appSurfaceH,
								  renderer->runner ? renderer->runner->currentRoomIndex : -1);
		if (width > 0 && height > 0 && (width != dr->appSurfaceW || height != dr->appSurfaceH)) {
			// Release the old surface and recreate at the requested dimensions.
			// This is needed for widescreen mods that call surface_resize(application_surface, ...)
			// during room initialization to change the app surface size.
			releaseApplicationSurface(dr);
			// Recreate using the same logic as ensureApplicationSurface but with
			// the new requested size instead of the game's default window size.
			flushBatch(dr);
			int32_t allocW = (width + 7) & ~7;
			int32_t allocH = (height + 7) & ~7;
			IDirect3DTexture9* sampleTex = nullptr;
#ifdef PLATFORM_XBOX360_XDK
			HRESULT hr = dev->CreateTexture((UINT)allocW, (UINT)allocH, 1, 0, D3DFMT_A8R8G8B8,
											D3DPOOL_DEFAULT, &sampleTex, nullptr);
#else
			// Create a render-target-capable texture. GetSurfaceLevel(0) on this
			// texture returns a surface that can be used as a render target directly,
			// eliminating the need for a separate CreateRenderTarget + StretchRect
			// resolve step (which fails on DXVK).
			HRESULT hr = dev->CreateTexture((UINT)allocW, (UINT)allocH, 1,
											D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
											D3DPOOL_DEFAULT, &sampleTex, nullptr);
#endif
			if (FAILED(hr) || !sampleTex) {
				Butterscotch_xdkDiagTrace("D3D9: surface_resize CreateTexture(app) failed %dx%d hr=0x%08X", allocW, allocH, (unsigned)hr);
				return;
			}

			IDirect3DSurface9* surface = nullptr;
#ifdef PLATFORM_XBOX360_XDK
			hr = dev->CreateRenderTarget((UINT)allocW, (UINT)allocH, D3DFMT_A8R8G8B8,
										 D3DMULTISAMPLE_NONE, 0, FALSE, &surface, nullptr);
#else
			// Get the surface level from the render-target texture itself
			hr = sampleTex->GetSurfaceLevel(0, &surface);
			if (FAILED(hr) || !surface) {
				Butterscotch_xdkDiagTrace("D3D9: surface_resize GetSurfaceLevel(app) failed hr=0x%08X", (unsigned)hr);
				sampleTex->Release();
				return;
			}
#endif
			dr->appSurfaceTexture = sampleTex;
			dr->appRenderTexture = nullptr;
			dr->appSurfaceLevel = surface;
			dr->appSurfaceW = width;
			dr->appSurfaceH = height;
			dr->appSurfaceAllocW = allocW;
			dr->appSurfaceAllocH = allocH;
			dr->appSurfaceResolved = false;
			*gGameW = width;
			*gGameH = height;
			Butterscotch_xdkDiagTrace("D3D9: application_surface recreated %dx%d alloc=%dx%d", width, height, allocW, allocH);
		}
		return;
	}

	// Dynamic surface resize (re-create render target and texture)
	if (surfaceID < 0 || (uint32_t)surfaceID >= dr->surfaceCount || !dr->surfaces[surfaceID]) {
		return;
	}

	// Round up to multiple of 8 for alignment (matching D3D9 expectations)
	int32_t allocW = (width + 7) & ~7;
	int32_t allocH = (height + 7) & ~7;

	flushBatch(dr);

	// Release old resources
	d3d9ReleaseSurfaceSlot(dr, (uint32_t)surfaceID);

	// Create a render-target-capable texture (same pattern as d3d9CreateSurface)
	IDirect3DTexture9* tex = nullptr;
	HRESULT hr = dev->CreateTexture((UINT)allocW, (UINT)allocH, 1, D3DUSAGE_RENDERTARGET,
									D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr);
	if (FAILED(hr) || !tex) {
		Butterscotch_xdkDiagTrace("D3D9: surface_resize CreateTexture failed %dx%d (alloc=%dx%d) hr=0x%08X", width, height, allocW, allocH, (unsigned)hr);
		return;
	}

	// Get the surface level from the texture itself
	IDirect3DSurface9* surface = nullptr;
	hr = tex->GetSurfaceLevel(0, &surface);
	if (FAILED(hr) || !surface) {
		Butterscotch_xdkDiagTrace("D3D9: surface_resize GetSurfaceLevel failed %dx%d (alloc=%dx%d) hr=0x%08X", width, height, allocW, allocH, (unsigned)hr);
		tex->Release();
		return;
	}

	dr->surfaces[surfaceID] = surface;
	dr->surfaceTexture[surfaceID] = tex;
	dr->surfaceWidth[surfaceID] = width;
	dr->surfaceHeight[surfaceID] = height;

	Butterscotch_xdkDiagTrace("D3D9: surface resize %d to %dx%d (alloc=%dx%d)", surfaceID, width, height, allocW, allocH);
}

static void d3d9SurfaceFree(Renderer* renderer, int32_t surfaceID) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	// Application surface is managed separately; surface_free for it is a no-op
	if (surfaceID == APPLICATION_SURFACE_ID) {
		return;
	}

	if (surfaceID < 0 || (uint32_t)surfaceID >= dr->surfaceCount || !dr->surfaces[surfaceID]) {
		return;
	}

	flushBatch(dr);
	d3d9ReleaseSurfaceSlot(dr, (uint32_t)surfaceID);
	dr->currentTextureIndex = -2; // Invalidate cached texture since we might be freeing it
	Butterscotch_xdkDiagTrace("D3D9: surface_free %d", surfaceID);
}

static void d3d9SurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY,
							int32_t srcSurfaceID, int32_t srcX, int32_t srcY,
							int32_t srcW, int32_t srcH, bool part) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);

	// Resolve source and destination surfaces
	IDirect3DSurface9* srcSurf = nullptr;
	IDirect3DSurface9* dstSurf = nullptr;

	if (srcSurfaceID == APPLICATION_SURFACE_ID) {
		if (!dr->appSurfaceLevel) {
			return;
		}
		resolveApplicationSurface(dr);
		srcSurf = (IDirect3DSurface9*)dr->appSurfaceLevel;
	} else if (srcSurfaceID >= 0 && (uint32_t)srcSurfaceID < dr->surfaceCount && dr->surfaces[srcSurfaceID]) {
		srcSurf = (IDirect3DSurface9*)dr->surfaces[srcSurfaceID];
	} else {
		return;
	}

	if (destSurfaceID == APPLICATION_SURFACE_ID) {
		if (!dr->appSurfaceLevel) {
			return;
		}
		dstSurf = (IDirect3DSurface9*)dr->appSurfaceLevel;
	} else if (destSurfaceID >= 0 && (uint32_t)destSurfaceID < dr->surfaceCount && dr->surfaces[destSurfaceID]) {
		dstSurf = (IDirect3DSurface9*)dr->surfaces[destSurfaceID];
	} else {
		return;
	}

	flushBatch(dr);
	// Xbox 360 does not have StretchRect; use D3DXLoadSurfaceFromSurface instead.
	RECT srcRect;
	srcRect.left = srcX;
	srcRect.top = srcY;
	srcRect.right = srcX + srcW;
	srcRect.bottom = srcY + srcH;
	RECT dstRect;
	dstRect.left = destX;
	dstRect.top = destY;

	// When part==true, callers intend a partial blit; when part==false,
	// they expect the entire source to be copied (handled below by
	// overriding srcRect/dstRect with full-surface desc values).
	dstRect.right = destX + (part ? srcW : srcW);
	dstRect.bottom = destY + (part ? srcH : srcH);

	if (!part) {
		// When part == false, ignore src rect and copy whole src using logical dimensions
		int32_t srcLogicalW, srcLogicalH;
		if (srcSurfaceID == APPLICATION_SURFACE_ID) {
			srcLogicalW = dr->appSurfaceW;
			srcLogicalH = dr->appSurfaceH;
		} else {
			srcLogicalW = dr->surfaceWidth[srcSurfaceID];
			srcLogicalH = dr->surfaceHeight[srcSurfaceID];
		}
		srcRect.left = 0;
		srcRect.top = 0;
		srcRect.right = srcLogicalW;
		srcRect.bottom = srcLogicalH;
		dstRect.left = destX;
		dstRect.top = destY;
		dstRect.right = destX + srcLogicalW;
		dstRect.bottom = destY + srcLogicalH;
	}
	D3DXLoadSurfaceFromSurface(dstSurf, nullptr, &dstRect, srcSurf, nullptr, &srcRect, D3DX_FILTER_POINT, 0);
}

static bool d3d9SurfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	IDirect3DDevice9* dev = Dev(dr);
	flushBatch(dr);

	int32_t w = 0, h = 0;

	// Resolve source surface
	IDirect3DSurface9* srcSurf = nullptr;

	if (surfaceID == APPLICATION_SURFACE_ID) {
		if (!dr->appSurfaceLevel) {
			return false;
		}
		resolveApplicationSurface(dr);
		srcSurf = (IDirect3DSurface9*)dr->appSurfaceLevel;
		w = dr->appSurfaceW;
		h = dr->appSurfaceH;
	} else if (surfaceID >= 0 && (uint32_t)surfaceID < dr->surfaceCount && dr->surfaces[surfaceID]) {
		srcSurf = (IDirect3DSurface9*)dr->surfaces[surfaceID];
		w = dr->surfaceWidth[surfaceID];
		h = dr->surfaceHeight[surfaceID];
	} else {
		return false;
	}

	if (!srcSurf || w <= 0 || h <= 0) {
		return false;
	}

	// Create a system-memory texture to receive the copy
	IDirect3DTexture9* resolveTex = nullptr;
	HRESULT hr = dev->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8,
									D3DPOOL_SYSTEMMEM, &resolveTex, nullptr);
	if (FAILED(hr) || !resolveTex) {
		Butterscotch_xdkDiagTrace("D3D9: surface_get_pixels CreateTexture(sysmem) failed hr=0x%08X", (unsigned)hr);
		return false;
	}

	// Get the system-memory texture's surface level for the copy target
	IDirect3DSurface9* staging = nullptr;
	resolveTex->GetSurfaceLevel(0, &staging);

	// Copy from the GPU render target surface to the system-memory surface
	// Xbox 360 has D3DXLoadSurfaceFromSurface but not D3DXLoadSurfaceFromTexture
	RECT srcRect = { 0, 0, w, h };
	RECT dstRect = { 0, 0, w, h };
	hr = D3DXLoadSurfaceFromSurface(staging, nullptr, &dstRect,
									srcSurf, nullptr, &srcRect,
									D3DX_FILTER_POINT, 0);
	staging->Release();

	if (FAILED(hr)) {
		Butterscotch_xdkDiagTrace("D3D9: surface_get_pixels D3DXLoadSurfaceFromSurface failed hr=0x%08X", (unsigned)hr);
		resolveTex->Release();
		return false;
	}

	// Lock and read pixels from the system memory texture
	D3DLOCKED_RECT lr;
	hr = resolveTex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY);
	if (FAILED(hr)) {
		Butterscotch_xdkDiagTrace("D3D9: surface_get_pixels LockRect failed hr=0x%08X", (unsigned)hr);
		resolveTex->Release();
		return false;
	}

	// Read D3DFMT_A8R8G8B8 back to RGBA, handling endianness
	for (int y2 = 0; y2 < h; y2++) {
		uint8_t* src = (uint8_t*)lr.pBits + y2 * lr.Pitch;
		uint8_t* dst = outRGBA + y2 * w * 4;
		for (int x2 = 0; x2 < w; x2++) {
			// A8R8G8B8 in little-endian memory: byte[0]=B, byte[1]=G, byte[2]=R, byte[3]=A
			// On big-endian (360), D3DXLoadSurfaceFromSurface byte-swaps the data
			// On little-endian (desktop), bytes are already B,G,R,A
#ifdef PLATFORM_XBOX360_XDK
			// D3DXLoadSurfaceFromSurface writes A8R8G8B8 in big-endian byte order on PowerPC
			// So byte[0]=A, byte[1]=R, byte[2]=G, byte[3]=B
			dst[x2 * 4 + 0] = src[x2 * 4 + 1]; // R
			dst[x2 * 4 + 1] = src[x2 * 4 + 2]; // G
			dst[x2 * 4 + 2] = src[x2 * 4 + 3]; // B
			dst[x2 * 4 + 3] = src[x2 * 4 + 0]; // A
#else
			// Little-endian: byte[0]=B, byte[1]=G, byte[2]=R, byte[3]=A
			dst[x2 * 4 + 0] = src[x2 * 4 + 2]; // R
			dst[x2 * 4 + 1] = src[x2 * 4 + 1]; // G
			dst[x2 * 4 + 2] = src[x2 * 4 + 0]; // B
			dst[x2 * 4 + 3] = src[x2 * 4 + 3]; // A
#endif
		}
	}

	resolveTex->UnlockRect(0);
	resolveTex->Release();

	D3D9_DIAG_LIMITED(16, "D3D9: surface_get_pixels %d (%dx%d) ok", surfaceID, w, h);
	return true;
}

static void d3d9DrawTiledPart(Renderer* renderer, int32_t tpagIndex, int32_t srcX, int32_t srcY,
							  int32_t srcW, int32_t srcH, float dstX, float dstY,
							  float dstW, float dstH, uint32_t color, float alpha) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	// Used by Renderer_nineSliceTileH/V/Tile2D fast path.
	// It must:
	// - tile srcW across dstW and srcH across dstH
	// - for incomplete edge tiles, draw only the remaining sub-rect
	// - respect (already-adjusted) srcX/srcY/srcW/srcH and dstX/dstY/dstW/dstH
	// - not apply mirroring (the fast path only runs for mode != NS_MIRROR)

	if (srcW <= 0 || srcH <= 0) {
		return;
	}
	if (dstW <= 0.0f || dstH <= 0.0f) {
		return;
	}

	// Fast-path semantics are angle=0 and mode != NS_MIRROR.
	// Still support fractional dstW/dstH by clamping last tile sizes.

	if (dr->drawPhase == RENDER_PHASE_POST) {
		D3D9_DIAG_LIMITED(96,
						  "D3D9POST: tiledPart tpag=%d src=%d,%d %dx%d dst=%.2f,%.2f %.2fx%.2f room=%d",
						  tpagIndex, srcX, srcY, srcW, srcH,
						  dstX, dstY, dstW, dstH,
						  renderer->runner ? renderer->runner->currentRoomIndex : -1);
	}

	// Tile grid: steps are (srcW, srcH) in destination space as well,
	// because Renderer_nineSliceAdjustForTiledPart passes adW/adH already
	// in destination pixels with scale applied by the nine-slice computation.
	float y = dstY;
	float remH = dstH;
	int32_t tileRow = 0;
	while (remH > 0.0f) {
		int32_t drawH = (remH < (float)srcH) ? (int32_t)remH : srcH;
		if (drawH <= 0) {
			drawH = 1;
		}

		float x = dstX;
		float remW = dstW;
		int32_t tileCol = 0;
		while (remW > 0.0f) {
			int32_t drawW = (remW < (float)srcW) ? (int32_t)remW : srcW;
			if (drawW <= 0) {
				drawW = 1;
			}

			// drawSpritePart expects src rect in tpag source-page space.
			// We draw the same srcX/srcY for each tile and shrink drawW/drawH for edges.
			d3d9DrawSpritePart(renderer, tpagIndex, srcX, srcY, drawW, drawH,
							   x, y, 1.0f, 1.0f, 0.0f,
							   0.0f, 0.0f,
							   color, alpha);

			x += (float)drawW;
			remW -= (float)drawW;
			tileCol++;
		}

		y += (float)drawH;
		remH -= (float)drawH;
		tileRow++;
	}
}

#ifdef D3D9_DISABLE_SHADERS

// Compile a GML shader on demand (lazy compilation)
static void ensureShaderCompiled(D3D9Renderer* dr, int32_t shaderIndex) {}

static void d3d9GpuSetShader(Renderer* renderer, int32_t shaderIndex) {}

static void d3d9GpuResetShader(Renderer* renderer) {}

static int32_t d3d9ShaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform) { return -1; }

static int32_t d3d9ShaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform) { return -1; }

static void d3d9ShaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4) {}

static void d3d9ShaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4) {}

static void d3d9ShaderSetUniformFArray(Renderer* renderer, int32_t handle, float* values, uint32_t count) {}

#else

// Compile a GML shader on demand (lazy compilation)
static void ensureShaderCompiled(D3D9Renderer* dr, int32_t shaderIndex) {
	if (shaderIndex < 0 || (uint32_t)shaderIndex >= dr->gmlShaderCount) {
		return;
	}
	D3D9GMLShader* gmlShader = &dr->gmlShaders[shaderIndex];
	if (gmlShader->compileAttempted) {
		return;
	}

	gmlShader->compileAttempted = true;

	DataWin* dw = dr->base.dataWin;
	if (!dw || (uint32_t)shaderIndex >= dw->shdr.count) {
		return;
	}

	Shader* shdr = &dw->shdr.shaders[shaderIndex];
	if (!shdr->present) {
		fprintf(stderr, "D3D9: Skipping shader %d because it isn't present!\n", shaderIndex);
		return;
	}

	fprintf(stderr, "D3D9: Compiling %s (lazy)\n", shdr->name);

	// Try native HLSL9 source first
	// (Dont because deltarune has stubbed hlsl9 shaders)
	const char* vertexShaderSource = shdr->hlsl9_Vertex;
	const char* fragmentShaderSource = shdr->hlsl9_Fragment;

	// If no native HLSL9 source, try the shader loader (translated from GLSL ES by preprocessor)
	if (true /*!vertexShaderSource || !fragmentShaderSource*/) {
		if (ShaderLoader_hasData()) {
			vertexShaderSource = ShaderLoader_getVertexSource((uint32_t)shaderIndex);
			fragmentShaderSource = ShaderLoader_getFragmentSource((uint32_t)shaderIndex);
			if (vertexShaderSource && fragmentShaderSource) {
				fprintf(stderr, "D3D9: Using translated HLSL9 for shader %s\n", shdr->name);
			}
		}
	}

	if (!vertexShaderSource || !fragmentShaderSource) {
		fprintf(stderr, "D3D9: Shader %s has no HLSL9 source and no translated source available, skipping\n", shdr->name);
		return;
	}

	IDirect3DDevice9* dev = Dev(dr);
	compileD3D9Program(gmlShader, vertexShaderSource, fragmentShaderSource, dev, shdr->name);
}

static void d3d9GpuSetShader(Renderer* renderer, int32_t shaderIndex) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (shaderIndex < 0 || (uint32_t)shaderIndex >= dr->gmlShaderCount) {
		renderer->currentShader = -1;
		return;
	}

	dr->renderStateDirty = true;

	// Compile on first use
	ensureShaderCompiled(dr, shaderIndex);

	D3D9GMLShader* shader = &dr->gmlShaders[shaderIndex];
	if (!shader->compiled) {
		renderer->currentShader = -1;
		return;
	}

	flushBatch(dr);
	IDirect3DDevice9* dev = Dev(dr);
	setShaders(dr, shader->pVertexShader, shader->pPixelShader);

	// GML shaders output clip-space coordinates (-1 to 1), so we need the
	// viewport transform enabled on Xbox 360. The default pass-through shader
	// uses D3DRS_VIEWPORTENABLE=FALSE for direct pixel mapping.
	setViewportEnable(dr, true);

	// Set built-in uniforms
	D3D9ShaderUniform* gmMatrices = findShaderUniform(shader, "gm_Matrices");
	if (gmMatrices != nullptr) {
		// gm_Matrices is float4x4[5] - 5 matrices, each 4 registers
		for (int m = 0; m < 5; m++) {
			dev->SetVertexShaderConstantF(
				gmMatrices->registerIndex + m * 4,
				renderer->gmlMatrices[m].m,
				4);
		}
	}

	renderer->currentShader = shaderIndex;
}

static void d3d9GpuResetShader(Renderer* renderer) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	flushBatch(dr);
	setShaders(dr, dr->pVertexShader, dr->pPixelShader);

	dr->renderStateDirty = true;

	renderer->currentShader = -1;
}

static int32_t d3d9ShaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (shaderIndex < 0 || (uint32_t)shaderIndex >= dr->gmlShaderCount) {
		return -1;
	}

	// Ensure shader is compiled before accessing uniforms
	ensureShaderCompiled(dr, shaderIndex);

	D3D9GMLShader* shader = &dr->gmlShaders[shaderIndex];
	if (!shader->compiled) {
		return -1;
	}

	D3D9ShaderUniform* u = findShaderUniform(shader, uniform);
	if (u == nullptr) {
		return -1;
	}

	// Return a handle that encodes the uniform index + 1 (0 = invalid)
	return (int32_t)(uintptr_t)(u - shader->uniforms) + 1;
}

static int32_t d3d9ShaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (shaderIndex < 0 || (uint32_t)shaderIndex >= dr->gmlShaderCount) {
		return -1;
	}

	// Ensure shader is compiled before accessing uniforms
	ensureShaderCompiled(dr, shaderIndex);

	D3D9GMLShader* shader = &dr->gmlShaders[shaderIndex];
	if (!shader->compiled) {
		return -1;
	}

	D3D9ShaderUniform* u = findShaderUniform(shader, uniform);
	if (u == nullptr || !u->isSampler) {
		return -1;
	}

	return (int32_t)u->samplerSlot;
}

static void d3d9ShaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (handle <= 0) {
		return;
	}

	int32_t shaderIndex = renderer->currentShader;
	if (shaderIndex < 0 || (uint32_t)shaderIndex >= dr->gmlShaderCount) {
		return;
	}
	D3D9GMLShader* shader = &dr->gmlShaders[shaderIndex];
	if (!shader->compiled) {
		return;
	}

	uint32_t uniformIdx = (uint32_t)(handle - 1);
	if (uniformIdx >= shader->uniformCount) {
		return;
	}
	D3D9ShaderUniform* u = &shader->uniforms[uniformIdx];
	if (u->isSampler) {
		return;
	}

	dr->renderStateDirty = true;

	IDirect3DDevice9* dev = Dev(dr);
	float values[4] = { value1, value2, value3, value4 };
	// Only set the shader stage this uniform belongs to
	if (u->isVertex) {
		dev->SetVertexShaderConstantF(u->registerIndex, values, u->registerCount);
	} else {
		dev->SetPixelShaderConstantF(u->registerIndex, values, u->registerCount);
	}
}

static void d3d9ShaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (handle <= 0) {
		return;
	}

	int32_t shaderIndex = renderer->currentShader;
	if (shaderIndex < 0 || (uint32_t)shaderIndex >= dr->gmlShaderCount) {
		return;
	}
	D3D9GMLShader* shader = &dr->gmlShaders[shaderIndex];
	if (!shader->compiled) {
		return;
	}

	uint32_t uniformIdx = (uint32_t)(handle - 1);
	if (uniformIdx >= shader->uniformCount) {
		return;
	}
	D3D9ShaderUniform* u = &shader->uniforms[uniformIdx];
	if (u->isSampler) {
		return;
	}

	dr->renderStateDirty = true;

	IDirect3DDevice9* dev = Dev(dr);
	float fvalues[4] = { (float)value1, (float)value2, (float)value3, (float)value4 };
	// Only set the shader stage this uniform belongs to
	if (u->isVertex) {
		dev->SetVertexShaderConstantF(u->registerIndex, fvalues, u->registerCount);
	} else {
		dev->SetPixelShaderConstantF(u->registerIndex, fvalues, u->registerCount);
	}
}

static void d3d9ShaderSetUniformFArray(Renderer* renderer, int32_t handle, float* values, uint32_t count) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (handle <= 0) {
		return;
	}

	int32_t shaderIndex = renderer->currentShader;
	if (shaderIndex < 0 || (uint32_t)shaderIndex >= dr->gmlShaderCount) {
		return;
	}
	D3D9GMLShader* shader = &dr->gmlShaders[shaderIndex];
	if (!shader->compiled) {
		return;
	}

	uint32_t uniformIdx = (uint32_t)(handle - 1);
	if (uniformIdx >= shader->uniformCount) {
		return;
	}
	D3D9ShaderUniform* u = &shader->uniforms[uniformIdx];
	if (u->isSampler) {
		return;
	}

	dr->renderStateDirty = true;

	IDirect3DDevice9* dev = Dev(dr);
	if (u->isVertex) {
		dev->SetVertexShaderConstantF(u->registerIndex, values, u->registerCount);
	} else {
		dev->SetPixelShaderConstantF(u->registerIndex, values, u->registerCount);
	}
}

#endif

// Texture handle encoding contract (matches GL):
// - 0 means "no texture"
// - for sprites: texID = (tpagIndex + 1)
// - for surfaces: texID = SURFACE_TEXTURE_FLAG | surfaceID
// NOTE: GL uses `GL_SURFACE_TEXTURE_FLAG` in gl_renderer.c, but the shared contract
// here is: top bit marks a surface-handle.
static const uint32_t D3D9_SURFACE_TEXTURE_FLAG = 0x80000000u;

static uint32_t d3d9SpriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
	(void)renderer;
	return (uint32_t)(tpagIndex + 1);
}

static float d3d9TextureGetTexelWidth(Renderer* renderer, uint32_t texID) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	// 0 means "no texture".
	if (texID == 0) {
		return 1.0f;
	}

	// Surface handles: texID = D3D9_SURFACE_TEXTURE_FLAG | surfaceID
	if ((texID & D3D9_SURFACE_TEXTURE_FLAG) != 0) {
		uint32_t sid = (texID & ~D3D9_SURFACE_TEXTURE_FLAG);
		if (!dr || sid >= dr->surfaceCount) {
			return 1.0f;
		}
		float w = d3d9GetSurfaceWidth(renderer, (int32_t)sid);
		if (w > 0.0f) {
			return 1.0f / w;
		}
		return 1.0f;
	}

	// Sprite handles: texID = (tpagIndex + 1)
	uint32_t tpagIndexPlus1 = texID;
	DataWin* dw = dr ? dr->base.dataWin : nullptr;
	if (dw && dw->tpag.count > 0 && tpagIndexPlus1 > 0) {
		uint32_t tpagIndex = tpagIndexPlus1 - 1;
		if (tpagIndex < dw->tpag.count) {
			TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
			int32_t texPageId = tpag->texturePageId;
			if (0 <= texPageId && (uint32_t)texPageId < dr->textureCount) {
				// This is a render-thread read of cached dimensions/UVs.
				// Use the synchronous ensure wrapper (it may upload the decoded
				// texture if already ready / safe), but never relies on
				// external worker-side file I/O.
				ensureTexturePageLoaded(dr, (uint32_t)texPageId);
				int32_t w = dr->textureWidths[texPageId];
				if (dr->textures[texPageId] && w > 0) {
					return 1.0f / (float)w;
				}
			}
		}
	}

	return 1.0f;
}

static float d3d9TextureGetTexelHeight(Renderer* renderer, uint32_t texID) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	if (texID == 0) {
		return 1.0f;
	}

	// Surface handles: texID = D3D9_SURFACE_TEXTURE_FLAG | surfaceID
	if ((texID & D3D9_SURFACE_TEXTURE_FLAG) != 0) {
		uint32_t sid = (texID & ~D3D9_SURFACE_TEXTURE_FLAG);
		if (!dr || sid >= dr->surfaceCount) {
			return 1.0f;
		}
		float h = d3d9GetSurfaceHeight(renderer, (int32_t)sid);
		if (h > 0.0f) {
			return 1.0f / h;
		}
		return 1.0f;
	}

	uint32_t tpagIndexPlus1 = texID;
	DataWin* dw = dr ? dr->base.dataWin : nullptr;
	if (dw && dw->tpag.count > 0 && tpagIndexPlus1 > 0) {
		uint32_t tpagIndex = tpagIndexPlus1 - 1;
		if (tpagIndex < dw->tpag.count) {
			TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
			int32_t texPageId = tpag->texturePageId;
			if (0 <= texPageId && (uint32_t)texPageId < dr->textureCount) {
				ensureTexturePageLoaded(dr, (uint32_t)texPageId);
				int32_t h = dr->textureHeights[texPageId];
				if (dr->textures[texPageId] && h > 0) {
					return 1.0f / (float)h;
				}
			}
		}
	}

	return 1.0f;
}

static bool d3d9TextureGetUVs(Renderer* renderer, uint32_t texID, float* outUVs) {
	if (!outUVs) {
		return false;
	}

	// Default UVs: full texture.
	outUVs[0] = 0.0f;
	outUVs[1] = 0.0f;
	outUVs[2] = 1.0f;
	outUVs[3] = 1.0f;

	if (texID == 0) {
		return false;
	}

	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	// Surface handles cover the whole surface (no sub-region) => full UVs.
	if ((texID & D3D9_SURFACE_TEXTURE_FLAG) != 0) {
		return true;
	}

	DataWin* dw = dr ? dr->base.dataWin : nullptr;
	if (!dw) {
		return false;
	}

	uint32_t tpagIndexPlus1 = texID;
	if (tpagIndexPlus1 == 0) {
		return false;
	}

	uint32_t tpagIndex = tpagIndexPlus1 - 1;
	if (tpagIndex >= dw->tpag.count) {
		return false;
	}

	TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

	int32_t texPageId = tpag->texturePageId;
	if (texPageId < 0 || (uint32_t)texPageId >= dr->textureCount) {
		return false;
	}

	ensureTexturePageLoaded(dr, (uint32_t)texPageId);
	if (!dr->textures[texPageId]) {
		return false;
	}

	float texW = (float)dr->textureWidths[texPageId];
	float texH = (float)dr->textureHeights[texPageId];
	if (texW <= 0.0f || texH <= 0.0f) {
		return false;
	}

	float u0 = texelStart((float)tpag->sourceX, texW);
	float v0 = texelStart((float)tpag->sourceY, texH);
	float u1 = texelEnd((float)tpag->sourceX, (float)tpag->sourceWidth, texW);
	float v1 = texelEnd((float)tpag->sourceY, (float)tpag->sourceHeight, texH);

	outUVs[0] = u0;
	outUVs[1] = v0;
	outUVs[2] = u1;
	outUVs[3] = v1;

	return true;
}

static void d3d9TextureSetStage(Renderer* renderer, int32_t slot, uint32_t texID) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (!dr) {
		return;
	}

	if (slot < 0 || slot >= 8) {
		return;
	}

	IDirect3DDevice9* dev = Dev(dr);

	dr->renderStateDirty = true;

	// 0 => no texture
	if (texID == 0) {
		dev->SetTexture((DWORD)slot, (IDirect3DBaseTexture9*)dr->whiteTexture);
		return;
	}

	// Surface handle: texID = D3D9_SURFACE_TEXTURE_FLAG | surfaceID
	if ((texID & D3D9_SURFACE_TEXTURE_FLAG) != 0) {
		uint32_t sid = (texID & ~D3D9_SURFACE_TEXTURE_FLAG);
		IDirect3DTexture9* surfaceTex = nullptr;
		if (sid == (uint32_t)APPLICATION_SURFACE_ID) {
			surfaceTex = (IDirect3DTexture9*)dr->appSurfaceTexture;
		} else if (sid < dr->surfaceCount && dr->surfaceTexture[sid]) {
			surfaceTex = (IDirect3DTexture9*)dr->surfaceTexture[sid];
		}

		dev->SetTexture((DWORD)slot,
						(IDirect3DBaseTexture9*)(surfaceTex ? surfaceTex : (IDirect3DTexture9*)dr->whiteTexture));
		return;
	}

	// Sprite handle: texID = (tpagIndex + 1)
	uint32_t tpagIndexPlus1 = texID;
	if (tpagIndexPlus1 == 0) {
		dev->SetTexture((DWORD)slot, (IDirect3DBaseTexture9*)dr->whiteTexture);
		return;
	}

	uint32_t tpagIndex = tpagIndexPlus1 - 1;
	DataWin* dw = dr ? dr->base.dataWin : nullptr;
	if (dw && tpagIndex < dw->tpag.count) {
		TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
		int32_t texPageId = tpag->texturePageId;
		if (texPageId >= 0 && (uint32_t)texPageId < dr->textureCount) {
			ensureTexturePageLoaded(dr, (uint32_t)texPageId);
			if (dr->textures[texPageId]) {
				dev->SetTexture((DWORD)slot, (IDirect3DBaseTexture9*)dr->textures[texPageId]);
				return;
			}
		}
	}

	dev->SetTexture((DWORD)slot, (IDirect3DBaseTexture9*)dr->whiteTexture);
}

static bool d3d9ShaderIsCompiled(Renderer* renderer, int32_t shader) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (shader < 0 || (uint32_t)shader >= dr->gmlShaderCount) {
		return false;
	}
	return dr->gmlShaders[shader].compiled;
}
static bool d3d9ShadersSupported() { return true; }

uint32_t d3d9SurfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	// Contract (matches GL renderer):
	// - 0 means "no texture"
	// - surface handle uses top-bit flag + surfaceID
	if (!dr) {
		return 0;
	}

	if (surfaceID == APPLICATION_SURFACE_ID) {
		return dr->appSurfaceTexture ? (D3D9_SURFACE_TEXTURE_FLAG | (uint32_t)APPLICATION_SURFACE_ID) : 0;
	}

	if (surfaceID < 0 || (uint32_t)surfaceID >= dr->surfaceCount) {
		return 0;
	}
	return dr->surfaceTexture[surfaceID] ? (D3D9_SURFACE_TEXTURE_FLAG | (uint32_t)surfaceID) : 0;
}

void d3d9DrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
	// Draw the tile using the standard shared helper in renderer.h.
	// This handles atlas clipping, scaling, and all the coordinate transforms.
	uint32_t savedColor = renderer->drawColor;
	float savedAlpha = renderer->drawAlpha;

	renderer->drawColor = tile->color;
	renderer->drawAlpha = tile->alpha;

	int32_t tpagIndex = Renderer_resolveObjectTPAGIndex(renderer->dataWin, tile);
	if (tpagIndex >= 0) {
		DataWin* dw = renderer->dataWin;
		TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

		int32_t srcX = tile->sourceX;
		int32_t srcY = tile->sourceY;
		int32_t srcW = (int32_t)tile->width;
		int32_t srcH = (int32_t)tile->height;
		float drawX = (float)tile->x + offsetX;
		float drawY = (float)tile->y + offsetY;

		// Clip to TPAG content region
		int32_t contentLeft = tpag->targetX;
		int32_t contentTop = tpag->targetY;
		if (contentLeft > srcX) {
			int32_t clip = contentLeft - srcX;
			drawX += (float)clip * tile->scaleX;
			srcW -= clip;
			srcX = contentLeft;
		}
		if (contentTop > srcY) {
			int32_t clip = contentTop - srcY;
			drawY += (float)clip * tile->scaleY;
			srcH -= clip;
			srcY = contentTop;
		}
		int32_t contentRight = tpag->targetX + tpag->sourceWidth;
		int32_t contentBottom = tpag->targetY + tpag->sourceHeight;
		if (srcX + srcW > contentRight) {
			srcW = contentRight - srcX;
		}
		if (srcY + srcH > contentBottom) {
			srcH = contentBottom - srcY;
		}
		if (srcW <= 0 || srcH <= 0) {
			renderer->drawColor = savedColor;
			renderer->drawAlpha = savedAlpha;
			return;
		}

		int32_t atlasOffX = srcX - tpag->targetX;
		int32_t atlasOffY = srcY - tpag->targetY;

		d3d9DrawSpritePart(renderer, tpagIndex, atlasOffX, atlasOffY, srcW, srcH,
						   drawX, drawY, tile->scaleX, tile->scaleY,
						   0.0f, 0.0f, 0.0f, tile->color, tile->alpha);
	}

	renderer->drawColor = savedColor;
	renderer->drawAlpha = savedAlpha;
}

void d3d9DrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
	// Default tiled sprite: tile along X/Y in steps of the sprite's native bounding size.
	// Batches all tile quads directly instead of per-tile d3d9DrawSprite calls.
	DataWin* dw = renderer->dataWin;
	if (!dw || 0 > tpagIndex || (uint32_t)tpagIndex >= dw->tpag.count) {
		return;
	}

	TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
	int32_t texPageId = tpag->texturePageId;
	if (0 > texPageId) {
		return;
	}

	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	if (!ensureTexturePageLoadedAsync(dr, (uint32_t)texPageId)) {
		return;
	}
	if (!dr->textures[texPageId]) {
		return;
	}

	ensureTexture(dr, texPageId);

	float texW = (float)dr->textureWidths[texPageId];
	float texH = (float)dr->textureHeights[texPageId];
	if (texW <= 0 || texH <= 0) return;

	float u0 = texelStart((float)tpag->sourceX, texW);
	float v0 = texelStart((float)tpag->sourceY, texH);
	float u1 = texelEnd((float)tpag->sourceX, (float)tpag->sourceWidth, texW);
	float v1 = texelEnd((float)tpag->sourceY, (float)tpag->sourceHeight, texH);

	float axScale = fabsf(xscale);
	float ayScale = fabsf(yscale);

	float sprW = (float)tpag->boundingWidth * axScale;
	float sprH = (float)tpag->boundingHeight * ayScale;
	if (sprW <= 0.0f || sprH <= 0.0f) return;

	// Per-tile quad geometry in local space (without origin offset)
	float localX0 = (float)tpag->targetX - originX;
	float localY0 = (float)tpag->targetY - originY;
	float quadOffX0 = originX * axScale + xscale * localX0;
	float quadOffY0 = originY * ayScale + yscale * localY0;
	float quadW = xscale * (float)tpag->targetWidth;
	float quadH = yscale * (float)tpag->targetHeight;

	float cr, cg, cb, ca;
	bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

	// Compute starting offsets so that x/y correspond to the scroll position.
	float startX = tileX ? fmodf(x, sprW) - sprW : x;
	float startY = tileY ? fmodf(y, sprH) - sprH : y;
	float endX = tileX ? roomW : x;
	float endY = tileY ? roomH : y;

	// Frustum clipping
	if (dr->viewportW > 0 && dr->viewportH > 0) {
		float gameLeft = ((float)dr->viewportX - dr->portOffsetX) / dr->portScaleX + dr->offsetX;
		float gameTop = ((float)dr->viewportY - dr->portOffsetY) / dr->portScaleY + dr->offsetY;
		float gameRight = ((float)(dr->viewportX + dr->viewportW) - dr->portOffsetX) / dr->portScaleX + dr->offsetX;
		float gameBottom = ((float)(dr->viewportY + dr->viewportH) - dr->portOffsetY) / dr->portScaleY + dr->offsetY;
		if (tileX) {
			if (gameLeft > startX) startX += floorf((gameLeft - startX) / sprW) * sprW;
			if (gameRight < endX) endX = gameRight;
		}
		if (tileY) {
			if (gameTop > startY) startY += floorf((gameTop - startY) / sprH) * sprH;
			if (gameBottom < endY) endY = gameBottom;
		}
	}
	if (startX >= endX || startY >= endY) return;

	int32_t tilesX = tileX ? (int32_t)((endX - startX) / sprW) + 1 : 1;
	int32_t tilesY = tileY ? (int32_t)((endY - startY) / sprH) + 1 : 1;
	if (tilesX <= 0 || tilesY <= 0) return;

	for (int32_t iy = 0; iy < tilesY; iy++) {
		float dy = startY + (float)iy * sprH;
		if (dy >= endY) break;
		for (int32_t ix = 0; ix < tilesX; ix++) {
			float dx = startX + (float)ix * sprW;
			if (dx >= endX) break;

			float vx0 = dx + quadOffX0;
			float vy0 = dy + quadOffY0;
			float vx1 = vx0 + quadW;
			float vy1 = vy0 + quadH;

			SpriteVertex* v = allocQuad(dr);
			setVertex(&v[0], vx0, vy0, u0, v0, cr, cg, cb, ca);
			setVertex(&v[1], vx1, vy0, u1, v0, cr, cg, cb, ca);
			setVertex(&v[2], vx1, vy1, u1, v1, cr, cg, cb, ca);
			setVertex(&v[3], vx0, vy1, u0, v1, cr, cg, cb, ca);
		}
	}
}

void d3d9DrawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha) {
	// Tiles the application surface (or supported surface) across the room.
	// We rely on surface_get_dimensions via renderer->vtable.
	float sw = renderer->vtable->getSurfaceWidth(renderer, surfaceID);
	float sh = renderer->vtable->getSurfaceHeight(renderer, surfaceID);
	if (sw <= 0.0f || sh <= 0.0f) {
		return;
	}

	float sprW = sw * xscale;
	float sprH = sh * yscale;
	if (sprW <= 0.0f || sprH <= 0.0f) {
		return;
	}
	if (!drawTex || texW <= 0 || texH <= 0) return;

	float sw = (float)surfW;
	float sh = (float)surfH;
	float sprW = sw * xscale;
	float sprH = sh * yscale;
	if (sprW <= 0.0f || sprH <= 0.0f) return;

	// Ensure the surface texture is bound for batched drawing
	flushBatch(dr);
	dr->currentTextureIndex = -1;
	dr->batchSurfaceTex = (void*)drawTex;

	float cr, cg, cb, ca;
	bgrToFloatColor(color, alpha, &cr, &cg, &cb, &ca);

	float startX = fmodf(x, sprW) - sprW;
	float startY = fmodf(y, sprH) - sprH;
	float endX = roomW;
	float endY = roomH;

	for (float ty = startY; ty <= endY; ty += sprH) {
		for (float tx = startX; tx <= endX; tx += sprW) {
			d3d9DrawSurface(renderer, surfaceID, 0, 0, (int32_t)sw, (int32_t)sh, tx, ty, xscale, yscale, 0.0f, color, alpha);
		}
	}
}

void d3d9SetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portW, int32_t portH, bool renderingToUserSurface) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;

	if (portW <= 0 || portH <= 0 || guiW <= 0 || guiH <= 0) {
		return;
	}

	renderer->cameraCurrent = GUI_CAMERA;
	GMLCamera* guiCamera = &renderer->runner->guiCamera;
	guiCamera->allocated = true;
	guiCamera->viewX = 0.0f;
	guiCamera->viewY = 0.0f;
	guiCamera->viewWidth = guiW;
	guiCamera->viewHeight = guiH;
	guiCamera->borderX = 0;
	guiCamera->borderY = 0;
	guiCamera->speedX = 0;
	guiCamera->speedY = 0;
	guiCamera->objectId = -1;
	guiCamera->viewAngle = 0;

	Matrix4f projectionMatrix;
	Matrix4f_Orthographic(&projectionMatrix, (float)guiW, (float)guiH, 32000.0, 0.0);
	if (renderingToUserSurface) {
		Matrix4f_flipClipY(&projectionMatrix);
	}

	Matrix4f viewMatrix;
	float cx = (float)guiW * 0.5f;
	float cy = (float)guiH * 0.5f;
	Matrix4f_identity(&viewMatrix);
	Matrix4f_LookAt(&viewMatrix, cx, cy, -16000.0, cx, cy, 16000.0, 0.0, 1.0, 0.0);
	guiCamera->viewMatrix = viewMatrix;
	guiCamera->projectionMatrix = projectionMatrix;

	d3d9ApplyProjection(renderer, &guiCamera->viewMatrix, &guiCamera->projectionMatrix);

	if (renderingToUserSurface) {
		dr->offsetX = 0.0f;
		dr->offsetY = 0.0f;
		dr->portScaleX = 1.0f;
		dr->portScaleY = 1.0f;
		dr->portOffsetX = 0.0f;
		dr->portOffsetY = 0.0f;
		return;
	}

	float scaleX = (float)portW / (float)guiW;
	float scaleY = (float)portH / (float)guiH;
	float uniformScale = (scaleX < scaleY) ? scaleX : scaleY;

	float offsetX = ((float)portW - (float)guiW * uniformScale) * 0.5f;
	float offsetY = ((float)portH - (float)guiH * uniformScale) * 0.5f;

	dr->portScaleX = uniformScale;
	dr->portScaleY = uniformScale;
	dr->portOffsetX = (float)portW * 0.0f + offsetX;
	dr->portOffsetY = (float)portH * 0.0f + offsetY;

	dr->renderStateDirty = true;
}

static void d3d9SetMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) {
	D3D9Renderer* dr = (D3D9Renderer*)renderer;
	flushBatch(dr);
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

// ===[ Public API ]===

Renderer* D3D9Renderer_create(void* pd3dDevice) {
	D3D9Renderer* dr = (D3D9Renderer*)safeCalloc(1, sizeof(D3D9Renderer));
	static RendererVtable d3d9RendererVtable;
	ZERO_STRUCT(d3d9RendererVtable);
	d3d9RendererVtable.init = d3d9Init;
	d3d9RendererVtable.destroy = d3d9Destroy;
	d3d9RendererVtable.beginFrame = d3d9BeginFrame;
	d3d9RendererVtable.endFrameInit = d3d9EndFrameInit;
	d3d9RendererVtable.endFrameEnd = d3d9EndFrameEnd;
	d3d9RendererVtable.beginView = d3d9BeginView;
	d3d9RendererVtable.endView = d3d9EndView;
	d3d9RendererVtable.applyProjection = d3d9ApplyProjection;
	d3d9RendererVtable.beginGUI = d3d9BeginGUI;
	d3d9RendererVtable.setGuiProjection = d3d9SetGuiProjection;
	d3d9RendererVtable.endGUI = d3d9EndGUI;
	d3d9RendererVtable.drawSprite = d3d9DrawSprite;
	d3d9RendererVtable.drawSpritePart = d3d9DrawSpritePart;
	d3d9RendererVtable.drawSpritePos = d3d9DrawSpritePos;
	d3d9RendererVtable.drawRectangle = d3d9DrawRectangle;
	d3d9RendererVtable.drawRectangleColor = d3d9DrawRectangleColor;
	d3d9RendererVtable.drawLine = d3d9DrawLine;
	d3d9RendererVtable.drawTriangle = d3d9DrawTriangle;
	d3d9RendererVtable.drawLineColor = d3d9DrawLineColor;
	d3d9RendererVtable.drawText = d3d9DrawText;
	d3d9RendererVtable.drawTextColor = d3d9DrawTextColor;
	d3d9RendererVtable.flush = d3d9Flush;
	d3d9RendererVtable.clearScreen = d3d9ClearScreen;
	d3d9RendererVtable.createSpriteFromSurface = d3d9CreateSpriteFromSurface;
	d3d9RendererVtable.deleteSprite = d3d9DeleteSprite;
	d3d9RendererVtable.gpuGetBlendFactors = d3d9GpuGetBlendFactors;
	d3d9RendererVtable.gpuGetBlendMode = d3d9GpuGetBlendMode;
	d3d9RendererVtable.gpuSetBlendMode = d3d9GpuSetBlendMode;
	d3d9RendererVtable.gpuSetBlendModeExt = d3d9GpuSetBlendModeExt;
	d3d9RendererVtable.gpuSetBlendEnable = d3d9GpuSetBlendEnable;
	d3d9RendererVtable.gpuSetAlphaTestEnable = d3d9GpuSetAlphaTestEnable;
	d3d9RendererVtable.gpuSetAlphaTestRef = d3d9GpuSetAlphaTestRef;
	d3d9RendererVtable.gpuSetColorWriteEnable = d3d9GpuSetColorWriteEnable;
	d3d9RendererVtable.gpuGetColorWriteEnable = d3d9GpuGetColorWriteEnable;
	d3d9RendererVtable.gpuGetBlendEnable = d3d9GpuGetBlendEnable;
	d3d9RendererVtable.gpuSetFog = d3d9GpuSetFog;
	d3d9RendererVtable.drawTile = d3d9DrawTile;
	// Doesnt exist anymore??? (after a merge)
	//  d3d9RendererVtable.drawTiled = d3d9DrawTiled;
	d3d9RendererVtable.createSurface = d3d9CreateSurface;
	d3d9RendererVtable.surfaceExists = d3d9SurfaceExists;
	d3d9RendererVtable.setRenderTarget = d3d9SetRenderTarget;
	d3d9RendererVtable.ensureApplicationSurface = d3d9EnsureApplicationSurface;
	d3d9RendererVtable.getSurfaceWidth = d3d9GetSurfaceWidth;
	d3d9RendererVtable.getSurfaceHeight = d3d9GetSurfaceHeight;
	d3d9RendererVtable.drawSurface = d3d9DrawSurface;
	d3d9RendererVtable.surfaceResize = d3d9SurfaceResize;
	d3d9RendererVtable.surfaceFree = d3d9SurfaceFree;
	d3d9RendererVtable.surfaceCopy = d3d9SurfaceCopy;
	d3d9RendererVtable.surfaceGetPixels = d3d9SurfaceGetPixels;
	d3d9RendererVtable.drawTiledPart = d3d9DrawTiledPart;
	d3d9RendererVtable.gpuSetShader = d3d9GpuSetShader;
	d3d9RendererVtable.gpuResetShader = d3d9GpuResetShader;
	d3d9RendererVtable.shaderGetUniform = d3d9ShaderGetUniform;
	d3d9RendererVtable.shaderGetSamplerIndex = d3d9ShaderGetSamplerIndex;
	d3d9RendererVtable.shaderSetUniformF = d3d9ShaderSetUniformF;
	d3d9RendererVtable.shaderSetUniformFArray = d3d9ShaderSetUniformFArray;
	d3d9RendererVtable.shaderSetUniformI = d3d9ShaderSetUniformI;
	d3d9RendererVtable.spriteGetTexture = d3d9SpriteGetTexture;
	d3d9RendererVtable.surfaceGetTexture = d3d9SurfaceGetTexture;
	d3d9RendererVtable.textureGetTexelWidth = d3d9TextureGetTexelWidth;
	d3d9RendererVtable.textureGetTexelHeight = d3d9TextureGetTexelHeight;
	d3d9RendererVtable.textureGetUVs = d3d9TextureGetUVs;
	d3d9RendererVtable.textureSetStage = d3d9TextureSetStage;
	d3d9RendererVtable.shaderIsCompiled = d3d9ShaderIsCompiled;
	d3d9RendererVtable.shadersSupported = d3d9ShadersSupported;
	d3d9RendererVtable.setMatrix = d3d9SetMatrix;
	d3d9RendererVtable.drawSpriteTiled = d3d9DrawSpriteTiled;
	d3d9RendererVtable.drawSurfaceTiled = d3d9DrawSurfaceTiled;
	dr->base.vtable = &d3d9RendererVtable;
	dr->base.drawColor = 0xFFFFFF;
	dr->base.drawAlpha = 1.0f;
	dr->base.drawFont = -1;
	dr->base.circlePrecision = 24;
	dr->base.currentShader = -1;
	dr->base.cameraCurrent = 0;
	dr->drawPhase = RENDER_PHASE_NONE;
	dr->pd3dDevice = pd3dDevice;
	dr->currentTextureIndex = -1;
	dr->boundTextureIndex = -2;
	dr->boundTexturePtr = nullptr;

	dr->renderStateDirty = true;

	// Initialize surface arrays to empty
	dr->surfaces = nullptr;
	dr->surfaceTexture = nullptr;
	dr->surfaceWidth = nullptr;
	dr->surfaceHeight = nullptr;
	dr->surfaceCount = 0;

	// Intialize blend modes
	dr->blendMode = bm_normal;
	dr->sFactor = 0;
	dr->dFactor = 0;
	dr->sFactorAlpha = 0;
	dr->dFactorAlpha = 0;

	// Init ptrs to null
	dr->boundVertexShader = nullptr;
	dr->boundPixelShader = nullptr;

#ifdef PLATFORM_XBOX360_XDK
	dr->boundViewportEnable = true;
#endif

	return (Renderer*)dr;
}
