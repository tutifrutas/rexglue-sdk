// Copyright 2026. Released under the BSD license; see the SDK LICENSE.
#pragma once

#include <cstdint>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>

namespace rex::graphics::vulkan::rov {

// Bump whenever the compiler resource mapping, constants, or stage I/O changes.
// New shader/pipeline caches must include this identity; old SPIR-V is not ABI
// compatible merely because the original guest microcode hash is unchanged.
inline constexpr uint32_t kShaderAbiVersion = 2;
inline constexpr char kCompilerIdentity[] = "dxbc-spirv@687a30131e50533fedd9e785a1bf3aea463504f2";
using SystemConstants = DxbcShaderTranslator::SystemConstants;

inline constexpr uint32_t kSetMemory = 0;
inline constexpr uint32_t kSetConstants = 1;
inline constexpr uint32_t kSetVertexTextures = 2;
inline constexpr uint32_t kSetPixelTextures = 3;

// Bindings 0 and 1 preserve the compute/resolve storage-buffer representation.
inline constexpr uint32_t kBindingSharedMemorySplit = 0;
inline constexpr uint32_t kBindingEdramStorage = 1;
inline constexpr uint32_t kBindingEdramTexel = 2;
inline constexpr uint32_t kBindingSharedMemoryWhole = 3;

// Existing generated Vulkan helper stages retain their own layout at binding0.
// Guest DXBC-derived shaders consume the Windows layout at binding5 instead.
inline constexpr uint32_t kBindingLegacyHelperConstants = 0;
inline constexpr uint32_t kBindingSystemConstants = 5;
inline constexpr uint32_t kConstantBufferCount = 6;
inline constexpr uint32_t kBindingVertexFloatConstants = 1;
inline constexpr uint32_t kBindingPixelFloatConstants = 2;
inline constexpr uint32_t kBindingBoolLoopConstants = 3;
inline constexpr uint32_t kBindingFetchConstants = 4;

}  // namespace rex::graphics::vulkan::rov
