#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/vulkan/rov_system_constants.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <bit>
#include <cmath>
#include <cstring>

using namespace rex::graphics;
using namespace rex::graphics::vulkan::rov;

TEST_CASE("ROV exponent bias covers the signed guest range", "[graphics][rov]") {
  RegisterFile regs;
  draw_util::ViewportInfo viewport{};
  ConstantInputs in{regs, viewport};
  for (int bias = -32; bias <= 31; ++bias) {
    reg::RB_COLOR_INFO color{};
    color.color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
    color.color_exp_bias = bias;
    regs[reg::RB_COLOR_INFO::rt_register_indices[0]] = color.value;
    INFO(bias);
    REQUIRE(BuildSystemConstants(in).color_exp_bias[0] == std::ldexp(1.0f, bias));
  }
}

TEST_CASE("ROV MSAA addressing uses Windows sample geometry", "[graphics][rov]") {
  RegisterFile regs;
  draw_util::ViewportInfo viewport{};
  ConstantInputs in{regs, viewport};
  in.scale_x = 3;
  in.scale_y = 2;
  reg::RB_DEPTH_INFO depth{};
  depth.depth_base = 17;
  regs[reg::RB_DEPTH_INFO::register_index] = depth.value;
  for (unsigned samples = 0; samples < 3; ++samples) {
    reg::RB_SURFACE_INFO surface{};
    surface.surface_pitch = 1024;
    surface.msaa_samples = static_cast<xenos::MsaaSamples>(samples);
    regs[reg::RB_SURFACE_INFO::register_index] = surface.value;
    const auto constants = BuildSystemConstants(in);
    REQUIRE(constants.sample_count_log2[0] == (samples == 2 ? 1 : 0));
    REQUIRE(constants.sample_count_log2[1] == (samples >= 1 ? 1 : 0));
    const unsigned tile = 80 * 16 * 3 * 2;
    REQUIRE(constants.edram_32bpp_tile_pitch_dwords_scaled ==
            ((1024 * (samples == 2 ? 2 : 1) + 79) / 80) * tile);
    REQUIRE(constants.edram_depth_base_dwords_scaled == 17 * tile);
  }
}

TEST_CASE("ROV stencil is unpacked and depth aliases disable tests", "[graphics][rov]") {
  RegisterFile regs;
  draw_util::ViewportInfo viewport{};
  ConstantInputs in{regs, viewport};
  in.primitive_polygonal = true;
  in.depth_control.stencil_enable = 1;
  in.depth_control.z_enable = 1;
  in.depth_control.z_write_enable = 1;
  in.depth_control.backface_enable = 1;
  in.depth_control.zfunc = xenos::CompareFunction::kLessEqual;
  reg::RB_DEPTH_INFO depth{};
  depth.depth_base = 20;
  regs[reg::RB_DEPTH_INFO::register_index] = depth.value;
  reg::RB_STENCILREFMASK front{};
  front.stencilref = 31;
  front.stencilmask = 0xAA;
  front.stencilwritemask = 0x55;
  regs[reg::RB_STENCILREFMASK::register_index] = front.value;
  reg::RB_STENCILREFMASK back{};
  back.stencilref = 13;
  back.stencilmask = 0xCC;
  back.stencilwritemask = 0x33;
  regs[XE_GPU_REG_RB_STENCILREFMASK_BF] = back.value;
  auto constants = BuildSystemConstants(in);
  REQUIRE(constants.flags & DxbcShaderTranslator::kSysFlag_ROVStencilTest);
  REQUIRE(constants.flags & DxbcShaderTranslator::kSysFlag_ROVDepthWrite);
  REQUIRE(constants.edram_stencil_front_reference == 31);
  REQUIRE(constants.edram_stencil_front_read_mask == 0xAA);
  REQUIRE(constants.edram_stencil_front_write_mask == 0x55);
  REQUIRE(constants.edram_stencil_back_reference == 13);
  REQUIRE(constants.edram_stencil_back_read_mask == 0xCC);
  REQUIRE(constants.edram_stencil_back_write_mask == 0x33);
  in.primitive_polygonal = false;
  constants = BuildSystemConstants(in);
  REQUIRE(std::memcmp(constants.edram_stencil_front, constants.edram_stencil_back, 16) == 0);
  reg::RB_COLOR_INFO color{};
  color.color_base = 20;
  color.color_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
  regs[reg::RB_COLOR_INFO::rt_register_indices[0]] = color.value;
  in.color_mask = 15;
  constants = BuildSystemConstants(in);
  REQUIRE_FALSE(constants.flags & DxbcShaderTranslator::kSysFlag_ROVDepthStencil);
}

TEST_CASE("ROV constants preserve texture signs and clip plane order", "[graphics][rov]") {
  RegisterFile regs;
  draw_util::ViewportInfo viewport{};
  ConstantInputs in{regs, viewport};
  for (unsigned i = 0; i < 32; ++i)
    in.texture_signs[i] = uint8_t(i);
  in.textures_resolution_scaled = 0x81000001;
  reg::PA_CL_CLIP_CNTL clip{};
  clip.ucp_ena = 0b100001;
  regs[reg::PA_CL_CLIP_CNTL::register_index] = clip.value;
  for (unsigned i = 0; i < 24; ++i)
    regs[XE_GPU_REG_PA_CL_UCP_0_X + i] = std::bit_cast<uint32_t>(float(i + 1));
  const auto constants = BuildSystemConstants(in);
  REQUIRE(constants.texture_swizzled_signs[0] == 0x03020100);
  REQUIRE(constants.texture_swizzled_signs[7] == 0x1F1E1D1C);
  REQUIRE(constants.textures_resolution_scaled == 0x81000001);
  REQUIRE(constants.user_clip_planes[0][0] == 1.0f);
  REQUIRE(constants.user_clip_planes[1][0] == 21.0f);
  REQUIRE(constants.user_clip_planes[2][0] == 0.0f);
  const auto again = BuildSystemConstants(in);
  REQUIRE(std::memcmp(&constants, &again, sizeof(constants)) == 0);
}

TEST_CASE("External shader compiler results reject empty or fatal output", "[graphics][rov]") {
  uint32_t ucode[3] = {};
  DxbcShader shader(xenos::ShaderType::kVertex, 0, ucode, 3, std::endian::native);
  auto* result = shader.GetOrCreateTranslation(0);
  REQUIRE_FALSE(result->SetTranslatedResult({}));
  REQUIRE(result->is_translated());
  REQUIRE_FALSE(result->is_valid());
  REQUIRE(result->SetTranslatedResult({1, 2, 3, 4}, {{false, "warning"}}));
  result->set_host_disassembly("old disassembly");
  REQUIRE_FALSE(result->SetTranslatedResult({1, 2, 3, 4}, {{true, "compiler failure"}}));
  REQUIRE(result->host_disassembly().empty());
  REQUIRE(result->errors().size() == 1);
}
