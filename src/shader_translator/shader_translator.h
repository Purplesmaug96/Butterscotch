#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Translates GLSL ES shader source code to HLSL9 (shader model 2.0/3.0).
// Returns true on success, with outHlsl pointing to allocated hlsl source (caller must free).
// isVertex: true for vertex shader, false for pixel shader.
bool ShaderTranslator_translateGLES2HLSL9(const char* glesSource, bool isVertex, char** outHlsl, size_t* outLen);

// Translates GLSL ES shader source code to HLSL4/5 (shader model 4.1/5.0).
// Useful for D3D11/UWP/other platforms where proper OpenGL is not available.
bool ShaderTranslator_translateGLES2HLSL11(const char* glesSource, bool isVertex, char** outHlsl, size_t* outLen);

// Initialize the shader translator (calls sh::Initialize()).
// Called automatically on first translate; can be called explicitly to control init timing.
void ShaderTranslator_init(void);

// Shutdown the shader translator (calls sh::Finalize()).
void ShaderTranslator_shutdown(void);

#ifdef __cplusplus
} // extern "C"
#endif
