#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/vulkan/rov_diagnostics.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace rex::graphics::vulkan::rov::diagnostics;
namespace {
struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("tekken6-rov-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(path));
  }
  ~TemporaryDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
}  // namespace

TEST_CASE("ROV diagnostics preserve bytes and escape strings", "[graphics][rov]") {
  const std::array bytes{std::byte{0x01}, std::byte{0xab}};
  DrawRecord record{
      7, 0x1234, "pixel", "compiler-x", 2, bytes, {{0, 2, "image", "edram"}}, "format=rgba8\n"};
  auto json = SerializeDraw(record);
  REQUIRE(json.find("\"system_constants_hex\":\"01ab\"") != std::string::npos);
  REQUIRE(json.find("\"draw_id\":7") != std::string::npos);
  REQUIRE(json.find("format=rgba8\\n") != std::string::npos);
  record.stage = std::string("a\0b", 3);
  REQUIRE(SerializeDraw(record).find("a\\u0000b") != std::string::npos);
}

TEST_CASE("ROV capture stops at its record budget", "[graphics][rov]") {
  TemporaryDirectory dir;
  Capture capture(dir.path);
  REQUIRE(capture.WriteIdentity({"vulkan", "dxbc-spirv", 2}));
  for (size_t i = 0; i < kMaximumRecords; ++i)
    REQUIRE(capture.WriteDraw({}));
  REQUIRE_FALSE(capture.WriteDraw({}));
  REQUIRE_FALSE(capture.enabled());
  REQUIRE_FALSE(capture.WriteDraw({}));
  std::ifstream input(dir.path / "rov-diagnostics.jsonl");
  size_t lines = 0;
  for (std::string line; std::getline(input, line);)
    ++lines;
  REQUIRE(lines == kMaximumRecords + 1);
}

TEST_CASE("Disabled and failed ROV captures do not write", "[graphics][rov]") {
  Capture disabled({});
  REQUIRE_FALSE(disabled.enabled());
  REQUIRE(disabled.WriteDraw({}));
  TemporaryDirectory dir;
  const auto file = dir.path / "not-a-directory";
  std::ofstream(file) << "preserve";
  Capture failed(file);
  REQUIRE_FALSE(failed.WriteDraw({}));
  REQUIRE_FALSE(failed.enabled());
  REQUIRE_FALSE(failed.WriteIdentity({}));
  std::ifstream stream(file);
  std::string contents;
  stream >> contents;
  REQUIRE(contents == "preserve");
}

TEST_CASE("ROV parallel writers produce intact records", "[graphics][rov]") {
  TemporaryDirectory dir;
  Capture capture(dir.path);
  std::array<std::thread, 4> threads;
  for (auto& thread : threads)
    thread = std::thread([&] {
      for (unsigned i = 0; i < 16; ++i)
        capture.WriteDraw({});
    });
  for (auto& thread : threads)
    thread.join();
  std::ifstream stream(dir.path / "rov-diagnostics.jsonl");
  size_t lines = 0;
  for (std::string line; std::getline(stream, line);) {
    REQUIRE(line.starts_with("{\"schema\":"));
    REQUIRE(line.ends_with("}"));
    ++lines;
  }
  REQUIRE(lines == 64);
}
