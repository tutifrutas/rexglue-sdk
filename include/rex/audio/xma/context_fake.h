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

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>

#include <rex/audio/xma/helpers.h>
#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/thread.h>

extern "C" {
#include "libavcodec/avcodec.h"
}  // extern "C"
#include <rex/audio/xma/context.h>


#include <array>
#include <atomic>
#include <mutex>
#include <queue>


namespace rex::audio {

class XmaContextFake : public XmaContext {
 public:
  static constexpr uint32_t kBitsPerPacketHeader = 32;
  static constexpr uint32_t kMaxFrameSizeinBits = 0x4000 - kBitsPerPacketHeader;

  explicit XmaContextFake();
  ~XmaContextFake() override;

  int Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) override;
  bool Work() override;

  void Enable() override;
  void Clear() override;
  void Disable() override;
  void Release() override;

 private:
  void ProcessPacket(XMA_CONTEXT_DATA* data);
  void Consume(memory::RingBuffer* output_rb, XMA_CONTEXT_DATA* data);
  void UpdateLoopStatus(XMA_CONTEXT_DATA* data);
  memory::RingBuffer PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data);

  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  static int GetSampleRate(int id);
  uint8_t* GetCurrentInputBuffer(XMA_CONTEXT_DATA* data);
  static uint32_t GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data);

  // Fake frame data - generate silence
  std::array<uint8_t, kBytesPerFrameChannel * 2> fake_frame_;

  // Minimal state tracking
  int32_t remaining_subframe_blocks_in_output_buffer_ = 0;
  uint8_t current_frame_remaining_subframes_ = 0;
};

}  // namespace rex::audio
