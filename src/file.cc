// FORK MODIFICATION NOTICE (2026)
// Changed by the GNU-free Kati fork, maintained by Rihards Paps and
// Haralds Paps.
// Changed build execution, evaluation, or portability for this fork.
// Upstream material retains its Apache-2.0 terms. Fork modifications are
// covered by PolyForm Perimeter 1.0.1; see docs/LICENSING.md and NOTICE
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

#include "file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fileutil.h"
#include "log.h"
#include "parser.h"
#include "stmt.h"

Makefile::Makefile(const std::string& filename, bool from_stdin)
    : mtime_(0), filename_(filename), exists_(false) {
  int fd = from_stdin ? dup(STDIN_FILENO) : open(filename.c_str(), O_RDONLY);
  if (fd < 0) {
    return;
  }

  struct stat st;
  if (fstat(fd, &st) < 0) {
    PERROR("fstat failed for %s", filename.c_str());
  }

  mtime_ = st.st_mtime;
  exists_ = true;
  if (from_stdin) {
    // Automake pipes filtered Makefiles to `make -f - am--depfiles`.
    // Pipes have no file size, and redirected stdin may already be positioned
    // partway through a regular file. Read the remaining stream through EOF.
    char chunk[8192];
    while (true) {
      ssize_t r = HANDLE_EINTR(read(fd, chunk, sizeof(chunk)));
      if (r == -1)
        PERROR("read failed for %s", filename.c_str());
      if (r == 0)
        break;
      buf_.append(chunk, static_cast<size_t>(r));
    }
  } else {
    size_t len = st.st_size;
    buf_.resize(len);
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
