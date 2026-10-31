// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include "session.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define SESSION_ASAN 1
#endif
#endif

namespace {
thread_local BuildSession* active_session = nullptr;
std::atomic<size_t> next_slot{4};
std::atomic<size_t> next_allocation_type{0};
}  // namespace

// Aligned chunks recover ownership directly from an object address. A bitmap
// and one deleter per type/size chunk replace per-object allocation headers.
struct alignas(std::max_align_t) BuildSession::Chunk {
  BuildSession* owner;
  Chunk* previous;
  Chunk* next;
  Pool* pool;
  void (*destroy)(void*);
  size_t slot_size;
  size_t used;
  size_t capacity;
  uint64_t live[kChunkSize / kAlignment / 64] = {};
};

BuildSession::BuildSession() : previous_(Bind(this)) {}
BuildSession::~BuildSession() {
  Bind(this);
  // Retain every service (including backing text) until every AST is destroyed.
  draining_ = true;
  while (allocations_) {
    for (Chunk* chunk = chunks_; chunk; chunk = chunk->next) {
      for (size_t index = 0; index < chunk->used; ++index) {
        if (chunk->live[index / 64] & (uint64_t{1} << (index % 64)))
          chunk->destroy(reinterpret_cast<char*>(chunk + 1) +
                         index * chunk->slot_size);
      }
    }
  }
  while (chunks_) {
    Chunk* next = chunks_->next;
    ::operator delete(chunks_, std::align_val_t(kChunkSize));
    chunks_ = next;
  }
  for (auto it = destructors_.rbegin(); it != destructors_.rend(); ++it)
    it->destroy(it->value);
  Bind(previous_);
}
BuildSession& BuildSession::Current() {
  if (!active_session)
    throw std::logic_error("no active build session");
  return *active_session;
}
BuildSession* BuildSession::Active() {
  return active_session;
}
BuildSession* BuildSession::Bind(BuildSession* session) {
  auto* previous = active_session;
  if (session != previous)
    std::fill(std::begin(active_services_), std::end(active_services_),
              nullptr);
  active_session = session;
  return previous;
}
size_t BuildSession::NextSlot() {
  return next_slot.fetch_add(1);
}
size_t BuildSession::NextAllocationType() {
  return next_allocation_type.fetch_add(1);
}
void* BuildSession::Allocate(size_t size, void (*destroy)(void*), size_t type) {
  if (type >= pools_.size() ||
      size > std::numeric_limits<size_t>::max() - sizeof(Chunk) - kAlignment)
    throw std::bad_alloc();
  const size_t slot_size =
      (std::max(size, sizeof(void*)) + kAlignment - 1) & ~(kAlignment - 1);
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  void* value;
  Chunk* chunk;
  size_t index;
  if (slot_size <= kMaxPooled) {
    if (!pools_[type])
      pools_[type] = std::make_unique<Pools>();
    Pool& pool = (*pools_[type])[slot_size / kAlignment - 1];
    if (pool.available) {
      value = pool.available;
#ifdef SESSION_ASAN
      __asan_unpoison_memory_region(value, sizeof(void*));
#endif
      pool.available = *static_cast<void**>(value);
      chunk = reinterpret_cast<Chunk*>(reinterpret_cast<uintptr_t>(value) &
                                       ~(uintptr_t{kChunkSize} - 1));
      index = (static_cast<char*>(value) - reinterpret_cast<char*>(chunk + 1)) /
              slot_size;
    } else {
      if (!pool.current || pool.current->used == pool.current->capacity)
        pool.current = NewChunk(slot_size, destroy, &pool);
      chunk = pool.current;
      index = chunk->used++;
      value = reinterpret_cast<char*>(chunk + 1) + index * slot_size;
    }
  } else {
    chunk = NewChunk(slot_size, destroy, nullptr);
    index = 0;
    chunk->used = 1;
    value = chunk + 1;
  }
#ifdef SESSION_ASAN
  __asan_unpoison_memory_region(value, size);
  __asan_poison_memory_region(static_cast<char*>(value) + size,
                              slot_size - size);
#endif
  chunk->live[index / 64] |= uint64_t{1} << (index % 64);
  ++allocations_;
  return value;
}

BuildSession::Chunk* BuildSession::NewChunk(size_t slot_size,
                                            void (*destroy)(void*),
                                            Pool* pool) {
  const size_t bytes = pool ? kChunkSize : sizeof(Chunk) + slot_size;
  auto* chunk =
      static_cast<Chunk*>(::operator new(bytes, std::align_val_t(kChunkSize)));
  new (chunk)
      Chunk{this,    nullptr,   chunks_, pool,
            destroy, slot_size, 0,       (bytes - sizeof(Chunk)) / slot_size,
            {}};
  if (chunks_)
    chunks_->previous = chunk;
  chunks_ = chunk;
#ifdef SESSION_ASAN
  __asan_poison_memory_region(chunk + 1, bytes - sizeof(Chunk));
#endif
  return chunk;
}

void BuildSession::Release(void* value) noexcept {
  if (!value)
    return;
  auto* chunk = reinterpret_cast<Chunk*>(reinterpret_cast<uintptr_t>(value) &
                                         ~(uintptr_t{kChunkSize} - 1));
  auto* owner = chunk->owner;
  // Workers have joined before teardown. Avoid a lock/unlock pair for every
  // survivor while the sole owner drains the arena.
  std::unique_lock<std::recursive_mutex> lock(owner->mutex_, std::defer_lock);
  if (!owner->draining_)
    lock.lock();
  const size_t index =
      (static_cast<char*>(value) - reinterpret_cast<char*>(chunk + 1)) /
      chunk->slot_size;
  chunk->live[index / 64] &= ~(uint64_t{1} << (index % 64));
  --owner->allocations_;
  if (!owner->draining_ && chunk->pool) {
    *static_cast<void**>(value) = chunk->pool->available;
    chunk->pool->available = value;
  }
#ifdef SESSION_ASAN
  __asan_poison_memory_region(value, chunk->slot_size);
#endif
  if (!owner->draining_ && !chunk->pool) {
    if (chunk->previous)
      chunk->previous->next = chunk->next;
    else
      owner->chunks_ = chunk->next;
    if (chunk->next)
      chunk->next->previous = chunk->previous;
    ::operator delete(chunk, std::align_val_t(kChunkSize));
  }
}
