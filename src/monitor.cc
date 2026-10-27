// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "monitor.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "fileutil.h"
#include "flags.h"
#include "graph_optimization.h"

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>

#include <cerrno>
#include <map>
#include <set>

namespace {
constexpr char kSocket[] = ".openmktr-monitor.sock";
constexpr size_t kLimit = 16 * 1024 * 1024;

bool Transfer(int fd, void* buffer, size_t length, bool send_data) {
  char* bytes = static_cast<char*>(buffer);
  while (length) {
    ssize_t n = send_data ? send(fd, bytes, length, MSG_NOSIGNAL)
                          : recv(fd, bytes, length, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    bytes += n;
    length -= n;
  }
  return true;
}

bool Send(int fd, const std::string& data) {
  uint32_t size = data.size();
  return size <= kLimit && Transfer(fd, &size, sizeof(size), true) &&
         Transfer(fd, const_cast<char*>(data.data()), size, true);
}

bool Receive(int fd, std::string* data) {
  uint32_t size;
  if (!Transfer(fd, &size, sizeof(size), false) || size > kLimit)
    return false;
  data->resize(size);
  return Transfer(fd, data->data(), size, false);
}

int Connect() {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  timeval timeout{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  strcpy(address.sun_path, kSocket);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

bool Request(const std::string& request, std::string* response) {
  int fd = Connect();
  if (fd < 0)
    return false;
  bool ok = Send(fd, request) && Receive(fd, response);
  close(fd);
  return ok;
}

class Server {
 public:
  bool Start() {
    char root[PATH_MAX];
    if (!getcwd(root, sizeof(root)))
      return false;
    root_ = root;
    timespec started;
    if (clock_gettime(CLOCK_MONOTONIC, &started) < 0)
      return false;
    instance_ = std::to_string(getpid()) + ":" +
                std::to_string(started.tv_sec) + ":" +
                std::to_string(started.tv_nsec);
    lock_ = open(".openmktr-monitor.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (lock_ < 0 || fcntl(lock_, F_SETLK, &lock) < 0)
      return false;
    events_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (events_ < 0 || !Watch(root_))
      return false;
    Drain();
    if (uncertain_)
      return false;
    socket_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, kSocket);
    unlink(kSocket);
    if (socket_ < 0 ||
        bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
            0 ||
        chmod(kSocket, 0600) < 0 || listen(socket_, 8) < 0)
      return false;
    return true;
  }

  void Run() {
    bool stop = false;
    while (!stop) {
      pollfd waiters[] = {{events_, POLLIN, 0}, {socket_, POLLIN, 0}};
      if (poll(waiters, 2, -1) < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      Drain();
      if (!(waiters[1].revents & POLLIN))
        continue;
      int fd = accept4(socket_, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0)
        continue;
      timeval timeout{2, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      std::string request, response = "fallback";
      if (Receive(fd, &request)) {
        Drain();
        if (request == "status") {
          response =
              uncertain_ ? "running; validation fallback required" : "ready";
          response += " pid=" + std::to_string(getpid());
        } else if (request == "stop") {
          response = "stopped";
          stop = true;
        } else if (request.compare(0, 11, "checkpoint:") == 0) {
          if (!uncertain_ && request.substr(11) == Checkpoint())
            response = "stable";
        } else if (!uncertain_) {
          const uint64_t sequence = sequence_;
          response =
              request == cached_request_ ? cached_response_ : Snapshot(request);
          Drain();
          if (uncertain_ || sequence != sequence_) {
            response = "fallback";
          } else if (response != "fallback") {
            cached_request_ = request;
            cached_response_ = response;
          }
        }
        Send(fd, response);
      }
      close(fd);
    }
    unlink(kSocket);
  }

 private:
  std::string Checkpoint() const {
    return instance_ + ':' + std::to_string(sequence_);
  }

  bool Watch(const std::string& directory) {
    if (directories_.count(directory))
      return true;
    if (directories_.size() >= 100000)
      return false;
    int wd = inotify_add_watch(events_, directory.c_str(),
                               IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                                   IN_MOVED_TO | IN_ATTRIB | IN_MODIFY |
                                   IN_CLOSE_WRITE | IN_DELETE_SELF |
                                   IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW);
    if (wd < 0)
      return false;
    directories_.insert(directory);
    watches_[wd] = directory;
    DIR* stream = opendir(directory.c_str());
    if (!stream)
      return false;
    bool ok = true;
    while (dirent* entry = readdir(stream)) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
          !strcmp(entry->d_name, ".git"))
        continue;
      std::string path = directory + '/' + entry->d_name;
      struct stat st;
      if (lstat(path.c_str(), &st) < 0) {
        ok = false;
        break;
      }
      if (S_ISDIR(st.st_mode) && !Watch(path)) {
        ok = false;
        break;
      }
    }
    closedir(stream);
    return ok;
  }

  void Drain() {
    alignas(inotify_event) char buffer[65536];
    for (;;) {
      ssize_t n = read(events_, buffer, sizeof(buffer));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        if (n < 0 && errno != EAGAIN)
          uncertain_ = true;
        return;
      }
      ++sequence_;
      cached_request_.clear();
      cached_response_.clear();
      for (size_t offset = 0; offset < static_cast<size_t>(n);) {
        const auto* event =
            reinterpret_cast<const inotify_event*>(buffer + offset);
        offset += sizeof(*event) + event->len;
        if (event->mask & (IN_Q_OVERFLOW | IN_UNMOUNT | IN_IGNORED |
                           IN_DELETE_SELF | IN_MOVE_SELF))
          uncertain_ = true;
        if ((event->mask & IN_ISDIR) &&
            (event->mask & (IN_CREATE | IN_MOVED_TO))) {
          auto found = watches_.find(event->wd);
          if (found == watches_.end() ||
              !Watch(found->second + '/' + event->name))
            uncertain_ = true;
        }
      }
    }
  }

  std::string Snapshot(const std::string& request) {
    std::string_view view(request);
    std::string path, response;
    AppendGraphString("snapshot2", &response);
    AppendGraphString(Checkpoint(), &response);
    while (!view.empty()) {
      if (!ConsumeGraphString(&view, &path) ||
          path.find('\0') != std::string::npos)
        return "fallback";
      std::string absolute =
          path.empty() || path.front() != '/' ? root_ + '/' + path : path;
      char resolved[PATH_MAX];
      if (!realpath(absolute.c_str(), resolved))
        return "fallback";
      // A symlink or .. may cross uncovered namespaces; ordinary validation
      // remains authoritative for those paths, external files and hard links.
      if (absolute != resolved ||
          absolute.compare(0, root_.size() + 1, root_ + '/') != 0)
        return "fallback";
      const auto slash = absolute.rfind('/');
      if (!directories_.count(absolute.substr(0, slash)))
        return "fallback";
      struct stat st;
      struct statfs fs;
      if (lstat(absolute.c_str(), &st) < 0 || !S_ISREG(st.st_mode) ||
          st.st_nlink != 1 || statfs(absolute.c_str(), &fs) < 0 ||
          (fs.f_type != 0xef53 && fs.f_type != 0x58465342 &&
           fs.f_type != 0x9123683e && fs.f_type != 0x01021994))
        return "fallback";
      // Inode watches also catch writes through aliases outside this tree.
      if (inotify_add_watch(events_, absolute.c_str(),
                            IN_ATTRIB | IN_MODIFY | IN_CLOSE_WRITE |
                                IN_DELETE_SELF | IN_MOVE_SELF |
                                IN_DONT_FOLLOW) < 0)
        return "fallback";
      AppendGraphString(GetFileIdentity(absolute), &response);
      char time[64];
      snprintf(time, sizeof(time), "%.17g", GetTimestampFromStat(st));
      AppendGraphString(time, &response);
      if (response.size() > kLimit)
        return "fallback";
    }
    return response;
  }

  int events_ = -1, socket_ = -1, lock_ = -1;
  bool uncertain_ = false;
  uint64_t sequence_ = 0;
  std::string root_, instance_, cached_request_, cached_response_;
  std::map<int, std::string> watches_;
  std::set<std::string> directories_;
};
}  // namespace

int MonitorCommand(const std::string& action) {
  std::string response;
  if (Request(action == "start" ? "status" : action, &response)) {
    printf("monitor: %s\n", response.c_str());
    return 0;
  }
  if (action != "start") {
    printf("monitor: stopped\n");
    return action == "stop" ? 0 : 1;
  }
  int ready[2];
  if (pipe(ready) < 0)
    return 1;
  pid_t child = fork();
  if (child == 0) {
    close(ready[0]);
    setsid();
    umask(0077);
    Server server;
    char success = server.Start() ? 1 : 0;
    if (write(ready[1], &success, 1) != 1)
      _exit(1);
    close(ready[1]);
    if (!success)
      _exit(1);
    int null = open("/dev/null", O_RDWR);
    for (int fd : {0, 1, 2})
      dup2(null, fd);
    if (null > 2)
      close(null);
    server.Run();
    _exit(0);
  }
  close(ready[1]);
  char success = 0;
  pollfd waiter{ready[0], POLLIN, 0};
  if (child > 0 && poll(&waiter, 1, 30000) > 0) {
    if (read(ready[0], &success, 1) != 1)
      success = 0;
  }
  close(ready[0]);
  printf("monitor: %s\n", success ? "ready" : "could not establish watches");
  return success ? 0 : 1;
}

bool MonitorSnapshot(const std::vector<std::string>& paths,
                     std::vector<std::string>* identities,
                     std::vector<double>* timestamps,
                     std::string* checkpoint) {
  if (!g_flags.GraphOptimizationEnabled("monitor"))
    return false;
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd)))
    return false;
  const std::string prefix = std::string(cwd) + '/';
  std::vector<size_t> covered;
  identities->assign(paths.size(), "");
  timestamps->assign(paths.size(), -1);
  std::string request, response;
  for (size_t i = 0; i < paths.size(); ++i) {
    const std::string& path = paths[i];
    if (!path.empty() &&
        (path.front() != '/' || path.compare(0, prefix.size(), prefix) == 0)) {
      AppendGraphString(path, &request);
      covered.push_back(i);
    } else {
      (*identities)[i] = GetFileIdentity(path);
      (*timestamps)[i] = GetTimestamp(path);
    }
  }
  if (covered.empty())
    return false;
  if (!Request(request, &response))
    return false;
  std::string_view view(response);
  std::string magic;
  if (!ConsumeGraphString(&view, &magic) || magic != "snapshot2" ||
      !ConsumeGraphString(&view, checkpoint))
    return false;
  for (size_t i : covered) {
    std::string identity, time;
    if (!ConsumeGraphString(&view, &identity) ||
        !ConsumeGraphString(&view, &time))
      return false;
    char* end;
    double stamp = strtod(time.c_str(), &end);
    if (*end)
      return false;
    (*identities)[i] = std::move(identity);
    (*timestamps)[i] = stamp;
  }
  return view.empty();
}

bool MonitorCheckpointValid(const std::string& checkpoint) {
  std::string response;
  return Request("checkpoint:" + checkpoint, &response) && response == "stable";
}
#else
int MonitorCommand(const std::string&) {
  fprintf(stderr, "filesystem monitoring requires Linux\n");
  return 2;
}
bool MonitorSnapshot(const std::vector<std::string>&,
                     std::vector<std::string>*,
                     std::vector<double>*,
                     std::string*) {
  return false;
}
bool MonitorCheckpointValid(const std::string&) {
  return false;
}
#endif
