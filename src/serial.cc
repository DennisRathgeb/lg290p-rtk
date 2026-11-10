// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/serial.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "internal/unique_fd.h"

namespace lgrtk {

namespace {

speed_t SpeedOf(int baud) noexcept {
  switch (baud) {
    case 9600:
      return B9600;
    case 115200:
      return B115200;
    case 230400:
      return B230400;
    case 460800:
      return B460800;
    case 921600:
      return B921600;
    default:
      return B0;
  }
}

bool Configure(int fd, int baud) noexcept {
  termios t{};
  if (tcgetattr(fd, &t) != 0) {
    return false;
  }
  cfmakeraw(&t);
  t.c_cflag |= CLOCAL | CREAD;
  t.c_cflag &= ~(CSTOPB | PARENB | CRTSCTS);
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  if (cfsetispeed(&t, SpeedOf(baud)) != 0 ||
      cfsetospeed(&t, SpeedOf(baud)) != 0) {
    return false;
  }
  return tcsetattr(fd, TCSANOW, &t) == 0;
}

}  // namespace

bool SerialPort::SupportedBaud(int baud) noexcept {
  return SpeedOf(baud) != B0;
}

std::optional<SerialPort> SerialPort::Open(const std::string& path, int baud,
                                           std::string& err) {
  if (!SupportedBaud(baud)) {
    err = "unsupported baud " + std::to_string(baud);
    return std::nullopt;
  }
  // Owned here until every check passed; closed on each early return.
  internal::UniqueFd fd(
      ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
  if (!fd) {
    const int e = errno;
    err = path + ": " + std::strerror(e);
    if (e == EBUSY) {
      err += " (busy: another process has the port open exclusively)";
    }
    if (e == EACCES) {
      err += " (add your user to the uucp/dialout group)";
    }
    return std::nullopt;
  }
  if (isatty(fd.Get()) == 0) {
    err = path + ": not a tty";
    return std::nullopt;
  }
  if (::ioctl(fd.Get(), TIOCEXCL) != 0 || !Configure(fd.Get(), baud)) {
    err = path + ": " + std::strerror(errno);
    return std::nullopt;
  }
  tcflush(fd.Get(), TCIOFLUSH);
  return SerialPort(fd.Release(), baud);
}

SerialPort::SerialPort(SerialPort&& o) noexcept
    : fd_(std::exchange(o.fd_, -1)), baud_(o.baud_) {}

SerialPort& SerialPort::operator=(SerialPort&& o) noexcept {
  if (this != &o) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = std::exchange(o.fd_, -1);
    baud_ = o.baud_;
  }
  return *this;
}

SerialPort::~SerialPort() {
  if (fd_ >= 0) {
    ::ioctl(fd_, TIOCNXCL);
    ::close(fd_);
  }
}

int SerialPort::Read(std::span<uint8_t> buf, int timeout_ms) noexcept {
  if (fd_ < 0) {
    return -1;
  }
  pollfd p{fd_, POLLIN, 0};
  const int r = ::poll(&p, 1, timeout_ms);
  if (r < 0) {
    return errno == EINTR ? 0 : -1;
  }
  if (r == 0) {
    return 0;
  }
  if (p.revents & POLLIN) {
    const ssize_t n = ::read(fd_, buf.data(), buf.size());
    if (n > 0) {
      return static_cast<int>(n);
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
      return 0;
    }
    return -1;  // EOF or EIO: the device went away
  }
  if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) {
    return -1;
  }
  return 0;
}

bool SerialPort::WriteSome(std::span<const uint8_t> data,
                           size_t& written) noexcept {
  written = 0;
  if (fd_ < 0) {
    return false;
  }
  while (written < data.size()) {
    const ssize_t n =
        ::write(fd_, data.data() + written, data.size() - written);
    if (n > 0) {
      written += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && errno == EAGAIN) {
      return true;
    }
    return false;
  }
  return true;
}

bool SerialPort::SetBaud(int baud) noexcept {
  if (fd_ < 0 || !SupportedBaud(baud) || !Configure(fd_, baud)) {
    return false;
  }
  baud_ = baud;
  return true;
}

void SerialPort::Flush() noexcept {
  if (fd_ >= 0) {
    tcflush(fd_, TCIOFLUSH);
  }
}

}  // namespace lgrtk
