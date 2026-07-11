//
// Butterscotch Shader Translator
//
// Wraps ANGLE's shader translator to convert GLSL ES shader source into:
//   - HLSL9 (D3D9 / Xbox 360) via SH_HLSL_3_0_OUTPUT
//   - HLSL11 (D3D11 / UWP / etc.) via SH_HLSL_4_1_OUTPUT
//
// This allows games without HLSL9 shader source to still use shaders on
// the Xbox 360 (and potentially other D3D9/D3D11 platforms) by translating
// the GLSL ES source that is always present in GameMaker games.
//
// Usage (preprocessor side):
//   1. Extract glslES_Vertex / glslES_Fragment from the SHDR chunk
//   2. Call ShaderTranslator_translateGLES2HLSL9() to get HLSL9 source
//   3. Store the HLSL9 source with the shader data
//
// Usage (runtime side):
//   On Xbox 360, the HLSL9 source is compiled with D3DXCompileShader
//   just like native HLSL9 shaders.
//

#include "shader_translator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ANGLE includes
#include "GLSLANG/ShaderLang.h"
#include "angle_gl.h"

// Simple wrapper to prevent including C++ headers in the C header
// We use a static initializer approach.

extern "C" {

static bool g_translatorInitialized = false;

void ShaderTranslator_init(void) {
    if (!g_translatorInitialized) {
        sh::Initialize();
        g_translatorInitialized = true;
    }
}

void ShaderTranslator_shutdown(void) {
    if (g_translatorInitialized) {
        sh::Finalize();
        g_translatorInitialized = false;
    }
}

// Translates GLSL ES source to the specified output format.
// Returns the HLSL source string on success (caller must free), NULL on failure.
static char* translateGLES(
    const char* glesSource,
    bool isVertex,
    ShShaderOutput outputFormat
) {
    if (!glesSource || !glesSource[0]) return NULL;

    ShaderTranslator_init();

    // Determine shader type
    sh::GLenum shaderType = isVertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;

    // Set up resources
    ShBuiltInResources resources = {};
    sh::InitBuiltInResources(&resources);

    // Conservative defaults for Xbox 360 / D3D9
    resources.MaxVertexAttribs             = 16;
    resources.MaxVertexUniformVectors      = 256;
    resources.MaxVaryingVectors            = 8;
    resources.MaxVertexTextureImageUnits   = 0;
    resources.MaxCombinedTextureImageUnits = 8;
    resources.MaxTextureImageUnits         = 8;
    resources.MaxFragmentUniformVectors    = 224;
    resources.MaxDrawBuffers               = 1;
    resources.MaxDualSourceDrawBuffers     = 1;

    // Enable common extensions
    resources.OES_standard_derivatives  = 1;
    resources.OES_EGL_image_external    = 0;
    resources.EXT_frag_depth            = 1;
    resources.EXT_shader_texture_lod    = 0;
    resources.EXT_draw_buffers          = 0;
    resources.OVR_multiview             = 0;

    // Construct the compiler
    ShHandle compiler = sh::ConstructCompiler(
        shaderType,
        SH_GLES2_SPEC,
        outputFormat,
        &resources
    );

    if (!compiler) {
        fprintf(stderr, "ShaderTranslator: Failed to construct compiler for %s shader\n",
                isVertex ? "vertex" : "fragment");
        return NULL;
    }

    // Set up compile options
    ShCompileOptions compileOptions = {};
    compileOptions.objectCode             = 1;
    compileOptions.initializeUninitializedLocals = true;

    // For HLSL output, we want to select view in vertex shader disabled
    if (outputFormat == SH_HLSL_3_0_OUTPUT || outputFormat == SH_HLSL_4_1_OUTPUT) {
        compileOptions.selectViewInNvGLSLVertexShader = false;
    }

    // Compile
    const char* sourceStrings[] = { glesSource };
    bool compiled = sh::Compile(compiler, sourceStrings, 1, compileOptions);

    char* result = NULL;

    if (compiled) {
        const std::string& code = sh::GetObjectCode(compiler);
        if (!code.empty()) {
            result = strdup(code.c_str());
        }
    }

    if (!compiled || !result) {
        // Get error info
        const std::string& infoLog = sh::GetInfoLog(compiler);
        fprintf(stderr, "ShaderTranslator: Failed to translate %s shader:\n%s\n",
                isVertex ? "vertex" : "fragment",
                infoLog.empty() ? "(no error info)" : infoLog.c_str());
    }

    sh::Destruct(compiler);

    return result;
}

bool ShaderTranslator_translateGLES2HLSL9(
    const char* glesSource,
    bool isVertex,
    char** outHlsl,
    size_t* outLen
) {
    if (!outHlsl) return false;

    *outHlsl = translateGLES(glesSource, isVertex, SH_HLSL_3_0_OUTPUT);

    if (*outHlsl) {
        if (outLen) *outLen = strlen(*outHlsl);
        return true;
    }

    // If HLSL 3.0 failed, try without some optimizations that may cause issues
    // with complex shaders - re-init compiler with simpler options
    // Actually, let's just return the failure
    if (outLen) *outLen = 0;
    return false;
}

bool ShaderTranslator_translateGLES2HLSL11(
    const char* glesSource,
    bool isVertex,
    char** outHlsl,
    size_t* outLen
) {
    if (!outHlsl) return false;

    *outHlsl = translateGLES(glesSource, isVertex, SH_HLSL_4_1_OUTPUT);

    if (*outHlsl) {
        if (outLen) *outLen = strlen(*outHlsl);
        return true;
    }

    if (outLen) *outLen = 0;
    return false;
}

} // extern "C"
