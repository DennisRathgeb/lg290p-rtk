// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_SERIAL_H_
#define LGRTK_SERIAL_H_

/// @file serial.h
/// A raw 8N1 serial port (tty), opened exclusively and non-blocking.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace lgrtk {

/// Owns one tty file descriptor. Movable, not copyable.
class SerialPort {
 public:
  /// Opens `path` raw 8N1 at `baud` with TIOCEXCL so no other process can
  /// interleave (ModemManager, a second driver). On failure returns nullopt
  /// and a reason naming the path in `err`.
  static std::optional<SerialPort> Open(const std::string& path, int baud,
                                        std::string& err);
  /// True for the speeds the LG290P supports (9600 … 921600).
  static bool SupportedBaud(int baud) noexcept;

  /// Moves the descriptor.
  SerialPort(SerialPort&& o) noexcept;
  /// Closes this port and takes over `o`'s descriptor.
  SerialPort& operator=(SerialPort&& o) noexcept;
  SerialPort(const SerialPort&) = delete;
  SerialPort& operator=(const SerialPort&) = delete;
  ~SerialPort();

  /// Waits up to `timeout_ms` for data; returns bytes read (>0), 0 on
  /// timeout, -1 when the device is gone or failed.
  int Read(std::span<uint8_t> buf, int timeout_ms) noexcept;
  /// Writes what fits without blocking; `written` gets the count. False on a
  /// device error.
  bool WriteSome(std::span<const uint8_t> data, size_t& written) noexcept;
  /// Changes the line speed.
  bool SetBaud(int baud) noexcept;
  /// Discards pending input and output.
  void Flush() noexcept;
  /// The file descriptor (for poll loops).
  [[nodiscard]] int Fd() const noexcept { return fd_; }
  /// Current line speed.
  [[nodiscard]] int Baud() const noexcept { return baud_; }

 private:
  SerialPort(int fd, int baud) : fd_(fd), baud_(baud) {}
  int fd_ = -1;
  int baud_ = 0;
};

}  // namespace lgrtk

#endif  // LGRTK_SERIAL_H_
