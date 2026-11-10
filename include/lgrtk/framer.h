// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_FRAMER_H_
#define LGRTK_FRAMER_H_

/// @file framer.h
/// Splits the receiver's byte stream into NMEA sentences and RTCM3 frames.
///
/// The LG290P interleaves both on one port (NMEA text, RTCM binary raw
/// observations or base output). The framer checks every NMEA checksum and
/// every RTCM CRC-24Q, resynchronises after garbage (wrong baud, boot noise)
/// and never allocates.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace lgrtk {

/// What a Frame holds.
enum class FrameKind : uint8_t { kNmea, kRtcm };

/// One complete, checked frame. `bytes` points into the framer's buffer and
/// is valid only during the callback.
struct Frame {
  FrameKind kind;  ///< NMEA sentence or RTCM3 frame
  std::span<const uint8_t>
      bytes;  ///< NMEA: "$...*HH" without CR/LF; RTCM: header..CRC
  /// The NMEA sentence as text (empty for RTCM).
  [[nodiscard]] std::string_view Text() const {
    return kind == FrameKind::kNmea
               ? std::string_view(reinterpret_cast<const char*>(bytes.data()),
                                  bytes.size())
               : std::string_view();
  }
};

/// CRC-24Q as used by RTCM3 (polynomial 0x1864CFB, init 0).
[[nodiscard]] uint32_t Crc24q(std::span<const uint8_t> data) noexcept;

/// True when `line` is "$...*HH" (optionally followed by CR/LF) with a
/// correct XOR checksum.
[[nodiscard]] bool NmeaChecksumOk(std::string_view line) noexcept;

/// Incremental framer over a byte stream.
class StreamFramer {
 public:
  /// Longest accepted NMEA sentence without CR/LF (PQTM replies exceed 82).
  static constexpr size_t kMaxNmea = 255;
  /// Longest RTCM3 frame: 3 header + 1023 payload + 3 CRC.
  static constexpr size_t kMaxRtcm = 1029;

  /// Running counts, for status reporting.
  struct Counters {
    uint64_t nmea = 0;           ///< good NMEA sentences
    uint64_t rtcm = 0;           ///< good RTCM frames
    uint64_t bad_nmea = 0;       ///< NMEA with wrong checksum or bad characters
    uint64_t bad_rtcm = 0;       ///< RTCM with wrong CRC
    uint64_t skipped_bytes = 0;  ///< bytes outside any frame
    uint64_t oversize = 0;       ///< NMEA lines longer than kMaxNmea
  };

  /// Feeds bytes; calls `on_frame(const Frame&)` for every complete frame,
  /// in stream order.
  template <class F>
  void Feed(std::span<const uint8_t> data, F&& on_frame) {
    while (!data.empty()) {
      data = data.subspan(Append(data));
      Frame f{};
      while (Next(f)) {
        on_frame(f);
      }
      Compact();
    }
  }

  /// Drops buffered partial data (after a reconnect).
  void Reset() noexcept {
    len_ = 0;
    pos_ = 0;
  }

  /// Frame statistics since construction.
  [[nodiscard]] const Counters& Stats() const noexcept { return counters_; }

 private:
  size_t Append(std::span<const uint8_t> data) noexcept;
  bool Next(Frame& out) noexcept;
  void Compact() noexcept;
  void Skip(size_t n) noexcept;

  std::array<uint8_t, 4096> buf_{};
  size_t len_ = 0;            // bytes in buf_
  size_t pos_ = 0;            // start of unprocessed data
  bool in_oversize_ = false;  // discarding the rest of an overlong line
  Counters counters_;
};

}  // namespace lgrtk

#endif  // LGRTK_FRAMER_H_
