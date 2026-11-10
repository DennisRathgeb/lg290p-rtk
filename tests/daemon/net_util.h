// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// Test helpers: a line-reading socket client and an event loop on a thread.
#ifndef LGRTK_TESTS_DAEMON_NET_UTIL_H_
#define LGRTK_TESTS_DAEMON_NET_UTIL_H_

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>

#include "daemon/event_loop.h"

namespace lgrtk::test {

/// A blocking client socket with a line reader.
class LineClient {
 public:
  LineClient() = default;
  ~LineClient() { Close(); }
  LineClient(const LineClient&) = delete;
  LineClient& operator=(const LineClient&) = delete;

  bool ConnectTcp(int port, int rcvbuf = 0) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (rcvbuf > 0) {
      setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  }
  bool ConnectUnix(const std::string& path) {
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    return connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
  }
  void Write(const std::string& s) const {
    (void)!send(fd_, s.data(), s.size(), MSG_NOSIGNAL);
  }
  /// Next line without "\r\n"; nullopt on timeout or EOF.
  std::optional<std::string> ReadLine(int timeout_ms = 2000) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    for (;;) {
      if (auto nl = buf_.find('\n'); nl != std::string::npos) {
        std::string line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        return line;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            end - std::chrono::steady_clock::now())
                            .count();
      if (left <= 0) {
        return std::nullopt;
      }
      pollfd p{fd_, POLLIN, 0};
      if (poll(&p, 1, static_cast<int>(left)) <= 0) {
        return std::nullopt;
      }
      char tmp[4096];
      const ssize_t n = recv(fd_, tmp, sizeof tmp, 0);
      if (n <= 0) {
        return std::nullopt;
      }
      buf_.append(tmp, static_cast<size_t>(n));
    }
  }
  /// First line whose text contains `needle`.
  std::optional<std::string> ReadUntil(const std::string& needle,
                                       int timeout_ms = 3000) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
      auto line = ReadLine(timeout_ms);
      if (!line) {
        return std::nullopt;
      }
      if (line->find(needle) != std::string::npos) {
        return line;
      }
    }
    return std::nullopt;
  }
  /// True when the server closed the connection within `timeout_ms`.
  bool ClosedByPeer(int timeout_ms = 3000) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    char tmp[65536];
    while (std::chrono::steady_clock::now() < end) {
      pollfd p{fd_, POLLIN, 0};
      if (poll(&p, 1, 50) > 0) {
        const ssize_t n = recv(fd_, tmp, sizeof tmp, 0);
        if (n == 0 || (n < 0 && errno == ECONNRESET)) {
          return true;
        }
      }
    }
    return false;
  }
  [[nodiscard]] int Fd() const { return fd_; }
  void Close() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
  std::string buf_;
};

/// Runs an EventLoop on its own thread; Sync() executes on it and waits.
class LoopThread {
 public:
  LoopThread() {
    std::string err;
    ok_ = loop_.Init(&err);
    thread_ = std::thread([this] { loop_.Run(); });
  }
  ~LoopThread() { Stop(); }
  LoopThread(const LoopThread&) = delete;
  LoopThread& operator=(const LoopThread&) = delete;

  daemon::EventLoop* Loop() { return &loop_; }
  [[nodiscard]] bool Ok() const { return ok_; }
  void Sync(const std::function<void()>& fn) {
    std::promise<void> done;
    loop_.Post([&] {
      fn();
      done.set_value();
    });
    done.get_future().wait();
  }
  void Stop() {
    if (thread_.joinable()) {
      loop_.Quit();
      thread_.join();
    }
  }

 private:
  daemon::EventLoop loop_;
  bool ok_ = false;
  std::thread thread_;
};

}  // namespace lgrtk::test

#endif  // LGRTK_TESTS_DAEMON_NET_UTIL_H_
