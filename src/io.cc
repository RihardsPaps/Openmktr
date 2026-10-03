// FORK MODIFICATION NOTICE (2026)
// Changed by the GNU-free Kati fork, maintained by Rihards Paps and
// Haralds Paps.
// Changed build execution, evaluation, or portability for this fork.
// Upstream material retains its Apache-2.0 terms. Fork modifications are
// covered by Mozilla Public License 2.0; see docs/LICENSING.md and NOTICE
// at the repository root. Original notices below remain applicable.

// Copyright 2015 Google Inc. All rights reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// +build ignore

#include "io.h"

#include <limits.h>

#include "log.h"

void DumpInt(FILE* fp, int v) {
  size_t r = fwrite(&v, sizeof(v), 1, fp);
  CHECK(r == 1);
}

void DumpString(FILE* fp, std::string_view s) {
  CHECK(s.size() <= INT_MAX);
  DumpInt(fp, s.size());
  size_t r = fwrite(s.data(), 1, s.size(), fp);
  CHECK(r == s.size());
}

int LoadInt(FILE* fp) {
  int v;
  size_t r = fread(&v, sizeof(v), 1, fp);
  if (r != 1)
    return -1;
  return v;
}

bool LoadString(FILE* fp, std::string* s) {
  int len = LoadInt(fp);
  constexpr int kMaxStampString = 256 * 1024 * 1024;
  if (len < 0 || len > kMaxStampString)
    return false;
  s->resize(len);
  if (s->empty())
    return true;
  size_t r = fread(s->data(), 1, s->size(), fp);
  if (r != s->size())
    return false;
  return true;
}
