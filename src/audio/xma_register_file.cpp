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

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/flags.h>
#include <rex/audio/xma/context.h>
#include <rex/audio/xma/context_fake.h>
#include <rex/audio/xma/context_master.h>
#include <rex/audio/xma/context_new.h>
#include <rex/audio/xma/context_old.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/helpers.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/stream.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
}  // extern "C"



#include <cstring>


namespace rex::audio {

XmaRegisterFile::XmaRegisterFile() { std::memset(values, 0, sizeof(values)); }

const XmaRegisterInfo* XmaRegisterFile::GetRegisterInfo(uint32_t index) {
  switch (index) {
#define XE_XMA_REGISTER(index, name)          \
  case index: {                               \
    static const XmaRegisterInfo reg_info = { \
        #name,                                \
    };                                        \
    return &reg_info;                         \
  }
#include <rex/audio/xma/register_table.inc>
#undef XE_XMA_REGISTER
    default:
      return nullptr;
  }
}

}  // namespace rex::audio
