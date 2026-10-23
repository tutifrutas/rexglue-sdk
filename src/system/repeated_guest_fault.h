#pragma once

#include <cstdint>

namespace rex::runtime {

// Keep one instance per host thread. Count only faults that memory callbacks
// could not handle, never normal write-watch or MMIO faults.
class RepeatedGuestFault {
 public:
  static constexpr uint32_t kLimit = 8;

  bool Record(uint64_t address) {
    if (!count_ || address_ != address) {
      address_ = address;
      count_ = 1;
    } else if (count_ < kLimit) {
      ++count_;
    }
    return count_ == kLimit;
  }

 private:
  uint64_t address_ = 0;
  uint32_t count_ = 0;
};

}  // namespace rex::runtime
