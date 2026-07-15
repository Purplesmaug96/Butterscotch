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
// ANGLE's standalone HLSL translator outputs placeholder markers like
// "@@ VERTEX ATTRIBUTES @@" that are normally filled in by libANGLE's
// D3D9 renderer. We post-process the output to replace these with
// proper HLSL declarations suitable for GameMaker shaders.
//

#include "shader_translator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

// ANGLE includes
#include "GLSLANG/ShaderLang.h"
#include "angle_gl.h"

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

// Post-process the translated HLSL source to replace ANGLE's placeholder
// markers with proper HLSL declarations.
//
// ANGLE's standalone HLSL translator outputs:
//   @@ VERTEX ATTRIBUTES @@  -> should be VS_INPUT struct
//   @@ VERTEX OUTPUT @@      -> should be VS_OUTPUT or PS_INPUT struct
//   @@ MAIN PROLOGUE @@      -> should copy input members to static variables
//
// For GameMaker shaders, the expected attributes/varyings are:
//   in_Position (float3), in_Colour (float4), in_TextureCoord (float2)
//   v_vTexcoord (float2), v_vColour (float4)
static std::string postProcessHLSL(const std::string& code, bool isVertex) {
    std::string result = code;

    // Replace @@ VERTEX ATTRIBUTES @@ with VS_INPUT struct definition
    // The static variables declared above tell us what attributes are used.
    size_t pos = result.find("@@ VERTEX ATTRIBUTES @@");
    if (pos != std::string::npos) {
        // Build the VS_INPUT struct based on what static attributes are declared
        std::string vertexAttrs =
            "struct VS_INPUT\n"
            "{\n";
        
        // Check which attributes are used by looking at the static declarations
        if (result.find("_in_Position") != std::string::npos) {
            vertexAttrs += "    float3 in_Position : POSITION;\n";
        }
        if (result.find("_in_Colour") != std::string::npos) {
            vertexAttrs += "    float4 in_Colour : COLOR;\n";
        }
        if (result.find("_in_TextureCoord") != std::string::npos) {
            vertexAttrs += "    float2 in_TextureCoord : TEXCOORD0;\n";
        }
        vertexAttrs += "};\n\n";
        
        result.replace(pos, strlen("@@ VERTEX ATTRIBUTES @@"), vertexAttrs);
    }

    // Replace @@ VERTEX OUTPUT @@ with output struct definition
    pos = result.find("@@ VERTEX OUTPUT @@");
    if (pos != std::string::npos) {
        std::string vertexOutput;
        if (isVertex) {
            vertexOutput =
                "struct VS_OUTPUT\n"
                "{\n"
                "    float4 gl_Position : POSITION;\n";
            if (result.find("_v_vTexcoord") != std::string::npos) {
                vertexOutput += "    float2 v_vTexcoord : TEXCOORD0;\n";
            }
            if (result.find("_v_vColour") != std::string::npos) {
                vertexOutput += "    float4 v_vColour : TEXCOORD1;\n";
            }
            vertexOutput += "};\n\n";
        } else {
            vertexOutput =
                "struct PS_INPUT\n"
                "{\n";
            if (result.find("_v_vTexcoord") != std::string::npos) {
                vertexOutput += "    float2 v_vTexcoord : TEXCOORD0;\n";
            }
            if (result.find("_v_vColour") != std::string::npos) {
                vertexOutput += "    float4 v_vColour : TEXCOORD1;\n";
            }
            vertexOutput += "};\n\n";
        }
        result.replace(pos, strlen("@@ VERTEX OUTPUT @@"), vertexOutput);
    }

    // Replace @@ MAIN PROLOGUE @@ with code that copies input to static variables
    pos = result.find("@@ MAIN PROLOGUE @@");
    if (pos != std::string::npos) {
        std::string prologue;
        if (isVertex) {
            prologue = "    _in_Position = input.in_Position;\n"
                       "    _in_Colour = input.in_Colour;\n"
                       "    _in_TextureCoord = input.in_TextureCoord;\n";
        } else {
            prologue = "    _v_vTexcoord = input.v_vTexcoord;\n"
                       "    _v_vColour = input.v_vColour;\n";
        }
        result.replace(pos, strlen("@@ MAIN PROLOGUE @@"), prologue);
    }

    // Fix the main function signature for vertex shaders
    if (isVertex) {
        // Replace "VS_OUTPUT main(VS_INPUT input)" if it was partially formed
        pos = result.find("main(");
        if (pos != std::string::npos) {
            // Find the opening brace of main
            size_t bracePos = result.find('{', pos);
            if (bracePos != std::string::npos) {
                // Replace the signature line
                size_t lineStart = result.rfind('\n', pos);
                if (lineStart == std::string::npos) lineStart = 0;
                std::string before = result.substr(0, lineStart);
                std::string after = result.substr(bracePos);
                result = before + "\nVS_OUTPUT main(VS_INPUT input)\n" + after;
            }
        }
    } else {
        // Fix the main function signature for fragment shaders
        pos = result.find("main(");
        if (pos != std::string::npos) {
            size_t bracePos = result.find('{', pos);
            if (bracePos != std::string::npos) {
                size_t lineStart = result.rfind('\n', pos);
                if (lineStart == std::string::npos) lineStart = 0;
                std::string before = result.substr(0, lineStart);
                std::string after = result.substr(bracePos);
                result = before + "\nfloat4 main(PS_INPUT input) : COLOR0\n" + after;
            }
        }
    }

    return result;
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
            // Post-process to replace ANGLE placeholder markers
            std::string processed = postProcessHLSL(code, isVertex);
            result = strdup(processed.c_str());
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