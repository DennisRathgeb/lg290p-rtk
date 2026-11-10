// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file unique_fd.h
/// Owning wrapper for a POSIX file descriptor (internal, not installed).
#ifndef LGRTK_SRC_INTERNAL_UNIQUE_FD_H_
#define LGRTK_SRC_INTERNAL_UNIQUE_FD_H_

#include <unistd.h>

#include <utility>

namespace lgrtk::internal {

/// Closes the descriptor it owns when destroyed. Move-only.
class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  /// Takes ownership of `fd` (-1 = none).
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { Reset(); }
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  /// The descriptor, still owned; -1 if none.
  [[nodiscard]] int Get() const noexcept { return fd_; }
  /// True when a descriptor is owned.
  [[nodiscard]] bool Valid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return Valid(); }
  /// Gives up ownership without closing.
  [[nodiscard]] int Release() noexcept { return std::exchange(fd_, -1); }
  /// Closes the owned descriptor (if any) and takes `fd`.
  void Reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace lgrtk::internal

#endif  // LGRTK_SRC_INTERNAL_UNIQUE_FD_H_
