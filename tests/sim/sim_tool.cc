// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// Runs a simulated LG290P and prints its tty path; exits when stdin closes.
// Stdin commands (one per line): "unplug", "replug". Used by tests/cli_test.sh
// and tests/daemon_test.sh.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "internal/text.h"
#include "sim/sim_lg290p.h"

int main(int argc, char** argv) {
  int baud = 460800;
  if (argc > 1 &&
      !lgrtk::internal::ParseNumber(std::string_view(argv[1]), baud)) {
    std::cerr << "usage: sim_lg290p_tool [BAUD]\n";
    return 2;
  }
  lgrtk::test::SimLg290p sim(baud);
  std::cout << sim.Path() << '\n' << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "unplug") {
      sim.Unplug();
    } else if (line == "replug") {
      sim.Replug();
    }
  }
  return 0;
}
