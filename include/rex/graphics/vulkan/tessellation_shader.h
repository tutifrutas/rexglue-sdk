// Copyright 2026. Released under the BSD license; see the SDK LICENSE.
#pragma once

#include <string>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::vulkan {
// Produces scalar per-control-point indices, matching DXBC domain-stage input.
std::string GetTessellationControlShaderGlsl(
    Shader::HostVertexShaderType host_vertex_shader_type,
    xenos::TessellationMode tessellation_mode, uint32_t output_control_points);
}  // namespace rex::graphics::vulkan
