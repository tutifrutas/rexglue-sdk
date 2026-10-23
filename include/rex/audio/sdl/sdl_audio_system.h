/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Ported from Xenia Canary e6cd2e4 into ReXGlue, 2026. Canary
 *              targets SDL2; only the SDL calls are translated to SDL3.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/audio/audio_system.h>

namespace rex::audio::sdl {

class SDLAudioSystem : public AudioSystem {
 public:
  explicit SDLAudioSystem(runtime::FunctionDispatcher* function_dispatcher);
  ~SDLAudioSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<AudioSystem> Create(runtime::FunctionDispatcher* function_dispatcher);

  std::string name() const override { return "SDL"; }

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  AudioDriver* CreateDriver(rex::thread::Semaphore* semaphore, uint32_t frequency,
                            uint32_t channels, bool need_format_conversion) override;

 protected:
  void Initialize() override;
};

}  // namespace rex::audio::sdl
