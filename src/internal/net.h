// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file net.h
/// TCP client helpers and HTTP chunked decoding for the NTRIP client
/// (internal, not installed).
#ifndef LGRTK_SRC_INTERNAL_NET_H_
#define LGRTK_SRC_INTERNAL_NET_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

#include "internal/unique_fd.h"

namespace lgrtk::internal {

/// Longest single wait on a socket: blocking loops re-check their stop flag
/// at least this often.
inline constexpr int kPollSliceMs = 50;

/// Opens a non-blocking TCP connection to `host`:`port`, trying every
/// resolved address, each for up to `timeout_ms`. Returns an invalid fd and a
/// reason in `err` on failure; "stopped" when `stop` became true while
/// waiting (a DNS lookup itself is not interruptible).
[[nodiscard]] UniqueFd Connect(const std::string& host, int port,
                               int timeout_ms, std::string& err,
                               const std::atomic<bool>* stop = nullptr);

/// Writes all of `s` to the non-blocking socket `fd`. False on a socket error
/// or when the peer does not take the data within two seconds.
[[nodiscard]] bool SendAll(int fd, std::string_view s);

/// `s` in lower case (ASCII), for case-insensitive header matching.
[[nodiscard]] std::string Lower(std::string s);

/// Parses the size line of an HTTP/1.1 chunk (hex digits, optionally followed
/// by ";extensions"; surrounding blanks allowed). False for anything else,
/// including a size that does not fit in size_t.
[[nodiscard]] bool ParseChunkSize(std::string_view line, size_t& size) noexcept;

/// Reads the status code of an HTTP status line ("HTTP/1.1 200 OK").
/// False when the line is not one.
[[nodiscard]] bool ParseHttpStatus(std::string_view status_line,
                                   int& status) noexcept;

/// HTTP/1.1 chunked transfer decoding, incremental: feed the body as it
/// arrives, get the payload in pieces.
class Dechunker {
 public:
  /// Longest accepted chunk size line, extensions included.
  static constexpr size_t kMaxSizeLine = 32;

  /// Decodes `in` and passes each payload piece to `out(std::string_view)`.
  /// False on malformed input (the stream cannot be decoded any further);
  /// everything after the last chunk is ignored.
  template <class F>
  bool Feed(std::string_view in, F&& out) {
    for (size_t i = 0; i < in.size();) {
      switch (state_) {
        case kSize: {
          const char c = in[i++];
          if (c == '\n') {
            size_t n = 0;
            if (!ParseChunkSize(line_, n)) {
              return false;
            }
            line_.clear();
            if (n == 0) {
              state_ = kDone;
            } else {
              remaining_ = n;
              state_ = kData;
            }
          } else if (c != '\r') {
            if (line_.size() > kMaxSizeLine) {
              return false;
            }
            line_ += c;
          }
          break;
        }
        case kData: {
          const size_t take = std::min(remaining_, in.size() - i);
          out(in.substr(i, take));
          i += take;
          remaining_ -= take;
          if (remaining_ == 0) {
            state_ = kCrlf;
          }
          break;
        }
        case kCrlf:
          if (in[i++] == '\n') {
            state_ = kSize;
          }
          break;
        case kDone:
          return true;
      }
    }
    return true;
  }

 private:
  enum State { kSize, kData, kCrlf, kDone } state_ = kSize;
  std::string line_;
  size_t remaining_ = 0;
};

}  // namespace lgrtk::internal

#endif  // LGRTK_SRC_INTERNAL_NET_H_
