/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Ported from Xenia Canary e6cd2e4 into ReXGlue, 2026
 */

#pragma once

#include <cstdint>
#include <string>



#include <cstdint>
#include <cstdlib>

namespace rex::audio {

struct XmaRegister {
#define XE_XMA_REGISTER(index, name) static const uint32_t name = index;
#include <rex/audio/xma/register_table.inc>
#undef XE_XMA_REGISTER
};

struct XmaRegisterInfo {
  const char* name;
};

class XmaRegisterFile {
 public:
  XmaRegisterFile();

  static const XmaRegisterInfo* GetRegisterInfo(uint32_t index);

  static constexpr size_t kRegisterCount = (0xFFFF + 1) / 4;
  uint32_t values[kRegisterCount];

  uint32_t operator[](uint32_t reg) const { return values[reg]; }
  uint32_t& operator[](uint32_t reg) { return values[reg]; }
};

}  // namespace rex::audio
