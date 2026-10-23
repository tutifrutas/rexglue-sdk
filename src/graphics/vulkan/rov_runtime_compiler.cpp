#include <rex/graphics/vulkan/rov_runtime_compiler.h>
#include <rex/graphics/vulkan/rov_shader_abi.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include <dxbc/dxbc_api.h>
#include <ir/ir_legalize.h>
#include <ir/passes/ir_pass_lower_io.h>
#include <spirv/spirv_builder.h>
#include <spirv/spirv_mapping.h>
#include <util/util_log.h>

namespace rex::graphics::vulkan {
namespace {

class CompileLogger final : public dxbc_spv::util::Logger {
 public:
  dxbc_spv::util::LogLevel getMinimumSeverity() override {
    return dxbc_spv::util::LogLevel::eInfo;
  }
  void message(dxbc_spv::util::LogLevel level, const char* text) override {
    if (level >= dxbc_spv::util::LogLevel::eWarn) {
      if (!message_.empty()) message_ += '\n';
      message_ += text ? text : "(empty dxbc-spirv diagnostic)";
    }
  }
  const std::string& message() const { return message_; }
 private:
  std::string message_;
};

class ExplicitMapping final : public dxbc_spv::spirv::ResourceMapping {
 public:
  explicit ExplicitMapping(const std::vector<RovDescriptorMapping>& mappings)
      : mappings_(mappings) {}

  dxbc_spv::spirv::DescriptorBinding mapDescriptor(dxbc_spv::ir::ScalarType type,
                                                    uint32_t space,
                                                    uint32_t index) override {
    auto found = std::find_if(mappings_.begin(), mappings_.end(),
                              [type, space, index](const auto& item) {
                                return item.descriptor_type == static_cast<uint32_t>(type) &&
                                       item.register_space == space &&
                                       item.register_index == index;
                              });
    if (found == mappings_.end()) {
      if (failure_.empty()) {
        failure_ = "unmapped DXBC descriptor type=" +
                   std::to_string(static_cast<uint32_t>(type)) + " space=" +
                   std::to_string(space) + " register=" + std::to_string(index);
      }
      return {UINT32_MAX, UINT32_MAX};
    }
    return {found->set, found->binding};
  }

  uint32_t mapPushData(dxbc_spv::ir::ShaderStageMask) override { return 0; }
  bool failed() const { return !failure_.empty(); }
  const std::string& failure() const { return failure_; }
 private:
  const std::vector<RovDescriptorMapping>& mappings_;
  std::string failure_;
};

}  // namespace

std::vector<RovDescriptorMapping> MakeRovDescriptorMappings(xenos::ShaderType stage,
                                                              uint32_t texture_count,
                                                              uint32_t sampler_count) {
  const uint32_t set = stage == xenos::ShaderType::kVertex ? rov::kSetVertexTextures
                                                           : rov::kSetPixelTextures;
  std::vector<RovDescriptorMapping> mappings;
  mappings.reserve(size_t(texture_count) + sampler_count + 1);
  constexpr uint32_t kSampler =
      static_cast<uint32_t>(dxbc_spv::ir::ScalarType::eSampler);
  constexpr uint32_t kCbv = static_cast<uint32_t>(dxbc_spv::ir::ScalarType::eCbv);
  constexpr uint32_t kSrv = static_cast<uint32_t>(dxbc_spv::ir::ScalarType::eSrv);
  constexpr uint32_t kUav = static_cast<uint32_t>(dxbc_spv::ir::ScalarType::eUav);
  mappings.push_back({kCbv, 0, 0, rov::kSetConstants,
                      rov::kBindingSystemConstants});
  mappings.push_back({kCbv, 0, 1, rov::kSetConstants,
                      stage == xenos::ShaderType::kVertex
                          ? rov::kBindingVertexFloatConstants
                          : rov::kBindingPixelFloatConstants});
  mappings.push_back({kCbv, 0, 2, rov::kSetConstants,
                      rov::kBindingBoolLoopConstants});
  mappings.push_back({kCbv, 0, 3, rov::kSetConstants,
                      rov::kBindingFetchConstants});
  // DXBC emits main-space t0 for shared memory and bindful textures from t1.
  mappings.push_back({kSrv, 0, 0, rov::kSetMemory, rov::kBindingSharedMemoryWhole});
  for (uint32_t slot = 0; slot < texture_count; ++slot) {
    mappings.push_back({kSrv, 0, slot + 1, set, slot});
  }
  mappings.push_back({kUav, 0, 0, rov::kSetMemory, rov::kBindingSharedMemoryWhole});
  mappings.push_back({kUav, 0, 1, rov::kSetMemory, rov::kBindingEdramTexel});
  for (uint32_t slot = 0; slot < sampler_count; ++slot) {
    mappings.push_back({kSampler, 0, slot, set, texture_count + slot});
  }
  return mappings;
}

RovCompilerResult CompileRovDxbc(const uint8_t* dxbc, size_t dxbc_size,
                                const std::vector<RovDescriptorMapping>& mappings,
                                const RovFloatControls& float_controls) {
  RovCompilerResult result;
  result.resource_mapping = mappings;
  if (!dxbc || !dxbc_size) {
    result.diagnostic = "DXBC input is empty";
    return result;
  }

  CompileLogger logger;
  auto builder = dxbc_spv::dxbc::compileShaderToLegalizedIr(dxbc, dxbc_size, {}, {});
  if (!builder) {
    result.diagnostic = logger.message().empty() ? "dxbc-spirv rejected DXBC" : logger.message();
    return result;
  }

  // Match the upstream compiler's SPIR-V lowering. Legacy D3D shader
  // constants are not used by the ReX DXBC generator, so no CLI-style legacy
  // CBV remapping is introduced here.
  dxbc_spv::ir::LowerIoPass lower_io(*builder);
  lower_io.lowerSampleCountToSpecConstant(0u);
  // Generic locations must match the retained Vulkan geometry helper. DXBC
  // assigns XEPSIZE after position and clip/cull registers, not TEXCOORDs.
  uint32_t interpolator_count = 0;
  auto [first, last] = builder->getDeclarations();
  for (auto it = first; it != last; ++it) {
    if (it->getOpCode() != dxbc_spv::ir::OpCode::eDclOutput) continue;
    const auto semantic = dxbc_spv::ir::IoMap::getSemanticForOp(*builder, *it);
    if (semantic.name == "TEXCOORD") ++interpolator_count;
  }
  for (auto it = first; it != last; ++it) {
    if (it->getOpCode() != dxbc_spv::ir::OpCode::eDclOutput) continue;
    const auto semantic = dxbc_spv::ir::IoMap::getSemanticForOp(*builder, *it);
    if (semantic.name == "XEPSIZE") {
      builder->rewriteOp(it->getDef(), dxbc_spv::ir::Op(*it).setOperand(1u, interpolator_count));
    }
  }
  dxbc_spv::spirv::SpirvBuilder::Options options{};
  options.includeDebugNames = true;
  // Follow DXVK's policy for this same backend, including its all-three gate:
  // dxvk@02e2028c3fe954c8913b1f39bce2f581739f0c41,
  // src/dxvk/dxvk_shader_ir.cpp. DXVK documents a ~20% NVIDIA performance loss
  // from enabling RTE alone; unsupported FTZ must not be emitted. Without
  // the complete subset we rely on driver defaults, as DXVK does, rather than
  // rewrite loads/moves/bitcasts or change DXBC mathematical semantics.
  // VKD3D also documents NVIDIA default-denorm reliance in
  // vkd3d-proton@ef9a02e873715c9e2da0d6db8a236aca8380b303,
  // libs/vkd3d-shader/dxil.c (vkd3d_dxil_converter_can_use_soft_float_fp16_conv).
  if (float_controls.independent_denorm_modes &&
      float_controls.float32_round_to_nearest_even &&
      float_controls.float32_flush_to_zero) {
    options.supportedRoundModesF32 = dxbc_spv::ir::RoundMode::eNearestEven;
    options.supportedDenormModesF32 = dxbc_spv::ir::DenormMode::eFlush;
  }
  options.supportsZeroInfNanPreserveF32 =
      float_controls.float32_signed_zero_inf_nan_preserve;
  ExplicitMapping mapping(mappings);
  dxbc_spv::spirv::SpirvBuilder spirv(*builder, mapping, options);
  spirv.buildSpirvBinary();
  if (mapping.failed()) {
    result.diagnostic = mapping.failure();
    return result;
  }
  result.spirv = spirv.getSpirvBinary();
  if (result.spirv.empty()) {
    result.diagnostic = logger.message().empty() ? "dxbc-spirv emitted empty SPIR-V" : logger.message();
    return result;
  }
  result.succeeded = true;
  result.diagnostic = logger.message();
  return result;
}

}  // namespace rex::graphics::vulkan
