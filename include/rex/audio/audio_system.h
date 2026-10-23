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

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/audio.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/thread/mutex.h>

namespace rex::stream {
class ByteStream;
}  // namespace rex::stream

namespace rex::audio {
class AudioDriver;
class XmaDecoder;
}  // namespace rex::audio

#include <atomic>


namespace rex::audio {

constexpr memory::fourcc_t kAudioSaveSignature = memory::make_fourcc("XAUD");

class AudioDriver;
class XmaDecoder;

class AudioSystem : public system::IAudioSystem {
 public:
  static constexpr size_t kMaximumQueuedFrames = 64;
  static constexpr uint32_t kAudioPumpInterval = 5333u;
  static constexpr uint32_t kAudioIntervalSlack = 400u;

  virtual ~AudioSystem();

  virtual std::string name() const = 0;

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }
  XmaDecoder* xma_decoder() const { return xma_decoder_.get(); }

  virtual X_STATUS Setup(system::KernelState* kernel_state) override;
  void Shutdown() override;

  X_STATUS RegisterClient(uint32_t callback, uint32_t callback_arg,
                          size_t* out_index);
  void UnregisterClient(size_t index);
  void SubmitFrame(size_t index, float* samples);

  // Creates an independent, non-registered driver instance.
  virtual AudioDriver* CreateDriver(rex::thread::Semaphore* semaphore,
                                    uint32_t frequency, uint32_t channels,
                                    bool need_format_conversion) = 0;

  bool Save(stream::ByteStream* stream);
  bool Restore(stream::ByteStream* stream);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  explicit AudioSystem(runtime::FunctionDispatcher* function_dispatcher);

  virtual void Initialize();

  void WorkerThreadMain();

  virtual X_STATUS CreateDriver(size_t index,
                                rex::thread::Semaphore* semaphore,
                                AudioDriver** out_driver) = 0;
  X_STATUS CreateClientDriver(size_t index, rex::thread::Semaphore* semaphore,
                              AudioDriver** out_driver);
  void DestroyDriver(AudioDriver* driver);

  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  std::unique_ptr<XmaDecoder> xma_decoder_;

  std::atomic<bool> worker_running_ = {false};
  system::object_ref<system::XHostThread> worker_thread_;

  rex::thread::global_critical_region global_critical_region_;
  static constexpr size_t kMaximumClientCount = 8;
  struct {
    AudioDriver* driver;
    uint64_t next_pump_us;
    uint32_t callback;
    uint32_t callback_arg;
    uint32_t wrapped_callback_arg;
    bool in_use;
    std::mutex lock;
  } clients_[kMaximumClientCount];

  int FindFreeClient();

  std::unique_ptr<rex::thread::Semaphore>
      client_semaphores_[kMaximumClientCount];
  // Event is always there in case we have no clients.
  std::unique_ptr<rex::thread::Event> pending_work_event_;

  bool paused_ = false;
  rex::thread::Fence pause_fence_;
  std::unique_ptr<rex::thread::Event> resume_event_;
};

}  // namespace rex::audio
