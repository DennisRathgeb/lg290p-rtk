// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/config.h"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace lgrtk::daemon {
namespace {

TEST(ConfigTest, EmptyFileGivesDefaults) {
  std::string err;
  auto c = ParseConfig("", &err);
  ASSERT_TRUE(c) << err;
  EXPECT_EQ(c->receiver.port, "/dev/ttyACM0");
  EXPECT_EQ(c->receiver.baud, 460800);
  EXPECT_EQ(c->gpsd_listen, "127.0.0.1:2947");
  EXPECT_EQ(c->raw_listen, "");
  EXPECT_EQ(c->chrony_sock, "");
  EXPECT_EQ(c->control_sock, "/run/lgrtkd/control.sock");
  EXPECT_EQ(c->device_name, "/dev/ttyACM0");
  EXPECT_FALSE(c->ntrip.enabled);
}

TEST(ConfigTest, ParsesAllSections) {
  std::string err;
  auto c = ParseConfig(R"(
[receiver]
port = "/dev/lg290p0"
baud = 921600
apply_config = false
[gpsd]
listen = "0.0.0.0:2948"
device_name = "lg290p"
[raw]
listen = "127.0.0.1:5000"
[chrony]
sock = "/run/chrony.lgrtk.sock"
[control]
sock = "/tmp/c.sock"
[ntrip]
enabled = true
host = "www.swipos.ch"
port = 2102
mountpoint = "MSM_GISGEO_LV95LHN95"
user = "me"
password_file = "/etc/lgrtk/ntrip-password"
version = 1
send_gga = false
)",
                       &err);
  ASSERT_TRUE(c) << err;
  EXPECT_EQ(c->receiver.port, "/dev/lg290p0");
  EXPECT_EQ(c->receiver.baud, 921600);
  EXPECT_FALSE(c->receiver.apply_config);
  EXPECT_EQ(c->gpsd_listen, "0.0.0.0:2948");
  EXPECT_EQ(c->device_name, "lg290p");
  EXPECT_EQ(c->raw_listen, "127.0.0.1:5000");
  EXPECT_EQ(c->chrony_sock, "/run/chrony.lgrtk.sock");
  EXPECT_EQ(c->control_sock, "/tmp/c.sock");
  EXPECT_TRUE(c->ntrip.enabled);
  EXPECT_EQ(c->ntrip.host, "www.swipos.ch");
  EXPECT_EQ(c->ntrip.port, 2102);
  EXPECT_EQ(c->ntrip.mountpoint, "MSM_GISGEO_LV95LHN95");
  EXPECT_EQ(c->ntrip.user, "me");
  EXPECT_EQ(c->ntrip.password_file, "/etc/lgrtk/ntrip-password");
  EXPECT_EQ(c->ntrip.version, 1);
  EXPECT_FALSE(c->ntrip.send_gga);
}

TEST(ConfigTest, UnknownKeyIsAnError) {
  std::string err;
  EXPECT_FALSE(ParseConfig("[receiver]\nprot = \"/dev/x\"\n", &err));
  EXPECT_NE(err.find("receiver.prot"), std::string::npos) << err;
  EXPECT_FALSE(ParseConfig("[recevier]\nport = \"/dev/x\"\n", &err));
  EXPECT_NE(err.find("recevier"), std::string::npos) << err;
}

TEST(ConfigTest, WrongTypeIsAnError) {
  std::string err;
  EXPECT_FALSE(ParseConfig("[receiver]\nbaud = \"fast\"\n", &err));
  EXPECT_NE(err.find("receiver.baud"), std::string::npos) << err;
}

TEST(ConfigTest, SyntaxErrorNamesTheLine) {
  std::string err;
  EXPECT_FALSE(ParseConfig("[receiver\n", &err));
  EXPECT_NE(err.find("line 1"), std::string::npos) << err;
}

TEST(ConfigTest, NtripEnabledNeedsHostAndMount) {
  std::string err;
  EXPECT_FALSE(ParseConfig("[ntrip]\nenabled = true\nhost = \"h\"\n", &err));
  EXPECT_NE(err.find("mountpoint"), std::string::npos) << err;
  EXPECT_TRUE(ParseConfig("[ntrip]\nenabled = false\nhost = \"h\"\n", &err));
}

TEST(ConfigTest, BadListenAddressIsAnError) {
  std::string err;
  EXPECT_FALSE(ParseConfig("[gpsd]\nlisten = \"nonsense\"\n", &err));
  EXPECT_NE(err.find("gpsd.listen"), std::string::npos) << err;
}

TEST(ConfigTest, PasswordFromFileStripsNewline) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lgrtk_cfg_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "pw") << "s3cret\n";
  NtripSection n;
  n.password_file = (dir / "pw").string();
  std::string err;
  auto pw = ReadNtripPassword(n, &err);
  ASSERT_TRUE(pw) << err;
  EXPECT_EQ(*pw, "s3cret");
  std::filesystem::remove_all(dir);
}

TEST(ConfigTest, PasswordFromCredentialsDirectory) {
  auto dir = std::filesystem::temp_directory_path() /
             ("lgrtk_cred_" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "ntrip-password") << "fromcred\r\n";
  setenv("CREDENTIALS_DIRECTORY", dir.c_str(), 1);
  NtripSection n;
  std::string err;
  auto pw = ReadNtripPassword(n, &err);
  unsetenv("CREDENTIALS_DIRECTORY");
  ASSERT_TRUE(pw) << err;
  EXPECT_EQ(*pw, "fromcred");
  std::filesystem::remove_all(dir);
}

TEST(ConfigTest, NoPasswordSourceGivesEmpty) {
  unsetenv("CREDENTIALS_DIRECTORY");
  NtripSection n;
  std::string err;
  auto pw = ReadNtripPassword(n, &err);
  ASSERT_TRUE(pw) << err;
  EXPECT_EQ(*pw, "");
}

TEST(ConfigTest, MissingPasswordFileIsAnError) {
  NtripSection n;
  n.password_file = "/nonexistent/lgrtk-pw";
  std::string err;
  EXPECT_FALSE(ReadNtripPassword(n, &err));
  EXPECT_NE(err.find("/nonexistent/lgrtk-pw"), std::string::npos) << err;
}

TEST(ConfigTest, ParseHostPort) {
  std::string host;
  int port = 0;
  EXPECT_TRUE(ParseHostPort("[::1]:2947", &host, &port));
  EXPECT_EQ(host, "::1");
  EXPECT_EQ(port, 2947);
  EXPECT_TRUE(ParseHostPort("0.0.0.0:1", &host, &port));
  EXPECT_EQ(host, "0.0.0.0");
  EXPECT_EQ(port, 1);
  EXPECT_FALSE(ParseHostPort(":2947", &host, &port));
  EXPECT_FALSE(ParseHostPort("host:", &host, &port));
  EXPECT_FALSE(ParseHostPort("host:70000", &host, &port));
  EXPECT_FALSE(ParseHostPort("host", &host, &port));
}

TEST(ConfigTest, ExampleFileParses) {
  std::string err;
  auto c = LoadConfig(LGRTK_SOURCE_DIR "/packaging/lgrtkd.toml", &err);
  ASSERT_TRUE(c) << err;
  EXPECT_EQ(c->receiver.port, "/dev/lg290p0");
}

TEST(ConfigTest, MissingFileIsAnError) {
  std::string err;
  EXPECT_FALSE(LoadConfig("/nonexistent/lgrtkd.toml", &err));
  EXPECT_NE(err.find("/nonexistent/lgrtkd.toml"), std::string::npos);
}

}  // namespace
}  // namespace lgrtk::daemon
