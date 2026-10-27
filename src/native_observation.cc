// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "native_observation.h"

#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>

#include "fileutil.h"
#include "find.h"
#include "flags.h"
#include "graph_optimization.h"
#include "strutil.h"

extern char** environ;

namespace {
constexpr size_t kPacketLimit = 4 * 1024 * 1024;

int PrivateTemporary(char* name) {
  int temporary = mkstemp(name);
  if (temporary < 0)
    return -1;
  unlink(name);
  // Keep action sources separate from stdio even if the caller closed one
  // of its standard descriptors. Close-on-exec also isolates concurrent spawn.
  int descriptor = fcntl(temporary, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  close(temporary);
  return descriptor;
}

bool ReadCount(std::string_view* view, size_t* count) {
  std::string value;
  if (!ConsumeGraphString(view, &value) || value.empty() ||
      value.find_first_not_of("0123456789") != std::string::npos)
    return false;
  char* end;
  unsigned long result = strtoul(value.c_str(), &end, 10);
  if (*end || result > 65536)
    return false;
  *count = result;
  return true;
}
}  // namespace

bool DecodeNativeFind(const std::string& command,
                      const std::string& packet,
                      std::string* result,
                      std::unique_ptr<FindCommand>* descriptor) {
  std::string_view view(packet);
  std::string version;
  if (packet.size() > kPacketLimit || !ConsumeGraphString(&view, &version) ||
      version != "native-find-v1" || !ConsumeGraphString(&view, result))
    return false;
  auto parsed = std::make_unique<FindCommand>();
  if (!parsed->Parse(command))
    return false;
  size_t count;
  if (!ReadCount(&view, &count))
    return false;
  for (size_t i = 0; i < count; ++i) {
    std::string value;
    if (!ConsumeGraphString(&view, &value))
      return false;
    parsed->found_files->push_back(std::move(value));
  }
  if (!ReadCount(&view, &count))
    return false;
  for (size_t i = 0; i < count; ++i) {
    std::string value;
    if (!ConsumeGraphString(&view, &value))
      return false;
    parsed->read_dirs->insert(std::move(value));
  }
  if (!view.empty())
    return false;
  *descriptor = std::move(parsed);
  return true;
}

int NativeFindWorker(const char* command, bool warnings_are_errors) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_FSIZE, &limit) == 0) {
    limit.rlim_cur = std::min<rlim_t>(limit.rlim_cur, kPacketLimit);
    setrlimit(RLIMIT_FSIZE, &limit);
  }
  g_flags.werror_find_emulator = warnings_are_errors;
  InitFindEmulator();
  FindCommand descriptor;
  std::string result;
  if (!descriptor.Parse(command) ||
      !FindEmulator::Get()->HandleFind(command, descriptor,
                                       Loc("<native-validation>", 1), &result))
    return 3;
  std::string packet;
  AppendGraphString("native-find-v1", &packet);
  AppendGraphString(result, &packet);
  AppendGraphString(std::to_string(descriptor.found_files->size()), &packet);
  for (const auto& path : *descriptor.found_files)
    AppendGraphString(path, &packet);
  AppendGraphString(std::to_string(descriptor.read_dirs->size()), &packet);
  for (const auto& path : *descriptor.read_dirs)
    AppendGraphString(path, &packet);
  if (packet.size() > kPacketLimit ||
      fwrite(packet.data(), 1, packet.size(), stdout) != packet.size())
    return 3;
  return 0;
}

bool QueryNativeFind(const std::string& command, std::string* packet) {
  if (command.size() > 64 * 1024 || command.find('\0') != std::string::npos)
    return false;
  char output_name[] = "/tmp/omktr-native-output-XXXXXX";
  char error_name[] = "/tmp/omktr-native-errors-XXXXXX";
  int output = PrivateTemporary(output_name);
  if (output < 0)
    return false;
  int errors = PrivateTemporary(error_name);
  if (errors < 0) {
    close(output);
    return false;
  }
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (!error) {
    error = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    if (!error)
      error = posix_spawn_file_actions_adddup2(&actions, errors, STDERR_FILENO);
    if (!error && output > STDERR_FILENO)
      error = posix_spawn_file_actions_addclose(&actions, output);
    if (!error && errors > STDERR_FILENO)
      error = posix_spawn_file_actions_addclose(&actions, errors);
    std::string executable = GetExecutablePath();
    char* arguments[] = {
        executable.data(), const_cast<char*>("--internal-native-find"),
        const_cast<char*>(command.c_str()),
        const_cast<char*>(g_flags.werror_find_emulator ? "1" : "0"), nullptr};
    pid_t child = -1;
    if (!error)
      error = posix_spawn(&child, executable.c_str(), &actions, nullptr,
                          arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    bool waited = !error && HANDLE_EINTR(waitpid(child, &status, 0)) == child;
    struct stat out_stat{}, err_stat{};
    bool accepted = waited && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                    fstat(output, &out_stat) == 0 &&
                    fstat(errors, &err_stat) == 0 && err_stat.st_size == 0 &&
                    out_stat.st_size >= 0 &&
                    static_cast<size_t>(out_stat.st_size) <= kPacketLimit &&
                    lseek(output, 0, SEEK_SET) == 0;
    if (accepted) {
      packet->resize(out_stat.st_size);
      size_t offset = 0;
      while (offset < packet->size()) {
        ssize_t read_size = HANDLE_EINTR(
            read(output, packet->data() + offset, packet->size() - offset));
        if (read_size <= 0) {
          accepted = false;
          break;
        }
        offset += read_size;
      }
    }
    close(output);
    close(errors);
    return accepted;
  }
  close(output);
  close(errors);
  return false;
}
