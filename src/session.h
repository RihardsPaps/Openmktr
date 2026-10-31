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
#include <type_traits>
#include <vector>

struct Flags;
class Symtab;
struct SymbolBindings;
struct UndefinedVariableState;

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
  static T& CurrentService() {
    const size_t slot = ServiceSlot<T>();
    if (slot >= services_count_)
      throw std::logic_error("too many build session services");
    void*& value = active_services_[slot];
    if (!value)
      value = &Current().Get<T>();
    return *static_cast<T*>(value);
  }

  template <class T>
  T& Get() {
    const size_t slot = ServiceSlot<T>();
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

  template <class T>
  void* Allocate(size_t size) {
    static const size_t type = NextAllocationType();
    return Allocate(size, [](void* p) { delete static_cast<T*>(p); }, type);
  }
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
  struct Chunk;
  struct Pool {
    Chunk* current = nullptr;
    void* available = nullptr;
  };
  static constexpr size_t kAlignment = alignof(std::max_align_t);
  static constexpr size_t kMaxPooled = 1024;
  static constexpr size_t kChunkSize = 64 * 1024;
  using Pools = std::array<Pool, kMaxPooled / kAlignment>;
  void* Allocate(size_t size, void (*destroy)(void*), size_t type);
  Chunk* NewChunk(size_t slot_size, void (*destroy)(void*), Pool* pool);
  static size_t NextAllocationType();
  struct Destructor {
    void* value;
    void (*destroy)(void*);
  };
  static size_t NextSlot();
  template <class T>
  static size_t ServiceSlot() {
    // These language services are read for almost every token/variable. Fixed
    // indices avoid a dynamic-static guard and index lookup on that hot path.
    // Ownership, lazy construction and binding invalidation remain identical.
    if constexpr (std::is_same_v<T, Flags>)
      return 0;
    else if constexpr (std::is_same_v<T, Symtab>)
      return 1;
    else if constexpr (std::is_same_v<T, SymbolBindings>)
      return 2;
    else if constexpr (std::is_same_v<T, UndefinedVariableState>)
      return 3;
    else {
      static const size_t slot = NextSlot();
      return slot;
    }
  }
  static constexpr size_t services_count_ = 128;
  // A binding invalidates this thread's borrowed pointers. Ownership and
  // publication still belong to the session; workers cache the same services.
  // Constant initialization is visible to callers, so hot lookups do not need
  // an out-of-line TLS initializer check before accessing the cache.
  inline static thread_local void* active_services_[services_count_] = {};
  BuildSession* previous_;
  std::array<std::atomic<void*>, services_count_> services_{};
  std::recursive_mutex mutex_;
  std::vector<Destructor> destructors_;
  Chunk* chunks_ = nullptr;
  std::array<std::unique_ptr<Pools>, 128> pools_{};
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
  return BuildSession::CurrentService<T>();
}

// Objects can be deleted early. Remaining objects are destroyed exactly once at
// invocation teardown. Borrowed AST edges must not delete peers during
// draining.
template <class T>
class SessionAllocated {
 public:
  static void* operator new(size_t size) {
    return BuildSession::Current().Allocate<T>(size);
  }
  static void operator delete(void* value) noexcept {
    BuildSession::Release(value);
  }

 protected:
  static bool DrainingSession() { return BuildSession::Current().draining(); }
};
#endif
