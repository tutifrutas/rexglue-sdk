// Copyright 2026. Released under the BSD license; see the SDK LICENSE.
#include <rex/graphics/vulkan/rov_diagnostics.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <rex/logging/macros.h>

namespace rex::graphics::vulkan::rov::diagnostics {
namespace {
std::string Escape(std::string_view value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
  return out;
}
std::string Hex(std::span<const std::byte> bytes) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (auto byte : bytes)
    out << std::setw(2) << std::to_integer<unsigned>(byte);
  return out.str();
}
}  // namespace

std::string SerializeIdentity(const Identity& i) {
  return "{\"schema\":\"rov-diagnostic-v1\",\"type\":\"identity\",\"backend\":" +
         Escape(i.backend) + ",\"compiler\":" + Escape(i.compiler) +
         ",\"abi_version\":" + std::to_string(i.abi_version) + "}";
}
std::string SerializeDraw(const DrawRecord& r) {
  std::ostringstream out;
  out << "{\"schema\":\"rov-diagnostic-v1\",\"type\":\"draw\",\"draw_id\":" << r.draw_id
      << ",\"shader_hash\":\"" << std::hex << r.shader_hash << std::dec
      << "\",\"stage\":" << Escape(r.stage) << ",\"compiler\":" << Escape(r.compiler)
      << ",\"abi_version\":" << r.abi_version
      << ",\"system_constants_size\":" << r.system_constants.size()
      << ",\"system_constants_hex\":" << Escape(Hex(r.system_constants)) << ",\"resources\":[";
  for (size_t n = 0; n < r.resources.size(); ++n) {
    const auto& x = r.resources[n];
    if (n)
      out << ',';
    out << "{\"set\":" << x.set << ",\"binding\":" << x.binding << ",\"kind\":" << Escape(x.kind)
        << ",\"id\":" << Escape(x.id) << '}';
  }
  out << "],\"resolve\":" << Escape(r.resolve_metadata) << '}';
  return out.str();
}

Capture::Capture(std::filesystem::path directory)
    : directory_(std::move(directory)), active_(!directory_.empty()) {}

// mutex_ is held by the caller. Stop a failed/full capture once: never flood the
// render log every subsequent draw, and never let diagnostics break gameplay.
bool Capture::WriteLine(const std::string& line) {
  if (line.size() > kMaximumCaptureBytes - bytes_) {
    active_.store(false, std::memory_order_relaxed);
    REXLOG_WARN("ROV diagnostics capture byte limit reached; capture stopped");
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);
  if (ec) {
    active_.store(false, std::memory_order_relaxed);
    REXLOG_ERROR("ROV diagnostics cannot create capture directory: {}", ec.message());
    return false;
  }
  std::ofstream file(directory_ / "rov-diagnostics.jsonl", std::ios::binary | std::ios::app);
  file.write(line.data(), static_cast<std::streamsize>(line.size()));
  file.flush();
  if (!file) {
    active_.store(false, std::memory_order_relaxed);
    REXLOG_ERROR("ROV diagnostics write/flush failed; capture stopped");
    return false;
  }
  bytes_ += line.size();
  return true;
}

bool Capture::WriteIdentity(const Identity& identity) {
  if (directory_.empty())
    return true;
  std::lock_guard lock(mutex_);
  if (!enabled())
    return false;
  return WriteLine(SerializeIdentity(identity) + '\n');
}

bool Capture::WriteDraw(const DrawRecord& record) {
  if (directory_.empty())
    return true;
  std::lock_guard lock(mutex_);
  if (!enabled())
    return false;
  if (records_ >= kMaximumRecords) {
    active_.store(false, std::memory_order_relaxed);
    REXLOG_WARN("ROV diagnostics draw limit reached; capture stopped");
    return false;
  }
  if (!WriteLine(SerializeDraw(record) + '\n'))
    return false;
  ++records_;
  return true;
}
}  // namespace rex::graphics::vulkan::rov::diagnostics
