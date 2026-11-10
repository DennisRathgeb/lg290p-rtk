// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// lgrtk: command-line tool for the Quectel LG290P.
//
// Talks to the receiver directly over its serial port (one owner at a time).
// Read-only unless a command says otherwise: probe, watch and status never
// change the receiver's configuration.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lgrtk/lg290p.h"
#include "lgrtk/nmea.h"
#include "lgrtk/ntrip.h"
#include "lgrtk/receiver.h"
#include "lgrtk/version.h"
#ifdef LGRTK_HAVE_PROJ
#include "lgrtk/projection.h"
#endif

namespace {

using lgrtk::Fix;
using lgrtk::Lg290pReceiver;
using lgrtk::ReceiverOptions;
using lgrtk::State;
namespace lg = lgrtk::lg290p;

constexpr int kUsage = 2;
constexpr int kRuntime = 1;

std::atomic<bool> g_interrupted{false};
void OnSignal(int) { g_interrupted = true; }

const char kHelp[] = R"(lgrtk - Quectel LG290P RTK receiver tool

usage: lgrtk [--port PATH] [--baud N] <command> [args]

  --port PATH   serial device (default /dev/ttyACM0, or $LGRTK_PORT)
  --baud N      line speed tried first (default 460800; others are detected)

commands (read-only):
  probe [--seconds N] [--raw FILE]    what the receiver sends: rates, fix, firmware
  watch [--count N]                   one line per fix
  status [--seconds N]                state machine events and counters
  version                             tool version

commands that change the receiver:
  config get <key> [MSG]              key: fix-rate | msg-rate MSG | rtk | elevation-mask |
                                           cnr-mask | decimals | constellations | mode | pps
  config set <key> <values...>        fix-rate MS | msg-rate MSG N [VER] | rtk DIFF REL TIMEOUT |
                                      elevation-mask DEG | cnr-mask DBHZ | decimals U P A D S C |
                                      constellations GPS GLO GAL BDS QZSS NAVIC (0/1)
                                      (saved to flash; fix-rate/constellations need `reset soft`)
  config apply-default                10 Hz, GGA/RMC/GST/EPE, high-precision NMEA, RTK auto
  reset hot|warm|cold|soft            restart the receiver
  reset factory --yes                 factory defaults (then reset soft)
  base survey-in SECONDS ACC_M        become a base: survey-in
  base fixed X Y Z                    become a base at an ECEF position (metres)
  base rover                          back to rover mode

corrections and coordinates:
  ntrip sourcetable HOST[:PORT] [--v1]
  ntrip run --host H [--port-ntrip P] --mount M [--user U] [--password-file F] [--v1] [--no-gga]
                                      receiver + NTRIP corrections, one line per fix
  project --from CRS --to CRS LAT LON H   e.g. --from EPSG:4937 --to EPSG:2056+5729

exit codes: 0 ok, 1 runtime error, 2 usage error
)";

int Usage(const std::string& msg) {
  std::cerr << "lgrtk: " << msg << "\n(run `lgrtk --help`)\n";
  return kUsage;
}

struct Args {
  std::vector<std::string> v;
  size_t i = 0;
  bool Has() const { return i < v.size(); }
  std::string Next() { return i < v.size() ? v[i++] : std::string(); }
  std::string Peek() const { return i < v.size() ? v[i] : std::string(); }
  /// Removes "--name value" anywhere after the current position.
  std::optional<std::string> Option(const std::string& name) {
    for (size_t k = i; k + 1 < v.size(); ++k) {
      if (v[k] == name) {
        std::string val = v[k + 1];
        v.erase(v.begin() + static_cast<long>(k), v.begin() + static_cast<long>(k) + 2);
        return val;
      }
    }
    return std::nullopt;
  }
  bool Flag(const std::string& name) {
    for (size_t k = i; k < v.size(); ++k) {
      if (v[k] == name) {
        v.erase(v.begin() + static_cast<long>(k));
        return true;
      }
    }
    return false;
  }
};

bool ToInt(const std::string& s, int& out) {
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (s.empty() || *end != '\0') return false;
  out = static_cast<int>(v);
  return true;
}

bool ToDouble(const std::string& s, double& out) {
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return !s.empty() && *end == '\0';
}

std::string FormatFix(const Fix& f) {
  char buf[256];
  const double t = f.utc_s;
  std::snprintf(buf, sizeof buf,
                "%02d:%02d:%06.3f %-10s %13.9f %14.9f %9.3f  sigma %6.3f %6.3f %6.3f (%s)  sats %2u  "
                "hdop %4.1f  age %4.1f",
                static_cast<int>(t / 3600), static_cast<int>(t / 60) % 60,
                t - 60.0 * static_cast<int>(t / 60), lgrtk::ToString(f.type), f.lat_deg, f.lon_deg,
                f.h_ell_m, static_cast<double>(f.sigma_e_m), static_cast<double>(f.sigma_n_m),
                static_cast<double>(f.sigma_u_m),
                f.sigma_source == lgrtk::SigmaSource::kGst   ? "gst"
                : f.sigma_source == lgrtk::SigmaSource::kEpe ? "epe"
                                                             : "-",
                f.sats, static_cast<double>(f.hdop), static_cast<double>(f.diff_age_s));
  return buf;
}

/// Starts a receiver and waits until it streams (or fails).
bool StartReceiver(Lg290pReceiver& rx, int timeout_s = 15) {
  if (!rx.Start()) return false;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  while (std::chrono::steady_clock::now() < end && !g_interrupted) {
    const auto st = rx.Status();
    if (st.state == State::kStreaming) return true;
    if (st.state == State::kFault) {
      std::cerr << "lgrtk: receiver fault: " << st.last_error << "\n";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const auto st = rx.Status();
  std::cerr << "lgrtk: receiver not ready (state " << lgrtk::ToString(st.state) << ")"
            << (st.last_error.empty() ? "" : ": " + st.last_error) << "\n";
  return false;
}

std::optional<lgrtk::nmea::OwnedReply> Ask(Lg290pReceiver& rx, lg::Command cmd) {
  auto fut = rx.Send(std::move(cmd));
  if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return std::nullopt;
  return fut.get();
}

int CmdProbe(Args& a, ReceiverOptions o) {
  int seconds = 60;
  if (auto s = a.Option("--seconds"); s && !ToInt(*s, seconds)) return Usage("--seconds needs a number");
  const auto raw = a.Option("--raw");
  if (a.Has()) return Usage("probe: unexpected argument " + a.Peek());
  o.apply_config = false;
  Lg290pReceiver rx(o);
  std::optional<lgrtk::RawRecorder> rec;
  if (raw) {
    std::string err;
    rec = lgrtk::RawRecorder::Open(*raw, err);
    if (!rec) {
      std::cerr << "lgrtk: " << err << "\n";
      return kRuntime;
    }
    rx.OnRaw([&](std::span<const uint8_t> b) { rec->Write(b); });
  }
  std::mutex mu;
  std::map<std::string, int> types;
  std::map<int, int> quality;
  std::optional<lgrtk::nmea::Gga> last_gga;
  double first_utc = -1;
  double last_utc = -1;
  int backwards = 0;
  rx.OnNmea([&](std::string_view line) {
    const auto body = lgrtk::nmea::Validate(line);
    std::string type(lgrtk::nmea::SentenceType(body));
    if (type.empty()) return;
    std::lock_guard lk(mu);
    ++types[type];
    if (type == "GGA") {
      if (auto g = lgrtk::nmea::ParseGga(body)) {
        ++quality[g->quality];
        last_gga = g;
        if (first_utc < 0) first_utc = g->utc_s;
        if (last_utc >= 0 && g->utc_s < last_utc) ++backwards;
        last_utc = g->utc_s;
      }
    }
  });
  if (!StartReceiver(rx)) return kRuntime;
  {
    std::lock_guard lk(mu);
    types.clear();
    quality.clear();
    first_utc = last_utc = -1;
    backwards = 0;
  }
  const auto t0 = std::chrono::steady_clock::now();
  while (!g_interrupted && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const auto st = rx.Status();
  rx.Stop();
  std::lock_guard lk(mu);
  std::printf("port: %s at %d baud\n", st.port.c_str(), st.baud);
  std::printf("firmware: %s\n", st.firmware.c_str());
  std::printf("bytes %llu, NMEA %llu, RTCM %llu, bad checksum %llu, bad CRC %llu\n",
              static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.framer.nmea),
              static_cast<unsigned long long>(st.framer.rtcm),
              static_cast<unsigned long long>(st.framer.bad_nmea),
              static_cast<unsigned long long>(st.framer.bad_rtcm));
  std::printf("sentences:");
  for (const auto& [t, n] : types) std::printf(" %s %.1f Hz", t.c_str(), n / dt);
  std::printf("\nGGA quality:");
  for (const auto& [q, n] : quality) {
    std::printf(" %s x%d", lgrtk::ToString(lgrtk::FixTypeFromGgaQuality(static_cast<uint8_t>(q))), n);
  }
  if (last_gga) {
    std::printf("\nlast GGA: sats %u, hdop %.2f, diff age %s\n", last_gga->sats, last_gga->hdop,
                last_gga->has_diff_age ? std::to_string(last_gga->diff_age_s).c_str() : "none");
  } else {
    std::printf("\nlast GGA: none\n");
  }
  std::printf("GST: %s\n", types.count("GST") ? "present" : "not seen (enable: lgrtk config set msg-rate GST 1)");
  std::printf("PQTMEPE: %s\n", types.count("PQTMEPE") ? "present" : "not seen");
  std::printf("UTC span %.1f s, backwards steps %d\n", last_utc >= 0 ? last_utc - first_utc : 0.0, backwards);
  if (rec) std::printf("raw: %llu bytes written to %s\n", static_cast<unsigned long long>(rec->bytes()), raw->c_str());
  return 0;
}

int CmdWatch(Args& a, ReceiverOptions o) {
  int count = 0;
  if (auto s = a.Option("--count"); s && !ToInt(*s, count)) return Usage("--count needs a number");
  if (a.Has()) return Usage("watch: unexpected argument " + a.Peek());
  o.apply_config = false;
  Lg290pReceiver rx(o);
  std::atomic<int> seen{0};
  rx.OnFix([&](const Fix& f) {
    if (count > 0 && seen >= count) return;
    std::printf("%s\n", FormatFix(f).c_str());
    std::fflush(stdout);
    ++seen;
  });
  rx.OnStatus([](const lgrtk::StatusEvent& e) {
    std::fprintf(stderr, "[%s -> %s] %s\n", lgrtk::ToString(e.from), lgrtk::ToString(e.to), e.reason.c_str());
  });
  if (!rx.Start()) return kRuntime;
  while (!g_interrupted && (count == 0 || seen < count)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  rx.Stop();
  return 0;
}

int CmdStatus(Args& a, ReceiverOptions o) {
  int seconds = 5;
  if (auto s = a.Option("--seconds"); s && !ToInt(*s, seconds)) return Usage("--seconds needs a number");
  o.apply_config = false;
  Lg290pReceiver rx(o);
  rx.OnStatus([](const lgrtk::StatusEvent& e) {
    std::printf("[%s -> %s] %s\n", lgrtk::ToString(e.from), lgrtk::ToString(e.to), e.reason.c_str());
    std::fflush(stdout);
  });
  if (!rx.Start()) return kRuntime;
  const auto t0 = std::chrono::steady_clock::now();
  while (!g_interrupted && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const auto st = rx.Status();
  rx.Stop();
  std::printf("state %s, firmware %s, baud %d, bytes %llu, epochs %llu, reconnects %llu, last fix age %.2f s\n",
              lgrtk::ToString(st.state), st.firmware.c_str(), st.baud,
              static_cast<unsigned long long>(st.bytes), static_cast<unsigned long long>(st.epochs),
              static_cast<unsigned long long>(st.reconnects),
              st.last_fix_age_ns < 0 ? -1.0 : static_cast<double>(st.last_fix_age_ns) / 1e9);
  if (!st.last_error.empty()) std::printf("last error: %s\n", st.last_error.c_str());
  return st.state == State::kFault ? kRuntime : 0;
}

/// Sends commands one by one and reports replies; returns an exit code.
int RunCommands(Lg290pReceiver& rx, const std::vector<lg::Command>& cmds) {
  for (const auto& c : cmds) {
    if (c.reply_to.empty()) {
      (void)Ask(rx, c);
      std::printf("sent %s", c.text.c_str());
      continue;
    }
    const auto r = Ask(rx, c);
    if (!r) {
      std::cerr << "lgrtk: no reply to " << c.text;
      return kRuntime;
    }
    if (!r->ok) {
      std::cerr << "lgrtk: receiver refused " << c.text << "  (error " << r->error_code
                << ": 1 invalid parameters, 2 failed, 3 unsupported)\n";
      return kRuntime;
    }
    std::printf("%s", r->command.c_str());
    for (const auto& v : r->values) std::printf(" %s", v.c_str());
    std::printf("\n");
  }
  return 0;
}

int CmdConfig(Args& a, ReceiverOptions o) {
  const std::string op = a.Next();
  const std::string key = a.Next();
  std::vector<std::string> vals;
  while (a.Has()) vals.push_back(a.Next());
  std::vector<int> n(vals.size());
  bool all_int = true;
  for (size_t i = 0; i < vals.size(); ++i) all_int = ToInt(vals[i], n[i]) && all_int;
  std::vector<lg::Command> cmds;
  if (op == "get") {
    if (key == "fix-rate") cmds.push_back(lg::GetFixRate());
    else if (key == "msg-rate" && !vals.empty())
      cmds.push_back(lg::GetMessageRate(vals[0], vals.size() > 1 ? std::atoi(vals[1].c_str()) : 0));
    else if (key == "rtk") cmds.push_back(lg::GetRtk());
    else if (key == "elevation-mask") cmds.push_back(lg::GetElevationMask());
    else if (key == "cnr-mask") cmds.push_back(lg::GetCnrMask());
    else if (key == "decimals") cmds.push_back(lg::GetNmeaDecimals());
    else if (key == "constellations") cmds.push_back(lg::GetConstellations());
    else if (key == "mode") cmds.push_back(lg::GetReceiverMode());
    else if (key == "pps") cmds.push_back(lg::GetPps());
    else return Usage("config get: unknown key '" + key + "'");
  } else if (op == "set") {
    std::optional<lg::Command> c;
    double d = 0;
    if (key == "fix-rate" && vals.size() == 1 && all_int) c = lg::SetFixRate(n[0]);
    else if (key == "msg-rate" && (vals.size() == 2 || vals.size() == 3) && ToInt(vals[1], n[1]))
      c = lg::SetMessageRate(vals[0], n[1], vals.size() == 3 ? std::atoi(vals[2].c_str()) : 0);
    else if (key == "rtk" && vals.size() == 3 && all_int) c = lg::SetRtk({n[0], n[1], n[2]});
    else if (key == "elevation-mask" && vals.size() == 1 && ToDouble(vals[0], d)) c = lg::SetElevationMask(d);
    else if (key == "cnr-mask" && vals.size() == 1 && ToDouble(vals[0], d)) c = lg::SetCnrMask(d);
    else if (key == "decimals" && vals.size() == 6 && all_int)
      c = lg::SetNmeaDecimals({n[0], n[1], n[2], n[3], n[4], n[5]});
    else if (key == "constellations" && vals.size() == 6 && all_int)
      c = lg::SetConstellations({n[0] != 0, n[1] != 0, n[2] != 0, n[3] != 0, n[4] != 0, n[5] != 0});
    else return Usage("config set: bad key or values for '" + key + "'");
    if (!c) return Usage("config set " + key + ": value out of range");
    cmds = {*c, lg::SaveParams()};
    if (c->needs_reset) std::fprintf(stderr, "note: %s takes effect after `lgrtk reset soft`\n", key.c_str());
  } else if (op == "apply-default") {
    o.config = lg::DefaultRoverConfig();
    o.apply_config = true;
    Lg290pReceiver rx(o);
    if (!StartReceiver(rx, 30)) return kRuntime;
    const auto st = rx.Status();
    std::printf("default rover configuration applied (%s)\n", st.last_error.empty() ? "ok" : st.last_error.c_str());
    return 0;
  } else {
    return Usage("config: expected get, set or apply-default");
  }
  o.apply_config = false;
  Lg290pReceiver rx(o);
  if (!StartReceiver(rx)) return kRuntime;
  return RunCommands(rx, cmds);
}

int CmdReset(Args& a, ReceiverOptions o) {
  const bool yes = a.Flag("--yes");
  const std::string kind = a.Next();
  std::vector<lg::Command> cmds;
  if (kind == "hot") cmds = {lg::Restart(lg::Start::kHot)};
  else if (kind == "warm") cmds = {lg::Restart(lg::Start::kWarm)};
  else if (kind == "cold") cmds = {lg::Restart(lg::Start::kCold)};
  else if (kind == "soft") cmds = {lg::SoftwareReset()};
  else if (kind == "factory") {
    if (!yes) return Usage("reset factory erases the receiver's saved configuration: add --yes");
    cmds = {lg::RestoreDefaults(), lg::SoftwareReset()};
  } else {
    return Usage("reset: expected hot, warm, cold, soft or factory");
  }
  o.apply_config = false;
  Lg290pReceiver rx(o);
  if (!StartReceiver(rx)) return kRuntime;
  return RunCommands(rx, cmds);
}

int CmdBase(Args& a, ReceiverOptions o) {
  const std::string mode = a.Next();
  std::vector<lg::Command> cmds;
  if (mode == "survey-in") {
    int s = 0;
    double acc = 0;
    if (!ToInt(a.Next(), s) || !ToDouble(a.Next(), acc)) return Usage("base survey-in SECONDS ACC_M");
    auto c = lg::SurveyIn(s, acc);
    if (!c) return Usage("base survey-in: values out of range");
    cmds = {lg::SetReceiverMode(lg::Mode::kBase), *c};
  } else if (mode == "fixed") {
    double x = 0, y = 0, z = 0;
    if (!ToDouble(a.Next(), x) || !ToDouble(a.Next(), y) || !ToDouble(a.Next(), z)) return Usage("base fixed X Y Z");
    cmds = {lg::SetReceiverMode(lg::Mode::kBase), lg::FixedBase(x, y, z)};
  } else if (mode == "rover") {
    cmds = {lg::DisableSurvey(), lg::SetReceiverMode(lg::Mode::kRover)};
  } else {
    return Usage("base: expected survey-in, fixed or rover");
  }
  cmds.push_back(lg::SaveParams());
  cmds.push_back(lg::SoftwareReset());
  o.apply_config = false;
  Lg290pReceiver rx(o);
  if (!StartReceiver(rx)) return kRuntime;
  const int rc = RunCommands(rx, cmds);
  if (rc == 0 && mode != "rover") std::printf("base mode set; watch progress with `lgrtk status` (PQTMSVINSTATUS)\n");
  return rc;
}

int CmdNtrip(Args& a, ReceiverOptions o) {
  const std::string op = a.Next();
  const bool v1 = a.Flag("--v1");
  if (op == "sourcetable") {
    std::string hp = a.Next();
    if (hp.empty()) return Usage("ntrip sourcetable HOST[:PORT]");
    int port = 2101;
    if (const size_t c = hp.rfind(':'); c != std::string::npos) {
      if (!ToInt(hp.substr(c + 1), port)) return Usage("bad port in " + hp);
      hp = hp.substr(0, c);
    }
    std::string err;
    const auto t = lgrtk::NtripClient::FetchSourcetable(hp, port, v1 ? 1 : 2, err);
    if (!t) {
      std::cerr << "lgrtk: " << err << "\n";
      return kRuntime;
    }
    std::cout << *t;
    return 0;
  }
  if (op != "run") return Usage("ntrip: expected sourcetable or run");
  lgrtk::NtripOptions no;
  no.version = v1 ? 1 : 2;
  no.send_gga = !a.Flag("--no-gga");
  if (auto h = a.Option("--host")) no.host = *h;
  if (auto p = a.Option("--port-ntrip"); p && !ToInt(*p, no.port)) return Usage("--port-ntrip needs a number");
  if (auto m = a.Option("--mount")) no.mountpoint = *m;
  if (auto u = a.Option("--user")) no.user = *u;
  if (auto f = a.Option("--password-file")) {
    std::ifstream in(*f);
    if (!in || !std::getline(in, no.password)) {
      std::cerr << "lgrtk: cannot read password file " << *f << "\n";
      return kRuntime;
    }
  } else if (const char* env = std::getenv("LGRTK_NTRIP_PASSWORD")) {
    no.password = env;
  }
  if (no.host.empty() || no.mountpoint.empty()) return Usage("ntrip run needs --host and --mount");
  o.config = lg::DefaultRoverConfig();
  Lg290pReceiver rx(o);
  lgrtk::NtripClient nc(no, rx);
  rx.OnFix([&](const Fix& f) {
    nc.SetPosition(f);
    const auto ns = nc.Status();
    std::printf("%s  ntrip %s %.1f s\n", FormatFix(f).c_str(), lgrtk::ToString(ns.state),
                ns.last_data_age_ns < 0 ? -1.0 : static_cast<double>(ns.last_data_age_ns) / 1e9);
    std::fflush(stdout);
  });
  rx.OnStatus([](const lgrtk::StatusEvent& e) {
    std::fprintf(stderr, "[receiver %s -> %s] %s\n", lgrtk::ToString(e.from), lgrtk::ToString(e.to), e.reason.c_str());
  });
  nc.OnStatus([](lgrtk::NtripState from, lgrtk::NtripState to, const std::string& why) {
    std::fprintf(stderr, "[ntrip %s -> %s] %s\n", lgrtk::ToString(from), lgrtk::ToString(to), why.c_str());
  });
  if (!rx.Start() || !nc.Start()) return kRuntime;
  while (!g_interrupted) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  nc.Stop();
  rx.Stop();
  return 0;
}

int CmdProject(Args& a) {
#ifdef LGRTK_HAVE_PROJ
  const auto from = a.Option("--from");
  const auto to = a.Option("--to");
  double lat = 0, lon = 0, h = 0;
  if (!from || !to || !ToDouble(a.Next(), lat) || !ToDouble(a.Next(), lon) || !ToDouble(a.Next(), h)) {
    return Usage("project --from CRS --to CRS LAT LON H");
  }
  std::string err;
  auto p = lgrtk::Projection::Create(*from, *to, err);
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
  return 0;
#else
  (void)a;
  std::cerr << "lgrtk: built without PROJ\n";
  return kRuntime;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  Args a;
  for (int i = 1; i < argc; ++i) a.v.emplace_back(argv[i]);
  if (a.v.empty() || a.Flag("--help") || a.Flag("-h")) {
    std::cout << kHelp;
    return a.v.empty() ? kUsage : 0;
  }
  ReceiverOptions o;
  if (const char* env = std::getenv("LGRTK_PORT")) o.port = env;
  if (auto p = a.Option("--port")) o.port = *p;
  if (auto b = a.Option("--baud"); b && !ToInt(*b, o.baud)) return Usage("--baud needs a number");
  const std::string cmd = a.Next();
  if (cmd == "version") {
    std::printf("lgrtk %s\n", lgrtk::kVersion);
    return 0;
  }
  if (cmd == "probe") return CmdProbe(a, o);
  if (cmd == "watch") return CmdWatch(a, o);
  if (cmd == "status") return CmdStatus(a, o);
  if (cmd == "config") return CmdConfig(a, o);
  if (cmd == "reset") return CmdReset(a, o);
  if (cmd == "base") return CmdBase(a, o);
  if (cmd == "ntrip") return CmdNtrip(a, o);
  if (cmd == "project") return CmdProject(a);
  return Usage("unknown command '" + cmd + "'");
}
