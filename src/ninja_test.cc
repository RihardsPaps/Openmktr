// FORK MODIFICATION NOTICE (2026)
// Own invocation state and publish coherent generations with typed freshness.
// Verify owned, C-string and bounded-view filesystem probes across mutations.
// Cover conservative depfile inference and reject ambiguous shell commands.
// Fork modifications are covered by MPL-2.0; see docs/LICENSING.md and NOTICE.

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

#include "ninja.h"
#include "session.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "fileutil.h"
#include "log.h"
#include "testutil.h"

namespace {

void TestOwnedFilesystemPaths() {
  char temporary[] = "/tmp/omktr-owned-path-XXXXXX";
  int fd = mkstemp(temporary);
  assert(fd >= 0);
  close(fd);
  const std::string name(temporary);
  const std::string buffer = name + "-not-part-of-the-view";
  const std::string_view view(buffer.data(), name.size());
  std::string embedded = name;
  embedded.append("\0suffix", 7);
  for (bool cached : {false, true}) {
    ScopedFileObservations observations(cached);
    const double stamp = GetTimestamp(name);
    assert(stamp >= 0);
    for (int repeat = 0; repeat < 2; ++repeat) {
      assert(Exists(name) && Exists(name.c_str()) && Exists(view));
      assert(Exists(embedded) && !Exists(buffer));
      ASSERT_EQ(GetTimestamp(name.c_str()), stamp);
      ASSERT_EQ(GetTimestamp(view), stamp);
      ASSERT_EQ(GetTimestamp(embedded), stamp);
      ASSERT_EQ(GetTimestamp(buffer), -2.0);
    }
    {
      ScopedFilesystemEffect effect;
      const int removed = unlink(name.c_str());
      assert(removed == 0);
    }
    assert(!Exists(name) && !Exists(view) && !Exists(embedded));
    ASSERT_EQ(GetTimestamp(name), -2.0);
    {
      ScopedFilesystemEffect effect;
      fd = open(name.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
      assert(fd >= 0);
      close(fd);
    }
    assert(Exists(name) && Exists(view) && Exists(embedded));
    assert(GetTimestamp(name) >= 0);
  }
  const int removed = unlink(name.c_str());
  assert(removed == 0);
}

std::string GetDepfile(std::string cmd, std::string* new_cmd) {
  new_cmd->clear();
  std::string r;
  if (GetDepfileFromCommand(&cmd, &r)) {
    *new_cmd = cmd;
    return r;
  }
  return "";
}

void TestGetDepfile() {
  std::string new_cmd;
  ASSERT_EQ(GetDepfile("clang -c source.c -MMD -o out.o", &new_cmd),
            "out.d.tmp");
  ASSERT_EQ(new_cmd,
            "( clang -c source.c -MMD -o out.o ) && cp 'out.d' 'out.d.tmp'");
  ASSERT_EQ(
      GetDepfile("ccache clang++ -c source.cc -MD -MFdeps.d -oout.o", &new_cmd),
      "deps.d.tmp");
  ASSERT_EQ(GetDepfile("clang -c source.c -MD -o old.o -o new.o -MF old.d "
                       "-MF new.d",
                       &new_cmd),
            "new.d.tmp");
  ASSERT_EQ(GetDepfile("clang\t-c\tsource.c\t-MMD\t-o\tout.o", &new_cmd),
            "out.d.tmp");
  // A .P-looking macro is a literal argument, not an Android removal command.
  ASSERT_EQ(
      GetDepfile("clang -c source.c -MMD -o out.o -DNAME=out.P", &new_cmd),
      "out.d.tmp");
  const char* ambiguous[] = {
      "clang -c source.c -MMD",
      "clang -c source.c -MMD -o",
      "clang -c source.c -MMD -o out.o -MF",
      "clang -c source.c -o out.o",
      "clang -coverage source.c -MMD -o out.o",
      "clang -c source.c -MMDsuffix -o out.o",
      "echo ' -c -MD -o out.o '",
      "clang -c source.c -MMD -o 'space name.o'",
      "clang -c source.c -MMD -o space\\ name.o",
      "clang -c source.c -MMD -o $OUTPUT",
      "clang -c source.c -MMD -o out.o # comment",
      "clang -c source.c -MMD -o out.o; false",
      "clang -c source.c -MMD -o out.o && clang -c second.c -o second.o",
      "echo -MD && clang -c source.c -o out.o",
      "clang -c source.c -MMD -o out.o | cat",
      "clang -c source.c -MMD -o out.o > log",
      "clang -c source.c -MMD -o out.o\nfalse",
      "wrapper clang -c source.c -MMD -o out.o",
      "clang @flags -c source.c -MMD -o out.o",
      "clang -c source.c -MMD -o out.o -MF -",
      "clang -c source.c -MMD -o out.o -E",
      "clang -c source.c -MMD -o out.o -Xclang -E",
      "clang -c source.c -include -MD -o out.o",
      "clang -c source.c -D -MD -o out.o",
      "clang -c source.c -MMD -o out.o -unknown-option",
      "clang -c source.s -MMD -o out.o",
  };
  for (const char* command : ambiguous) {
    std::string cmd = command;
    std::string depfile;
    assert(!GetDepfileFromCommand(&cmd, &depfile));
    ASSERT_EQ(cmd, command);
    ASSERT_EQ(depfile, "");
  }
  assert(!g_last_error);
}

}  // namespace

int main() {
  BuildSession session;
  g_log_no_exit = true;
  TestGetDepfile();
  TestOwnedFilesystemPaths();
  assert(!g_failed);
}
