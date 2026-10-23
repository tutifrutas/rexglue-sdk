#include <catch2/catch_test_macros.hpp>

#include <rex/platform.h>
#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <vector>
#include <thread>

#if REX_PLATFORM_LINUX
#include <sched.h>
#include <sys/ioctl.h>
#include <linux/ntsync.h>
#endif

namespace {
using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::Wait;
using rex::thread::WaitResult;

// Keep the setter running after Set so a woken thread cannot hide a stolen
// release by winning the scheduler race. New threads inherit this affinity.
struct SingleCpuAffinity {
#if REX_PLATFORM_LINUX
  cpu_set_t original;
  bool changed = false;
  SingleCpuAffinity() {
    if (sched_getaffinity(0, sizeof(original), &original) != 0) {
      return;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, &original)) {
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(cpu, &one);
        changed = sched_setaffinity(0, sizeof(one), &one) == 0;
        break;
      }
    }
  }
  ~SingleCpuAffinity() {
    if (changed) {
      sched_setaffinity(0, sizeof(original), &original);
    }
  }
#endif
};
}  // namespace

TEST_CASE("Auto-reset event stores only one unclaimed release", "[core][threading]") {
  auto event = Event::CreateAutoResetEvent(false);
  CHECK(Wait(event.get(), false, 1ms) == WaitResult::kTimeout);
  event->Set();
  event->Set();
  CHECK(Wait(event.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(Wait(event.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("A new event waiter cannot steal an existing waiter's release", "[core][threading]") {
  SingleCpuAffinity affinity;
  for (int iteration = 0; iteration < 8; ++iteration) {
    auto event = Event::CreateAutoResetEvent(false);
    std::atomic<bool> entering = false;
    WaitResult blocked_result = WaitResult::kFailed;
    std::thread blocked([&] {
      entering = true;
      blocked_result = Wait(event.get(), false, 150ms);
    });
    while (!entering.load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(10ms);
    event->Set();
    auto new_result = Wait(event.get(), false, 0ms);
    blocked.join();
    CHECK(blocked_result == WaitResult::kSuccess);
    CHECK(new_result == WaitResult::kTimeout);
  }
}

TEST_CASE("Reset cannot revoke a manual event release already assigned to waiters",
          "[core][threading]") {
  SingleCpuAffinity affinity;
  auto event = Event::CreateManualResetEvent(false);
  std::atomic<int> entering = 0;
  WaitResult results[2] = {WaitResult::kFailed, WaitResult::kFailed};
  std::thread waiters[2];
  for (int i = 0; i < 2; ++i) {
    waiters[i] = std::thread([&, i] {
      ++entering;
      results[i] = Wait(event.get(), false, 150ms);
    });
  }
  while (entering.load() != 2) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(10ms);
  event->Set();
  event->Reset();
  for (auto& waiter : waiters) {
    waiter.join();
  }
  CHECK(results[0] == WaitResult::kSuccess);
  CHECK(results[1] == WaitResult::kSuccess);
  CHECK(Wait(event.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("Failed SignalAndWait preserves the wait event's existing signal", "[core][threading]") {
  auto mutex = rex::thread::Mutant::Create(false);
  auto event = Event::CreateAutoResetEvent(true);
  CHECK(rex::thread::SignalAndWait(mutex.get(), event.get(), false, 0ms) == WaitResult::kFailed);
  CHECK(Wait(event.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(Wait(event.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("SignalAndWait preserves a stored event handoff after releasing the mutex",
          "[core][threading]") {
  SingleCpuAffinity affinity;
  auto mutex = rex::thread::Mutant::Create(false);
  auto event = Event::CreateAutoResetEvent(false);
  auto ready = rex::thread::Semaphore::Create(0, 1);
  std::atomic<bool> failed = false;
  std::thread producer([&] {
    for (int i = 0; i < 64; ++i) {
      if (Wait(ready.get(), false, 500ms) != WaitResult::kSuccess ||
          Wait(mutex.get(), false, 500ms) != WaitResult::kSuccess) {
        failed = true;
        return;
      }
      // Wine/NTSYNC has no atomic signal-and-wait ioctl. Pulse cannot be
      // used for a handoff: it is allowed to run before the waiter registers.
      event->Set();
      mutex->Release();
    }
  });
  for (int i = 0; i < 64; ++i) {
    if (Wait(mutex.get(), false, 500ms) != WaitResult::kSuccess) {
      failed = true;
      break;
    }
    ready->Release(1, nullptr);
    if (rex::thread::SignalAndWait(mutex.get(), event.get(), false, 500ms) !=
        WaitResult::kSuccess) {
      failed = true;
      break;
    }
  }
  producer.join();
  CHECK_FALSE(failed.load());
}

#if REX_PLATFORM_LINUX
TEST_CASE("NTSYNC semaphores enforce limits and return the previous count", "[core][threading]") {
  using rex::thread::Semaphore;
  CHECK_FALSE(Semaphore::Create(-1, 1));
  CHECK_FALSE(Semaphore::Create(2, 1));
  CHECK_FALSE(Semaphore::Create(0, 0));
  auto semaphore = Semaphore::Create(1, 2);
  REQUIRE(semaphore);
  int previous = -1;
  CHECK_FALSE(semaphore->Release(0, &previous));
  CHECK(semaphore->Release(1, &previous));
  CHECK(previous == 1);
  CHECK_FALSE(semaphore->Release(1, &previous));
  CHECK(previous == 1);
  CHECK(Wait(semaphore.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(Wait(semaphore.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(Wait(semaphore.get(), false, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("NTSYNC wait-all acquires mixed objects atomically", "[core][threading]") {
  auto event = Event::CreateAutoResetEvent(true);
  auto semaphore = rex::thread::Semaphore::Create(0, 2);
  auto mutex = rex::thread::Mutant::Create(false);
  rex::thread::WaitHandle* handles[] = {event.get(), semaphore.get(), mutex.get()};
  CHECK(rex::thread::WaitAll(handles, 3, false, 2ms) == WaitResult::kTimeout);
  CHECK(Wait(event.get(), false, 0ms) == WaitResult::kSuccess);
  event->Set();
  REQUIRE(semaphore->Release(1, nullptr));
  CHECK(rex::thread::WaitAll(handles, 3, false, 0ms) == WaitResult::kSuccess);
  CHECK(Wait(event.get(), false, 0ms) == WaitResult::kTimeout);
  CHECK(Wait(semaphore.get(), false, 0ms) == WaitResult::kTimeout);
  CHECK(mutex->Release());
}

TEST_CASE("NTSYNC wait arrays validate bounds and duplicate wait-all objects",
          "[core][threading]") {
  auto event = Event::CreateAutoResetEvent(true);
  rex::thread::WaitHandle* handles[] = {event.get(), event.get()};
  CHECK(rex::thread::WaitAll(handles, 2, false, 0ms) == WaitResult::kFailed);
  auto [result, index] = rex::thread::WaitAny(handles, 2, false, 0ms);
  CHECK(result == WaitResult::kSuccess);
  CHECK(index == 0);
  CHECK(rex::thread::WaitAny(handles, 0, false, 0ms).first == WaitResult::kFailed);
  CHECK(rex::thread::WaitAny(handles, 65, false, 0ms).first == WaitResult::kFailed);
  CHECK(Wait(nullptr, false, 0ms) == WaitResult::kFailed);
}

TEST_CASE("NTSYNC mutex recursion rejects non-owner release", "[core][threading]") {
  auto mutex = rex::thread::Mutant::Create(true);
  REQUIRE(mutex);
  CHECK(Wait(mutex.get(), false, 0ms) == WaitResult::kSuccess);
  bool wrong_owner = true;
  std::thread other([&] { wrong_owner = mutex->Release(); });
  other.join();
  CHECK_FALSE(wrong_owner);
  CHECK(mutex->Release());
  CHECK(mutex->Release());
  CHECK_FALSE(mutex->Release());
}

TEST_CASE("NTSYNC abandons mutexes when an ordinary host thread exits", "[core][threading]") {
  auto mutex = rex::thread::Mutant::Create(false);
  WaitResult acquired = WaitResult::kFailed;
  std::thread owner([&] { acquired = Wait(mutex.get(), false, 0ms); });
  owner.join();
  CHECK(acquired == WaitResult::kSuccess);
  CHECK(Wait(mutex.get(), false, 100ms) == WaitResult::kAbandoned);
  CHECK(mutex->Release());
  CHECK(Wait(mutex.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(mutex->Release());
}

TEST_CASE("NTSYNC wait-any and wait-all report abandoned ownership", "[core][threading]") {
  auto mutex = rex::thread::Mutant::Create(false);
  auto event = Event::CreateAutoResetEvent(false);
  rex::thread::WaitHandle* handles[] = {event.get(), mutex.get()};
  std::thread owner([&] { Wait(mutex.get(), false, 0ms); });
  owner.join();
  auto [result, index] = rex::thread::WaitAny(handles, 2, false, 100ms);
  CHECK(result == WaitResult::kAbandoned);
  CHECK(index == 1);
  REQUIRE(mutex->Release());
  std::thread second_owner([&] { Wait(mutex.get(), false, 0ms); });
  second_owner.join();
  event->Set();
  CHECK(rex::thread::WaitAll(handles, 2, false, 100ms) == WaitResult::kAbandoned);
  CHECK(Wait(event.get(), false, 0ms) == WaitResult::kTimeout);
  CHECK(mutex->Release());
}

TEST_CASE("NTSYNC alertable zero-time waits dispatch queued callbacks without consuming objects",
          "[core][threading]") {
  auto current = rex::thread::Thread::GetCurrentThread();
  auto event = Event::CreateAutoResetEvent(true);
  int callbacks = 0;
  current->QueueUserCallback([&] { ++callbacks; });
  current->QueueUserCallback([&] { ++callbacks; });
  CHECK(Wait(event.get(), true, 0ms) == WaitResult::kUserCallback);
  CHECK(callbacks == 2);
  CHECK(Wait(event.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(Wait(event.get(), true, 0ms) == WaitResult::kTimeout);
}

TEST_CASE("NTSYNC alert event wakes a blocked thread without polling", "[core][threading]") {
  auto ready = Event::CreateAutoResetEvent(false);
  auto blocked = Event::CreateAutoResetEvent(false);
  std::atomic<int> callbacks = 0;
  WaitResult result = WaitResult::kFailed;
  auto thread = rex::thread::Thread::Create({}, [&] {
    ready->Set();
    result = Wait(blocked.get(), true, 500ms);
  });
  REQUIRE(thread);
  REQUIRE(Wait(ready.get(), false, 500ms) == WaitResult::kSuccess);
  thread->QueueUserCallback([&] { ++callbacks; });
  CHECK(Wait(thread.get(), false, 500ms) == WaitResult::kSuccess);
  CHECK(result == WaitResult::kUserCallback);
  CHECK(callbacks == 1);
  CHECK(Wait(thread.get(), false, 0ms) == WaitResult::kSuccess);
}

TEST_CASE("NTSYNC non-alertable waits defer callbacks", "[core][threading]") {
  auto current = rex::thread::Thread::GetCurrentThread();
  auto event = Event::CreateAutoResetEvent(false);
  bool called = false;
  current->QueueUserCallback([&] { called = true; });
  CHECK(Wait(event.get(), false, 2ms) == WaitResult::kTimeout);
  CHECK_FALSE(called);
  CHECK(rex::thread::AlertableSleep(0ms) == rex::thread::SleepResult::kAlerted);
  CHECK(called);
}

TEST_CASE("NTSYNC thread completion abandons mutexes before releasing joiners",
          "[core][threading]") {
  auto mutex = rex::thread::Mutant::Create(false);
  auto thread = rex::thread::Thread::Create({}, [&] {
    Wait(mutex.get(), false, 100ms);
    rex::thread::Thread::Exit(7);
  });
  REQUIRE(thread);
  CHECK(Wait(thread.get(), false, 500ms) == WaitResult::kSuccess);
  CHECK(Wait(mutex.get(), false, 100ms) == WaitResult::kAbandoned);
  CHECK(mutex->Release());
}

TEST_CASE("NTSYNC cancellation completes a blocked thread and abandons its mutexes",
          "[core][threading]") {
  auto mutex = rex::thread::Mutant::Create(false);
  auto ready = Event::CreateAutoResetEvent(false);
  auto blocked = Event::CreateAutoResetEvent(false);
  auto thread = rex::thread::Thread::Create({}, [&] {
    Wait(mutex.get(), false, 100ms);
    ready->Set();
    Wait(blocked.get(), false);
  });
  REQUIRE(thread);
  REQUIRE(Wait(ready.get(), false, 500ms) == WaitResult::kSuccess);
  thread->Terminate(9);
  CHECK(Wait(thread.get(), false, 500ms) == WaitResult::kSuccess);
  CHECK(Wait(mutex.get(), false, 100ms) == WaitResult::kAbandoned);
  CHECK(mutex->Release());
}

TEST_CASE("NTSYNC thread lifetime is independent of its public wrapper", "[core][threading]") {
  auto proceed = Event::CreateManualResetEvent(false);
  auto done = Event::CreateManualResetEvent(false);
  auto thread = rex::thread::Thread::Create({}, [&] {
    Wait(proceed.get(), false, 500ms);
    rex::thread::Thread::GetCurrentThread()->QueueUserCallback([] {});
    rex::thread::AlertableSleep(0ms);
    done->Set();
  });
  REQUIRE(thread);
  thread.reset();
  proceed->Set();
  CHECK(Wait(done.get(), false, 500ms) == WaitResult::kSuccess);
}

TEST_CASE("NTSYNC suspended creation publishes a real system ID and resumes once",
          "[core][threading]") {
  rex::thread::Thread::CreationParameters params;
  params.create_suspended = true;
  std::atomic<uint32_t> tid = 0;
  auto thread =
      rex::thread::Thread::Create(params, [&] { tid = rex::thread::current_thread_system_id(); });
  REQUIRE(thread);
  CHECK(tid == 0);
  CHECK(thread->system_id() != 0);
  CHECK(Wait(thread.get(), false, 2ms) == WaitResult::kTimeout);
  uint32_t previous = 0;
  CHECK(thread->Resume(&previous));
  CHECK(previous == 1);
  CHECK(Wait(thread.get(), false, 500ms) == WaitResult::kSuccess);
  CHECK(tid == thread->system_id());
}

TEST_CASE("NTSYNC timer events participate in mixed wait-all", "[core][threading]") {
  auto timer = rex::thread::Timer::CreateSynchronizationTimer();
  auto event = Event::CreateAutoResetEvent(true);
  REQUIRE(timer);
  REQUIRE(timer->SetOnceAfter(2ms));
  rex::thread::WaitHandle* handles[] = {event.get(), timer.get()};
  CHECK(rex::thread::WaitAll(handles, 2, false, 500ms) == WaitResult::kSuccess);
  CHECK(Wait(timer.get(), false, 0ms) == WaitResult::kTimeout);
  CHECK(timer->Cancel());
  CHECK(timer->SetOnceAfter(100ms));
  CHECK(timer->Cancel());
  CHECK(Wait(timer.get(), false, 2ms) == WaitResult::kTimeout);
}

TEST_CASE("Linux event handles are actual NTSYNC kernel objects", "[core][threading]") {
  auto event = Event::CreateManualResetEvent(true);
  ntsync_event_args state{};
  int fd = int(reinterpret_cast<intptr_t>(event->native_handle()));
  REQUIRE(ioctl(fd, NTSYNC_IOC_EVENT_READ, &state) == 0);
  CHECK(state.manual == 1);
  CHECK(state.signaled == 1);
  event->Reset();
  REQUIRE(ioctl(fd, NTSYNC_IOC_EVENT_READ, &state) == 0);
  CHECK(state.signaled == 0);
}

TEST_CASE("NTSYNC wait-all supports the kernel's complete 64-object limit", "[core][threading]") {
  std::vector<std::unique_ptr<Event>> events;
  std::vector<rex::thread::WaitHandle*> handles;
  for (int i = 0; i < NTSYNC_MAX_WAIT_COUNT; ++i) {
    events.push_back(Event::CreateAutoResetEvent(true));
    handles.push_back(events.back().get());
  }
  CHECK(rex::thread::WaitAll(handles, false, 0ms) == WaitResult::kSuccess);
  CHECK(rex::thread::WaitAny(handles, false, 0ms).first == WaitResult::kTimeout);
}

TEST_CASE("Closing a waited-on mutex wrapper does not lose owner-death tracking",
          "[core][threading]") {
  SingleCpuAffinity affinity;
  auto mutex = rex::thread::Mutant::Create(false);
  auto release_owner = Event::CreateManualResetEvent(false);
  auto owned = Event::CreateManualResetEvent(false);
  std::thread owner([&] {
    Wait(mutex.get(), false, 100ms);
    owned->Set();
    Wait(release_owner.get(), false, 500ms);
  });
  auto raw = mutex.get();
  REQUIRE(Wait(owned.get(), false, 100ms) == WaitResult::kSuccess);
  std::atomic<bool> entering = false;
  WaitResult result = WaitResult::kFailed;
  std::thread waiter([&] {
    entering = true;
    result = Wait(raw, false, 500ms);
  });
  while (!entering)
    std::this_thread::yield();
  std::this_thread::sleep_for(10ms);
  // The waiter holds a kernel-object reference independently of this wrapper.
  mutex.reset();
  release_owner->Set();
  owner.join();
  waiter.join();
  CHECK(result == WaitResult::kAbandoned);
}

TEST_CASE("NTSYNC timers may rearm and destroy themselves from callbacks", "[core][threading]") {
  auto timer = rex::thread::Timer::CreateManualResetTimer();
  auto done = Event::CreateManualResetEvent(false);
  std::atomic<int> callbacks = 0;
  REQUIRE(timer->SetOnceAfter(1ms, [&] {
    ++callbacks;
    timer->SetOnceAfter(1ms, [&] {
      ++callbacks;
      timer.reset();
      done->Set();
    });
  }));
  REQUIRE(Wait(done.get(), false, 500ms) == WaitResult::kSuccess);
  CHECK(callbacks == 2);
  CHECK_FALSE(timer);
}

TEST_CASE("Cancelled long-lived NTSYNC timers release kernel descriptors", "[core][threading]") {
  // Warm both the timer queue and current thread's internal kernel objects.
  auto warm = rex::thread::Timer::CreateSynchronizationTimer();
  REQUIRE(warm->SetOnceAfter(1ms));
  REQUIRE(Wait(warm.get(), false, 500ms) == WaitResult::kSuccess);
  warm.reset();
  auto count = [] {
    size_t result = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      // The timer queue and logging threads may open unrelated descriptors
      // concurrently. Only count the kernel objects owned by NTSYNC timers.
      std::error_code error;
      auto target = std::filesystem::read_symlink(entry.path(), error);
      if (!error && target == "anon_inode:ntsync")
        ++result;
    }
    return result;
  };
  auto before = count();
  // Ensure the filter sees a live timer, so a misspelled inode name cannot
  // turn the leak check into an always-zero comparison.
  {
    auto timer = rex::thread::Timer::CreateManualResetTimer();
    REQUIRE(count() == before + 1);
  }
  REQUIRE(count() == before);
  for (int i = 0; i < 32; ++i) {
    auto timer = rex::thread::Timer::CreateManualResetTimer();
    REQUIRE(timer->SetOnceAfter(24h));
    REQUIRE(timer->Cancel());
  }
  CHECK(count() == before);
}

TEST_CASE("NTSYNC concurrent timer changes leave no stale signal", "[core][threading]") {
  auto timer = rex::thread::Timer::CreateSynchronizationTimer();
  std::thread setter([&] {
    for (int i = 0; i < 64; ++i)
      timer->SetOnceAfter(100ms);
  });
  std::thread canceller([&] {
    for (int i = 0; i < 64; ++i)
      timer->Cancel();
  });
  setter.join();
  canceller.join();
  REQUIRE(timer->Cancel());
  CHECK(Wait(timer.get(), false, 5ms) == WaitResult::kTimeout);
}
#endif

#if REX_PLATFORM_LINUX
TEST_CASE("Guest thread priorities map to nice values, not SCHED_FIFO", "[core][threading]") {
  using rex::thread::ThreadPriority;
  auto release = Event::CreateManualResetEvent(false);
  int policy = -1;
  auto thread = rex::thread::Thread::Create({}, [&] {
    Wait(release.get(), false, 2000ms);
    policy = sched_getscheduler(0);
  });
  REQUIRE(thread);
  // Lowering priority never needs privileges.
  thread->set_priority(ThreadPriority::kBelowNormal);
  CHECK(thread->priority() == ThreadPriority::kBelowNormal);
  release->Set();
  CHECK(Wait(thread.get(), false, 2000ms) == WaitResult::kSuccess);
  CHECK(policy == SCHED_OTHER);
}
#endif
