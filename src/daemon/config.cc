// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#define TOML_EXCEPTIONS 0  // NOLINT(cppcoreguidelines-macro-usage)
#include <toml++/toml.hpp>

#include "internal/host_port.h"

namespace lgrtk::daemon {
namespace {

// Reads typed values out of one table and remembers the first error.
class Reader {
 public:
  Reader(const toml::table& root, std::string* err) : root_(root), err_(err) {}

  [[nodiscard]] bool Ok() const { return ok_; }

  // Rejects tables and keys outside `allowed` ("table" -> keys).
  void CheckKeys(
      const std::vector<std::pair<std::string, std::vector<std::string>>>&
          allowed) {
    for (const auto& [name, node] : root_) {
      const std::string table(name.str());
      const auto* keys = Find(allowed, table);
      if (keys == nullptr) {
        Fail("unknown table [" + table + "]");
        return;
      }
      const auto* tbl = node.as_table();
      if (tbl == nullptr) {
        Fail("'" + table + "' must be a table");
        return;
      }
      for (const auto& [key, unused] : *tbl) {
        const std::string k(key.str());
        bool known = false;
        for (const auto& a : *keys) {
          known = known || a == k;
        }
        if (!known) {
          Fail("unknown key " + table + "." + k);
          return;
        }
      }
    }
  }

  void Get(const char* table, const char* key, std::string* out) {
    const auto* node = Node(table, key);
    if (node == nullptr) {
      return;
    }
    if (const auto* v = node->as_string()) {
      *out = v->get();
    } else {
      Fail(std::string(table) + "." + key + " must be a string");
    }
  }
  void Get(const char* table, const char* key, int* out, int64_t lo,
           int64_t hi) {
    const auto* node = Node(table, key);
    if (node == nullptr) {
      return;
    }
    const auto* v = node->as_integer();
    if (v == nullptr) {
      Fail(std::string(table) + "." + key + " must be an integer");
    } else if (v->get() < lo || v->get() > hi) {
      Fail(std::string(table) + "." + key + " out of range " +
           std::to_string(lo) + ".." + std::to_string(hi));
    } else {
      *out = static_cast<int>(v->get());
    }
  }
  void Get(const char* table, const char* key, bool* out) {
    const auto* node = Node(table, key);
    if (node == nullptr) {
      return;
    }
    if (const auto* v = node->as_boolean()) {
      *out = v->get();
    } else {
      Fail(std::string(table) + "." + key + " must be true or false");
    }
  }

  void Fail(std::string msg) {
    if (ok_) {
      *err_ = std::move(msg);
      ok_ = false;
    }
  }

 private:
  static const std::vector<std::string>* Find(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& v,
      const std::string& name) {
    for (const auto& [n, keys] : v) {
      if (n == name) {
        return &keys;
      }
    }
    return nullptr;
  }
  const toml::node* Node(const char* table, const char* key) const {
    const auto* tbl = root_[table].as_table();
    return tbl == nullptr ? nullptr : tbl->get(key);
  }

  const toml::table& root_;
  std::string* err_;
  bool ok_ = true;
};

void CheckListen(Reader* r, const char* name, const std::string& value) {
  std::string host;
  int port = 0;
  if (!value.empty() && !ParseHostPort(value, &host, &port)) {
    r->Fail(std::string(name) + " must be host:port, got '" + value + "'");
  }
}

}  // namespace

std::optional<DaemonConfig> ParseConfig(std::string_view text,
                                        std::string* err) {
  toml::parse_result parsed = toml::parse(text);
  if (!parsed) {
    const auto& e = parsed.error();
    std::ostringstream os;
    os << "line " << e.source().begin.line << ": " << e.description();
    *err = os.str();
    return std::nullopt;
  }
  const toml::table& root = parsed.table();
  Reader r(root, err);
  r.CheckKeys({
      {"receiver", {"port", "baud", "apply_config"}},
      {"gpsd", {"listen", "device_name"}},
      {"raw", {"listen"}},
      {"chrony", {"sock"}},
      {"control", {"sock"}},
      {"ntrip",
       {"enabled", "host", "port", "mountpoint", "user", "password_file",
        "version", "send_gga"}},
  });
  DaemonConfig c;
  r.Get("receiver", "port", &c.receiver.port);
  r.Get("receiver", "baud", &c.receiver.baud, 4800, 3000000);
  r.Get("receiver", "apply_config", &c.receiver.apply_config);
  r.Get("gpsd", "listen", &c.gpsd_listen);
  r.Get("gpsd", "device_name", &c.device_name);
  r.Get("raw", "listen", &c.raw_listen);
  r.Get("chrony", "sock", &c.chrony_sock);
  r.Get("control", "sock", &c.control_sock);
  r.Get("ntrip", "enabled", &c.ntrip.enabled);
  r.Get("ntrip", "host", &c.ntrip.host);
  r.Get("ntrip", "port", &c.ntrip.port, 1, 65535);
  r.Get("ntrip", "mountpoint", &c.ntrip.mountpoint);
  r.Get("ntrip", "user", &c.ntrip.user);
  r.Get("ntrip", "password_file", &c.ntrip.password_file);
  r.Get("ntrip", "version", &c.ntrip.version, 1, 2);
  r.Get("ntrip", "send_gga", &c.ntrip.send_gga);
  if (c.gpsd_listen.empty()) {
    r.Fail("gpsd.listen must not be empty");
  }
  CheckListen(&r, "gpsd.listen", c.gpsd_listen);
  CheckListen(&r, "raw.listen", c.raw_listen);
  if (c.receiver.port.empty()) {
    r.Fail("receiver.port must not be empty");
  }
  if (c.ntrip.enabled && c.ntrip.host.empty()) {
    r.Fail("ntrip.host is required when ntrip.enabled = true");
  }
  if (c.ntrip.enabled && c.ntrip.mountpoint.empty()) {
    r.Fail("ntrip.mountpoint is required when ntrip.enabled = true");
  }
  if (!r.Ok()) {
    return std::nullopt;
  }
  if (c.device_name.empty()) {
    c.device_name = c.receiver.port;
  }
  return c;
}

std::optional<DaemonConfig> LoadConfig(const std::string& path,
                                       std::string* err) {
  std::ifstream in(path);
  if (!in) {
    *err = "cannot read " + path;
    return std::nullopt;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  auto c = ParseConfig(ss.str(), err);
  if (!c) {
    *err = path + ": " + *err;
  }
  return c;
}

std::optional<std::string> ReadNtripPassword(const NtripSection& ntrip,
                                             std::string* err) {
  if (!ntrip.password_file.empty()) {
    return internal::ReadFirstLine(ntrip.password_file, err);
  }
  const char* dir =
      std::getenv("CREDENTIALS_DIRECTORY");  // NOLINT(concurrency-mt-unsafe)
  if (dir != nullptr) {
    const std::string path = std::string(dir) + "/ntrip-password";
    if (std::ifstream(path)) {
      return internal::ReadFirstLine(path, err);
    }
  }
  return std::string();
}

}  // namespace lgrtk::daemon
