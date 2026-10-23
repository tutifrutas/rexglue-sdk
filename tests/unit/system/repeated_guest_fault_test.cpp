#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "system/repeated_guest_fault.h"

using rex::runtime::RepeatedGuestFault;

TEST_CASE("Repeated unhandled guest faults reach a bounded limit", "[system][memory]") {
  RepeatedGuestFault faults;
  for (uint32_t i = 1; i < RepeatedGuestFault::kLimit; ++i)
    CHECK_FALSE(faults.Record(0));
  CHECK(faults.Record(0));
  CHECK(faults.Record(0));
  CHECK_FALSE(faults.Record(0x1234));
  for (uint32_t i = 1; i < RepeatedGuestFault::kLimit - 1; ++i)
    CHECK_FALSE(faults.Record(0x1234));
  CHECK(faults.Record(0x1234));
}

TEST_CASE("Repeated guest fault state is independent per thread", "[system][memory]") {
  static thread_local RepeatedGuestFault faults;
  for (uint32_t i = 1; i < RepeatedGuestFault::kLimit; ++i)
    CHECK_FALSE(faults.Record(0x5678));
  bool other_thread_hit_limit = true;
  std::thread other([&] { other_thread_hit_limit = faults.Record(0x5678); });
  other.join();
  CHECK_FALSE(other_thread_hit_limit);
  CHECK(faults.Record(0x5678));
}
