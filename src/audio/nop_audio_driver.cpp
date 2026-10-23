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

#include <rex/assert.h>
#include <rex/audio/nop_audio_driver.h>

namespace rex::audio::nop {

NopAudioDriver::NopAudioDriver(rex::thread::Semaphore* semaphore)
    : semaphore_(semaphore) {}

NopAudioDriver::~NopAudioDriver() = default;

bool NopAudioDriver::Initialize() { return true; }

void NopAudioDriver::SubmitFrame(float* frame) {
  if (semaphore_) {
    semaphore_->Release(1, nullptr);
  }
}

}  // namespace rex::audio::nop
