#pragma once

#include "renderer.h"

#define RENDER_PHASE_NONE 0
#define RENDER_PHASE_PRE 1
#define RENDER_PHASE_WORLD 2
#define RENDER_PHASE_POST 3
#define RENDER_PHASE_GUI 4

// Maximum quads per batch before flushing
#define D3D9_MAX_QUADS 2048
#define D3D9_VERTS_PER_QUAD 4
#define D3D9_INDICES_PER_QUAD 6
#define D3D9_MAX_TRIS 2048
#define D3D9_VERTS_PER_TRI 3

// Vertex: position(2f), texcoord(2f), color(D3DCOLOR)
#define D3D9_VERTEX_STRIDE 20

// Maximum number of shader uniforms we track
#define D3D9_MAX_SHADER_UNIFORMS 64

// Shaders
#define D3D9_DISABLE_SHADERS

// 2bpp instead of 4
#define D3D9_USE_16BIT_TEXTURES

typedef struct {
	char* name;			   // owned
	int32_t registerIndex; // constant register index
	int32_t registerCount; // number of registers (4 floats each)
	uint32_t samplerSlot;  // for sampler uniforms
	bool isSampler;
	bool isVertex;		   // true = VS uniform, false = PS uniform
} D3D9ShaderUniform;

typedef struct {
	void* pVertexShader; // IDirect3DVertexShader9*
	void* pPixelShader;	 // IDirect3DPixelShader9*
	bool compiled;
	bool compileAttempted; // true once we've tried (and possibly failed) to compile
	uint32_t uniformCount;
	D3D9ShaderUniform uniforms[D3D9_MAX_SHADER_UNIFORMS]; // fixed-size array to avoid heap allocs
} D3D9GMLShader;

typedef struct {
	Renderer base; // Must be first field

	void* pd3dDevice; // IDirect3DDevice9* (opaque in C header)

	// Shader programs (compiled at init from HLSL source)
	void* pVertexShader;
	void* pPixelShader;
	void* pVertexDecl;

	// Currently bound shaders
	void* boundVertexShader;
	void* boundPixelShader;

#ifdef PLATFORM_XBOX360_XDK
	// Only used on 360
	bool boundViewportEnable;
#endif

	// GML shader support
	D3D9GMLShader* defaultShaderProgram;
	D3D9GMLShader* gmlShaders;
	uint32_t gmlShaderCount;

	// Sprite batch state
	int32_t quadCount;
	int32_t triCount;
	int32_t currentTextureIndex;
	uint8_t* vertexData; // CPU-side staging (D3D9_MAX_QUADS * D3D9_VERTS_PER_QUAD * D3D9_VERTEX_STRIDE)

	// Bound texture cache (to avoid redundant SetTexture calls during flushBatch)
	int32_t boundTextureIndex;
	void* boundTexturePtr;

	// Shared GPU render-state cache. Used to avoid needlessly repeating
	// static sampler/render-state setup work every BeginFrame.
	bool renderStateDirty;

	// Textures loaded from TXTR pages (decoded PNG -> D3D textures)
	void** textures; // IDirect3DTexture9*[]
	int32_t* textureWidths;
	int32_t* textureHeights;
	uint32_t* textureBlobSizes;
	uint32_t* textureLastUsedFrame;
	uint32_t frameCounter;
	uint32_t loadedTexturePages;
	uint32_t textureBytesUsed;
	uint32_t textureCount;

	// 1x1 white texture for primitives
	void* whiteTexture;

	// View transform state
	float portScaleX, portScaleY;	// portW/viewW, portH/viewH
	float offsetX, offsetY;			// viewX, viewY
	float portOffsetX, portOffsetY; // portX, portY (game coords)

	// Screen-space viewport bounds (set by beginView, used for frustum clipping in tiled sprites)
	int32_t viewportX, viewportY, viewportW, viewportH;

	// Frame dimensions
	int32_t gameW, gameH;
	int32_t screenW, screenH;
	bool renderingToApplicationSurface;

	// Backing render target for GameMaker's application_surface.
	// Other dynamic surfaces are still unsupported on this backend.
	void* appSurfaceTexture; // IDirect3DTexture9* resolved sample texture
	void* appRenderTexture;	 // reserved for future surface-backed textures
	void* appSurfaceLevel;	 // IDirect3DSurface9* render target
	int32_t appSurfaceW;
	int32_t appSurfaceH;
	int32_t appSurfaceAllocW;
	int32_t appSurfaceAllocH;
	bool appSurfaceResolved;

	// Letterbox: uniform-scaled render area within screen
	float renderScale;	 // uniform scale factor
	float renderOffsetX; // pixel offset for centering
	float renderOffsetY;

	// Dynamic sprite tracking
	uint32_t originalTexturePageCount;
	uint32_t originalTpagCount;
	uint32_t originalSpriteCount;

	// Saved view transform state for implicitApplicationSurface restore
	// The D3D9 renderer needs to restore the view state when surface_reset_target pops
	// back to the application surface implicitly (the GL renderer uses CPort + previousViewMatrix for this).
	float savedOffsetX, savedOffsetY;
	float savedPortScaleX, savedPortScaleY;
	float savedPortOffsetX, savedPortOffsetY;
	bool savedViewStateValid;

	// Dynamic surface management (user-created surfaces via surface_create)
	void** surfaces;	   // IDirect3DSurface9*[] render target surfaces
	void** surfaceTexture; // IDirect3DTexture9*[] color buffer textures
	int32_t* surfaceWidth;
	int32_t* surfaceHeight;
	uint32_t surfaceCount;

	// ===[ Async texture loading (decode on worker threads only) ]===
	// State per decoded texture page. This backend currently supports:
	// - worker threads: decode PNG bytes -> RGBA CPU buffer
	// - render thread: create/upload IDirect3DTexture9 using decoded RGBA
	// - render-time call sites: skip drawing until the page is uploaded

	// 0 = idle/unqueued
	// 1 = queued (a worker is decoding)
	// 2 = decoded/ready (upload on render thread)
	// 3 = failed (do not keep retrying every frame)
	uint8_t* textureLoadState;

	// Decoded CPU RGBA buffers owned by the renderer until uploaded.
	// width/height are in pixels; byteSize is width*height*4.
	uint8_t** texturePendingRGBA;
	uint32_t* texturePendingW;
	uint32_t* texturePendingH;
	uint32_t* texturePendingByteSize;

	// Simple counters/knobs
	uint32_t textureDecodeWorkerConcurrency;
	uint32_t textureDecodeInFlight;

	// Mutex/condition-variable implemented as opaque pointers to avoid
	// including C++ threading headers in the C header.
	void* textureLoadMutex;
	void* textureLoadCond;

	// Serializes GPU-side texture cache mutations (eviction + pointer swaps)
	// with draw-batch submission to avoid releasing textures while DXVK holds refs.
	void* textureGpuMutex;

	// Render-thread bookkeeping
	uint32_t textureDecodedUploadCursor;

	uint8_t drawPhase;

	// Fog state
	bool fogEnable;
	uint32_t fogColor;

	// Cache: last applied shader constants (avoid redundant SetVertexShaderConstantF)
	int32_t cachedGameW, cachedGameH;
	bool cachedFogEnable;
	uint32_t cachedFogColor;

	// Cache: GML shader matrix uniforms (5 matrices * 4 registers * 4 floats)
	Matrix4f cachedGmlMatrices[5];
	bool cachedGmlMatricesValid;

	// Blend mode
	int32_t blendMode;
	int32_t sFactor;
	int32_t dFactor;
	int32_t sFactorAlpha;
	int32_t dFactorAlpha;

	// Cache: sampler state has been applied this frame (avoids redundant SetSamplerState)
	bool samplerStateApplied;
	// Cache: blend state is currently at "normal" defaults (avoids redundant SetRenderState)
	bool blendIsNormal;

	// Surface texture override for batch system (Xbox 360 only)
	void* batchSurfaceTex; // IDirect3DTexture9*, non-null = use instead of dr->textures[]
} D3D9Renderer;

Renderer* D3D9Renderer_create(void* pd3dDevice);

#ifdef __cplusplus
extern "C"
#endif
	bool D3D9Renderer_ensureTextureLoaded(D3D9Renderer* dr, uint32_t textureIndex);
