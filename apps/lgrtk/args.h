// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// lgrtk CLI: command-line arguments, number parsing, exit codes.
#ifndef LGRTK_APPS_LGRTK_ARGS_H_
#define LGRTK_APPS_LGRTK_ARGS_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lgrtk::cli {

inline constexpr int kOk = 0;       ///< exit code: success
inline constexpr int kRuntime = 1;  ///< exit code: runtime error
inline constexpr int kUsage = 2;    ///< exit code: usage error

/// Prints `msg` and a hint to `lgrtk --help` on stderr; returns kUsage.
int Usage(const std::string& msg);

/// The arguments after the program name, consumed front to back.
class Args {
 public:
  Args() = default;
  /// Takes argv[1..argc).
  Args(int argc, const char* const* argv);

  /// True while unconsumed arguments remain.
  [[nodiscard]] bool Has() const { return i_ < v_.size(); }
  /// True when no arguments were given at all.
  [[nodiscard]] bool Empty() const { return v_.empty(); }
  /// Consumes the next argument ("" when none is left).
  std::string Next();
  /// The next argument without consuming it ("" when none is left).
  [[nodiscard]] std::string Peek() const;
  /// Removes "--name value" anywhere after the current position.
  std::optional<std::string> Option(const std::string& name);
  /// Removes "--name" anywhere after the current position; true if present.
  bool Flag(const std::string& name);

 private:
  std::vector<std::string> v_;
  size_t i_ = 0;
};

/// Parses all of `s` as a decimal integer (an optional leading '+' or '-');
/// false on anything else or when it does not fit an int.
[[nodiscard]] bool ToInt(std::string_view s, int& out);
/// Parses all of `s` as a decimal number; false on anything else.
[[nodiscard]] bool ToDouble(std::string_view s, double& out);

}  // namespace lgrtk::cli

#endif  // LGRTK_APPS_LGRTK_ARGS_H_
