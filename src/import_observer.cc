// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "import_observer.h"
#include "fileutil.h"
#include "log.h"
#include "process.h"
#include "stats.h"

#if defined(__linux__) && defined(__x86_64__)
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>
#include <vector>

namespace {
// Linux seccomp/BPF ABI, kept local so the portable image needs no kernel
// development headers. Metadata-heavy validation runs without ptrace stops;
// operations with observable effects still stop before executing.
struct FilterInstruction {
  uint16_t code;
  uint8_t yes;
  uint8_t no;
  uint32_t value;
};
bool InstallFilter() {
  constexpr uint32_t trace = 0x7ff00000;
  constexpr uint32_t allow = 0x7fff0000;
  std::vector<FilterInstruction> instructions{{0x20, 0, 0, 4},
                                              {0x15, 1, 0, 0xc000003e},
                                              {0x06, 0, 0, trace},
                                              {0x20, 0, 0, 0}};
  // BeginImportObservation closes every inherited descriptor above stderr.
  // New descriptors are private opens or duplicates whose source is checked
  // below. Thus fd >= 3 cannot alias the caller's input: reads and seeks on
  // these descriptors need no tracer stop. Standard descriptors stay checked.
  for (unsigned syscall :
       {SYS_read, SYS_readv, SYS_pread64, SYS_preadv, SYS_preadv2, SYS_getdents,
        SYS_getdents64, SYS_lseek}) {
    instructions.push_back({0x15, 0, 4, syscall});
    instructions.push_back({0x20, 0, 0, 16});  // args[0], low 32-bit fd.
    instructions.push_back({0x35, 1, 0, 3});   // Unsigned fd >= 3.
    instructions.push_back({0x06, 0, 0, trace});
    instructions.push_back({0x06, 0, 0, allow});
  }
  const unsigned pure[] = {SYS_close,
                           SYS_stat,
                           SYS_lstat,
                           SYS_fstat,
                           SYS_newfstatat,
                           SYS_statx,
                           SYS_access,
                           SYS_faccessat,
                           SYS_faccessat2,
                           SYS_readlink,
                           SYS_readlinkat,
                           SYS_getcwd,
                           SYS_chdir,
                           SYS_fchdir,
                           SYS_munmap,
                           SYS_mprotect,
                           SYS_mremap,
                           SYS_madvise,
                           SYS_brk,
                           SYS_rt_sigaction,
                           SYS_rt_sigprocmask,
                           SYS_rt_sigreturn,
                           SYS_sigaltstack,
                           SYS_getpid,
                           SYS_getppid,
                           SYS_gettid,
                           SYS_getuid,
                           SYS_geteuid,
                           SYS_getgid,
                           SYS_getegid,
                           SYS_getresuid,
                           SYS_getresgid,
                           SYS_getgroups,
                           SYS_uname,
                           SYS_sysinfo,
                           SYS_clock_gettime,
                           SYS_gettimeofday,
                           SYS_time,
                           SYS_nanosleep,
                           SYS_clock_nanosleep,
                           SYS_getrandom,
                           SYS_getrusage,
                           SYS_getrlimit,
                           SYS_sched_getaffinity,
                           SYS_sched_yield,
                           SYS_futex,
                           SYS_membarrier,
                           SYS_set_tid_address,
                           SYS_set_robust_list,
                           SYS_rseq,
                           SYS_arch_prctl,
                           SYS_wait4,
                           SYS_waitid,
                           SYS_poll,
                           SYS_ppoll,
                           SYS_select,
                           SYS_pselect6,
                           SYS_pipe,
                           SYS_pipe2,
                           SYS_execve,
                           SYS_execveat,
                           SYS_fork,
                           SYS_vfork,
                           SYS_exit,
                           SYS_exit_group,
                           SYS_fsync,
                           SYS_fdatasync,
                           SYS_statfs,
                           SYS_fstatfs};
  for (unsigned syscall : pure) {
    instructions.push_back({0x15, 0, 1, syscall});
    instructions.push_back({0x06, 0, 0, allow});
  }
  instructions.push_back({0x06, 0, 0, trace});
  struct {
    uint16_t count;
    FilterInstruction* data;
  } program{static_cast<uint16_t>(instructions.size()), instructions.data()};
  return prctl(PR_SET_SECCOMP, 2, &program, 0, 0) == 0;
}

// Entry prefix of Linux's PTRACE_GET_SYSCALL_INFO ABI. This avoids requiring
// kernel development headers with the reference musl toolchain.
struct ImportSyscall {
  uint8_t op;
  uint8_t padding[3];
  uint32_t arch;
  uint64_t instruction_pointer;
  uint64_t stack_pointer;
  struct {
    uint64_t nr;
    uint64_t args[6];
  } entry;
};
ssize_t ReadRemoteMemory(pid_t pid,
                         uint64_t address,
                         void* output,
                         size_t size) {
  iovec local{output, size};
  iovec remote{reinterpret_cast<void*>(address), size};
  return HANDLE_EINTR(
      syscall(SYS_process_vm_readv, pid, &local, 1, &remote, 1, 0));
}

bool ReadMemory(pid_t pid, uint64_t address, void* output, size_t size) {
  if (address > UINT64_MAX - size)
    return false;
  auto* bytes = static_cast<char*>(output);
  const ssize_t copied = ReadRemoteMemory(pid, address, output, size);
  if (copied == static_cast<ssize_t>(size))
    return true;
  // Older kernels or tracing policies can deny bulk reads. Keep a bounded
  // ptrace fallback, aligning each word so a final byte at a page boundary
  // never requires access to the following, potentially unmapped page.
  for (size_t offset = 0; offset < size;) {
    const uint64_t current = address + offset;
    const uint64_t aligned =
        current & ~(static_cast<uint64_t>(sizeof(long)) - 1);
    const size_t skip = current - aligned;
    errno = 0;
    const long word = ptrace(PTRACE_PEEKDATA, pid, aligned, 0);
    if (errno)
      return false;
    const size_t count = std::min(sizeof(long) - skip, size - offset);
    memcpy(bytes + offset, reinterpret_cast<const char*>(&word) + skip, count);
    offset += count;
  }
  return true;
}

bool ReadString(pid_t pid, uint64_t address, std::string* output) {
  COLLECT_STATS("import path reads");
  output->clear();
  static const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || address > UINT64_MAX - 4096)
    return false;
  for (size_t offset = 0; offset < 4096;) {
    char buffer[512];
    size_t count = std::min(
        {sizeof(buffer), 4096 - offset,
         static_cast<size_t>(page_size - (address + offset) % page_size)});
    const ssize_t copied =
        ReadRemoteMemory(pid, address + offset, buffer, count);
    if (copied > 0) {
      count = static_cast<size_t>(copied);
    } else {
      count = std::min(count, sizeof(long) - (address + offset) % sizeof(long));
      if (!ReadMemory(pid, address + offset, buffer, count))
        return false;
    }
    const auto* end = static_cast<const char*>(memchr(buffer, 0, count));
    output->append(buffer, end ? static_cast<size_t>(end - buffer) : count);
    if (end)
      return true;
    offset += count;
  }
  return false;
}

std::string Link(const std::string& path) {
  char bytes[4096];
  const ssize_t size = readlink(path.c_str(), bytes, sizeof(bytes));
  return size < 0 || size == sizeof(bytes) ? "" : std::string(bytes, size);
}

std::string Canonical(std::string path) {
  std::string suffix;
  char resolved[4096];
  while (!realpath(path.c_str(), resolved)) {
    if (errno != ENOENT)
      return "";
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash + 1 == path.size())
      return "";
    const std::string component = path.substr(slash + 1);
    if (component == ".." || component == ".")
      return "";
    suffix = "/" + component + suffix;
    path.resize(slash ? slash : 1);
  }
  return std::string(resolved) + suffix;
}

struct Observer {
  std::string cache;
  std::string caller_input;
  std::set<std::string> temporary;
  std::set<pid_t> children;

  bool Input(pid_t pid, int fd) const {
    const std::string path =
        Link("/proc/" + std::to_string(pid) + "/fd/" + std::to_string(fd));
    return path == "/dev/null" || (!path.empty() && path != caller_input);
  }

  bool ReadOnlyPath(const std::string& path) const {
    if (path.empty())
      return false;
    struct stat metadata;
    if (stat(path.c_str(), &metadata) != 0)
      return errno == ENOENT || errno == ENOTDIR;
    return S_ISREG(metadata.st_mode) || S_ISDIR(metadata.st_mode) ||
           path == "/dev/null" || path == "/dev/zero" ||
           path == "/dev/random" || path == "/dev/urandom";
  }

  bool Allowed(const std::string& path) const {
    if (path == "/dev/null" || path == cache ||
        path.compare(0, cache.size() + 1, cache + "/") == 0)
      return true;
    for (const auto& root : temporary)
      if (path == root || path.compare(0, root.size() + 1, root + "/") == 0)
        return true;
    return false;
  }

  std::string Path(pid_t pid,
                   int fd,
                   uint64_t address,
                   bool follow = true) const {
    std::string path;
    if (!ReadString(pid, address, &path) || path.empty())
      return "";
    const std::string proc = "/proc/" + std::to_string(pid);
    if (path.front() != '/') {
      const std::string base =
          Link(proc + (fd == AT_FDCWD ? "/cwd" : "/fd/" + std::to_string(fd)));
      if (base.empty())
        return "";
      path = base + "/" + path;
    }
    if (path.compare(0, 11, "/proc/self/") == 0)
      path = proc + path.substr(10);
    if (follow)
      return Canonical(path);
    const size_t slash = path.rfind('/');
    const std::string parent = Canonical(path.substr(0, slash ? slash : 1));
    return parent.empty() ? "" : parent + "/" + path.substr(slash + 1);
  }

  bool File(pid_t pid,
            int fd,
            uint64_t address,
            bool inode_mutation = true,
            bool follow = true) const {
    return AllowedFile(Path(pid, fd, address, inode_mutation && follow),
                       inode_mutation);
  }

  bool AllowedFile(const std::string& path, bool inode_mutation = true) const {
    if (path.empty() || !Allowed(path)) {
      LOG_STAT("import observation rejected path: %s", path.c_str());
      return false;
    }
    struct stat metadata;
    // A cache-local hard link must not turn an allowed write into a mutation
    // of another invocation's output or an arbitrary external file.
    return !inode_mutation || lstat(path.c_str(), &metadata) != 0 ||
           S_ISDIR(metadata.st_mode) || metadata.st_nlink == 1;
  }

  bool Open(pid_t pid, int fd, uint64_t address, uint64_t flags) const {
    const std::string path = Path(pid, fd, address);
    if (path == "/dev/tty") {
      // Bash probes the controlling terminal even for a noninteractive shell.
      // The real tty device cannot open when this task has no controlling
      // terminal. Permit that guaranteed failure, never access to a live tty.
      struct stat metadata;
      if (stat(path.c_str(), &metadata) == 0 && S_ISCHR(metadata.st_mode) &&
          major(metadata.st_rdev) == 5 && minor(metadata.st_rdev) == 0) {
        std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
        std::string status;
        std::getline(input, status);
        const size_t end = status.rfind(')');
        char state;
        long parent, group, session, terminal;
        if (end != std::string::npos &&
            sscanf(status.c_str() + end + 1, " %c %ld %ld %ld %ld", &state,
                   &parent, &group, &session, &terminal) == 5 &&
            terminal == 0)
          return true;
      }
    }
    const bool writable = (flags & O_ACCMODE) != O_RDONLY ||
                          (flags & (O_CREAT | O_TRUNC)) ||
                          (flags & O_TMPFILE) == O_TMPFILE;
    return writable ? AllowedFile(path) : ReadOnlyPath(path);
  }

  bool Descriptor(pid_t pid, int fd) const {
    std::string path =
        Link("/proc/" + std::to_string(pid) + "/fd/" + std::to_string(fd));
    if (path.compare(0, 6, "pipe:[") == 0 ||
        path.compare(0, 11, "anon_inode:") == 0)
      return true;
    const std::string deleted = " (deleted)";
    if (path.size() >= deleted.size() &&
        path.compare(path.size() - deleted.size(), deleted.size(), deleted) ==
            0)
      path.resize(path.size() - deleted.size());
    if (!Allowed(Canonical(path))) {
      LOG_STAT("import observation rejected descriptor: %s", path.c_str());
      return false;
    }
    struct stat metadata;
    const std::string descriptor =
        "/proc/" + std::to_string(pid) + "/fd/" + std::to_string(fd);
    return stat(descriptor.c_str(), &metadata) != 0 ||
           !S_ISREG(metadata.st_mode) || metadata.st_nlink <= 1;
  }

  bool ReadOnlyDescriptor(pid_t pid, int fd) const {
    std::ifstream info("/proc/" + std::to_string(pid) + "/fdinfo/" +
                       std::to_string(fd));
    std::string line;
    while (std::getline(info, line)) {
      unsigned long flags;
      if (sscanf(line.c_str(), "flags: %lo", &flags) == 1)
        return (flags & O_ACCMODE) == O_RDONLY &&
               ReadOnlyPath(Link("/proc/" + std::to_string(pid) + "/fd/" +
                                 std::to_string(fd)));
    }
    return false;
  }

  bool Directory(pid_t pid, int fd, uint64_t address) {
    const std::string path = Path(pid, fd, address);
    if (path.empty())
      return false;
    struct stat metadata;
    if (lstat(path.c_str(), &metadata) == 0)
      return true;  // mkdir of an existing path has no filesystem effect.
    if (Allowed(path))
      return true;
    // Kbuild compiler probes use fresh .tmp_<shell-pid> directories in the
    // object tree. Observe and remove those transient probe directories too.
    const std::string cwd =
        Canonical(Link("/proc/" + std::to_string(pid) + "/cwd"));
    const std::string prefix = cwd + "/.tmp_";
    if (path.compare(0, prefix.size(), prefix) != 0 ||
        path.find('/', prefix.size()) != std::string::npos) {
      LOG_STAT("import observation rejected directory: %s (pid %d, exe %s)",
               path.c_str(), pid,
               Link("/proc/" + std::to_string(pid) + "/exe").c_str());
      return false;
    }
    const std::string owner = path.substr(prefix.size());
    if (owner.empty() ||
        owner.find_first_not_of("0123456789") != std::string::npos ||
        !children.count(strtol(owner.c_str(), nullptr, 10)))
      return false;
    temporary.insert(path);
    return true;
  }

  bool Syscall(pid_t pid, const ImportSyscall& call) {
    const auto* a = call.entry.args;
    auto file = [&](int fd, uint64_t path) { return File(pid, fd, path); };
    auto entry = [&](int fd, uint64_t path) {
      return File(pid, fd, path, false);
    };
    auto output = [&](uint64_t fd) {
      return Descriptor(pid, static_cast<int>(fd));
    };
    switch (call.entry.nr) {
#ifdef SYS_open
      case SYS_open:
        return Open(pid, AT_FDCWD, a[0], a[1]);
#endif
      case SYS_openat:
        return Open(pid, a[0], a[1], a[2]);
#ifdef SYS_openat2
      case SYS_openat2: {
        uint64_t flags;
        return ReadMemory(pid, a[2], &flags, sizeof(flags)) &&
               Open(pid, a[0], a[1], flags);
      }
#endif
#ifdef SYS_creat
      case SYS_creat:
        return file(AT_FDCWD, a[0]);
#endif
#ifdef SYS_mkdir
      case SYS_mkdir:
        return Directory(pid, AT_FDCWD, a[0]);
#endif
      case SYS_mkdirat:
        return Directory(pid, a[0], a[1]);
#ifdef SYS_unlink
      case SYS_unlink:
      case SYS_rmdir:
        return entry(AT_FDCWD, a[0]);
      case SYS_truncate:
      case SYS_chmod:
      case SYS_chown:
      case SYS_utime:
      case SYS_utimes:
        return file(AT_FDCWD, a[0]);
      case SYS_lchown:
        return File(pid, AT_FDCWD, a[0], true, false);
#endif
      case SYS_unlinkat:
        return entry(a[0], a[1]);
      case SYS_fchmodat:
      case SYS_futimesat:
        return file(a[0], a[1]);
      case SYS_fchownat:
        return File(pid, a[0], a[1], true, !(a[4] & AT_SYMLINK_NOFOLLOW));
      case SYS_utimensat:
        return a[1] ? File(pid, a[0], a[1], true, !(a[3] & AT_SYMLINK_NOFOLLOW))
                    : output(a[0]);
#ifdef SYS_fchmodat2
      case SYS_fchmodat2:
        return File(pid, a[0], a[1], true, !(a[3] & AT_SYMLINK_NOFOLLOW));
#endif
#ifdef SYS_rename
      case SYS_rename:
      case SYS_link:
        return entry(AT_FDCWD, a[0]) && entry(AT_FDCWD, a[1]);
      case SYS_symlink:
        return entry(AT_FDCWD, a[1]);
#endif
      case SYS_renameat:
      case SYS_renameat2:
      case SYS_linkat:
        return entry(a[0], a[1]) && entry(a[2], a[3]);
      case SYS_symlinkat:
        return entry(a[1], a[2]);
      case SYS_write:
      case SYS_writev:
      case SYS_pwrite64:
      case SYS_pwritev:
      case SYS_pwritev2:
      case SYS_ftruncate:
      case SYS_fchmod:
      case SYS_fchown:
      case SYS_fallocate:
      case SYS_flock:
        return output(a[0]);
      case SYS_read:
      case SYS_getdents:
      case SYS_getdents64:
      case SYS_readv:
      case SYS_pread64:
      case SYS_preadv:
      case SYS_preadv2:
        return Input(pid, a[0]);
      case SYS_dup:
        return Input(pid, a[0]);
      case SYS_dup2:
      case SYS_dup3:
        return a[0] == a[1] || Input(pid, a[0]);
#ifdef SYS_pidfd_open
      case SYS_pidfd_open:
        // The process supervisor polls its own child's exit after stdout
        // closes. Opening that handle neither consumes input nor signals it.
        return a[1] == 0 && children.count(a[0]);
#endif
      case SYS_lseek:
        return (a[1] == 0 && a[2] == SEEK_CUR) || Input(pid, a[0]);
      case SYS_sendfile:
        return Input(pid, a[1]) && output(a[0]);
      case SYS_splice:
      case SYS_copy_file_range:
        return Input(pid, a[0]) && output(a[2]);
      case SYS_mmap:
        // A shared read-only mapping is safe only with a read-only descriptor:
        // the kernel then rejects mprotect attempts to make it writable.
        return !(a[3] & MAP_SHARED) || (a[3] & MAP_ANONYMOUS) ||
               (!(a[2] & PROT_WRITE) && ReadOnlyDescriptor(pid, a[4])) ||
               output(a[4]);
      case SYS_ioctl:
        // TCGETS2 is the read-only extended termios query used by newer Bash.
        // Its x86_64 kernel ABI value avoids requiring Linux UAPI headers.
        return a[1] == TCGETS || a[1] == 0x802c542a || a[1] == TIOCGWINSZ ||
               a[1] == TIOCGPGRP || a[1] == FIONREAD || a[1] == FIOCLEX ||
               a[1] == FIONCLEX;
      case SYS_prctl:
        return a[0] == PR_GET_NO_NEW_PRIVS || a[0] == PR_GET_NAME ||
               a[0] == PR_SET_NAME || a[0] == PR_GET_DUMPABLE;
      case SYS_prlimit64:
        return a[0] == 0 || a[0] == static_cast<uint64_t>(pid) || !a[2];
      case SYS_fcntl:
        if (a[1] == F_SETFL || a[1] == F_DUPFD || a[1] == F_DUPFD_CLOEXEC)
          return Input(pid, a[0]);
        return a[1] == F_GETFD || a[1] == F_GETFL || a[1] == F_SETFD ||
               output(a[0]);
      case SYS_umask:
      case SYS_setpgid:
      case SYS_setsid:
      case SYS_getpgrp:
      case SYS_getpgid:
      case SYS_getsid:
      case SYS_close_range:
        return true;
      case SYS_clone:
        return !(a[0] & CLONE_UNTRACED);
#ifdef SYS_clone3
      case SYS_clone3: {
        uint64_t flags;
        return ReadMemory(pid, a[0], &flags, sizeof(flags)) &&
               !(flags & CLONE_UNTRACED);
      }
#endif
      case SYS_kill:
        return a[1] == 0 || children.count(a[0]);
      case SYS_tgkill:
        return children.count(a[0]) && children.count(a[1]);
      case SYS_tkill:
        return children.count(a[0]);
      case SYS_socket:
      case SYS_connect:
      case SYS_bind:
      case SYS_sendto:
      case SYS_sendmsg:
      case SYS_sendmmsg:
      case SYS_ptrace:
      case SYS_process_vm_writev:
      case SYS_mount:
      case SYS_umount2:
      case SYS_pivot_root:
      case SYS_chroot:
      case SYS_unshare:
      case SYS_setns:
      case SYS_setxattr:
      case SYS_lsetxattr:
      case SYS_fsetxattr:
      case SYS_removexattr:
      case SYS_lremovexattr:
      case SYS_fremovexattr:
      case SYS_mknodat:
      case SYS_reboot:
      case SYS_swapon:
      case SYS_swapoff:
      case SYS_init_module:
      case SYS_finit_module:
      case SYS_delete_module:
      case SYS_bpf:
      case SYS_perf_event_open:
      case SYS_open_by_handle_at:
      case SYS_io_setup:
      case SYS_io_submit:
      case SYS_semget:
      case SYS_semctl:
      case SYS_semop:
      case SYS_shmget:
      case SYS_shmctl:
      case SYS_shmat:
      case SYS_msgget:
      case SYS_msgsnd:
      case SYS_msgctl:
#ifdef SYS_io_uring_setup
      case SYS_io_uring_setup:
      case SYS_io_uring_enter:
#endif
        return false;
      default:
        return false;
    }
  }
};
}  // namespace

bool BeginImportObservation() {
  // Discovery must not retain writable descriptors belonging to its caller.
#ifdef SYS_close_range
  if (syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
    return false;
#else
  return false;
#endif
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
    return false;
  return raise(SIGSTOP) == 0 && InstallFilter();
}

bool ImportObservationActive() {
  if (!getenv("KATI_IMPORT_OBSERVED") ||
      prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
    return false;
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line))
    if (line.compare(0, 10, "TracerPid:") == 0)
      return strtol(line.c_str() + 10, nullptr, 10) ==
             strtol(getenv("KATI_IMPORT_OBSERVED"), nullptr, 10);
  return false;
}

int ObserveImport(pid_t child, const std::string& directory) {
  Observer observer;
  observer.cache = Canonical(directory);
  observer.caller_input = Link("/proc/" + std::to_string(child) + "/fd/0");
  observer.children.insert(child);
  int status = 0;
  pid_t initial;
  do {
    initial = waitpid(child, &status, 0);
  } while (initial < 0 && errno == EINTR);
  bool valid =
      !observer.cache.empty() && initial == child && WIFSTOPPED(status);
  if (initial == child && (WIFEXITED(status) || WIFSIGNALED(status)))
    observer.children.erase(child);
  int result = 3;
  sigset_t blocked, original;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGCHLD);
  const bool masked = sigprocmask(SIG_BLOCK, &blocked, &original) == 0;
  const int notifications =
      masked ? signalfd(-1, &blocked, SFD_CLOEXEC | SFD_NONBLOCK) : -1;
  valid = valid && notifications >= 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  if (valid) {
    valid = ptrace(PTRACE_SETOPTIONS, child, 0,
                   PTRACE_O_EXITKILL | PTRACE_O_TRACESECCOMP |
                       PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                       PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC) == 0 &&
            ptrace(PTRACE_CONT, child, 0, 0) == 0;
  }
  while (valid && !observer.children.empty()) {
    if (GetBuildSignal() || std::chrono::steady_clock::now() >= deadline) {
      valid = false;
      break;
    }
    const pid_t pid = waitpid(-1, &status, __WALL | WNOHANG);
    if (pid == 0) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now())
              .count();
      if (remaining <= 0) {
        valid = false;
        break;
      }
      pollfd descriptor{notifications, POLLIN, 0};
      if (poll(&descriptor, 1, static_cast<int>(remaining)) < 0 &&
          errno != EINTR) {
        valid = false;
        break;
      }
      signalfd_siginfo notification;
      while (read(notifications, &notification, sizeof(notification)) > 0) {
      }
      continue;
    }
    if (pid < 0) {
      if (errno == EINTR)
        continue;
      valid = false;
      break;
    }
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
      observer.children.erase(pid);
      if (pid == child)
        result = WIFEXITED(status) ? WEXITSTATUS(status) : 3;
      continue;
    }
    if (!WIFSTOPPED(status)) {
      valid = false;
      break;
    }
    int signal = WSTOPSIG(status);
    const unsigned event = static_cast<unsigned>(status) >> 16;
    if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK ||
        event == PTRACE_EVENT_CLONE) {
      unsigned long descendant;
      if (ptrace(PTRACE_GETEVENTMSG, pid, 0, &descendant) != 0) {
        valid = false;
        break;
      }
      observer.children.insert(descendant);
      signal = 0;
    } else if (event == PTRACE_EVENT_SECCOMP) {
      ImportSyscall call = {};
      if (ptrace(PTRACE_GET_SYSCALL_INFO, pid, sizeof(call), &call) < 0) {
        valid = false;
        break;
      }
      if (call.op == 3) {
        const unsigned architecture = 0xc000003e;
        if (call.arch != architecture || (call.entry.nr & 0x40000000) ||
            !observer.Syscall(pid, call)) {
          LOG_STAT("import observation rejected syscall %llu (pid %d)",
                   static_cast<unsigned long long>(call.entry.nr), pid);
          valid = false;
          break;
        }
      } else {
        valid = false;
        break;
      }
      signal = 0;
    } else if (event == PTRACE_EVENT_EXEC) {
      unsigned long former;
      if (ptrace(PTRACE_GETEVENTMSG, pid, 0, &former) != 0) {
        valid = false;
        break;
      }
      if (static_cast<pid_t>(former) != pid)
        observer.children.erase(static_cast<pid_t>(former));
      observer.children.insert(pid);
      signal = 0;
    } else if (signal == SIGSTOP) {
      signal = 0;
    }
    if (ptrace(PTRACE_CONT, pid, 0, signal) != 0 && errno != ESRCH)
      valid = false;
  }
  for (pid_t pid : observer.children)
    kill(pid, SIGKILL);
  while (!observer.children.empty()) {
    const pid_t pid = waitpid(-1, &status, __WALL);
    if (pid < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (WIFEXITED(status) || WIFSIGNALED(status))
      observer.children.erase(pid);
    else if (WIFSTOPPED(status))
      ptrace(PTRACE_CONT, pid, 0, SIGKILL);
  }
  for (const auto& path : observer.temporary) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  if (notifications >= 0)
    close(notifications);
  if (masked)
    sigprocmask(SIG_SETMASK, &original, nullptr);
  return valid ? result : 3;
}
#else
bool BeginImportObservation() {
  return false;
}
bool ImportObservationActive() {
  return false;
}
int ObserveImport(pid_t, const std::string&) {
  return 3;
}
#endif
