// Copyright 2026. Released under the BSD license; see the SDK LICENSE.
#include <catch2/catch_test_macros.hpp>
#include <SPIRV/spirv.hpp>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/dxbc.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/rov_runtime_compiler.h>
#include <rex/graphics/vulkan/tessellation_shader.h>
#include <rex/string/buffer.h>

#include <array>
#include <bit>
#include <map>
#include <set>
#include <tuple>

using namespace rex::graphics;
using namespace rex::graphics::vulkan;

namespace {
// Compare the actual compiled interfaces, not the GLSL spelling. Domain inputs
// and control-stage outputs must both be non-patch arrays of scalar float32.
std::pair<uint32_t, uint32_t> ScalarControlPointInterface(
    const std::vector<uint32_t>& code, spv::StorageClass storage) {
  struct Type { spv::Op op; std::vector<uint32_t> args; };
  struct Variable { uint32_t type, id, storage; };
  std::map<uint32_t, Type> types;
  std::map<uint32_t, uint32_t> constants;
  std::set<uint32_t> location_zero, patch;
  std::vector<Variable> variables;
  REQUIRE(code.size() >= 5);
  for (size_t i = 5; i < code.size();) {
    const uint32_t count = code[i] >> 16;
    const auto op = spv::Op(code[i] & 0xffff);
    REQUIRE(count > 0);
    REQUIRE(i + count <= code.size());
    if (op == spv::OpTypeFloat || op == spv::OpTypeArray || op == spv::OpTypePointer)
      types.emplace(code[i + 1], Type{op, {code.begin() + i + 2, code.begin() + i + count}});
    else if (op == spv::OpConstant && count == 4)
      constants[code[i + 2]] = code[i + 3];
    else if (op == spv::OpVariable)
      variables.push_back({code[i + 1], code[i + 2], code[i + 3]});
    else if (op == spv::OpDecorate && code[i + 2] == spv::DecorationLocation && code[i + 3] == 0)
      location_zero.insert(code[i + 1]);
    else if (op == spv::OpDecorate && code[i + 2] == spv::DecorationPatch)
      patch.insert(code[i + 1]);
    i += count;
  }
  for (const auto& var : variables) {
    if (var.storage != uint32_t(storage) || !location_zero.contains(var.id)) continue;
    REQUIRE_FALSE(patch.contains(var.id));
    const auto& pointer = types.at(var.type);
    REQUIRE(pointer.op == spv::OpTypePointer);
    const auto& array = types.at(pointer.args.at(1));
    REQUIRE(array.op == spv::OpTypeArray);
    const auto& scalar = types.at(array.args.at(0));
    REQUIRE(scalar.op == spv::OpTypeFloat);
    return {scalar.args.at(0), constants.at(array.args.at(1))};
  }
  FAIL("No location-zero control-point interface found");
  return {};
}
}  // namespace

TEST_CASE("ROV control and domain stages agree on scalar control-point indices", "[graphics][rov]") {
  using Host = Shader::HostVertexShaderType;
  const std::array cases{
      std::tuple{Host::kTriangleDomainCPIndexed, xenos::TessellationMode::kContinuous, 3u},
      std::tuple{Host::kQuadDomainCPIndexed, xenos::TessellationMode::kContinuous, 4u},
      std::tuple{Host::kTriangleDomainPatchIndexed, xenos::TessellationMode::kContinuous, 1u},
      std::tuple{Host::kQuadDomainPatchIndexed, xenos::TessellationMode::kContinuous, 1u},
      std::tuple{Host::kTriangleDomainPatchIndexed, xenos::TessellationMode::kAdaptive, 1u},
      std::tuple{Host::kQuadDomainPatchIndexed, xenos::TessellationMode::kAdaptive, 1u}};
  for (const auto& [host, mode, points] : cases) {
    CAPTURE(uint32_t(host), uint32_t(mode));
    const auto source = GetTessellationControlShaderGlsl(host, mode, points);
    std::vector<uint32_t> control;
    std::string error;
    REQUIRE(VulkanCommandProcessor::CompileGlslToSpirv(
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, source, control, error));
    INFO(error);
    // Original six-word test program: alloc position; exec_end one ALU;
    // add oPos, r0, r1. Both domain registers stay live, including CP indices.
    constexpr uint32_t alloc = uint32_t(ucode::ControlFlowOpcode::kAlloc) << 12 |
        uint32_t(ucode::AllocType::kVsPosition) << 9;
    constexpr uint32_t exec = 1u | (1u << 12);
    constexpr std::array<uint32_t, 6> ucode_words{
        0, alloc | (exec << 16), uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 28,
        uint32_t(ucode::ExportRegister::kVSPosition) | (1u << 15) | (15u << 16),
        0, (1u << 31) | (1u << 30) | (1u << 8)};
    DxbcShader shader(xenos::ShaderType::kVertex, 0, ucode_words.data(), ucode_words.size(), std::endian::native);
    rex::string::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    DxbcShaderTranslator translator(rex::ui::GraphicsProvider::GpuVendorID::kNvidia, false, true);
    DxbcShaderTranslator::Modification modification(translator.GetDefaultVertexShaderModification(0, host));
    modification.vertex.interpolator_mask = 0;
    auto* translated = shader.GetOrCreateTranslation(modification.value);
    REQUIRE(translator.TranslateAnalyzedShader(*translated));
    const auto& dxbc = translated->translated_binary();
    const auto domain = CompileRovDxbc(dxbc.data(), dxbc.size(),
        MakeRovDescriptorMappings(xenos::ShaderType::kVertex, 0, 0));
    INFO(domain.diagnostic);
    REQUIRE(domain.succeeded);
    const auto output = ScalarControlPointInterface(control, spv::StorageClassOutput);
    const auto input = ScalarControlPointInterface(domain.spirv, spv::StorageClassInput);
    CHECK(output == input);
    CHECK(output.first == 32);
    CHECK(output.second == points);
  }
}
