// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/chrony.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <utility>

#include "daemon/gpsd_json.h"

namespace lgrtk::daemon {
namespace {

constexpr int kMagic = 0x534f434b;
constexpr int64_t kNsPerS = 1000000000;

static_assert(sizeof(ChronySample) == 40, "chrony sock_sample layout");

}  // namespace

std::optional<ChronySample> MakeChronySample(const Fix& fix,
                                             int64_t arrival_realtime_ns) {
  if (!fix.has_position || fix.type == FixType::kNone) {
    return std::nullopt;
  }
  const auto fix_ns = FixUnixNs(fix);
  if (!fix_ns) {
    return std::nullopt;
  }
  const int64_t diff = *fix_ns - arrival_realtime_ns;
  ChronySample s{};
  s.tv.tv_sec = static_cast<time_t>(arrival_realtime_ns / kNsPerS);
  s.tv.tv_usec =
      static_cast<suseconds_t>((arrival_realtime_ns % kNsPerS) / 1000);
  s.offset = static_cast<double>(diff) / 1e9;
  s.magic = kMagic;
  return s;
}

ChronySock::ChronySock(std::string path) : path_(std::move(path)) {
  fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
}

ChronySock::~ChronySock() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

void ChronySock::Send(const ChronySample& s) {
  sockaddr_un a{};
  if (fd_ < 0 || path_.size() >= sizeof a.sun_path) {
    return;
  }
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path_.c_str(), path_.size() + 1);
  if (sendto(fd_, &s, sizeof s, MSG_NOSIGNAL, reinterpret_cast<sockaddr*>(&a),
             sizeof a) == static_cast<ssize_t>(sizeof s)) {
    ++sent_;
  }
}

}  // namespace lgrtk::daemon
