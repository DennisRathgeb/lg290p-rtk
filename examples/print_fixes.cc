// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// Minimal lgrtk program: configure the receiver as a 10 Hz rover and print
// every fix with its state. Usage: print_fixes [/dev/ttyACM0]
#include <lgrtk/receiver.h>

#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char** argv) {
  lgrtk::ReceiverOptions opt;
  if (argc > 1) opt.port = argv[1];
  opt.config = lgrtk::lg290p::DefaultRoverConfig();  // GGA/RMC/GST/EPE at 10 Hz

  lgrtk::Lg290pReceiver rx(opt);
  rx.OnStatus([](const lgrtk::StatusEvent& e) {
    std::printf("[%s -> %s] %s\n", lgrtk::ToString(e.from),
                lgrtk::ToString(e.to), e.reason.c_str());
  });
  rx.OnFix([](const lgrtk::Fix& f) {
    std::printf("%-9s lat %.8f lon %.8f h %.3f  sigma E/N/U %.3f %.3f %.3f\n",
                lgrtk::ToString(f.type), f.lat_deg, f.lon_deg, f.h_ell_m,
                f.sigma_e_m, f.sigma_n_m, f.sigma_u_m);
  });
  rx.Start();
  std::this_thread::sleep_for(std::chrono::seconds(30));
  rx.Stop();
}
