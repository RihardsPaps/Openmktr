// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include "session.h"

#include <new>

namespace {
thread_local BuildSession* active_session = nullptr;
std::atomic<size_t> next_slot{0};
}  // namespace

struct alignas(std::max_align_t) BuildSession::Allocation {
  BuildSession* owner;
  Allocation* previous;
  Allocation* next;
  void (*destroy)(void*);
};

BuildSession::BuildSession() : previous_(Bind(this)) {}
BuildSession::~BuildSession() {
  Bind(this);
  // Retain every service (including backing text) until every AST is destroyed.
  draining_ = true;
  while (objects_)
    objects_->destroy(objects_ + 1);
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
  active_session = session;
  return previous;
}
size_t BuildSession::NextSlot() {
  return next_slot.fetch_add(1);
}
void* BuildSession::Allocate(size_t size, void (*destroy)(void*)) {
  auto* allocation =
      static_cast<Allocation*>(::operator new(sizeof(Allocation) + size));
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  *allocation = {this, nullptr, objects_, destroy};
  if (objects_)
    objects_->previous = allocation;
  objects_ = allocation;
  ++allocations_;
  return allocation + 1;
}
void BuildSession::Release(void* value) noexcept {
  if (!value)
    return;
  auto* allocation = static_cast<Allocation*>(value) - 1;
  auto* owner = allocation->owner;
  std::lock_guard<std::recursive_mutex> lock(owner->mutex_);
  if (allocation->previous)
    allocation->previous->next = allocation->next;
  else
    owner->objects_ = allocation->next;
  if (allocation->next)
    allocation->next->previous = allocation->previous;
  --owner->allocations_;
  ::operator delete(allocation);
}
