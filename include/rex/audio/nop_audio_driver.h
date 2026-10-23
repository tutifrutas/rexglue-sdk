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

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace rex::audio::nop {

// Accepts frames and discards them. The audio worker paces the guest's
// callbacks itself and uses the semaphore only as back-pressure, so a frame
// is released as soon as it is submitted.
class NopAudioDriver : public AudioDriver {
 public:
  explicit NopAudioDriver(rex::thread::Semaphore* semaphore);
  ~NopAudioDriver() override;

  bool Initialize() override;
  void SubmitFrame(float* frame) override;
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float volume) override {}
  void Shutdown() override {}

 private:
  rex::thread::Semaphore* semaphore_ = nullptr;
};

}  // namespace rex::audio::nop
