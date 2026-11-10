// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_PROJECTION_H_
#define LGRTK_PROJECTION_H_

/// @file projection.h
/// Fast coordinate transformation with PROJ (target `lgrtk::projection`).

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "lgrtk/fix.h"
#include "lgrtk/local_frame.h"

namespace lgrtk {

/// A transformation between two CRSs, built once (pipeline selection, grid
/// loading), then microseconds per point. Movable, not copyable; not
/// thread-safe: use one object per thread.
///
/// Example: ETRS89 / CHTRS95 geographic 3-D (`EPSG:4937`) to Swiss LV95 with
/// LHN95 heights (`EPSG:2056+5729`; needs the swisstopo geoid grid from
/// `projsync --source-id ch_swisstopo`).
class Projection {
 public:
  /// Builds `from_crs` → `to_crs` (any PROJ CRS string: "EPSG:2056", WKT,
  /// PROJJSON). Input is always latitude, longitude (degrees), height
  /// (metres); output x, y, z in the target CRS (easting, northing, height
  /// for projected CRSs). Refuses (nullopt + reason) when PROJ only finds a
  /// "ballpark" transformation or the best one needs a grid that is not
  /// installed; the reason names the grid.
  static std::optional<Projection> Create(std::string_view from_crs,
                                          std::string_view to_crs,
                                          std::string& err);
  /// Moves the PROJ context and pipeline.
  Projection(Projection&&) noexcept;
  /// Moves the PROJ context and pipeline.
  Projection& operator=(Projection&&) noexcept;
  ~Projection();

  /// Geodetic → target. False if PROJ fails for this point.
  bool Forward(double lat_deg, double lon_deg, double h_m,
               double out[3]) const noexcept;
  /// Target → geodetic.
  bool Inverse(const double in[3], double& lat_deg, double& lon_deg,
               double& h_m) const noexcept;
  /// The PROJ pipeline in use (for logs).
  [[nodiscard]] std::string Description() const;

 private:
  struct Impl;
  explicit Projection(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// The CRS each kind of fix is in. The receiver cannot know the frame of its
/// corrections; the application declares it (frames are never guessed).
struct FixFrames {
  std::string corrected_crs;  ///< e.g. "EPSG:4937" (ETRS89/CHTRS95) for swipos;
                              ///< empty = refuse
  std::string
      autonomous_crs;  ///< e.g. "EPSG:4979" (WGS 84 ~ ITRF); empty = refuse
};

/// Projects fixes with the transformation that matches their FixFrame, so a
/// corrected and an autonomous position can never be mixed silently.
class FixProjector {
 public:
  /// Builds one Projection per declared frame to `to_crs`.
  static std::optional<FixProjector> Create(const FixFrames& frames,
                                            std::string_view to_crs,
                                            std::string& err);
  /// Projects `fix` into `out`; false (with a reason) for a fix without a
  /// position or in a frame without a declared CRS.
  bool Project(const Fix& fix, double out[3], std::string& err) const;

 private:
  std::optional<Projection> corrected_;
  std::optional<Projection> autonomous_;
};

}  // namespace lgrtk

#endif  // LGRTK_PROJECTION_H_
