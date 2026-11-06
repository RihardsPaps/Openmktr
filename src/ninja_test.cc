// FORK MODIFICATION NOTICE (2026)
// Own invocation state and publish coherent generations with typed freshness.
// Verify owned, C-string and bounded-view filesystem probes across mutations.
// Cover conservative depfile inference and reject ambiguous shell commands.
// Exercise quoted literal arguments and decline shell-dependent depfile paths.
// Round-trip shared Kbuild header ownership without repeating absolute paths.
// Exercise long fixdep lists and reject unsupported dependency syntax.
// Keep scoped index paths alive after releasing the immutable transport.
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

#include <algorithm>

#include "fileutil.h"
#include "graph_import.h"
#include "graph_optimization.h"
#include "log.h"
#include "testutil.h"

namespace {

void TestLongKbuildDependencyList() {
  char temporary[] = "/tmp/omktr-kbuild-index-XXXXXX";
  assert(mkdtemp(temporary));
  const std::string directory(temporary);
  const std::string record = directory + "/.main.o.cmd";
  const std::string source = "source_main.o := main.c\n";
  auto publish = [&](const std::string& text) {
    FILE* file = fopen(record.c_str(), "w");
    assert(file);
    assert(fwrite(text.data(), 1, text.size(), file) == text.size());
    assert(fclose(file) == 0);
  };
  std::string dependencies = "deps_main.o := \\\n";
  for (int i = 0; i < 4000; ++i)
    dependencies +=
        "  $(wildcard include/config/CFG" + std::to_string(i) + ") \\\n";
  dependencies += "  include/config/SPACE\\ NAME\n";
  publish(source + dependencies);
  KbuildDependencyIndex index;
  index.Read(directory, directory);
  assert(index.complete);
  assert(index.objects.size() == 1);
  assert(index.Inputs(directory).size() == 4003);
  assert(index.Covers(directory + "/include/config/CFG3999"));
  assert(index.Covers(directory + "/include/config/SPACE NAME"));
  char proof_name[] = "/tmp/omktr-kbuild-proof-XXXXXX";
  int descriptor = mkstemp(proof_name);
  assert(descriptor >= 0);
  close(descriptor);
  assert(WriteGraphCache(proof_name, index.TreeProof()));
  const std::string object = directory + "/main.o";
  descriptor = open(object.c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600);
  assert(descriptor >= 0);
  close(descriptor);
  assert(ValidateImportedGraph(proof_name));
  const std::string unknown = directory + "/unknown.o";
  descriptor = open(unknown.c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600);
  assert(descriptor >= 0);
  close(descriptor);
  assert(!ValidateImportedGraph(proof_name));
  assert(unlink(unknown.c_str()) == 0);
  assert(unlink(object.c_str()) == 0);
  assert(symlink(record.c_str(), object.c_str()) == 0);
  assert(!ValidateImportedGraph(proof_name));
  assert(unlink(object.c_str()) == 0);
  descriptor = open(object.c_str(), O_CREAT | O_WRONLY | O_EXCL, 0600);
  assert(descriptor >= 0);
  close(descriptor);
  index.Read(directory, directory);
  assert(index.complete && WriteGraphCache(proof_name, index.TreeProof()));
  assert(unlink(object.c_str()) == 0);
  assert(ValidateImportedGraph(proof_name));
  assert(unlink(record.c_str()) == 0);
  assert(!ValidateImportedGraph(proof_name));
  assert(unlink(proof_name) == 0);
  for (const std::string& invalid :
       {"$(UNKNOWN)", "$(wildcard include/BROKEN", "$(wildcard include/*.h)"}) {
    publish(source + "deps_main.o := " + invalid + "\n");
    index.Read(directory, directory);
    assert(!index.complete);
  }
  assert(unlink(record.c_str()) == 0);
  assert(rmdir(directory.c_str()) == 0);
}

void TestSharedKbuildDependencyIndex() {
  KbuildDependencyIndex index;
  index.complete = true;
  std::vector<std::string> headers;
  std::string root = "/source";
  for (int part = 0; part < 8; ++part)
    root += "/" + std::string(64, 's');
  for (int i = 0; i < 128; ++i)
    headers.push_back(root + "/header" + std::to_string(i) + ".h");
  std::sort(headers.begin(), headers.end());
  std::vector<Symbol> symbols;
  for (const std::string& header : headers)
    symbols.push_back(Intern(header));
  std::sort(symbols.begin(), symbols.end());
  for (int i = 0; i < 128; ++i)
    index.objects["/build/group" + std::to_string(i / 64) + "/object" +
                  std::to_string(i) + ".o"] = symbols;
  index.covered_inputs.insert(symbols.begin(), symbols.end());
  const std::string encoded = index.Encode();
  for (auto& object : index.objects)
    std::reverse(object.second.begin(), object.second.end());
  assert(index.Encode() == encoded);
  for (auto& object : index.objects)
    std::sort(object.second.begin(), object.second.end());
  // The dependency relation has over 8 MiB of repeated absolute header names.
  // The immutable transport should grow with shared IDs, not those spellings.
  assert(encoded.size() < 1024 * 1024);
  KbuildDependencyIndex restored;
  assert(restored.Decode(encoded));
  assert(restored.complete);
  assert(restored.objects == index.objects);
  assert(restored.covered_inputs == index.covered_inputs);
  assert(restored.Inputs("/build") == headers);
  assert(restored.Covers(headers.front()));
  assert(restored.Decode(encoded, "/build/group0"));
  assert(restored.complete && restored.objects.size() == 64);
  assert(restored.Inputs("/build/group0") == headers);
  assert(restored.Inputs("/build/group1").empty());
  // Previously published v2 records use first-seen dictionary order rather
  // than lexical IDs. They must remain readable with the compact storage.
  std::string legacy, dictionary, references;
  for (const char* path : {"/source/z.h", "/source/a.h"})
    AppendGraphString(path, &dictionary);
  references.append("\1\0\0\0\0\0\0\0", 8);
  for (const std::string& part :
       {std::string("kbuild-dependencies-v2"), std::string("1"), dictionary,
        std::string("/build/legacy.o"), references})
    AppendGraphString(part, &legacy);
  assert(restored.Decode(legacy));
  assert(restored.Inputs("/build") ==
         (std::vector<std::string>{"/source/a.h", "/source/z.h"}));
  legacy.back() = static_cast<char>(0xff);
  assert(!restored.Decode(legacy));
  const std::string kept = "/source/scoped-unique-header.h";
  const std::string skipped = "/source/unscoped-unique-header.h";
  assert(!FindSymbol(skipped).IsValid());
  {
    std::string payload, names;
    AppendGraphString(kept, &names);
    AppendGraphString(skipped, &names);
    for (const std::string& part :
         {std::string("kbuild-dependencies-v2"), std::string("1"), names,
          std::string("/build/keep/one.o"), std::string(4, '\0'),
          std::string("/build/skip/two.o"), std::string("\1\0\0\0", 4)})
      AppendGraphString(part, &payload);
    assert(restored.Decode(payload, "/build/keep"));
    assert(!FindSymbol(skipped).IsValid());
    KbuildDependencyIndex invalid;
    payload.back() = static_cast<char>(0xff);
    assert(!invalid.Decode(payload, "/build/keep"));
  }
  assert(restored.complete);
  assert(restored.Inputs("/build/keep") == (std::vector<std::string>{kept}));
}

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

void TestRelaxedDepfiles() {
  std::string command = "clang -c source.c -MMD -MF 'space name.d' -o out.o";
  std::string depfile;
  assert(GetDepfileFromCommand(&command, &depfile, true, false));
  ASSERT_EQ(depfile, "space name.d");
  for (const char* source : {"clang -c source.c -MMD -MF \"$DEPFILE\" -o out.o",
                             "clang -c source.c -MMD -o out.o # -MF ignored.d",
                             "clang @flags -c source.c -MMD -o out.o",
                             "clang -c source.c -MMD -o ''",
                             "nasm -f elf64 -MD '' -o out.o source.asm"}) {
    command = source;
    depfile.clear();
    assert(!GetDepfileFromCommand(&command, &depfile, true, false));
    ASSERT_EQ(command, source);
    ASSERT_EQ(depfile, "");
  }
}

}  // namespace

int main() {
  BuildSession session;
  g_log_no_exit = true;
  TestGetDepfile();
  TestRelaxedDepfiles();
  TestOwnedFilesystemPaths();
  TestSharedKbuildDependencyIndex();
  TestLongKbuildDependencyList();
  assert(!g_failed);
}
