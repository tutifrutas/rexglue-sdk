// Copyright 2026. Released under the BSD license; see the SDK LICENSE.
#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/vulkan/rov_runtime_compiler.h>
#include <ir/ir.h>
#include <SPIRV/spirv.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <tuple>

using namespace rex::graphics;
using namespace rex::graphics::vulkan;

TEST_CASE("ROV runtime mappings match the guest descriptor ABI", "[graphics][rov]") {
  using Type = dxbc_spv::ir::ScalarType;
  for (auto stage : {xenos::ShaderType::kVertex, xenos::ShaderType::kPixel}) {
    const auto mappings = MakeRovDescriptorMappings(stage, 2, 2);
    const uint32_t texture_set = stage == xenos::ShaderType::kVertex ? 2 : 3;
    std::set<std::tuple<uint32_t, uint32_t, uint32_t>> registers;
    for (const auto& entry : mappings)
      REQUIRE(registers.emplace(entry.descriptor_type, entry.register_space, entry.register_index).second);
    auto check = [&](Type type, uint32_t index, uint32_t set, uint32_t binding) {
      const auto entry = std::find_if(mappings.begin(), mappings.end(), [&](const auto& item) {
        return item.descriptor_type == uint32_t(type) && item.register_space == 0 && item.register_index == index;
      });
      REQUIRE(entry != mappings.end());
      CHECK(entry->set == set);
      CHECK(entry->binding == binding);
    };
    REQUIRE(mappings.size() == 11);
    check(Type::eCbv, 0, 1, 5);  // Windows guest constants, NOT helper binding 0.
    check(Type::eCbv, 1, 1, stage == xenos::ShaderType::kVertex ? 1 : 2);
    check(Type::eCbv, 2, 1, 3);
    check(Type::eCbv, 3, 1, 4);
    check(Type::eSrv, 0, 0, 3);  // Whole shared-memory buffer.
    check(Type::eUav, 0, 0, 3);
    check(Type::eUav, 1, 0, 2);  // R32_UINT EDRAM texel view.
    check(Type::eSrv, 1, texture_set, 0);
    check(Type::eSrv, 2, texture_set, 1);
    check(Type::eSampler, 0, texture_set, 2);
    check(Type::eSampler, 1, texture_set, 3);
  }
}

TEST_CASE("ROV runtime compiles generated depth-only DXBC and rejects unmapped resources", "[graphics][rov]") {
  DxbcShaderTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kNvidia, false, true);
  const auto dxbc = translator.CreateDepthOnlyPixelShader();
  REQUIRE_FALSE(dxbc.empty());
  const auto compiled = CompileRovDxbc(dxbc.data(), dxbc.size(), MakeRovDescriptorMappings(xenos::ShaderType::kPixel, 0, 0));
  INFO(compiled.diagnostic);
  REQUIRE(compiled.succeeded);
  REQUIRE(compiled.spirv.size() >= 5);
  CHECK(compiled.spirv[0] == 0x07230203);
  const auto unmapped = CompileRovDxbc(dxbc.data(), dxbc.size(), {});
  REQUIRE_FALSE(unmapped.succeeded);
  CHECK(unmapped.spirv.empty());
  CHECK(unmapped.diagnostic.find("type=") != std::string::npos);
  CHECK(unmapped.diagnostic.find("space=") != std::string::npos);
  CHECK(unmapped.diagnostic.find("register=") != std::string::npos);
}

TEST_CASE("ROV runtime rejects empty and malformed DXBC", "[graphics][rov]") {
  CHECK_FALSE(CompileRovDxbc(nullptr, 0, {}).succeeded);
  const std::array<uint8_t, 32> invalid{};
  const auto malformed = CompileRovDxbc(invalid.data(), invalid.size(), {});
  CHECK_FALSE(malformed.succeeded);
  CHECK(malformed.spirv.empty());
}

TEST_CASE("ROV float controls follow the qualified DXVK policy", "[graphics][rov]") {
  DxbcShaderTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kNvidia, false, true);
  const auto dxbc = translator.CreateDepthOnlyPixelShader();
  const auto mappings = MakeRovDescriptorMappings(xenos::ShaderType::kPixel, 0, 0);
  for (unsigned mask = 0; mask < 16; ++mask) {
    CAPTURE(mask);
    const RovFloatControls controls{bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)};
    const auto compiled = CompileRovDxbc(dxbc.data(), dxbc.size(), mappings, controls);
    INFO(compiled.diagnostic);
    REQUIRE(compiled.succeeded);
    std::set<spv::ExecutionMode> modes;
    for (size_t i = 5; i < compiled.spirv.size();) {
      const auto count = compiled.spirv[i] >> 16;
      REQUIRE(count > 0);
      REQUIRE(i + count <= compiled.spirv.size());
      if ((compiled.spirv[i] & 0xffff) == spv::OpExecutionMode) {
        const auto mode = spv::ExecutionMode(compiled.spirv[i + 2]);
        if (mode == spv::ExecutionModeRoundingModeRTE || mode == spv::ExecutionModeDenormFlushToZero ||
            mode == spv::ExecutionModeSignedZeroInfNanPreserve) {
          REQUIRE(count == 4);
          CHECK(compiled.spirv[i + 3] == 32);
          modes.insert(mode);
        }
      }
      i += count;
    }
    const bool explicit_modes = (mask & 7) == 7;
    CHECK(modes.contains(spv::ExecutionModeRoundingModeRTE) == explicit_modes);
    CHECK(modes.contains(spv::ExecutionModeDenormFlushToZero) == explicit_modes);
    CHECK(modes.contains(spv::ExecutionModeSignedZeroInfNanPreserve) == bool(mask & 8));
  }
}
