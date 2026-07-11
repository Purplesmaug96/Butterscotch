#pragma once

#include "common.h"

#include "data_win.h"

// Process all shaders: translate GLSL ES to HLSL9 where needed.
// Returns a SHADERS.BIN blob (caller must free via free()).
uint8_t* ShaderProcessor_processShaders(
    Shader* shaders,
    uint32_t shaderCount,
    size_t* outSize
);