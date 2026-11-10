// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/raw_tap.h"

#include <string_view>
#include <utility>

namespace lgrtk::daemon {

RawTap::RawTap(EventLoop* loop, size_t cap_bytes)
    : clients_(loop, ClientSet::Handlers{}, cap_bytes,
               /*read_lines=*/false) {}

bool RawTap::Listen(const std::string& host, int port, std::string* err) {
  internal::UniqueFd fd = ListenTcp(host, port, err);
  if (!fd) {
    return false;
  }
  port_ = BoundPort(fd.Get());
  clients_.AddListener(std::move(fd));
  return true;
}

void RawTap::Publish(std::span<const uint8_t> bytes) {
  const std::string_view data(reinterpret_cast<const char*>(bytes.data()),
                              bytes.size());
  for (const auto id : clients_.Ids()) {
    clients_.Send(id, data);
  }
}

}  // namespace lgrtk::daemon
