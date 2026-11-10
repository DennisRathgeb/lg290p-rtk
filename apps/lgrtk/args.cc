// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "apps/lgrtk/args.h"

#include <iostream>

#include "internal/text.h"

namespace lgrtk::cli {
namespace {

// from_chars takes no '+' sign; accept one in front of a digit or '.'.
std::string_view SkipPlus(std::string_view s) {
  if (s.size() > 1 && s.front() == '+' && s[1] != '-' && s[1] != '+') {
    s.remove_prefix(1);
  }
  return s;
}

}  // namespace

int Usage(const std::string& msg) {
  std::cerr << "lgrtk: " << msg << "\n(run `lgrtk --help`)\n";
  return kUsage;
}

Args::Args(int argc, const char* const* argv) {
  for (int k = 1; k < argc; ++k) {
    v_.emplace_back(argv[k]);
  }
}

std::string Args::Next() { return i_ < v_.size() ? v_[i_++] : std::string(); }

std::string Args::Peek() const {
  return i_ < v_.size() ? v_[i_] : std::string();
}

std::optional<std::string> Args::Option(const std::string& name) {
  for (size_t k = i_; k + 1 < v_.size(); ++k) {
    if (v_[k] == name) {
      std::string val = v_[k + 1];
      const auto at = v_.begin() + static_cast<std::ptrdiff_t>(k);
      v_.erase(at, at + 2);
      return val;
    }
  }
  return std::nullopt;
}

bool Args::Flag(const std::string& name) {
  for (size_t k = i_; k < v_.size(); ++k) {
    if (v_[k] == name) {
      v_.erase(v_.begin() + static_cast<std::ptrdiff_t>(k));
      return true;
    }
  }
  return false;
}

bool ToInt(std::string_view s, int& out) {
  return internal::ParseNumber(SkipPlus(s), out);
}

bool ToDouble(std::string_view s, double& out) {
  return internal::ParseNumber(SkipPlus(s), out);
}

}  // namespace lgrtk::cli
