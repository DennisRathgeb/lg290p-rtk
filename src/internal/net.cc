// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "internal/net.h"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <system_error>
#include <utility>

#include "internal/clock.h"

namespace lgrtk::internal {

namespace {

// How long SendAll waits for a peer that does not read.
constexpr int64_t kSendDeadlineMs = 2000;

// Owns a getaddrinfo() result.
struct AddrInfoList {
  addrinfo* head = nullptr;
  AddrInfoList() = default;
  AddrInfoList(const AddrInfoList&) = delete;
  AddrInfoList& operator=(const AddrInfoList&) = delete;
  AddrInfoList(AddrInfoList&&) = delete;
  AddrInfoList& operator=(AddrInfoList&&) = delete;
  ~AddrInfoList() {
    if (head != nullptr) {
      ::freeaddrinfo(head);
    }
  }
};

}  // namespace

UniqueFd Connect(const std::string& host, int port, int timeout_ms,
                 std::string& err, const std::atomic<bool>* stop) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  AddrInfoList res;
  const std::string service = std::to_string(port);
  const int rc =
      ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res.head);
  if (rc != 0) {
    err = "cannot resolve " + host + ": " + gai_strerror(rc);
    return {};
  }
  UniqueFd out;
  for (addrinfo* a = res.head; a != nullptr && !out; a = a->ai_next) {
    UniqueFd s(::socket(a->ai_family,
                        a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                        a->ai_protocol));
    if (!s) {
      continue;
    }
    if (::connect(s.Get(), a->ai_addr, a->ai_addrlen) != 0 &&
        errno != EINPROGRESS) {
      err = host + ":" + service + ": " + std::strerror(errno);
      continue;
    }
    // Wait in short slices so a stop request is not held up by an
    // unreachable host.
    pollfd p{s.Get(), POLLOUT, 0};
    int waited = 0;
    int ready = 0;
    bool stopped = false;
    while (waited < timeout_ms && ready == 0) {
      if (stop != nullptr && stop->load()) {
        stopped = true;
        break;
      }
      ready = ::poll(&p, 1, kPollSliceMs);
      waited += kPollSliceMs;
    }
    if (ready <= 0) {
      err = stopped ? "stopped" : host + ":" + service + ": connect timeout";
      if (stopped) {
        break;
      }
      continue;
    }
    int soerr = 0;
    socklen_t len = sizeof soerr;
    ::getsockopt(s.Get(), SOL_SOCKET, SO_ERROR, &soerr, &len);
    if (soerr != 0) {
      err = host + ":" + service + ": " + std::strerror(soerr);
      continue;
    }
    out = std::move(s);
  }
  return out;
}

bool SendAll(int fd, std::string_view s) {
  size_t done = 0;
  const int64_t deadline = MonotonicNs() + MsToNs(kSendDeadlineMs);
  while (done < s.size()) {
    const ssize_t n =
        ::send(fd, s.data() + done, s.size() - done, MSG_NOSIGNAL);
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
      if (MonotonicNs() > deadline) {
        return false;
      }
      pollfd p{fd, POLLOUT, 0};
      ::poll(&p, 1, kPollSliceMs);
      continue;
    }
    return false;
  }
  return true;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool ParseChunkSize(std::string_view line, size_t& size) noexcept {
  // chunk-size [ BWS ";" chunk-ext ] (RFC 9112 7.1); blanks around the size
  // are tolerated.
  line = line.substr(0, line.find(';'));
  auto blank = [](char c) { return c == ' ' || c == '\t'; };
  while (!line.empty() && blank(line.front())) {
    line.remove_prefix(1);
  }
  while (!line.empty() && blank(line.back())) {
    line.remove_suffix(1);
  }
  size_t v = 0;
  const auto r = std::from_chars(line.data(), line.data() + line.size(), v, 16);
  if (line.empty() || r.ec != std::errc{} ||
      r.ptr != line.data() + line.size()) {
    return false;
  }
  size = v;
  return true;
}

}  // namespace lgrtk::internal
