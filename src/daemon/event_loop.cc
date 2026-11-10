// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/event_loop.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <utility>

namespace lgrtk::daemon {
namespace {

int64_t NowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string Errno(const std::string& what) {
  return what + ": " + std::strerror(errno);  // NOLINT(concurrency-mt-unsafe)
}

}  // namespace

// ---- EventLoop ----

EventLoop::~EventLoop() {
  if (wake_fd_ >= 0) {
    close(wake_fd_);
  }
}

bool EventLoop::Init(std::string* err) {
  wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd_ < 0) {
    *err = Errno("eventfd");
    return false;
  }
  Add(wake_fd_, POLLIN, [this](short) {
    uint64_t v = 0;
    while (read(wake_fd_, &v, sizeof v) == sizeof v) {
    }
    DrainPosted();
  });
  return true;
}

void EventLoop::Add(int fd, short events, std::function<void(short)> cb) {
  watches_[fd] = Watch{events, ++gen_, std::move(cb)};
}

void EventLoop::Modify(int fd, short events) {
  auto it = watches_.find(fd);
  if (it != watches_.end()) {
    it->second.events = events;
  }
}

void EventLoop::Remove(int fd) { watches_.erase(fd); }

void EventLoop::Post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    posted_.push_back(std::move(fn));
  }
  Wake();
}

void EventLoop::Every(int64_t period_ms, std::function<void()> fn) {
  timers_.push_back(Timer{period_ms, NowMs() + period_ms, std::move(fn)});
}

void EventLoop::Quit() {
  quit_ = true;
  Wake();
}

void EventLoop::Wake() {
  const uint64_t one = 1;
  if (wake_fd_ >= 0) {
    (void)!write(wake_fd_, &one, sizeof one);
  }
}

void EventLoop::DrainPosted() {
  std::vector<std::function<void()>> todo;
  {
    std::lock_guard<std::mutex> lock(mu_);
    todo.swap(posted_);
  }
  for (auto& fn : todo) {
    fn();
  }
}

void EventLoop::Run() {
  std::vector<pollfd> fds;
  std::vector<uint64_t> gens;
  while (!quit_) {
    fds.clear();
    gens.clear();
    for (const auto& [fd, w] : watches_) {
      fds.push_back(pollfd{fd, w.events, 0});
      gens.push_back(w.gen);
    }
    int timeout = -1;
    const int64_t now = NowMs();
    for (const auto& t : timers_) {
      const int64_t wait = t.next_ms > now ? t.next_ms - now : 0;
      if (timeout < 0 || wait < timeout) {
        timeout = static_cast<int>(wait);
      }
    }
    const int n = poll(fds.data(), fds.size(), timeout);
    if (n < 0 && errno != EINTR) {
      break;
    }
    for (size_t i = 0; i < fds.size() && n > 0; ++i) {
      if (fds[i].revents == 0) {
        continue;
      }
      // The watch may have been removed or replaced by an earlier callback.
      auto it = watches_.find(fds[i].fd);
      if (it == watches_.end() || it->second.gen != gens[i]) {
        continue;
      }
      auto cb = it->second.cb;  // copy: the callback may Remove() itself
      cb(fds[i].revents);
    }
    const int64_t after = NowMs();
    for (size_t i = 0; i < timers_.size(); ++i) {
      if (timers_[i].next_ms <= after) {
        timers_[i].next_ms = after + timers_[i].period_ms;
        auto fn = timers_[i].fn;
        fn();
      }
    }
  }
  DrainPosted();
}

// ---- sockets ----

int ListenTcp(const std::string& host, int port, std::string* err) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST | AI_NUMERICSERV;
  addrinfo* res = nullptr;
  const std::string service = std::to_string(port);
  if (const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
      rc != 0) {
    *err = "listen " + host + ":" + service + ": " + gai_strerror(rc);
    return -1;
  }
  const int fd = socket(res->ai_family,
                        res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *err = Errno("socket");
    freeaddrinfo(res);
    return -1;
  }
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(fd, res->ai_addr, res->ai_addrlen) != 0 || listen(fd, 16) != 0) {
    *err = Errno("listen " + host + ":" + service);
    freeaddrinfo(res);
    close(fd);
    return -1;
  }
  freeaddrinfo(res);
  return fd;
}

int ListenUnix(const std::string& path, std::string* err) {
  sockaddr_un a{};
  if (path.empty() || path.size() >= sizeof a.sun_path) {
    *err = "unix socket path too long or empty: " + path;
    return -1;
  }
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  struct stat st{};
  if (lstat(path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) {
      *err = path + " exists and is not a socket";
      return -1;
    }
    unlink(path.c_str());  // stale socket of a previous run
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    *err = Errno("socket");
    return -1;
  }
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
      listen(fd, 8) != 0) {
    *err = Errno("listen " + path);
    close(fd);
    return -1;
  }
  chmod(path.c_str(), 0660);
  return fd;
}

int BoundPort(int fd) {
  sockaddr_storage a{};
  socklen_t len = sizeof a;
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
    return 0;
  }
  if (a.ss_family == AF_INET) {
    return ntohs(reinterpret_cast<sockaddr_in*>(&a)->sin_port);
  }
  if (a.ss_family == AF_INET6) {
    return ntohs(reinterpret_cast<sockaddr_in6*>(&a)->sin6_port);
  }
  return 0;
}

// ---- ClientSet ----

ClientSet::ClientSet(EventLoop* loop, Handlers handlers, size_t cap_bytes,
                     bool read_lines, size_t max_line, size_t max_clients)
    : loop_(loop),
      h_(std::move(handlers)),
      cap_(cap_bytes),
      read_lines_(read_lines),
      max_line_(max_line),
      max_clients_(max_clients) {}

ClientSet::~ClientSet() {
  for (int fd : listeners_) {
    loop_->Remove(fd);
    close(fd);
  }
  for (auto& [id, c] : clients_) {
    loop_->Remove(c->fd);
    close(c->fd);
  }
}

void ClientSet::AddListener(int listen_fd) {
  listeners_.push_back(listen_fd);
  loop_->Add(listen_fd, POLLIN,
             [this, listen_fd](short) { Accept(listen_fd); });
}

void ClientSet::Accept(int listen_fd) {
  for (;;) {
    const int fd =
        accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
          errno == ENOMEM) {
        // Out of descriptors: the listener stays readable, so stop watching
        // it (no busy loop) until a client leaves.
        accept_paused_ = true;
        for (const int l : listeners_) {
          loop_->Modify(l, 0);
        }
      }
      return;  // EAGAIN, or a transient error: try on the next event
    }
    if (clients_.size() >= max_clients_) {
      close(fd);
      continue;
    }
    const Id id = next_id_++;
    clients_[id] = std::make_unique<Client>(Client{fd, {}, {}});
    loop_->Add(fd, POLLIN, [this, id](short re) { OnEvents(id, re); });
    if (h_.on_open) {
      h_.on_open(id);
    }
  }
}

void ClientSet::OnEvents(Id id, short revents) {
  auto it = clients_.find(id);
  if (it == clients_.end()) {
    return;
  }
  Client* c = it->second.get();
  if ((revents & POLLOUT) != 0) {
    Flush(id, c);
    if (!clients_.contains(id)) {
      return;
    }
  }
  if ((revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
    return;
  }
  char buf[4096];
  const ssize_t n = recv(c->fd, buf, sizeof buf, 0);
  if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
    Close(id);
    return;
  }
  if (n < 0 || !read_lines_) {
    return;
  }
  c->in.append(buf, static_cast<size_t>(n));
  size_t start = 0;
  for (;;) {
    const size_t nl = c->in.find('\n', start);
    if (nl == std::string::npos) {
      break;
    }
    std::string line = c->in.substr(start, nl - start);
    start = nl + 1;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (h_.on_line) {
      h_.on_line(id, line);
    }
    // The handler may have closed the client.
    auto again = clients_.find(id);
    if (again == clients_.end()) {
      return;
    }
    c = again->second.get();
  }
  c->in.erase(0, start);
  if (c->in.size() > max_line_) {
    Close(id);
  }
}

void ClientSet::Send(Id id, std::string_view data) {
  auto it = clients_.find(id);
  if (it == clients_.end() || data.empty()) {
    return;
  }
  Client* c = it->second.get();
  if (c->out.size() + data.size() > cap_) {
    ++dropped_;
    Close(id);
    return;
  }
  const bool was_empty = c->out.empty();
  c->out.append(data);
  if (was_empty) {
    Flush(id, c);
  }
}

void ClientSet::Flush(Id id, Client* c) {
  while (!c->out.empty()) {
    const ssize_t n = send(c->fd, c->out.data(), c->out.size(), MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      Close(id);
      return;
    }
    c->out.erase(0, static_cast<size_t>(n));
  }
  loop_->Modify(c->fd, c->out.empty() ? POLLIN : POLLIN | POLLOUT);
}

void ClientSet::Close(Id id) {
  auto it = clients_.find(id);
  if (it == clients_.end()) {
    return;
  }
  loop_->Remove(it->second->fd);
  close(it->second->fd);
  clients_.erase(it);
  if (accept_paused_) {
    accept_paused_ = false;
    for (const int l : listeners_) {
      loop_->Modify(l, POLLIN);
    }
  }
  if (h_.on_close) {
    h_.on_close(id);
  }
}

std::vector<ClientSet::Id> ClientSet::Ids() const {
  std::vector<Id> ids;
  ids.reserve(clients_.size());
  for (const auto& [id, c] : clients_) {
    ids.push_back(id);
  }
  return ids;
}

}  // namespace lgrtk::daemon
