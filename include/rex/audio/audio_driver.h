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

namespace rex::audio {

class AudioDriver {
 public:
  static constexpr uint32_t kFrameFrequencyDefault = 48000;
  static constexpr uint32_t kFrameChannelsDefault = 6;
  static constexpr uint32_t kChannelSamplesDefault = 256;
  static constexpr uint32_t kFrameSamplesMax =
      kFrameChannelsDefault * kChannelSamplesDefault;
  static constexpr uint32_t kFrameSizeMax = sizeof(float) * kFrameSamplesMax;

  virtual ~AudioDriver();

  virtual bool Initialize() = 0;
  virtual void Shutdown() = 0;

  virtual void SubmitFrame(float* samples) = 0;
  virtual void Pause() = 0;
  virtual void Resume() = 0;
  virtual void SetVolume(float volume) = 0;
};

}  // namespace rex::audio
