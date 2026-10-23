#include <catch2/catch_test_macros.hpp>

#include <rex/platform.h>
#include <rex/system/xobject.h>
#include <rex/thread.h>

#include <thread>

#if REX_PLATFORM_LINUX
namespace {
using namespace std::chrono_literals;
using namespace rex::system;
using rex::X_STATUS;

class WaitObject final : public XObject {
 public:
  explicit WaitObject(std::unique_ptr<rex::thread::WaitHandle> handle)
      : XObject(Type::Undefined), handle_(std::move(handle)) {}
  rex::thread::WaitHandle* GetWaitHandle() override { return handle_.get(); }
  void WaitCallback() override { ++acquisitions; }
  int acquisitions = 0;

 private:
  std::unique_ptr<rex::thread::WaitHandle> handle_;
};
class InvalidHandle final : public rex::thread::WaitHandle {
 public:
  void* native_handle() const override { return nullptr; }
};
}  // namespace

TEST_CASE("Guest single waits distinguish failure from abandoned acquisition", "[kernel][ntsync]") {
  WaitObject invalid(std::make_unique<InvalidHandle>());
  uint64_t timeout = 0;
  CHECK(invalid.Wait(0, 0, 0, &timeout) == X_STATUS_UNSUCCESSFUL);
  CHECK(invalid.acquisitions == 0);

  auto mutex = rex::thread::Mutant::Create(false);
  auto raw = mutex.get();
  std::thread owner([&] { rex::thread::Wait(raw, false, 0ms); });
  owner.join();
  WaitObject abandoned(std::move(mutex));
  CHECK(abandoned.Wait(0, 0, 0, &timeout) == X_STATUS_ABANDONED_WAIT_0);
  CHECK(abandoned.acquisitions == 1);
  CHECK(raw->Release());
}

TEST_CASE("Guest wait-any reports the abandoned index and updates only its owner",
          "[kernel][ntsync]") {
  WaitObject event(rex::thread::Event::CreateAutoResetEvent(false));
  auto mutex = rex::thread::Mutant::Create(false);
  auto raw = mutex.get();
  std::thread owner([&] { rex::thread::Wait(raw, false, 0ms); });
  owner.join();
  WaitObject abandoned(std::move(mutex));
  XObject* objects[] = {&event, &abandoned};
  uint64_t timeout = 0;
  CHECK(XObject::WaitMultiple(2, objects, 1, 0, 0, 0, &timeout) == X_STATUS_ABANDONED_WAIT_0 + 1);
  CHECK(event.acquisitions == 0);
  CHECK(abandoned.acquisitions == 1);
  CHECK(raw->Release());
}

TEST_CASE("Guest abandoned wait-all updates every acquired object", "[kernel][ntsync]") {
  WaitObject event(rex::thread::Event::CreateAutoResetEvent(true));
  auto mutex = rex::thread::Mutant::Create(false);
  auto raw = mutex.get();
  std::thread owner([&] { rex::thread::Wait(raw, false, 0ms); });
  owner.join();
  WaitObject abandoned(std::move(mutex));
  XObject* objects[] = {&event, &abandoned};
  uint64_t timeout = 0;
  CHECK(XObject::WaitMultiple(2, objects, 0, 0, 0, 0, &timeout) == X_STATUS_ABANDONED_WAIT_0);
  CHECK(event.acquisitions == 1);
  CHECK(abandoned.acquisitions == 1);
  CHECK(raw->Release());
}

TEST_CASE("Guest failed wait-all and signal-and-wait do not claim ownership", "[kernel][ntsync]") {
  WaitObject event(rex::thread::Event::CreateAutoResetEvent(false));
  WaitObject invalid(std::make_unique<InvalidHandle>());
  XObject* objects[] = {&event, &invalid};
  uint64_t timeout = 0;
  CHECK(XObject::WaitMultiple(2, objects, 0, 0, 0, 0, &timeout) == X_STATUS_UNSUCCESSFUL);
  CHECK(XObject::SignalAndWait(&event, &invalid, 0, 0, 0, &timeout) == X_STATUS_UNSUCCESSFUL);
  CHECK(rex::thread::Wait(event.GetWaitHandle(), false, 0ms) == rex::thread::WaitResult::kTimeout);
  CHECK(event.acquisitions == 0);
  CHECK(invalid.acquisitions == 0);
}
#endif
