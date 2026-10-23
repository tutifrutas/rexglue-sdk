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
#include <rex/audio/nop/nop_audio_system.h>
#include <rex/audio/nop_audio_driver.h>
#include <rex/audio/pacing_audio_driver.h>

namespace rex::audio::nop {

std::unique_ptr<AudioSystem> NopAudioSystem::Create(runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<NopAudioSystem>(function_dispatcher);
}

NopAudioSystem::NopAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

NopAudioSystem::~NopAudioSystem() = default;

X_STATUS NopAudioSystem::CreateDriver(size_t index,
                                      rex::thread::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  assert_not_null(out_driver);
  // The audio worker already paces registered guest clients.
  *out_driver = new NopAudioDriver(semaphore);
  return X_STATUS_SUCCESS;
}

AudioDriver* NopAudioSystem::CreateDriver(rex::thread::Semaphore* semaphore,
                                          uint32_t frequency, uint32_t channels,
                                          bool need_format_conversion) {
  if (!channels || channels > AudioDriver::kFrameSamplesMax) {
    return nullptr;
  }
  return new PacingAudioDriver(semaphore, frequency,
                               AudioDriver::kFrameSamplesMax / channels);
}

}  // namespace rex::audio::nop
