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

#include <memory>
#include <string>

#include <rex/audio/audio_system.h>

namespace rex::audio::nop {

class NopAudioSystem : public AudioSystem {
 public:
  explicit NopAudioSystem(runtime::FunctionDispatcher* function_dispatcher);
  ~NopAudioSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<AudioSystem> Create(runtime::FunctionDispatcher* function_dispatcher);

  std::string name() const override { return "NOP"; }

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  AudioDriver* CreateDriver(rex::thread::Semaphore* semaphore,
                            uint32_t frequency, uint32_t channels,
                            bool need_format_conversion) override;
};

}  // namespace rex::audio::nop
