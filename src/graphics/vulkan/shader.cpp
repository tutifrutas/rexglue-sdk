/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>
#include <cstring>

#include <rex/assert.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/provider.h>

namespace rex::graphics::vulkan {

VulkanShader::VulkanTranslation::~VulkanTranslation() {
  if (shader_module_) {
    const ui::vulkan::VulkanDevice* const vulkan_device =
        static_cast<const VulkanShader&>(shader()).vulkan_device_;
    vulkan_device->functions().vkDestroyShaderModule(vulkan_device->device(), shader_module_,
                                                     nullptr);
  }
}

VkShaderModule VulkanShader::VulkanTranslation::GetOrCreateShaderModule() {
  if (!is_valid()) {
    return VK_NULL_HANDLE;
  }
  if (shader_module_ != VK_NULL_HANDLE) {
    return shader_module_;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      static_cast<const VulkanShader&>(shader()).vulkan_device_;
  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = translated_binary().size();
  shader_module_create_info.pCode = reinterpret_cast<const uint32_t*>(translated_binary().data());
  if (vulkan_device->functions().vkCreateShaderModule(vulkan_device->device(),
                                                      &shader_module_create_info, nullptr,
                                                      &shader_module_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "VulkanShader::VulkanTranslation: Failed to create a Vulkan shader "
        "module for shader {:016X} modification {:016X}",
        shader().ucode_data_hash(), modification());
    MakeInvalid();
    return VK_NULL_HANDLE;
  }
  return shader_module_;
}

VulkanShader::VulkanShader(const ui::vulkan::VulkanDevice* const vulkan_device,
                           const xenos::ShaderType shader_type, const uint64_t ucode_data_hash,
                           const uint32_t* const ucode_dwords, const size_t ucode_dword_count,
                           const std::endian ucode_source_endian)
    : SpirvShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian),
      vulkan_device_(vulkan_device) {
  assert_not_null(vulkan_device);
}

void VulkanShader::SetRovBindings(const DxbcShader& dxbc_shader) {
  std::lock_guard<std::mutex> lock(rov_bindings_mutex_);
  if (rov_bindings_set_up_) {
    return;
  }
  rov_texture_bindings_.reserve(dxbc_shader.GetTextureBindingsAfterTranslation().size());
  for (const auto& binding : dxbc_shader.GetTextureBindingsAfterTranslation()) {
    SpirvShader::TextureBinding& compact = rov_texture_bindings_.emplace_back();
    std::memset(&compact, 0, sizeof(compact));
    compact.fetch_constant = binding.fetch_constant;
    compact.dimension = binding.dimension;
    compact.is_signed = uint32_t(binding.is_signed);
  }
  rov_sampler_bindings_.reserve(dxbc_shader.GetSamplerBindingsAfterTranslation().size());
  for (const auto& binding : dxbc_shader.GetSamplerBindingsAfterTranslation()) {
    SpirvShader::SamplerBinding& compact = rov_sampler_bindings_.emplace_back();
    std::memset(&compact, 0, sizeof(compact));
    compact.fetch_constant = binding.fetch_constant;
    compact.mag_filter = binding.mag_filter;
    compact.min_filter = binding.min_filter;
    compact.mip_filter = binding.mip_filter;
    compact.aniso_filter = binding.aniso_filter;
  }
  rov_used_texture_mask_ = dxbc_shader.GetUsedTextureMaskAfterTranslation();
  rov_bindings_set_up_ = true;
}

Shader::Translation* VulkanShader::CreateTranslationInstance(uint64_t modification) {
  return new VulkanTranslation(*this, modification);
}

}  // namespace rex::graphics::vulkan
