// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/gpsd_server.h"

#include <utility>

namespace lgrtk::daemon {
namespace {

// Parses one of our own "...\r\n" objects back for embedding in POLL.
nlohmann::ordered_json Embedded(const std::string& line) {
  return nlohmann::ordered_json::parse(line, nullptr, false);
}

}  // namespace

GpsdServer::GpsdServer(EventLoop* loop, std::string device, size_t cap_bytes)
    : device_(std::move(device)),
      clients_(
          loop,
          ClientSet::Handlers{[this](ClientSet::Id id) {
                                watch_[id] = Watch{};
                                clients_.Send(id, VersionJson());
                              },
                              [this](ClientSet::Id id, std::string_view line) {
                                OnLine(id, line);
                              },
                              [this](ClientSet::Id id) { watch_.erase(id); }},
          ClientSet::Options{.cap_bytes = cap_bytes}) {}

bool GpsdServer::Listen(const std::string& host, int port, std::string* err) {
  port_ = clients_.AddTcpListener(host, port, err);
  return port_ != 0;
}

void GpsdServer::OnLine(ClientSet::Id id, std::string_view line) {
  for (const std::string_view command : SplitGpsdCommands(line)) {
    OnCommand(id, command);
  }
}

void GpsdServer::OnCommand(ClientSet::Id id, std::string_view line) {
  const auto cmd = ParseGpsdCommand(line);
  if (!cmd) {
    clients_.Send(id, ErrorJson("Invalid request: " + std::string(line)));
    return;
  }
  switch (cmd->kind) {
    case GpsdCommand::kVersion:
      clients_.Send(id, VersionJson());
      break;
    case GpsdCommand::kDevices:
      clients_.Send(id, DevicesJson(device_, active_, baud_));
      break;
    case GpsdCommand::kWatch: {
      Watch& w = watch_[id];
      const bool was_enabled = w.enable;
      ApplyWatch(cmd->arg, &w);
      if (w.enable && !was_enabled) {
        clients_.Send(id, DevicesJson(device_, active_, baud_));
      }
      clients_.Send(id, WatchJson(w));
      break;
    }
    case GpsdCommand::kPoll:
      clients_.Send(id, PollJson());
      break;
    case GpsdCommand::kUnknown:
      clients_.Send(
          id, ErrorJson("Unrecognized request '" + std::string(line) + "'"));
      break;
  }
}

std::string GpsdServer::PollJson() const {
  nlohmann::ordered_json o;
  o["class"] = "POLL";
  if (last_fix_) {
    if (auto ns = FixUnixNs(*last_fix_)) {
      o["time"] = IsoTime(*ns);
    }
  }
  o["active"] = active_ ? 1 : 0;
  auto tpv = nlohmann::ordered_json::array();
  auto gst = nlohmann::ordered_json::array();
  auto sky = nlohmann::ordered_json::array();
  if (last_fix_) {
    tpv.push_back(Embedded(TpvJson(*last_fix_, device_)));
    const std::string g = GstJson(*last_fix_, device_);
    if (!g.empty()) {
      gst.push_back(Embedded(g));
    }
  }
  if (!last_sky_.empty()) {
    sky.push_back(Embedded(last_sky_));
  }
  o["tpv"] = std::move(tpv);
  o["gst"] = std::move(gst);
  o["sky"] = std::move(sky);
  return o.dump() + "\r\n";
}

void GpsdServer::ToWatchers(bool json, bool nmea, std::string_view data) {
  for (const auto id : clients_.Ids()) {
    auto it = watch_.find(id);
    if (it == watch_.end() || !it->second.enable) {
      continue;
    }
    if ((json && it->second.json) || (nmea && it->second.nmea)) {
      clients_.Send(id, data);  // may drop the client; ids are re-checked
    }
  }
}

void GpsdServer::PublishFix(const Fix& fix) {
  last_fix_ = fix;
  ToWatchers(true, false, TpvJson(fix, device_));
  const std::string gst = GstJson(fix, device_);
  if (!gst.empty()) {
    ToWatchers(true, false, gst);
  }
}

void GpsdServer::PublishNmea(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.remove_suffix(1);
  }
  if (line.empty()) {
    return;
  }
  std::string out(line);
  out += "\r\n";
  ToWatchers(false, true, out);
}

void GpsdServer::PublishSky(const std::string& sky_json) {
  last_sky_ = sky_json;
  ToWatchers(true, false, sky_json);
}

void GpsdServer::SetActive(bool active, int baud) {
  if (active == active_ && baud == baud_) {
    return;
  }
  const bool lost = active_ && !active;
  active_ = active;
  baud_ = baud;
  ToWatchers(true, false, DevicesJson(device_, active_, baud_));
  if (lost) {
    last_fix_.reset();
    ToWatchers(true, false, NoFixTpvJson(device_));
  }
}

}  // namespace lgrtk::daemon
