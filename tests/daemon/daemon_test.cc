// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/daemon.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "internal/unique_fd.h"
#include "internal/unix_socket.h"

namespace lgrtk::daemon {
namespace {

// Receives what the daemon sends to $NOTIFY_SOCKET while it exists.
class NotifySink {
 public:
  NotifySink()
      : path_("/tmp/lgrtk_daemon_notify_" + std::to_string(getpid())),
        fd_(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
    unlink(path_.c_str());
    internal::UnixAddress a;
    ok_ = fd_ && internal::MakeUnixAddress(path_, a) &&
          bind(fd_.Get(), a.Get(), a.len) == 0;
    setenv("NOTIFY_SOCKET", path_.c_str(), 1);
  }
  NotifySink(const NotifySink&) = delete;
  NotifySink& operator=(const NotifySink&) = delete;
  NotifySink(NotifySink&&) = delete;
  NotifySink& operator=(NotifySink&&) = delete;
  ~NotifySink() {
    unsetenv("NOTIFY_SOCKET");
    unlink(path_.c_str());
  }

  [[nodiscard]] bool Ok() const { return ok_; }
  // Every message received so far.
  std::vector<std::string> Drain() {
    std::vector<std::string> got;
    pollfd p{fd_.Get(), POLLIN, 0};
    while (poll(&p, 1, 100) > 0) {
      char buf[512];
      const ssize_t n = recv(fd_.Get(), buf, sizeof buf, 0);
      if (n <= 0) {
        break;
      }
      got.emplace_back(buf, static_cast<size_t>(n));
    }
    return got;
  }

 private:
  std::string path_;
  internal::UniqueFd fd_;
  bool ok_ = false;
};

int Count(const std::vector<std::string>& v, const std::string& s) {
  int n = 0;
  for (const auto& x : v) {
    n += x == s ? 1 : 0;
  }
  return n;
}

TEST(DaemonTest, StopNotifiesSystemdOnce) {
  NotifySink sink;
  ASSERT_TRUE(sink.Ok());
  {
    Daemon d{DaemonConfig{}};
    d.Stop();
    d.Stop();
  }  // the destructor stops as well
  EXPECT_EQ(Count(sink.Drain(), "STOPPING=1"), 1);
}

}  // namespace
}  // namespace lgrtk::daemon
