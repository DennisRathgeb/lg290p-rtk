// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_LOCAL_FRAME_H_
#define LGRTK_LOCAL_FRAME_H_

/// @file local_frame.h
/// Geodetic tangent-plane ENU around an origin (no PROJ needed).

namespace lgrtk {

/// East-North-Up coordinates in metres around a fixed origin on the GRS80
/// ellipsoid (identical to WGS 84 at the millimetre level). Exact: goes
/// through ECEF, no small-angle approximation. Note: this is not a map grid;
/// LV95/UTM axes differ from ENU by the meridian convergence (about 1 deg in
/// eastern Switzerland), so use Projection for plan coordinates.
class LocalFrame {
 public:
  /// Origin latitude/longitude in degrees, ellipsoidal height in metres.
  LocalFrame(double lat0_deg, double lon0_deg, double h0_m) noexcept;
  /// Geodetic → ENU (metres).
  void ToEnu(double lat_deg, double lon_deg, double h_m, double enu[3]) const noexcept;
  /// ENU (metres) → geodetic.
  void FromEnu(const double enu[3], double& lat_deg, double& lon_deg, double& h_m) const noexcept;

  /// Geodetic → ECEF (metres), GRS80.
  static void GeodeticToEcef(double lat_deg, double lon_deg, double h_m, double xyz[3]) noexcept;
  /// ECEF → geodetic, GRS80 (iterative, < 0.1 mm).
  static void EcefToGeodetic(const double xyz[3], double& lat_deg, double& lon_deg,
                             double& h_m) noexcept;

 private:
  double origin_[3];  // ECEF
  double r_[3][3];    // rows: east, north, up unit vectors in ECEF
};

}  // namespace lgrtk

#endif  // LGRTK_LOCAL_FRAME_H_
