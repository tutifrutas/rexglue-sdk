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

#include <algorithm>
#include <cstring>
#include <string>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");


namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(rex::thread::Semaphore* semaphore,
                               uint32_t frequency, uint32_t channels,
                               bool need_format_conversion)
    : semaphore_(semaphore),
      frame_frequency_(frequency),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion) {
  switch (frame_channels_) {
    case 6:
      channel_samples_ = 256;
      break;
    case 2:
      channel_samples_ = 768;
      break;
    default:
      assert_unhandled_case(frame_channels_);
  }
  frame_size_ = sizeof(float) * frame_channels_ * channel_samples_;
  assert_true(frame_size_ <= kFrameSizeMax);
  assert_true(!need_format_conversion_ || frame_channels_ == 6);
}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
};

bool SDLAudioDriver::Initialize() {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return false;
  }
  sdl_initialized_ = true;

  // SDL2's desired_spec.samples: one guest frame per device buffer.
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,
              std::to_string(channel_samples_).c_str());

  SDL_AudioSpec desired_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32;
  desired_spec.channels = frame_channels_;
  // Allow the hardware to decide between 5.1 and stereo,
  // unless the input is stereo
  if (frame_channels_ != 2) {
    SDL_AudioSpec device_spec = {};
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device_spec,
                                 nullptr) &&
        (device_spec.channels == 2 || device_spec.channels == 6)) {
      desired_spec.channels = device_spec.channels;
    }
    // If the system is 4 or 7.1, let SDL convert
  }
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &desired_spec, SDLStreamCallback,
                                          this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }
  sdl_device_channels_ = desired_spec.channels;

  SDL_ResumeAudioStreamDevice(sdl_stream_);

  return true;
}

void SDLAudioDriver::SubmitFrame(float* frame) {
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_channels_ * channel_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, frame, frame_size_);

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
  }
}

void SDLAudioDriver::Pause() { SDL_PauseAudioStreamDevice(sdl_stream_); }

void SDLAudioDriver::Resume() { SDL_ResumeAudioStreamDevice(sdl_stream_); }

void SDLAudioDriver::Shutdown() {
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  };
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  };
}

void SDLAudioDriver::SDLCallback(void* userdata, Uint8* stream, int len) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::sdl_callback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  assert_true(len == sizeof(float) * driver->channel_samples_ *
                         driver->sdl_device_channels_);

  std::unique_lock<std::mutex> guard(driver->frames_mutex_);
  if (driver->frames_queued_.empty()) {
    std::memset(stream, 0, len);
  } else {
    auto buffer = driver->frames_queued_.front();
    driver->frames_queued_.pop();
    if (REXCVAR_GET(audio_mute)) {
      std::memset(stream, 0, len);
    } else if (driver->need_format_conversion_) {
      switch (driver->sdl_device_channels_) {
        case 2:
          conversion::sequential_6_BE_to_interleaved_2_LE(
              reinterpret_cast<float*>(stream), buffer,
              driver->channel_samples_);
          break;
        case 6:
          conversion::sequential_6_BE_to_interleaved_6_LE(
              reinterpret_cast<float*>(stream), buffer,
              driver->channel_samples_);
          break;
        default:
          assert_unhandled_case(driver->sdl_device_channels_);
          break;
      }
    } else {
      assert_true(driver->sdl_device_channels_ == driver->frame_channels_);
      if (driver->volume_ != 1.0f) {
        std::memset(stream, 0, len);
        SDL_MixAudio(stream, reinterpret_cast<Uint8*>(buffer), SDL_AUDIO_F32, len,
                     driver->volume_);
      } else {
        std::memcpy(stream, buffer, len);
      }
    }
    driver->frames_unused_.push(buffer);

    auto ret = driver->semaphore_->Release(1, nullptr);
    assert_true(ret);
  }
};
void SDLAudioDriver::SDLStreamCallback(void* userdata, SDL_AudioStream* stream,
                                       int additional_amount,
                                       [[maybe_unused]] int total_amount) {
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int len = static_cast<int>(sizeof(float) * driver->channel_samples_ *
                                   driver->sdl_device_channels_);
  Uint8* buffer = SDL_stack_alloc(Uint8, len);
  while (additional_amount > 0) {
    SDLCallback(userdata, buffer, len);
    if (!SDL_PutAudioStreamData(stream, buffer, len)) {
      break;
    }
    additional_amount -= len;
  }
  SDL_stack_free(buffer);
}

}  // namespace rex::audio::sdl
