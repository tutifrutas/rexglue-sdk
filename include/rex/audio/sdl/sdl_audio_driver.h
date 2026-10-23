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

#include <mutex>
#include <queue>
#include <stack>

#include <SDL3/SDL.h>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace rex::audio::sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(rex::thread::Semaphore* semaphore,
                 uint32_t frequency = kFrameFrequencyDefault,
                 uint32_t channels = kFrameChannelsDefault,
                 bool need_format_conversion = true);
  ~SDLAudioDriver() override;

  bool Initialize() override;
  void SubmitFrame(float* frame) override;
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override { volume_ = volume; };
  void Shutdown() override;

 protected:
  // SDL2's fixed-size pull, one device buffer of `len` bytes per call.
  static void SDLCallback(void* userdata, Uint8* stream, int len);
  // SDL3 stream callback: delivers SDL2-sized buffers through SDLCallback.
  static void SDLStreamCallback(void* userdata, SDL_AudioStream* stream,
                                int additional_amount, int total_amount);

  rex::thread::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  bool sdl_initialized_ = false;
  uint8_t sdl_device_channels_ = 0;

  float volume_ = 1.0f;

  uint32_t frame_frequency_;
  uint32_t frame_channels_;
  uint32_t channel_samples_;
  uint32_t frame_size_;
  bool need_format_conversion_;
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};
};

}  // namespace rex::audio::sdl
