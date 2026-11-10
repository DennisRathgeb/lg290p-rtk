// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/fix.h"

namespace lgrtk {

const char* ToString(FixType t) noexcept {
  switch (t) {
    case FixType::kNone:
      return "none";
    case FixType::kSingle:
      return "single";
    case FixType::kDgnss:
      return "dgnss";
    case FixType::kPps:
      return "pps";
    case FixType::kRtkFixed:
      return "rtk-fixed";
    case FixType::kRtkFloat:
      return "rtk-float";
    case FixType::kDeadReckoning:
      return "dead-reckoning";
    case FixType::kManual:
      return "manual";
    case FixType::kSimulation:
      return "simulation";
  }
  return "?";
}

FixType FixTypeFromGgaQuality(uint8_t quality) noexcept {
  switch (quality) {
    case 1:
      return FixType::kSingle;
    case 2:
      return FixType::kDgnss;
    case 3:
      return FixType::kPps;
    case 4:
      return FixType::kRtkFixed;
    case 5:
      return FixType::kRtkFloat;
    case 6:
      return FixType::kDeadReckoning;
    case 7:
      return FixType::kManual;
    case 8:
      return FixType::kSimulation;
    default:
      return FixType::kNone;
  }
}

uint8_t ToGgaQuality(FixType t) noexcept {
  switch (t) {
    case FixType::kNone:
      return 0;
    case FixType::kSingle:
      return 1;
    case FixType::kDgnss:
      return 2;
    case FixType::kPps:
      return 3;
    case FixType::kRtkFixed:
      return 4;
    case FixType::kRtkFloat:
      return 5;
    case FixType::kDeadReckoning:
      return 6;
    case FixType::kManual:
      return 7;
    case FixType::kSimulation:
      return 8;
  }
  return 0;
}

}  // namespace lgrtk
