// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/projection.h"

#include <proj.h>

#include <cmath>
#include <string>

namespace lgrtk {

struct Projection::Impl {
  PJ_CONTEXT* ctx = nullptr;
  PJ* pj = nullptr;
  ~Impl() {
    if (pj != nullptr) proj_destroy(pj);
    if (ctx != nullptr) proj_context_destroy(ctx);
  }
};

namespace {

std::string ContextError(PJ_CONTEXT* ctx) {
  const int e = proj_context_errno(ctx);
  const char* s = proj_context_errno_string(ctx, e);
  return s != nullptr ? s : "unknown PROJ error";
}

// Grids the best candidate operation needs but cannot find.
std::string MissingGrids(PJ_CONTEXT* ctx, PJ* src, PJ* dst) {
  PJ_OPERATION_FACTORY_CONTEXT* fc = proj_create_operation_factory_context(ctx, nullptr);
  if (fc == nullptr) return {};
  proj_operation_factory_context_set_grid_availability_use(
      ctx, fc, PROJ_GRID_AVAILABILITY_DISCARD_OPERATION_IF_MISSING_GRID);
  proj_operation_factory_context_set_spatial_criterion(
      ctx, fc, PROJ_SPATIAL_CRITERION_PARTIAL_INTERSECTION);
  PJ_OBJ_LIST* all = nullptr;
  std::string missing;
  // List every candidate (ignoring availability) and report the grids of the
  // first one that is not usable.
  proj_operation_factory_context_set_grid_availability_use(ctx, fc,
                                                           PROJ_GRID_AVAILABILITY_IGNORED);
  all = proj_create_operations(ctx, src, dst, fc);
  if (all != nullptr && proj_list_get_count(all) > 0) {
    PJ* op = proj_list_get(ctx, all, 0);
    const int n = proj_coordoperation_get_grid_used_count(ctx, op);
    for (int i = 0; i < n; ++i) {
      const char* short_name = nullptr;
      int available = 0;
      if (proj_coordoperation_get_grid_used(ctx, op, i, &short_name, nullptr, nullptr, nullptr,
                                            nullptr, nullptr, &available) &&
          !available && short_name != nullptr) {
        missing += (missing.empty() ? "" : ", ") + std::string(short_name);
      }
    }
    proj_destroy(op);
  }
  if (all != nullptr) proj_list_destroy(all);
  proj_operation_factory_context_destroy(fc);
  return missing;
}

}  // namespace

Projection::Projection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Projection::Projection(Projection&&) noexcept = default;
Projection& Projection::operator=(Projection&&) noexcept = default;
Projection::~Projection() = default;

std::optional<Projection> Projection::Create(std::string_view from_crs, std::string_view to_crs,
                                             std::string& err) {
  auto impl = std::make_unique<Impl>();
  impl->ctx = proj_context_create();
  if (impl->ctx == nullptr) {
    err = "proj_context_create failed";
    return std::nullopt;
  }
  proj_log_level(impl->ctx, PJ_LOG_NONE);
  const std::string from(from_crs);
  const std::string to(to_crs);
  PJ* src = proj_create(impl->ctx, from.c_str());
  PJ* dst = proj_create(impl->ctx, to.c_str());
  if (src == nullptr || dst == nullptr) {
    err = "unknown CRS " + (src == nullptr ? from : to) + ": " + ContextError(impl->ctx);
    if (src != nullptr) proj_destroy(src);
    if (dst != nullptr) proj_destroy(dst);
    return std::nullopt;
  }
  const std::string missing = MissingGrids(impl->ctx, src, dst);
  PJ* raw = proj_create_crs_to_crs_from_pj(impl->ctx, src, dst, nullptr, nullptr);
  proj_destroy(src);
  proj_destroy(dst);
  if (raw == nullptr) {
    err = "no transformation " + from + " -> " + to + ": " + ContextError(impl->ctx);
    return std::nullopt;
  }
  PJ* norm = proj_normalize_for_visualization(impl->ctx, raw);  // lon, lat order
  proj_destroy(raw);
  if (norm == nullptr) {
    err = "proj_normalize_for_visualization failed: " + ContextError(impl->ctx);
    return std::nullopt;
  }
  impl->pj = norm;
  if (!missing.empty()) {
    err = from + " -> " + to + " needs grid(s) not installed: " + missing +
          " (projsync --file <name>, or install proj-data)";
    return std::nullopt;
  }
  if (proj_coordoperation_has_ballpark_transformation(impl->ctx, impl->pj)) {
    err = from + " -> " + to + " has only a ballpark transformation (metre-level error)";
    return std::nullopt;
  }
  return Projection(std::move(impl));
}

bool Projection::Forward(double lat_deg, double lon_deg, double h_m, double out[3]) const noexcept {
  PJ_COORD c = proj_coord(lon_deg, lat_deg, h_m, HUGE_VAL);
  c = proj_trans(impl_->pj, PJ_FWD, c);
  if (c.xyz.x == HUGE_VAL || std::isnan(c.xyz.x)) return false;
  out[0] = c.xyz.x;
  out[1] = c.xyz.y;
  out[2] = c.xyz.z;
  return true;
}

bool Projection::Inverse(const double in[3], double& lat_deg, double& lon_deg,
                         double& h_m) const noexcept {
  PJ_COORD c = proj_coord(in[0], in[1], in[2], HUGE_VAL);
  c = proj_trans(impl_->pj, PJ_INV, c);
  if (c.xyz.x == HUGE_VAL || std::isnan(c.xyz.x)) return false;
  lon_deg = c.lpz.lam;
  lat_deg = c.lpz.phi;
  h_m = c.lpz.z;
  return true;
}

std::string Projection::Description() const {
  PJ_PROJ_INFO info = proj_pj_info(impl_->pj);
  std::string d = info.description != nullptr ? info.description : "";
  if (info.definition != nullptr && info.definition[0] != '\0') d += " | " + std::string(info.definition);
  return d;
}

}  // namespace lgrtk
