// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "daemon_config.h"
#include "doctest.h"
#include "persisted_state.h"
#include "shared_region.h"

using namespace isotone;
using namespace isotone::posix;

namespace {

// Names are per process, so a run collides neither with another run's leftovers
// nor with a developer's live daemon.
std::string unique(const std::string& stem) {
    return stem + "-test-" + std::to_string(::getpid());
}

std::string temp_dir() {
    return "/tmp/isotone-transport-test-" + std::to_string(::getpid());
}

void remove_tree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (dir != nullptr) {
        while (const dirent* entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            remove_tree(path + "/" + name);
        }
        ::closedir(dir);
        ::rmdir(path.c_str());
        return;
    }
    ::unlink(path.c_str());
}

}  // namespace

TEST_CASE("sanitize_key keeps a node name that is already safe") {
    CHECK(sanitize_key("alsa_output.pci-0000_00_1f.3.analog-stereo") ==
          "alsa_output.pci-0000_00_1f.3.analog-stereo");
    CHECK(sanitize_key("").empty());
}

TEST_CASE("sanitize_key replaces what an shm name cannot carry") {
    CHECK(sanitize_key("bluez_output/AA:BB:CC") == "bluez_output_AA_BB_CC");
    CHECK(sanitize_key("a b\tc") == "a_b_c");
}

TEST_CASE("sanitize_key keeps long names distinct") {
    const std::string a(400, 'x');
    const std::string b = a + "different";
    const std::string ka = sanitize_key(a);
    const std::string kb = sanitize_key(b);

    // Both fit an shm name, and truncation alone would have made them equal.
    CHECK(ka.size() <= 247);
    CHECK(kb.size() <= 247);
    CHECK(ka != kb);
    CHECK(region_name(a).size() <= 256);
}

TEST_CASE("region_name is the prefixed key") {
    CHECK(region_name("sink") == "/isotone.sink");
    CHECK(region_name("").empty());
}

TEST_CASE("a created region is valid and another process can open it") {
    const std::string name = region_name(unique("create"));
    SharedRegion::unlink_region(name);

    SharedRegion host;
    REQUIRE(host.create_or_open(name) == 0);
    CHECK(host.created());
    CHECK(host.is_open());
    REQUIRE(host.params() != nullptr);
    REQUIRE(host.ring() != nullptr);
    CHECK(host.params()->hdr.magic == kParamMagic);
    CHECK(host.params()->hdr.version == kParamVersion);

    param_block_write(host.params(), [](ParamBlock* b) { b->preamp_db = -3.5f; });

    SharedRegion client;
    REQUIRE(client.open(name) == 0);
    CHECK_FALSE(client.created());
    ParamBlock read{};
    REQUIRE(param_block_read(client.params(), &read));
    CHECK(read.preamp_db == doctest::Approx(-3.5));

    client.close();
    host.close();
    CHECK(SharedRegion::unlink_region(name) == 0);
}

TEST_CASE("seed fills a region this call creates") {
    const std::string name = region_name(unique("seed"));
    SharedRegion::unlink_region(name);

    SharedRegion host;
    float wanted = -9.0f;
    REQUIRE(host.create_or_open(
                name, [](ParamBlock* b, void* ctx) { b->preamp_db = *static_cast<float*>(ctx); },
                &wanted) == 0);
    CHECK(host.params()->preamp_db == doctest::Approx(-9.0));

    host.close();
    SharedRegion::unlink_region(name);
}

TEST_CASE("opening a region no daemon has made is ENOENT") {
    SharedRegion client;
    CHECK(client.open(region_name(unique("absent"))) == ENOENT);
    CHECK_FALSE(client.is_open());
    CHECK(client.open("") == EINVAL);
}

TEST_CASE("a stale region left by a killed daemon is taken back over") {
    const std::string name = region_name(unique("stale"));
    SharedRegion::unlink_region(name);

    SharedRegion first;
    REQUIRE(first.create_or_open(name) == 0);
    // What a daemon killed between shm_open and the header write leaves behind.
    first.params()->hdr.magic = 0;
    first.close();

    SharedRegion second;
    REQUIRE(second.create_or_open(name) == 0);
    CHECK(second.created());
    CHECK(second.params()->hdr.magic == kParamMagic);

    second.close();
    SharedRegion::unlink_region(name);
}

TEST_CASE("an existing good region is adopted, not re-initialised") {
    const std::string name = region_name(unique("adopt"));
    SharedRegion::unlink_region(name);

    SharedRegion first;
    REQUIRE(first.create_or_open(name) == 0);
    param_block_write(first.params(), [](ParamBlock* b) { b->preamp_db = -6.0f; });

    SharedRegion second;
    REQUIRE(second.create_or_open(name) == 0);
    CHECK_FALSE(second.created());
    CHECK(second.params()->preamp_db == doctest::Approx(-6.0));

    second.close();
    first.close();
    SharedRegion::unlink_region(name);
}

TEST_CASE("persisted_state_dir follows the XDG base directory specification") {
    const char* saved = std::getenv("XDG_CONFIG_HOME");
    const std::string keep = saved != nullptr ? saved : "";

    ::setenv("XDG_CONFIG_HOME", "/somewhere/config", 1);
    CHECK(persisted_state_dir() == "/somewhere/config/isotone/devices");

    // A relative XDG_CONFIG_HOME is invalid and must be ignored, not resolved
    // against the working directory.
    ::setenv("XDG_CONFIG_HOME", "relative/path", 1);
    ::setenv("HOME", "/home/someone", 1);
    CHECK(persisted_state_dir() == "/home/someone/.config/isotone/devices");

    ::unsetenv("XDG_CONFIG_HOME");
    CHECK(persisted_state_dir() == "/home/someone/.config/isotone/devices");

    if (saved != nullptr) ::setenv("XDG_CONFIG_HOME", keep.c_str(), 1);
}

TEST_CASE("the saved file and the shared region name the same sink") {
    CHECK(persisted_state_path("/d", "bluez_output/AA:BB") == "/d/bluez_output_AA_BB.bin");
    CHECK(persisted_state_path("/d", "").empty());
    CHECK(persisted_state_path("", "sink").empty());
}

TEST_CASE("a saved block reads back, and its host fields do not travel") {
    const std::string dir = temp_dir();
    remove_tree(dir);
    const std::string path = persisted_state_path(dir + "/devices", "sink");

    ParamBlock out{};
    CHECK(read_persisted_state(path, &out) == PersistedRead::Absent);

    ParamBlock block{};
    init_param_block(&block);
    block.preamp_db = -4.25f;
    block.band_count = 1;
    block.hdr.sample_rate = 48000;    // host fields: these must not survive
    block.hdr.host_heartbeat = 1234;

    REQUIRE(write_persisted_state(path, block) == 0);
    REQUIRE(read_persisted_state(path, &out) == PersistedRead::Loaded);
    CHECK(out.preamp_db == doctest::Approx(-4.25));
    CHECK(out.band_count == 1);
    CHECK(out.hdr.sample_rate == 0);
    CHECK(out.hdr.host_heartbeat == 0);
    CHECK(out.hdr.magic == kParamMagic);

    remove_tree(dir);
}

TEST_CASE("a file that is not a block for this build counts as never saved") {
    const std::string dir = temp_dir();
    remove_tree(dir);
    REQUIRE(::mkdir(dir.c_str(), 0700) == 0);
    const std::string path = dir + "/garbage.bin";

    FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    const char junk[64] = {};
    std::fwrite(junk, 1, sizeof(junk), f);
    std::fclose(f);

    ParamBlock out{};
    CHECK(read_persisted_state(path, &out) == PersistedRead::Invalid);

    remove_tree(dir);
}

TEST_CASE("the write is atomic: no temporary file is left behind") {
    const std::string dir = temp_dir();
    remove_tree(dir);
    const std::string path = persisted_state_path(dir, "sink");

    ParamBlock block{};
    init_param_block(&block);
    REQUIRE(write_persisted_state(path, block) == 0);

    struct stat st {};
    CHECK(::stat(path.c_str(), &st) == 0);
    CHECK(::stat((path + ".tmp").c_str(), &st) != 0);

    remove_tree(dir);
}

TEST_CASE("the daemon's channel count is kept in its config, and only counts it lays out") {
    const std::string dir = temp_dir() + "-config";
    remove_tree(dir);
    const std::string path = dir + "/isotone/daemon.conf";

    CHECK(read_daemon_channels(path) == 0);   // no file: the daemon's default
    REQUIRE(write_daemon_channels(path, 6) == 0);
    CHECK(read_daemon_channels(path) == 6);
    REQUIRE(write_daemon_channels(path, 3) == 0);   // 2.1
    CHECK(read_daemon_channels(path) == 3);

    // A count the daemon cannot lay out is refused, and the file keeps the last.
    CHECK(write_daemon_channels(path, 5) == EINVAL);
    CHECK(write_daemon_channels(path, 0) == EINVAL);
    CHECK(read_daemon_channels(path) == 3);

    // Other lines survive a write; a bad value reads as none.
    {
        FILE* f = std::fopen(path.c_str(), "w");
        REQUIRE(f != nullptr);
        std::fputs("# kept\nchannels=7\n", f);
        std::fclose(f);
    }
    CHECK(read_daemon_channels(path) == 0);
    REQUIRE(write_daemon_channels(path, 8) == 0);
    CHECK(read_daemon_channels(path) == 8);
    {
        FILE* f = std::fopen(path.c_str(), "r");
        REQUIRE(f != nullptr);
        char text[128] = {};
        const size_t n = std::fread(text, 1, sizeof(text) - 1, f);
        std::fclose(f);
        CHECK(std::string(text, n) == "# kept\nchannels=8\n");
    }
    remove_tree(dir);
}

TEST_CASE("the daemon's config sits beside its saved state") {
    const char* saved = std::getenv("XDG_CONFIG_HOME");
    const std::string keep = saved != nullptr ? saved : "";
    ::setenv("XDG_CONFIG_HOME", "/somewhere/config", 1);
    CHECK(daemon_config_path() == "/somewhere/config/isotone/daemon.conf");
    CHECK(persisted_state_dir() == "/somewhere/config/isotone/devices");
    if (saved != nullptr) ::setenv("XDG_CONFIG_HOME", keep.c_str(), 1);
    else ::unsetenv("XDG_CONFIG_HOME");
}
