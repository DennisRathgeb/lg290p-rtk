// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// Runs a simulated LG290P and prints its tty path; exits when stdin closes.
// Stdin commands (one per line): "unplug", "replug". Used by tests/cli_test.sh
// and tests/daemon_test.sh.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "sim/sim_lg290p.h"

int main(int argc, char** argv) {
  const int baud = argc > 1 ? std::atoi(argv[1]) : 460800;
  lgrtk::test::SimLg290p sim(baud);
  std::cout << sim.Path() << std::endl;
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
