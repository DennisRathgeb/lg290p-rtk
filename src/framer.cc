// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/framer.h"

#include <algorithm>
#include <cstring>

namespace lgrtk {

namespace {

constexpr uint8_t kRtcmPreamble = 0xD3;

int HexValue(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

bool IsStart(uint8_t b) noexcept { return b == '$' || b == kRtcmPreamble; }

}  // namespace

uint32_t Crc24q(std::span<const uint8_t> data) noexcept {
  uint32_t crc = 0;
  for (uint8_t b : data) {
    crc ^= static_cast<uint32_t>(b) << 16;
    for (int i = 0; i < 8; ++i) {
      crc <<= 1;
      if (crc & 0x1000000u) {
        crc ^= 0x1864CFBu;
      }
    }
  }
  return crc & 0xFFFFFFu;
}

bool NmeaChecksumOk(std::string_view line) noexcept {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  if (line.size() < 4 || line[0] != '$') {
    return false;
  }
  const size_t star = line.rfind('*');
  if (star == std::string_view::npos || star + 3 != line.size()) {
    return false;
  }
  const int hi = HexValue(line[star + 1]);
  const int lo = HexValue(line[star + 2]);
  if (hi < 0 || lo < 0) {
    return false;
  }
  unsigned sum = 0;
  for (size_t i = 1; i < star; ++i) {
    sum ^= static_cast<unsigned char>(line[i]);
  }
  return sum == static_cast<unsigned>(hi * 16 + lo);
}

size_t StreamFramer::Append(std::span<const uint8_t> data) noexcept {
  const size_t n = std::min(data.size(), buf_.size() - len_);
  std::memcpy(buf_.data() + len_, data.data(), n);
  len_ += n;
  return n;
}

void StreamFramer::Skip(size_t n) noexcept {
  pos_ += n;
  counters_.skipped_bytes += n;
}

bool StreamFramer::Next(Frame& out) noexcept {
  while (pos_ < len_) {
    const uint8_t* p = buf_.data() + pos_;
    const size_t avail = len_ - pos_;

    if (!IsStart(p[0])) {
      size_t k = 1;
      while (k < avail && !IsStart(p[k])) {
        ++k;
      }
      Skip(k);
      continue;
    }

    if (p[0] == '$') {
      // Scan for the line end. A non-printable byte means this '$' was noise;
      // another '$' means this sentence was cut off and a new one starts
      // there (lost bytes, a receiver reboot, the CH343 replay on open).
      size_t i = 1;
      bool noise = false;
      bool restart = false;
      for (; i < avail; ++i) {
        const uint8_t b = p[i];
        if (b == '\n') {
          break;
        }
        if (b == '\r') {
          continue;
        }
        if (b == '$') {
          restart = true;
          break;
        }
        if (b < 0x20 || b > 0x7E) {
          noise = true;
          break;
        }
        if (i > kMaxNmea + 1) {
          break;
        }
      }
      if (noise) {
        Skip(1);
        continue;
      }
      if (restart) {
        ++counters_.bad_nmea;
        Skip(i);  // resynchronise at the inner '$'
        continue;
      }
      if (i > kMaxNmea + 1) {
        ++counters_.oversize;
        Skip(i);  // the rest is skipped byte-wise up to the next '$' or 0xD3
        continue;
      }
      if (i == avail) {
        return false;  // incomplete line, wait for more
      }
      size_t end = i;  // index of '\n'
      while (end > 0 && p[end - 1] == '\r') {
        --end;
      }
      const std::string_view line(reinterpret_cast<const char*>(p), end);
      pos_ += i + 1;
      if (!NmeaChecksumOk(line)) {
        ++counters_.bad_nmea;
        continue;
      }
      ++counters_.nmea;
      out = Frame{FrameKind::kNmea, std::span<const uint8_t>(p, end)};
      return true;
    }

    // RTCM3: 0xD3, 6 reserved zero bits + 10-bit length, payload, CRC-24Q.
    if (avail < 3) {
      return false;
    }
    if ((p[1] & 0xFC) != 0) {
      Skip(1);
      continue;
    }
    const size_t total = ((static_cast<size_t>(p[1] & 0x03) << 8) | p[2]) + 6;
    if (avail < total) {
      return false;
    }
    const uint32_t want = (static_cast<uint32_t>(p[total - 3]) << 16) |
                          (static_cast<uint32_t>(p[total - 2]) << 8) |
                          p[total - 1];
    if (Crc24q(std::span<const uint8_t>(p, total - 3)) != want) {
      ++counters_.bad_rtcm;
      Skip(1);
      continue;
    }
    ++counters_.rtcm;
    pos_ += total;
    out = Frame{FrameKind::kRtcm, std::span<const uint8_t>(p, total)};
    return true;
  }
  return false;
}

void StreamFramer::Compact() noexcept {
  if (pos_ == 0) {
    return;
  }
  std::memmove(buf_.data(), buf_.data() + pos_, len_ - pos_);
  len_ -= pos_;
  pos_ = 0;
}

}  // namespace lgrtk
