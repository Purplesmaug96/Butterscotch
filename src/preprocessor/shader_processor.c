//
// Shader Processor for Butterscotch Preprocessor
//
// Translates GLSL ES shader source (from the SHDR chunk) to HLSL9
// using ANGLE's shader translator, and writes the translated shaders
// to a SHADERS.BIN file that can be loaded at runtime on Xbox 360.
//
// This allows games that only have GLSL ES shader source (which is
// always present in GameMaker games) to use shaders on D3D9 platforms
// like Xbox 360.
//

#include "shader_processor.h"
#include "byte_writer.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Include the shader translator
#include "shader_translator.h"

// Write a single shader entry to the byte writer
// Format:
//   uint8_t  nameLen
//   char[]   name (nameLen bytes)
//   uint8_t  type (0=vertex, 1=fragment)
//   uint32_t hlslLen
//   char[]   hlslSource (hlslLen bytes)
static void writeShaderEntry(ByteWriter* w, const char* name, bool isVertex, const char* hlslSource) {
    size_t nameLen = name ? strlen(name) : 0;
    size_t hlslLen = hlslSource ? strlen(hlslSource) : 0;

    ByteWriter_writeUint8(w, (uint8_t)nameLen);
    if (nameLen > 0) {
        ByteWriter_writeBytes(w, (const uint8_t*)name, nameLen);
    }
    ByteWriter_writeUint8(w, (uint8_t)(isVertex ? 0 : 1));
    ByteWriter_writeUint32(w, (uint32_t)hlslLen);
    if (hlslLen > 0) {
        ByteWriter_writeBytes(w, (const uint8_t*)hlslSource, hlslLen);
    }
}

// Process all shaders from the DataWin SHDR chunk:
// For each shader that has GLSL ES source but no HLSL9 source,
// translate the GLSL ES source to HLSL9 using ANGLE.
//
// Returns a SHADERS.BIN blob containing all translated shaders.
// The format is:
//   uint8_t  version (0)
//   uint16_t shaderCount
//   (for each shader:)
//     uint8_t  nameLen
//     char[]   name
//     uint8_t  type (0=vertex, 1=fragment)
//     uint32_t hlslLen
//     char[]   hlslSource
//
// If a shader already has HLSL9 source, it is kept as-is.
// If a shader has no GLSL ES source either, it is skipped.
uint8_t* ShaderProcessor_processShaders(
    Shader* shaders,
    uint32_t shaderCount,
    size_t* outSize
) {
    if (shaderCount == 0 || !shaders) {
        *outSize = 0;
        return NULL;
    }

    ByteWriter w = ByteWriter_create(4096);

    // Header
    ByteWriter_writeUint8(&w, 0);                           // version
    ByteWriter_writeUint16(&w, (uint16_t)shaderCount);      // shaderCount

    uint32_t translatedCount = 0;
    uint32_t keptCount = 0;
    uint32_t skippedCount = 0;

    for (uint32_t i = 0; i < shaderCount; i++) {
        Shader* shdr = &shaders[i];
        if (!shdr->present) {
            skippedCount++;
            // Write an empty entry
            writeShaderEntry(&w, shdr->name, false, NULL);
            continue;
        }

        const char* name = shdr->name ? shdr->name : "unnamed";

        // Process vertex shader
        if (shdr->hlsl9_Vertex && shdr->hlsl9_Vertex[0]) {
            // Already has HLSL9 source, keep it
            writeShaderEntry(&w, name, true, shdr->hlsl9_Vertex);
            keptCount++;
        } else if (shdr->glslES_Vertex && shdr->glslES_Vertex[0]) {
            // Translate GLSL ES to HLSL9
            char* hlsl = NULL;
            if (ShaderTranslator_translateGLES2HLSL9(shdr->glslES_Vertex, true, &hlsl, NULL)) {
                writeShaderEntry(&w, name, true, hlsl);
                free(hlsl);
                translatedCount++;
            } else {
                // Fall back to glsl_Vertex if available
                if (shdr->glsl_Vertex && shdr->glsl_Vertex[0]) {
                    char* hlsl2 = NULL;
                    if (ShaderTranslator_translateGLES2HLSL9(shdr->glsl_Vertex, true, &hlsl2, NULL)) {
                        writeShaderEntry(&w, name, true, hlsl2);
                        free(hlsl2);
                        translatedCount++;
                    } else {
                        writeShaderEntry(&w, name, true, NULL);
                        skippedCount++;
                    }
                } else {
                    writeShaderEntry(&w, name, true, NULL);
                    skippedCount++;
                }
            }
        } else if (shdr->glsl_Vertex && shdr->glsl_Vertex[0]) {
            // Try GLSL (non-ES) as fallback
            char* hlsl = NULL;
            if (ShaderTranslator_translateGLES2HLSL9(shdr->glsl_Vertex, true, &hlsl, NULL)) {
                writeShaderEntry(&w, name, true, hlsl);
                free(hlsl);
                translatedCount++;
            } else {
                writeShaderEntry(&w, name, true, NULL);
                skippedCount++;
            }
        } else {
            writeShaderEntry(&w, name, true, NULL);
            skippedCount++;
        }

        // Process fragment shader
        if (shdr->hlsl9_Fragment && shdr->hlsl9_Fragment[0]) {
            writeShaderEntry(&w, name, false, shdr->hlsl9_Fragment);
            keptCount++;
        } else if (shdr->glslES_Fragment && shdr->glslES_Fragment[0]) {
            char* hlsl = NULL;
            if (ShaderTranslator_translateGLES2HLSL9(shdr->glslES_Fragment, false, &hlsl, NULL)) {
                writeShaderEntry(&w, name, false, hlsl);
                free(hlsl);
                translatedCount++;
            } else {
                if (shdr->glsl_Fragment && shdr->glsl_Fragment[0]) {
                    char* hlsl2 = NULL;
                    if (ShaderTranslator_translateGLES2HLSL9(shdr->glsl_Fragment, false, &hlsl2, NULL)) {
                        writeShaderEntry(&w, name, false, hlsl2);
                        free(hlsl2);
                        translatedCount++;
                    } else {
                        writeShaderEntry(&w, name, false, NULL);
                        skippedCount++;
                    }
                } else {
                    writeShaderEntry(&w, name, false, NULL);
                    skippedCount++;
                }
            }
        } else if (shdr->glsl_Fragment && shdr->glsl_Fragment[0]) {
            char* hlsl = NULL;
            if (ShaderTranslator_translateGLES2HLSL9(shdr->glsl_Fragment, false, &hlsl, NULL)) {
                writeShaderEntry(&w, name, false, hlsl);
                free(hlsl);
                translatedCount++;
            } else {
                writeShaderEntry(&w, name, false, NULL);
                skippedCount++;
            }
        } else {
            writeShaderEntry(&w, name, false, NULL);
            skippedCount++;
        }
    }

    printf("Shader processing: %u translated, %u kept (had HLSL9), %u skipped\n",
           translatedCount, keptCount, skippedCount);

    return ByteWriter_detach(&w, outSize);
}