// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
// Native Linux NT synchronization. See docs/linux-ntsync.md for upstream
// contracts and research. This is an independent implementation of the Linux
// UAPI, not a copy of Wine's LGPL implementation.
#include <rex/platform.h>
#include <rex/thread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <linux/ntsync.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <span>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <rex/chrono/chrono_steady_cast.h>
#include <rex/logging.h>
#include <rex/thread/timer_queue.h>

namespace rex::thread {
namespace {
// Guest execution permits asynchronous termination. Never allow that to
// interrupt allocation, refcount bookkeeping, or a host-side critical section.
// Only the blocking wait ioctl temporarily restores asynchronous cancellation.
class DeferredCancellation {
 public:
  DeferredCancellation() { pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &previous_); }
  ~DeferredCancellation() noexcept(false) { pthread_setcanceltype(previous_, nullptr); }

 private:
  int previous_;
};

struct Object {
  enum class Kind { kEvent, kSemaphore, kMutex };
  Object(int descriptor, Kind type) : fd(descriptor), kind(type) {}
  Object(Object&& other) noexcept : fd(std::exchange(other.fd, -1)), kind(other.kind) {}
  Object(const Object&) = delete;
  ~Object() {
    // close() is a pthread cancellation point; releasing an owning C++
    // reference must not trigger forced unwinding inside a noexcept destructor.
    if (fd >= 0)
      syscall(SYS_close, fd);
  }
  int fd;
  const Kind kind;
};

struct Device {
  Device() : fd(open("/dev/ntsync", O_RDONLY | O_CLOEXEC)) {
    if (fd < 0) {
      std::fprintf(stderr, "ReXGlue: native Linux synchronization requires /dev/ntsync: %s\n",
                   std::strerror(errno));
      std::abort();
    }
    REXSYS_INFO("Linux synchronization backend: NTSYNC (/dev/ntsync)");
  }
  ~Device() { close(fd); }
  int fd;
  std::mutex mutexes_lock;
  std::vector<std::weak_ptr<Object>> mutexes;
};
Device& GetDevice() {
  // The timer queue and runtime worker threads have independent static/TLS
  // destruction order. Keep the instance alive until process teardown, just as
  // Wine's process-wide device handle; the OS closes its fd on exit.
  static Device* const device = new Device;
  return *device;
}

std::shared_ptr<Object> CreateObject(Object::Kind kind, unsigned long request, void* args) {
  DeferredCancellation guard;
  auto& device = GetDevice();
  int fd = ioctl(device.fd, request, args);
  if (fd < 0) {
    REXSYS_ERROR("NTSYNC object creation failed: {}", std::strerror(errno));
    return {};
  }
  // Zero is the UAPI sentinel for 'no alert'; don't expose stdio descriptors.
  if (fd < 3) {
    int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    close(fd);
    if (replacement < 0)
      return {};
    fd = replacement;
  }
  Object owned_fd(fd, kind);
  auto object = std::make_shared<Object>(std::move(owned_fd));
  if (kind == Object::Kind::kMutex) {
    std::lock_guard lock(device.mutexes_lock);
    std::erase_if(device.mutexes, [](const auto& entry) { return entry.expired(); });
    device.mutexes.push_back(object);
  }
  return object;
}
std::shared_ptr<Object> CreateEvent(bool manual, bool signaled) {
  ntsync_event_args args{uint32_t(manual), uint32_t(signaled)};
  return CreateObject(Object::Kind::kEvent, NTSYNC_IOC_CREATE_EVENT, &args);
}
bool EventOperation(const std::shared_ptr<Object>& event, unsigned long request) {
  uint32_t previous = 0;
  if (ioctl(event->fd, request, &previous) == 0)
    return true;
  REXSYS_ERROR("NTSYNC event operation failed: {}", std::strerror(errno));
  return false;
}

// Linux does not abandon mutexes automatically. Track every live kernel mutex,
// including ones retained by an in-flight wait after its wrapper is closed.
void AbandonMutexes(uint32_t owner) {
  DeferredCancellation guard;
  auto& device = GetDevice();
  std::lock_guard lock(device.mutexes_lock);
  for (auto it = device.mutexes.begin(); it != device.mutexes.end();) {
    if (auto object = it->lock()) {
      if (ioctl(object->fd, NTSYNC_IOC_MUTEX_KILL, &owner) < 0 && errno != EPERM) {
        REXSYS_ERROR("NTSYNC mutex abandonment failed: {}", std::strerror(errno));
      }
      ++it;
    } else {
      it = device.mutexes.erase(it);
    }
  }
}

struct ThreadState {
  ThreadState() : completion(CreateEvent(true, false)), alert(CreateEvent(true, false)) {
    if (sem_init(&suspend_semaphore, 0, 0) != 0)
      std::abort();
  }
  ~ThreadState() { sem_destroy(&suspend_semaphore); }
  bool valid() const { return completion && alert; }
  std::shared_ptr<Object> completion, alert;
  pthread_t pthread{};
  uint32_t tid = 0;
  std::mutex lock;
  std::condition_variable started_cv;
  bool started = false;
  std::atomic<bool> finished{false};
  std::atomic<uint32_t> suspend_count{0};
  sem_t suspend_semaphore;
  std::deque<std::function<void()>> callbacks;
};

void FinishThread(const std::shared_ptr<ThreadState>& state) {
  DeferredCancellation guard;
  {
    std::lock_guard lock(state->lock);
    if (state->finished.exchange(true))
      return;
  }
  AbandonMutexes(state->tid);
  EventOperation(state->completion, NTSYNC_IOC_EVENT_SET);
}
struct ThreadContext {
  std::shared_ptr<ThreadState> state;
  ~ThreadContext() {
    if (state)
      FinishThread(state);
  }
};
thread_local ThreadContext context;
// Used by the suspend signal handler without invoking TLS initialization.
thread_local ThreadState* signal_state = nullptr;

int SuspendSignal() {
  return SIGRTMIN;
}
void WaitSuspended(ThreadState& state) {
  while (state.suspend_count.load(std::memory_order_acquire)) {
    while (sem_wait(&state.suspend_semaphore) < 0 && errno == EINTR) {}
  }
}
void SuspendHandler(int) {
  if (signal_state)
    WaitSuspended(*signal_state);
}
void InstallSuspendHandler() {
  static std::once_flag installed;
  std::call_once(installed, [] {
    struct sigaction action{};
    action.sa_handler = SuspendHandler;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(SuspendSignal(), &action, nullptr) != 0)
      std::abort();
  });
}
std::shared_ptr<ThreadState> CurrentState() {
  if (!context.state) {
    InstallSuspendHandler();
    auto state = std::make_shared<ThreadState>();
    if (!state->valid())
      std::abort();
    state->pthread = pthread_self();
    state->tid = uint32_t(syscall(SYS_gettid));
    state->started = true;
    context.state = state;
    signal_state = state.get();
  }
  return context.state;
}

bool DispatchCallbacks(const std::shared_ptr<ThreadState>& state) {
  // Detach only callbacks pending at entry. A callback may queue more work;
  // that belongs to the next alertable wait, not an unbounded dispatch loop.
  std::deque<std::function<void()>> callbacks;
  {
    std::lock_guard lock(state->lock);
    callbacks.swap(state->callbacks);
    EventOperation(state->alert, NTSYNC_IOC_EVENT_RESET);
  }
  for (auto& callback : callbacks)
    if (callback)
      callback();
  return !callbacks.empty();
}

class NativeWaitHandle {
 public:
  virtual ~NativeWaitHandle() = default;
  virtual std::shared_ptr<Object> object() const = 0;
};
std::shared_ptr<Object> GetObject(WaitHandle* handle) {
  auto native = dynamic_cast<NativeWaitHandle*>(handle);
  return native ? native->object() : nullptr;
}

uint64_t Deadline(std::chrono::nanoseconds timeout) {
  if (timeout == std::chrono::nanoseconds::max())
    return UINT64_MAX;
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t ns = uint64_t(now.tv_sec) * 1000000000ull + now.tv_nsec;
  uint64_t duration = uint64_t(std::max(timeout.count(), int64_t(0)));
  return duration >= UINT64_MAX - ns ? UINT64_MAX : ns + duration;
}
uint64_t Deadline(std::chrono::milliseconds timeout) {
  if (timeout == std::chrono::milliseconds::max())
    return UINT64_MAX;
  if (timeout.count() > INT64_MAX / 1000000)
    return UINT64_MAX;
  return Deadline(std::chrono::nanoseconds(std::max(timeout.count(), int64_t(0)) * 1000000));
}
std::pair<WaitResult, size_t> WaitObjects(std::span<const std::shared_ptr<Object>> objects,
                                          bool all, bool alertable, uint64_t deadline) {
  auto state = CurrentState();
  // APCs queued before entry run even for a zero-duration alertable wait.
  if (alertable && DispatchCallbacks(state))
    return {WaitResult::kUserCallback, 0};
  std::array<int, NTSYNC_MAX_WAIT_COUNT> fds{};
  for (size_t i = 0; i < objects.size(); ++i)
    fds[i] = objects[i]->fd;
  ntsync_wait_args args{};
  args.timeout = deadline;
  args.objs = reinterpret_cast<uintptr_t>(fds.data());
  args.count = uint32_t(objects.size());
  args.owner = state->tid;
  args.alert = alertable ? uint32_t(state->alert->fd) : 0;
  unsigned long request = all && objects.size() > 1 ? NTSYNC_IOC_WAIT_ALL : NTSYNC_IOC_WAIT_ANY;
  for (;;) {
    // ioctl is not a pthread cancellation point. Bound asynchronous cancellation
    // to this syscall; object references and owner tracking already exist.
    int previous_type;
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &previous_type);
    int result = ioctl(GetDevice().fd, request, &args);
    int error = errno;
    pthread_setcanceltype(previous_type, nullptr);
    if (!result) {
      if (args.index == objects.size()) {
        if (DispatchCallbacks(state))
          return {WaitResult::kUserCallback, 0};
        continue;
      }
      return {WaitResult::kSuccess, args.index};
    }
    if (error == EINTR) {
      pthread_testcancel();
      continue;
    }
    if (error == ETIMEDOUT)
      return {WaitResult::kTimeout, 0};
    if (error == EOWNERDEAD)
      return {WaitResult::kAbandoned, args.index};
    REXSYS_ERROR("NTSYNC wait failed: {}", std::strerror(error));
    return {WaitResult::kFailed, 0};
  }
}
bool SignalObject(const std::shared_ptr<Object>& object) {
  switch (object->kind) {
    case Object::Kind::kEvent:
      return EventOperation(object, NTSYNC_IOC_EVENT_SET);
    case Object::Kind::kSemaphore: {
      uint32_t count = 1;
      return ioctl(object->fd, NTSYNC_IOC_SEM_RELEASE, &count) == 0;
    }
    case Object::Kind::kMutex: {
      ntsync_mutex_args args{CurrentState()->tid, 0};
      return ioctl(object->fd, NTSYNC_IOC_MUTEX_UNLOCK, &args) == 0;
    }
  }
  return false;
}

template <typename T>
class ObjectHandle : public T, public NativeWaitHandle {
 public:
  explicit ObjectHandle(std::shared_ptr<Object> object) : object_(std::move(object)) {}
  std::shared_ptr<Object> object() const override { return object_; }
  void* native_handle() const override { return reinterpret_cast<void*>(intptr_t(object_->fd)); }

 protected:
  std::shared_ptr<Object> object_;
};
class LinuxEvent final : public ObjectHandle<Event> {
 public:
  using ObjectHandle::ObjectHandle;
  void Set() override {
    DeferredCancellation guard;
    EventOperation(object_, NTSYNC_IOC_EVENT_SET);
  }
  void Reset() override {
    DeferredCancellation guard;
    EventOperation(object_, NTSYNC_IOC_EVENT_RESET);
  }
  void Pulse() override {
    DeferredCancellation guard;
    EventOperation(object_, NTSYNC_IOC_EVENT_PULSE);
  }
};
class LinuxSemaphore final : public ObjectHandle<Semaphore> {
 public:
  using ObjectHandle::ObjectHandle;
  bool Release(int count, int* previous) override {
    DeferredCancellation guard;
    if (count < 1)
      return false;
    uint32_t value = uint32_t(count);
    if (ioctl(object_->fd, NTSYNC_IOC_SEM_RELEASE, &value) < 0)
      return false;
    if (previous)
      *previous = int(value);
    return true;
  }
};
class LinuxMutant final : public ObjectHandle<Mutant> {
 public:
  using ObjectHandle::ObjectHandle;
  bool Release() override {
    DeferredCancellation guard;
    return SignalObject(object_);
  }
};

// Timer scheduling remains the runtime's existing timer queue. The signaled
// state is a kernel event, so timer/thread/object wait-all uses one atomic ioctl.
class LinuxTimer final : public ObjectHandle<Timer> {
  struct State {
    std::mutex lock;
    std::condition_variable callbacks_done;
    std::shared_ptr<Object> event;
    std::weak_ptr<TimerQueueWaitItem> item;
    std::function<void()> callback;
    uint64_t generation = 0;
    bool armed = false;
    bool in_callback = false;
    std::thread::id callback_thread;
  };

 public:
  explicit LinuxTimer(std::shared_ptr<Object> event)
      : ObjectHandle(event), state_(std::make_shared<State>()) {
    state_->event = std::move(event);
  }
  ~LinuxTimer() override { Cancel(); }
  bool SetOnceAfter(rex::chrono::hundrednanoseconds time, std::function<void()> cb) override {
    return SetOnceAt(GClock_::now() + time, std::move(cb));
  }
  bool SetOnceAt(WClock_::time_point time, std::function<void()> cb) override {
    return SetOnceAt(std::chrono::clock_cast<GClock_>(time), std::move(cb));
  }
  bool SetOnceAt(GClock_::time_point time, std::function<void()> cb) override {
    return Arm(time, {}, std::move(cb));
  }
  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds time, std::chrono::milliseconds period,
                         std::function<void()> cb) override {
    return SetRepeatingAt(GClock_::now() + time, period, std::move(cb));
  }
  bool SetRepeatingAt(WClock_::time_point time, std::chrono::milliseconds period,
                      std::function<void()> cb) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(time), period, std::move(cb));
  }
  bool SetRepeatingAt(GClock_::time_point time, std::chrono::milliseconds period,
                      std::function<void()> cb) override {
    return period.count() > 0 && Arm(time, period, std::move(cb));
  }
  bool Cancel() override {
    DeferredCancellation guard;
    std::shared_ptr<TimerQueueWaitItem> item;
    {
      std::lock_guard lock(state_->lock);
      ++state_->generation;
      state_->armed = false;
      state_->callback = {};
      item = state_->item.lock();
      state_->item.reset();
    }
    // Disarm can wait for a user callback which itself rearms/cancels this
    // timer, so never hold State::lock while calling it.
    if (item)
      item->Disarm();
    std::unique_lock lock(state_->lock);
    if (state_->callback_thread != std::this_thread::get_id()) {
      state_->callbacks_done.wait(lock, [&] { return !state_->in_callback; });
    }
    return true;
  }

 private:
  bool Arm(GClock_::time_point time, std::chrono::milliseconds period, std::function<void()> cb) {
    DeferredCancellation guard;
    auto state = state_;
    uint64_t generation;
    std::shared_ptr<TimerQueueWaitItem> previous;
    bool reset;
    {
      std::lock_guard lock(state->lock);
      previous = state->item.lock();
      state->item.reset();
      generation = ++state->generation;
      reset = EventOperation(object_, NTSYNC_IOC_EVENT_RESET);
      state->armed = reset;
      state->callback = reset ? std::move(cb) : std::function<void()>{};
    }
    if (previous)
      previous->Disarm();
    if (!reset)
      return false;
    auto completion = [weak = std::weak_ptr<State>(state), generation](void*) {
      auto state = weak.lock();
      if (!state)
        return;
      std::function<void()> callback;
      {
        std::lock_guard lock(state->lock);
        if (!state->armed || state->generation != generation)
          return;
        callback = state->callback;
        state->in_callback = true;
        state->callback_thread = std::this_thread::get_id();
        EventOperation(state->event, NTSYNC_IOC_EVENT_SET);
      }
      if (callback)
        callback();
      {
        std::lock_guard lock(state->lock);
        state->in_callback = false;
        state->callback_thread = {};
        state->callbacks_done.notify_all();
      }
    };
    auto item = period.count() ? QueueTimerRecurring(std::move(completion), nullptr, time, period)
                               : QueueTimerOnce(std::move(completion), nullptr, time);
    {
      std::lock_guard lock(state->lock);
      if (state->generation == generation) {
        state->item = item;
        return !item.expired();
      }
    }
    // Cancel/rearm may race with scheduling. An old generation must never be
    // published as the current timer or fire a later user callback.
    if (auto pending = item.lock())
      pending->Disarm();
    return false;
  }
  std::shared_ptr<State> state_;
};

class LinuxThread final : public Thread, public NativeWaitHandle {
 public:
  explicit LinuxThread(std::shared_ptr<ThreadState> state) : state_(std::move(state)) {}
  std::shared_ptr<Object> object() const override { return state_->completion; }
  void* native_handle() const override { return reinterpret_cast<void*>(state_->pthread); }
  uint32_t system_id() const override { return state_->tid; }
  void set_name(std::string name) override {
    DeferredCancellation guard;
    std::lock_guard lock(state_->lock);
    Thread::set_name(name);
    if (state_->finished)
      return;
    pthread_setname_np(state_->pthread, name.substr(0, 15).c_str());
  }
  uint64_t affinity_mask() override {
    DeferredCancellation guard;
    std::lock_guard lock(state_->lock);
    if (state_->finished)
      return 0;
    cpu_set_t cpus;
    if (pthread_getaffinity_np(state_->pthread, sizeof(cpus), &cpus))
      return 0;
    uint64_t mask = 0;
    for (unsigned i = 0; i < 64; ++i)
      if (CPU_ISSET(i, &cpus))
        mask |= uint64_t(1) << i;
    return mask;
  }
  void set_affinity_mask(uint64_t mask) override {
    DeferredCancellation guard;
    std::lock_guard lock(state_->lock);
    if (state_->finished)
      return;
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    for (unsigned i = 0; i < 64; ++i)
      if (mask & (uint64_t(1) << i))
        CPU_SET(i, &cpus);
    int error = pthread_setaffinity_np(state_->pthread, sizeof(cpus), &cpus);
    if (error)
      REXSYS_WARN("set_affinity_mask: {}", std::strerror(error));
  }
  int priority() override {
    DeferredCancellation guard;
    std::lock_guard lock(state_->lock);
    if (state_->finished)
      return -1;
    errno = 0;
    int nice_value = getpriority(PRIO_PROCESS, state_->tid);
    return errno ? -1 : ThreadPriority::kNormal - nice_value * 4;
  }
  void set_priority(int value) override {
    DeferredCancellation guard;
    std::lock_guard lock(state_->lock);
    if (state_->finished)
      return;
    // Guest priorities map to per-thread nice values under the normal
    // scheduler; SCHED_FIFO would let a spinning guest thread starve the host.
    int nice_value = std::clamp((ThreadPriority::kNormal - value) / 4, -4, 4);
    if (setpriority(PRIO_PROCESS, state_->tid, nice_value) == 0)
      return;
    if (errno == EACCES || errno == EPERM) {
      // Lowering nice needs RLIMIT_NICE or CAP_SYS_NICE; keep the current one.
      static std::once_flag warned;
      std::call_once(warned, [] {
        REXSYS_WARN("set_priority: raising thread priority not permitted (RLIMIT_NICE); "
                    "guest priority boosts are ignored");
      });
      return;
    }
    REXSYS_WARN("set_priority: {}", std::strerror(errno));
  }
  void QueueUserCallback(std::function<void()> callback) override {
    DeferredCancellation guard;
    std::lock_guard lock(state_->lock);
    if (state_->finished)
      return;
    state_->callbacks.push_back(std::move(callback));
    EventOperation(state_->alert, NTSYNC_IOC_EVENT_SET);
  }
  bool Resume(uint32_t* previous) override {
    DeferredCancellation guard;
    std::lock_guard lock(state_->lock);
    uint32_t count = state_->suspend_count;
    if (previous)
      *previous = count;
    if (!count || state_->finished)
      return false;
    state_->suspend_count = count - 1;
    if (count == 1)
      sem_post(&state_->suspend_semaphore);
    return true;
  }
  bool Suspend(uint32_t* previous) override {
    DeferredCancellation guard;
    {
      std::lock_guard lock(state_->lock);
      uint32_t count = state_->suspend_count;
      if (previous)
        *previous = count;
      if (count == UINT32_MAX || state_->finished)
        return false;
      state_->suspend_count = count + 1;
      if (!pthread_equal(state_->pthread, pthread_self())) {
        if (pthread_kill(state_->pthread, SuspendSignal())) {
          state_->suspend_count = count;
          return false;
        }
        return true;
      }
    }
    WaitSuspended(*state_);
    return true;
  }
  void Terminate(int exit_code) override {
    DeferredCancellation guard;
    if (pthread_equal(state_->pthread, pthread_self()))
      pthread_exit(reinterpret_cast<void*>(intptr_t(exit_code)));
    std::lock_guard lock(state_->lock);
    if (state_->finished)
      return;
    int error = pthread_cancel(state_->pthread);
    if (error)
      REXSYS_WARN("thread cancellation: {}", std::strerror(error));
  }

 private:
  std::shared_ptr<ThreadState> state_;
};
struct StartData {
  std::shared_ptr<ThreadState> state;
  std::function<void()> routine;
};
void CleanupThread(void*) {
  // pthread cleanup runs before TLS destructors. Abandon before notifying
  // joiners; they may immediately acquire the exiting thread's mutexes.
  // No further cancellation may interrupt exit bookkeeping or TLS teardown.
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr);
  FinishThread(context.state);
  signal_state = nullptr;
}
void* StartThread(void* argument) {
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);
  std::unique_ptr<StartData> data(static_cast<StartData*>(argument));
  auto state = data->state;
  auto routine = std::move(data->routine);
  data.reset();
  context.state = state;
  signal_state = state.get();
  pthread_cleanup_push(CleanupThread, state.get());
  {
    std::lock_guard lock(state->lock);
    state->pthread = pthread_self();
    state->tid = uint32_t(syscall(SYS_gettid));
    state->started = true;
    state->started_cv.notify_all();
  }
  WaitSuspended(*state);
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr);
  routine();
  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, nullptr);
  pthread_cleanup_pop(1);
  return nullptr;
}
}  // namespace

void EnableAffinityConfiguration() {}
uint32_t current_thread_system_id() {
  return uint32_t(syscall(SYS_gettid));
}
void MaybeYield() {
  sched_yield();
}
void SyncMemory() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}
void Sleep(std::chrono::microseconds duration) {
  if (duration.count() <= 0) {
    sched_yield();
    return;
  }
  timespec request{time_t(duration.count() / 1000000), long((duration.count() % 1000000) * 1000)};
  while (nanosleep(&request, &request) < 0 && errno == EINTR) {}
}
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  DeferredCancellation guard;
  uint64_t deadline =
      duration == std::chrono::microseconds::max() ? UINT64_MAX
      : duration.count() > INT64_MAX / 1000
          ? UINT64_MAX
          : Deadline(std::chrono::nanoseconds(std::max(duration.count(), int64_t(0)) * 1000));
  auto result = WaitObjects({}, false, true, deadline).first;
  return result == WaitResult::kUserCallback ? SleepResult::kAlerted : SleepResult::kSuccess;
}
TlsHandle AllocateTlsHandle() {
  pthread_key_t key;
  return pthread_key_create(&key, nullptr) ? kInvalidTlsHandle : TlsHandle(key);
}
bool FreeTlsHandle(TlsHandle key) {
  return pthread_key_delete(pthread_key_t(key)) == 0;
}
uintptr_t GetTlsValue(TlsHandle key) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(pthread_key_t(key)));
}
bool SetTlsValue(TlsHandle key, uintptr_t value) {
  return pthread_setspecific(pthread_key_t(key), reinterpret_cast<void*>(value)) == 0;
}

WaitResult Wait(WaitHandle* handle, bool alertable, std::chrono::milliseconds timeout) {
  DeferredCancellation guard;
  auto object = GetObject(handle);
  return object ? WaitObjects(std::array{object}, false, alertable, Deadline(timeout)).first
                : WaitResult::kFailed;
}
std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* handles[], size_t count, bool all,
                                           bool alertable, std::chrono::milliseconds timeout) {
  DeferredCancellation guard;
  if (!handles || !count || count > NTSYNC_MAX_WAIT_COUNT)
    return {WaitResult::kFailed, 0};
  std::vector<std::shared_ptr<Object>> objects;
  objects.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    auto object = GetObject(handles[i]);
    if (!object)
      return {WaitResult::kFailed, 0};
    if (all && std::find(objects.begin(), objects.end(), object) != objects.end())
      return {WaitResult::kFailed, 0};
    objects.push_back(std::move(object));
  }
  return WaitObjects(objects, all, alertable, Deadline(timeout));
}
WaitResult SignalAndWait(WaitHandle* signal, WaitHandle* wait, bool alertable,
                         std::chrono::milliseconds timeout) {
  DeferredCancellation guard;
  // As in Wine, retain and validate BOTH objects before signaling either one.
  // NTSYNC has no combined signal-and-wait ioctl; don't claim atomicity.
  auto to_signal = GetObject(signal), to_wait = GetObject(wait);
  if (!to_signal || !to_wait || dynamic_cast<Timer*>(signal) || dynamic_cast<Thread*>(signal))
    return WaitResult::kFailed;
  if (!SignalObject(to_signal))
    return WaitResult::kFailed;
  return WaitObjects(std::array{to_wait}, false, alertable, Deadline(timeout)).first;
}
std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial) {
  DeferredCancellation guard;
  auto object = CreateEvent(true, initial);
  return object ? std::make_unique<LinuxEvent>(std::move(object)) : nullptr;
}
std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial) {
  DeferredCancellation guard;
  auto object = CreateEvent(false, initial);
  return object ? std::make_unique<LinuxEvent>(std::move(object)) : nullptr;
}
std::unique_ptr<Semaphore> Semaphore::Create(int initial, int maximum) {
  DeferredCancellation guard;
  if (initial < 0 || maximum < 1 || initial > maximum)
    return nullptr;
  ntsync_sem_args args{uint32_t(initial), uint32_t(maximum)};
  auto object = CreateObject(Object::Kind::kSemaphore, NTSYNC_IOC_CREATE_SEM, &args);
  return object ? std::make_unique<LinuxSemaphore>(std::move(object)) : nullptr;
}
std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  DeferredCancellation guard;
  ntsync_mutex_args args{initial_owner ? CurrentState()->tid : 0, uint32_t(initial_owner)};
  auto object = CreateObject(Object::Kind::kMutex, NTSYNC_IOC_CREATE_MUTEX, &args);
  return object ? std::make_unique<LinuxMutant>(std::move(object)) : nullptr;
}
std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  DeferredCancellation guard;
  auto object = CreateEvent(true, false);
  return object ? std::make_unique<LinuxTimer>(std::move(object)) : nullptr;
}
std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  DeferredCancellation guard;
  auto object = CreateEvent(false, false);
  return object ? std::make_unique<LinuxTimer>(std::move(object)) : nullptr;
}
std::unique_ptr<Thread> Thread::Create(CreationParameters params, std::function<void()> routine) {
  DeferredCancellation guard;
  InstallSuspendHandler();
  auto state = std::make_shared<ThreadState>();
  if (!state->valid())
    return nullptr;
  state->suspend_count = params.create_suspended ? 1 : 0;
  pthread_attr_t attr;
  if (pthread_attr_init(&attr))
    return nullptr;
  int error = pthread_attr_setstacksize(&attr, params.stack_size);
  if (!error)
    error = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  auto data = std::make_unique<StartData>(StartData{state, std::move(routine)});
  pthread_t thread;
  if (!error)
    error = pthread_create(&thread, &attr, StartThread, data.get());
  pthread_attr_destroy(&attr);
  if (error) {
    REXSYS_ERROR("thread creation: {}", std::strerror(error));
    return nullptr;
  }
  data.release();
  {
    std::unique_lock lock(state->lock);
    state->started_cv.wait(lock, [&] { return state->started; });
  }
  auto result = std::make_unique<LinuxThread>(state);
  if (params.initial_priority)
    result->set_priority(params.initial_priority);
  return result;
}
Thread* Thread::GetCurrentThread() {
  DeferredCancellation guard;
  thread_local std::unique_ptr<LinuxThread> wrapper;
  if (!wrapper)
    wrapper = std::make_unique<LinuxThread>(CurrentState());
  return wrapper.get();
}
void Thread::Exit(int code) {
  pthread_exit(reinterpret_cast<void*>(intptr_t(code)));
}
void set_current_thread_name(std::string_view name) {
  DeferredCancellation guard;
  std::string truncated(name.substr(0, 15));
  pthread_setname_np(pthread_self(), truncated.c_str());
}
}  // namespace rex::thread
