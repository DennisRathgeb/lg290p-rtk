// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "sim/fake_caster.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "test_util.h"

namespace lgrtk::test {

namespace {

void SendAll(int fd, const void* data, size_t n) {
  const auto* p = static_cast<const char*>(data);
  while (n > 0) {
    const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) return;
    p += w;
    n -= static_cast<size_t>(w);
  }
}

}  // namespace

std::vector<uint8_t> FakeCaster::Frame() {
  std::vector<uint8_t> payload(40, 0x5A);
  payload[0] = 0x43;  // message 1077 header bits (content irrelevant)
  payload[1] = 0x50;
  return Rtcm(payload);
}

FakeCaster::FakeCaster(Mode mode) : mode_(mode) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
      ::listen(listen_fd_, 4) != 0) {
    std::abort();
  }
  socklen_t len = sizeof a;
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len);
  port_ = ntohs(a.sin_port);
  thread_ = std::thread([this] { Loop(); });
}

FakeCaster::~FakeCaster() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  ::close(listen_fd_);
}

std::vector<std::string> FakeCaster::Requests() const {
  std::lock_guard lk(mu_);
  return requests_;
}

std::string FakeCaster::ClientBytes() const {
  std::lock_guard lk(mu_);
  return client_bytes_;
}

void FakeCaster::Loop() {
  while (!stop_) {
    pollfd p{listen_fd_, POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) continue;
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) continue;
    ++connections_;
    Serve(fd);
    ::close(fd);
  }
}

void FakeCaster::Serve(int fd) {
  // Read the request headers.
  std::string req;
  char buf[1024];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (req.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) continue;
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return;
    req.append(buf, static_cast<size_t>(n));
  }
  const size_t end = req.find("\r\n\r\n");
  {
    std::lock_guard lk(mu_);
    requests_.push_back(req.substr(0, end));
    if (end != std::string::npos) client_bytes_ += req.substr(end + 4);
  }
  switch (mode_) {
    case Mode::kUnauthorized: {
      const char r[] = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n";
      SendAll(fd, r, sizeof r - 1);
      return;
    }
    case Mode::kV1Icy: {
      const char r[] = "ICY 200 OK\r\n";
      SendAll(fd, r, sizeof r - 1);
      break;
    }
    default: {
      const char r[] =
          "HTTP/1.1 200 OK\r\nNtrip-Version: Ntrip/2.0\r\nContent-Type: gnss/data\r\n"
          "Transfer-Encoding: chunked\r\n\r\n";
      SendAll(fd, r, sizeof r - 1);
      break;
    }
  }
  const auto frame = Frame();
  const bool chunked = mode_ != Mode::kV1Icy;
  int sent = 0;
  while (!stop_) {
    // Collect what the client sends (GGA).
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 50) > 0) {
      const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) return;
      std::lock_guard lk(mu_);
      client_bytes_.append(buf, static_cast<size_t>(n));
    }
    if (mode_ == Mode::kSilent) continue;
    if (mode_ == Mode::kDropAfterOne && sent == 1) return;
    if (chunked) {
      // Split each frame over two chunks to exercise reassembly.
      const size_t half = frame.size() / 2;
      char head[32];
      std::snprintf(head, sizeof head, "%zx\r\n", half);
      SendAll(fd, head, std::char_traits<char>::length(head));
      SendAll(fd, frame.data(), half);
      SendAll(fd, "\r\n", 2);
      std::snprintf(head, sizeof head, "%zx\r\n", frame.size() - half);
      SendAll(fd, head, std::char_traits<char>::length(head));
      SendAll(fd, frame.data() + half, frame.size() - half);
      SendAll(fd, "\r\n", 2);
    } else {
      SendAll(fd, frame.data(), frame.size());
    }
    ++sent;
  }
}

}  // namespace lgrtk::test
