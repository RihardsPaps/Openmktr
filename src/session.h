// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#ifndef SESSION_H_
#define SESSION_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

// One invocation owns all mutable language services and AST allocations.
// Workers bind the same session; independent invocations bind different ones.
// The service registry publishes each instance once and keeps its address
// fixed.
class BuildSession {
 public:
  BuildSession();
  ~BuildSession();
  BuildSession(const BuildSession&) = delete;
  BuildSession& operator=(const BuildSession&) = delete;
  static BuildSession& Current();
  static BuildSession* Active();
  static BuildSession* Bind(BuildSession* session);

  template <class T>
  T& Get() {
    static const size_t slot = NextSlot();
    if (slot >= services_.size())
      throw std::logic_error("too many build session services");
    void* value = services_[slot].load(std::memory_order_acquire);
    if (!value) {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      value = services_[slot].load(std::memory_order_relaxed);
      if (!value) {
        auto owned = std::make_unique<T>();
        value = owned.get();
        destructors_.push_back(
            {value, [](void* p) { delete static_cast<T*>(p); }});
        owned.release();
        services_[slot].store(value, std::memory_order_release);
      }
    }
    return *static_cast<T*>(value);
  }

  void* Allocate(size_t size, void (*destroy)(void*));
  static void Release(void* value) noexcept;
  bool draining() const { return draining_; }
  size_t allocations() const { return allocations_; }
  void Cancel(int signal) {
    cancellation_.store(signal, std::memory_order_relaxed);
  }
  int cancellation() const {
    return cancellation_.load(std::memory_order_relaxed);
  }

 private:
  struct Allocation;
  struct Destructor {
    void* value;
    void (*destroy)(void*);
  };
  static size_t NextSlot();
  BuildSession* previous_;
  std::array<std::atomic<void*>, 128> services_{};
  std::recursive_mutex mutex_;
  std::vector<Destructor> destructors_;
  Allocation* objects_ = nullptr;
  size_t allocations_ = 0;
  bool draining_ = false;
  std::atomic<int> cancellation_{0};
};

class SessionBinding {
 public:
  explicit SessionBinding(BuildSession* session)
      : previous_(BuildSession::Bind(session)) {}
  ~SessionBinding() { BuildSession::Bind(previous_); }
  SessionBinding(const SessionBinding&) = delete;
  SessionBinding& operator=(const SessionBinding&) = delete;

 private:
  BuildSession* previous_;
};

template <class T>
T& SessionService() {
  return BuildSession::Current().Get<T>();
}

// Objects can be deleted early. Remaining objects are destroyed exactly once at
// invocation teardown. Borrowed AST edges must not delete peers during
// draining.
template <class T>
class SessionAllocated {
 public:
  static void* operator new(size_t size) {
    return BuildSession::Current().Allocate(
        size, [](void* p) { delete static_cast<T*>(p); });
  }
  static void operator delete(void* value) noexcept {
    BuildSession::Release(value);
  }

 protected:
  static bool DrainingSession() { return BuildSession::Current().draining(); }
};
#endif
