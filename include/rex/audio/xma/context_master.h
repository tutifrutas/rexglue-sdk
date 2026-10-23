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


// Forward declarations
struct AVCodec;
struct AVCodecParserContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace rex::audio {

class XmaContextMaster : public XmaContext {
 public:
  explicit XmaContextMaster();
  ~XmaContextMaster();

  int Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr);
  bool Work();

  void Enable();
  void Clear();
  void Disable();
  void Release();

 private:
  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  static bool TrySetupNextLoop(XMA_CONTEXT_DATA* data,
                               bool ignore_input_buffer_offset);
  static void NextPacket(XMA_CONTEXT_DATA* data);
  static int GetSampleRate(int id);
  // Get the offset of the next frame. Does not traverse packets.
  static size_t GetNextFrame(uint8_t* block, size_t size, size_t bit_offset);
  // Get the containing packet number of the frame pointed to by the offset.
  static int GetFramePacketNumber(uint8_t* block, size_t size,
                                  size_t bit_offset);
  // Get the packet number and the index of the frame inside that packet
  static std::tuple<int, int> GetFrameNumber(uint8_t* block, size_t size,
                                             size_t bit_offset);
  // Get the number of frames contained in the packet (including truncated) and
  // if the last frame is split.
  static std::tuple<int, bool> GetPacketFrameCount(uint8_t* packet);

  bool ValidFrameOffset(uint8_t* block, size_t size_bytes,
                        size_t frame_offset_bits);
  void Decode(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(uint8_t* packet, int sample_rate, bool is_two_channel);

  // uint32_t decoded_consumed_samples_ = 0; // TODO do this dynamically
  // int decoded_idx_ = -1;

  // bool partial_frame_saved_ = false;
  // bool partial_frame_size_known_ = false;
  // size_t partial_frame_total_size_bits_ = 0;
  // size_t partial_frame_start_offset_bits_ = 0;
  // size_t partial_frame_offset_bits_ = 0;  // blah internal don't use this
  // std::vector<uint8_t> partial_frame_buffer_;
  uint32_t packets_skip_ = 0;

  bool is_stream_done_ = false;
  // bool split_frame_pending_ = false;
  uint32_t split_frame_len_ = 0;
  uint32_t split_frame_len_partial_ = 0;
  uint8_t split_frame_padding_start_ = 0;
  // first byte contains bit offset information
  std::array<uint8_t, 1 + 4096> xma_frame_;

  // uint8_t* current_frame_ = nullptr;
  // conversion buffer for 2 channel frame
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_frame_;
  // std::vector<uint8_t> current_frame_ = std::vector<uint8_t>(0);
};

}  // namespace rex::audio
