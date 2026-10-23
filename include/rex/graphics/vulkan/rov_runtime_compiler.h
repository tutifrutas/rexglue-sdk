// Copyright 2026. Released under the BSD license; see the SDK LICENSE.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics::vulkan {


// Explicit library-level resource mapping used by the runtime DXBC compiler.
// binding is the compact host descriptor slot, not the DXBC register number.
struct RovDescriptorMapping {
  // dxbc_spv::ir::ScalarType numeric value (kept opaque in this public header).
  uint32_t descriptor_type = 0;
  uint32_t register_space = 0;
  uint32_t register_index = 0;
  uint32_t set = 0;
  uint32_t binding = 0;
};

// Actual device capabilities, not requested DXBC modes. Defaults advertise no
// optional float controls. Xenos guest arithmetic uses F32, not F16/F64.
struct RovFloatControls {
  bool independent_denorm_modes = false;
  bool float32_round_to_nearest_even = false;
  bool float32_flush_to_zero = false;
  bool float32_signed_zero_inf_nan_preserve = false;
};

struct RovCompilerResult {
  std::vector<uint32_t> spirv;
  std::vector<RovDescriptorMapping> resource_mapping;
  std::string diagnostic;
  bool succeeded = false;
};

// Compiles generated guest DXBC through dxbc-spirv's library API. The caller
// supplies explicit mappings derived from the DXBC translator's registration
// order and the Vulkan descriptor layout; no CLI defaults are used.
std::vector<RovDescriptorMapping> MakeRovDescriptorMappings(xenos::ShaderType stage,
                                                              uint32_t texture_count,
                                                              uint32_t sampler_count);
RovCompilerResult CompileRovDxbc(const uint8_t* dxbc, size_t dxbc_size,
                                const std::vector<RovDescriptorMapping>& mappings,
                                const RovFloatControls& float_controls = {});

}  // namespace rex::graphics::vulkan
