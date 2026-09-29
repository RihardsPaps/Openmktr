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

#include "file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>

#include "fileutil.h"
#include "log.h"
#include "parser.h"
#include "stmt.h"

Makefile::Makefile(const std::string& filename)
    : mtime_(0), filename_(filename), exists_(false) {
  if (filename == "-") {
    exists_ = true;
    // A generated include can restart the process after stdin has been
    // consumed. Keep an anonymous seekable copy on an inherited descriptor
    // so -f - reads the same makefile after execvp.
    const char* saved_fd = getenv("KATI_STDIN_MAKEFILE_FD");
    if (saved_fd != nullptr) {
      char* end = nullptr;
      long descriptor = strtol(saved_fd, &end, 10);
      struct stat st;
      if (*end != '\0' || descriptor < 0 ||
          fstat(static_cast<int>(descriptor), &st) != 0 ||
          !S_ISREG(st.st_mode) ||
          lseek(static_cast<int>(descriptor), 0, SEEK_SET) < 0)
        ERROR("invalid saved standard-input makefile descriptor");
      char chunk[8192];
      while (true) {
        ssize_t count = HANDLE_EINTR(
            read(static_cast<int>(descriptor), chunk, sizeof(chunk)));
        if (count < 0)
          PERROR("read failed for saved standard-input makefile");
        if (count == 0)
          break;
        buf_.append(chunk, static_cast<size_t>(count));
      }
      Parse(this);
      return;
    }
    char chunk[8192];
    while (true) {
      ssize_t count = HANDLE_EINTR(read(STDIN_FILENO, chunk, sizeof(chunk)));
      if (count < 0)
        PERROR("read failed for standard input");
      if (count == 0)
        break;
      buf_.append(chunk, static_cast<size_t>(count));
    }
    FILE* saved = tmpfile();
    if (saved == nullptr ||
        fwrite(buf_.data(), 1, buf_.size(), saved) != buf_.size() ||
        fflush(saved) != 0)
      PERROR("saving standard-input makefile failed");
    const int descriptor = fileno(saved);
    const int descriptor_flags = fcntl(descriptor, F_GETFD);
    if (descriptor_flags < 0 ||
        fcntl(descriptor, F_SETFD, descriptor_flags & ~FD_CLOEXEC) < 0)
      PERROR("preserving standard-input makefile descriptor failed");
    if (setenv("KATI_STDIN_MAKEFILE_FD", std::to_string(descriptor).c_str(),
               1) != 0)
      PERROR("recording standard-input makefile descriptor failed");
    Parse(this);
    return;
  }
  int fd = open(filename.c_str(), O_RDONLY);
  if (fd < 0) {
    return;
  }

  struct stat st;
  if (fstat(fd, &st) < 0) {
    PERROR("fstat failed for %s", filename.c_str());
  }

  size_t len = st.st_size;
  mtime_ = st.st_mtime;
  buf_.resize(len);
  exists_ = true;
  size_t remaining = len;
  while (remaining > 0) {
    size_t completed = len - remaining;
    ssize_t r = HANDLE_EINTR(read(fd, &buf_[completed], remaining));
    if (r == -1) {
      PERROR("read failed for %s", filename.c_str());
    }
    if (r == 0)
      ERROR("short read for %s", filename.c_str());
    remaining -= r;
  }

  if (close(fd) < 0) {
    PERROR("close failed for %s", filename.c_str());
  }

  Parse(this);
}

Makefile::~Makefile() {
  for (Stmt* stmt : stmts_)
    delete stmt;
}
