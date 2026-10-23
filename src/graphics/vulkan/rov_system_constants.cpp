// Copyright 2026. BSD license; see LICENSE.
// Adapted from this SDK's Windows D3D12CommandProcessor::UpdateSystemConstantValues.
// Keep the Windows ROV rules, not the legacy SPIR-V flag/constant layout.
#include <rex/graphics/vulkan/rov_system_constants.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <type_traits>
#include <rex/graphics/pipeline/render_target/cache.h>
#include <rex/math.h>

namespace rex::graphics::vulkan::rov {

SystemConstants BuildSystemConstants(const ConstantInputs& in) {
  static_assert(std::is_trivially_copyable_v<SystemConstants>);
  SystemConstants out;
  // Deterministic padding, including unused clip planes and inactive RTs, is
  // important for dirty comparisons and diagnostic byte-for-byte captures.
  std::memset(&out, 0, sizeof(out));
  const auto& regs = in.registers;
  const auto clip = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto raster = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  const auto color = regs.Get<reg::RB_COLORCONTROL>();
  const auto depth = regs.Get<reg::RB_DEPTH_INFO>();
  const auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  const auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  const auto dc = in.depth_control;
  reg::RB_COLOR_INFO rt[4];
  bool depth_stencil = dc.stencil_enable || dc.z_enable;
  for (uint32_t i = 0; i < 4; ++i) {
    rt[i] = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    out.edram_rt_keep_mask[i][0] = UINT32_MAX;
    out.edram_rt_keep_mask[i][1] = UINT32_MAX;
    if (in.edram_rov) {
      RenderTargetCache::GetPSIColorFormatInfo(
          rt[i].color_format, (in.color_mask >> (i * 4)) & 15, out.edram_rt_clamp[i][0],
          out.edram_rt_clamp[i][1], out.edram_rt_clamp[i][2], out.edram_rt_clamp[i][3],
          out.edram_rt_keep_mask[i][0], out.edram_rt_keep_mask[i][1]);
      if (depth.depth_base == rt[i].color_base && (out.edram_rt_keep_mask[i][0] != UINT32_MAX ||
                                                   out.edram_rt_keep_mask[i][1] != UINT32_MAX)) {
        depth_stencil = false;
      }
    }
  }
  using T = DxbcShaderTranslator;
  if (in.shared_memory_is_uav)
    out.flags |= T::kSysFlag_SharedMemoryIsUAV;
  if (vte.vtx_xy_fmt)
    out.flags |= T::kSysFlag_XYDividedByW;
  if (vte.vtx_z_fmt)
    out.flags |= T::kSysFlag_ZDividedByW;
  if (vte.vtx_w0_fmt)
    out.flags |= T::kSysFlag_WNotReciprocal;
  if (in.primitive_polygonal)
    out.flags |= T::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs))
    out.flags |= T::kSysFlag_PrimitiveLine;
  if (depth.depth_format == xenos::DepthRenderTargetFormat::kD24FS8)
    out.flags |= T::kSysFlag_DepthFloat24;
  const auto alpha = color.alpha_test_enable ? color.alpha_func : xenos::CompareFunction::kAlways;
  out.flags |= uint32_t(alpha) << T::kSysFlag_AlphaPassIfLess_Shift;
  if (!in.gamma_render_target_as_unorm16) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rt[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
        out.flags |= T::kSysFlag_ConvertColor0ToGamma << i;
    }
  }
  if (in.edram_rov && depth_stencil) {
    out.flags |= T::kSysFlag_ROVDepthStencil;
    if (dc.z_enable) {
      out.flags |= uint32_t(dc.zfunc) << T::kSysFlag_ROVDepthPassIfLess_Shift;
      if (dc.z_write_enable)
        out.flags |= T::kSysFlag_ROVDepthWrite;
    } else {
      out.flags |= T::kSysFlag_ROVDepthPassIfLess | T::kSysFlag_ROVDepthPassIfEqual |
                   T::kSysFlag_ROVDepthPassIfGreater;
    }
    if (dc.stencil_enable)
      out.flags |= T::kSysFlag_ROVStencilTest;
    if (alpha == xenos::CompareFunction::kAlways && !color.alpha_to_mask_enable)
      out.flags |= T::kSysFlag_ROVDepthStencilEarlyWrite;
  }
  out.tessellation_factor_range_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  out.tessellation_factor_range_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  out.line_loop_closing_index = in.line_loop_closing_index;
  out.vertex_index_endian = in.index_endian;
  out.vertex_index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  out.vertex_index_min = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  out.vertex_index_max = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  if (!clip.clip_disable) {
    uint32_t remaining = clip.ucp_ena, source, destination = 0;
    while (rex::bit_scan_forward(remaining, &source)) {
      remaining &= ~(1u << source);
      std::memcpy(out.user_clip_planes[destination++], &regs[XE_GPU_REG_PA_CL_UCP_0_X + source * 4],
                  sizeof(float) * 4);
    }
  }
  std::copy_n(in.viewport.ndc_scale, 3, out.ndc_scale);
  std::copy_n(in.viewport.ndc_offset, 3, out.ndc_offset);
  if (initiator.prim_type == xenos::PrimitiveType::kPointList) {
    const auto limits = regs.Get<reg::PA_SU_POINT_MINMAX>();
    const auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
    out.point_vertex_diameter_min = float(limits.min_size) * (2.0f / 16.0f);
    out.point_vertex_diameter_max = float(limits.max_size) * (2.0f / 16.0f);
    out.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    out.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    out.point_screen_diameter_to_ndc_radius[0] =
        float(in.scale_x) / std::max(in.viewport.xy_extent[0], 1u);
    out.point_screen_diameter_to_ndc_radius[1] =
        float(in.scale_y) / std::max(in.viewport.xy_extent[1], 1u);
  }
  for (uint32_t i = 0; i < 32; ++i)
    out.texture_swizzled_signs[i >> 2] |= uint32_t(in.texture_signs[i]) << ((i & 3) * 8);
  out.textures_resolution_scaled = in.textures_resolution_scaled;
  out.sample_count_log2[0] = surface.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  out.sample_count_log2[1] = surface.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  out.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  out.alpha_to_mask = color.alpha_to_mask_enable ? (color.value >> 24) | (1u << 8) : 0;
  const uint32_t tile =
      xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples * in.scale_x * in.scale_y;
  if (in.edram_rov) {
    out.edram_32bpp_tile_pitch_dwords_scaled =
        (surface.surface_pitch * (surface.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1) +
         xenos::kEdramTileWidthSamples - 1) /
        xenos::kEdramTileWidthSamples * tile;
    out.edram_depth_base_dwords_scaled = depth.depth_base * tile;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    int32_t bias = rt[i].color_exp_bias;
    if (!in.edram_rov && ((rt[i].color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
                           !in.fixed_rg16_truncated) ||
                          (rt[i].color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
                           !in.fixed_rgba16_truncated)))
      bias -= 5;
    // Unsigned arithmetic expresses the Windows exponent adjustment without
    // undefined left shifting of negative signed exponent biases.
    out.color_exp_bias[i] = std::bit_cast<float>(0x3F800000u + (uint32_t(bias) << 23));
    if (in.edram_rov && (out.edram_rt_keep_mask[i][0] != UINT32_MAX ||
                         out.edram_rt_keep_mask[i][1] != UINT32_MAX)) {
      out.edram_rt_base_dwords_scaled[i] = rt[i].color_base * tile;
      out.edram_rt_format_flags[i] = RenderTargetCache::AddPSIColorFormatFlags(rt[i].color_format);
      out.edram_rt_blend_factors_ops[i] =
          regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
    }
  }
  if (in.edram_rov) {
    if (in.primitive_polygonal) {
      if (raster.poly_offset_front_enable) {
        out.edram_poly_offset_front_scale =
            regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        out.edram_poly_offset_front_offset =
            regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (raster.poly_offset_back_enable) {
        out.edram_poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        out.edram_poly_offset_back_offset =
            regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else if (raster.poly_offset_para_enable) {
      out.edram_poly_offset_front_scale = out.edram_poly_offset_back_scale =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      out.edram_poly_offset_front_offset = out.edram_poly_offset_back_offset =
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    const float factor = xenos::kPolygonOffsetScaleSubpixelUnit * std::max(in.scale_x, in.scale_y);
    out.edram_poly_offset_front_scale *= factor;
    out.edram_poly_offset_back_scale *= factor;
    if (depth_stencil && dc.stencil_enable) {
      const auto front = regs.Get<reg::RB_STENCILREFMASK>();
      out.edram_stencil_front_reference = front.stencilref;
      out.edram_stencil_front_read_mask = front.stencilmask;
      out.edram_stencil_front_write_mask = front.stencilwritemask;
      out.edram_stencil_front_func_ops = (dc.value >> 8) & 0xFFF;
      if (in.primitive_polygonal && dc.backface_enable) {
        const auto back = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
        out.edram_stencil_back_reference = back.stencilref;
        out.edram_stencil_back_read_mask = back.stencilmask;
        out.edram_stencil_back_write_mask = back.stencilwritemask;
        out.edram_stencil_back_func_ops = (dc.value >> 20) & 0xFFF;
      } else {
        std::copy_n(out.edram_stencil_front, 4, out.edram_stencil_back);
      }
    }
    out.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    out.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    out.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    out.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }
  return out;
}

}  // namespace rex::graphics::vulkan::rov
