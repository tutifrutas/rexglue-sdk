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

#include <rex/memory.h>

// This file contains some functions used to help parse XMA data.


#include <stdint.h>

namespace rex::audio::xma {

static constexpr uint32_t kMaxFrameLength = 0x7FFF;

// Get number of frames that /begin/ in this packet. This is valid only for XMA2
// packets
static const uint8_t GetPacketFrameCount(const uint8_t* packet) {
  return packet[0] >> 2;
}

static const uint8_t GetPacketMetadata(const uint8_t* packet) {
  return packet[2] & 0x7;
}

static const bool IsPacketXma2Type(const uint8_t* packet) {
  return GetPacketMetadata(packet) == 1;
}

static const uint8_t GetPacketSkipCount(const uint8_t* packet) {
  return packet[3];
}

// Get the first frame offset in bits
static uint32_t GetPacketFrameOffset(const uint8_t* packet) {
  uint32_t val = (uint16_t)(((packet[0] & 0x3) << 13) | (packet[1] << 5) |
                            (packet[2] >> 3));
  return val + 32;
}

}  // namespace rex::audio::xma
