// Copyright 2026. BSD license; see LICENSE.
#pragma once

#include <array>
#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/vulkan/rov_shader_abi.h>

namespace rex::graphics::vulkan::rov {

// Inputs whose owners are outside the register file. No GPU objects are needed
// to test the Windows ROV constant-generation contract.
struct ConstantInputs {
  const RegisterFile& registers;
  const draw_util::ViewportInfo& viewport;
  reg::RB_DEPTHCONTROL depth_control{};
  uint32_t color_mask = 0;
  uint32_t scale_x = 1;
  uint32_t scale_y = 1;
  uint32_t line_loop_closing_index = 0;
  xenos::Endian index_endian = xenos::Endian::kNone;
  bool shared_memory_is_uav = false;
  bool primitive_polygonal = false;
  bool edram_rov = true;
  bool gamma_render_target_as_unorm16 = false;
  bool fixed_rg16_truncated = false;
  bool fixed_rgba16_truncated = false;
  std::array<uint8_t, 32> texture_signs{};
  uint32_t textures_resolution_scaled = 0;
};

SystemConstants BuildSystemConstants(const ConstantInputs& inputs);

}  // namespace rex::graphics::vulkan::rov
