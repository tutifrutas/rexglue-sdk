// Copyright 2026. Released under the BSD license; see the SDK LICENSE.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rex::graphics::vulkan::rov::diagnostics {

inline constexpr size_t kMaximumRecords = 256;
inline constexpr size_t kMaximumCaptureBytes = 16 * 1024 * 1024;

struct Identity {
  std::string backend;
  std::string compiler;
  uint32_t abi_version = 0;
};

struct Resource {
  uint32_t set = 0;
  uint32_t binding = 0;
  std::string kind;
  std::string id;
};

struct DrawRecord {
  uint64_t draw_id = 0;
  uint64_t shader_hash = 0;
  std::string stage;
  std::string compiler;
  uint32_t abi_version = 0;
  // Exact native-memory bytes of rov::SystemConstants; caller supplies its size.
  std::span<const std::byte> system_constants;
  std::vector<Resource> resources;
  std::string resolve_metadata;
};

// An empty directory disables captures. Errors continue to be logged by callers.
class Capture {
 public:
  explicit Capture(std::filesystem::path directory);
  bool enabled() const noexcept { return active_.load(std::memory_order_relaxed); }
  bool WriteIdentity(const Identity& identity);
  bool WriteDraw(const DrawRecord& record);

 private:
  bool WriteLine(const std::string& line);
  std::filesystem::path directory_;
  std::atomic<bool> active_;
  std::mutex mutex_;
  size_t records_ = 0;
  size_t bytes_ = 0;
};

std::string SerializeIdentity(const Identity& identity);
std::string SerializeDraw(const DrawRecord& record);

}  // namespace rex::graphics::vulkan::rov::diagnostics
