// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// RTK rover: receiver + NTRIP corrections + Swiss LV95/LHN95 coordinates.
// Usage: ntrip_rover HOST MOUNTPOINT USER  (password in $NTRIP_PASSWORD)
#include <lgrtk/ntrip.h>
#include <lgrtk/projection.h>
#include <lgrtk/receiver.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s HOST MOUNTPOINT USER\n", argv[0]);
    return 2;
  }
  // Corrected positions from swipos are in CHTRS95 (= ETRS89, EPSG:4937);
  // LV95 + LHN95 heights is EPSG:2056+5729 (needs `make grids`).
  std::string err;
  auto lv95 = lgrtk::Projection::Create("EPSG:4937", "EPSG:2056+5729", err);
  if (!lv95) {
    std::fprintf(stderr, "projection: %s\n", err.c_str());
    return 1;
  }

  lgrtk::Lg290pReceiver rx(lgrtk::ReceiverOptions{});
  lgrtk::NtripOptions no;
  no.host = argv[1];
  no.mountpoint = argv[2];
  no.user = argv[3];
  if (const char* pw = std::getenv("NTRIP_PASSWORD")) no.password = pw;
  lgrtk::NtripClient ntrip(no, rx);  // the receiver is the CorrectionSink

  rx.OnFix([&](const lgrtk::Fix& f) {
    ntrip.SetPosition(f);  // GGA for VRS mountpoints
    if (!f.has_position || f.frame != lgrtk::FixFrame::kCorrected) return;
    double enh[3];
    if (lv95->Forward(f.lat_deg, f.lon_deg, f.h_ell_m, enh)) {
      std::printf("%-9s E %.3f N %.3f H %.3f  age %.1f s\n",
                  lgrtk::ToString(f.type), enh[0], enh[1], enh[2],
                  f.diff_age_s);
    }
  });
  rx.Start();
  ntrip.Start();
  std::this_thread::sleep_for(std::chrono::minutes(10));
}
