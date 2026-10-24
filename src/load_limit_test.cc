// Copyright 2026 Rihards Paps and Haralds Paps
// SPDX-License-Identifier: MPL-2.0

#include "load_limit.h"

#include <assert.h>

int main() {
  assert(LoadAllowsJob(-1, 4, 100));
  assert(LoadAllowsJob(-1, 4, -1));
  assert(LoadAllowsJob(0, 0, 100));
  assert(LoadAllowsJob(2.5, 0, -1));
  assert(LoadAllowsJob(2.5, 1, 2.49));
  assert(!LoadAllowsJob(2.5, 1, 2.5));
  assert(!LoadAllowsJob(2.5, 1, 100));
  assert(!LoadAllowsJob(2.5, 1, -1));
  assert(!LoadAllowsJob(0, 1, 0));
  // A subsequent lower sample admits waiting work; a finished last recipe
  // always permits progress, including after failed sampling.
  assert(LoadAllowsJob(2.5, 1, 1));
  assert(LoadAllowsJob(2.5, 0, -1));
  return 0;
}
