// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_TESTS_SIM_FAKE_CASTER_H_
#define LGRTK_TESTS_SIM_FAKE_CASTER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lgrtk::test {

/// A local NTRIP caster on 127.0.0.1 for tests.
class FakeCaster {
 public:
  enum class Mode {
    kV2Chunked,     ///< HTTP/1.1 200, chunked RTCM
    kV1Icy,         ///< "ICY 200 OK", raw RTCM
    kUnauthorized,  ///< HTTP 401
    kDropAfterOne,  ///< sends one frame, then closes (every connection)
    kSilent,        ///< 200 OK, then no data
  };
  explicit FakeCaster(Mode mode);
  ~FakeCaster();
  FakeCaster(const FakeCaster&) = delete;
  FakeCaster& operator=(const FakeCaster&) = delete;

  int port() const { return port_; }
  int connections() const { return connections_; }
  std::vector<std::string> Requests() const;  ///< request headers per connection
  std::string ClientBytes() const;             ///< everything clients sent after the headers
  /// The RTCM frame the caster sends repeatedly.
  static std::vector<uint8_t> Frame();

 private:
  void Loop();
  void Serve(int fd);

  Mode mode_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> connections_{0};
  mutable std::mutex mu_;
  std::vector<std::string> requests_;
  std::string client_bytes_;
  std::thread thread_;
};

}  // namespace lgrtk::test

#endif  // LGRTK_TESTS_SIM_FAKE_CASTER_H_
