// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// lgrtk project: one coordinate transformation through PROJ.
#include <cstdio>
#include <iostream>
#include <string>

#include "apps/lgrtk/args.h"
#include "apps/lgrtk/commands.h"
#ifdef LGRTK_HAVE_PROJ
#include "lgrtk/projection.h"
#endif

namespace lgrtk::cli {

int CmdProject(Args& a, ReceiverOptions /*o*/) {
#ifdef LGRTK_HAVE_PROJ
  const auto from = a.Option("--from");
  const auto to = a.Option("--to");
  double lat = 0;
  double lon = 0;
  double h = 0;
  if (!from || !to || !ToDouble(a.Next(), lat) || !ToDouble(a.Next(), lon) ||
      !ToDouble(a.Next(), h)) {
    return Usage("project --from CRS --to CRS LAT LON H");
  }
  std::string err;
  auto p = Projection::Create(*from, *to, err);
  if (!p) {
    std::cerr << "lgrtk: " << err << "\n";
    return kRuntime;
  }
  double out[3];
  if (!p->Forward(lat, lon, h, out)) {
    std::cerr << "lgrtk: transformation failed for this point\n";
    return kRuntime;
  }
  std::printf("%.4f %.4f %.4f\n", out[0], out[1], out[2]);
  return kOk;
#else
  (void)a;
  std::cerr << "lgrtk: built without PROJ\n";
  return kRuntime;
#endif
}

}  // namespace lgrtk::cli
