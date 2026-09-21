// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Launch at sign-in on Linux: the .desktop file under XDG's autostart directory.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "autostart_xdg.h"
#include "doctest.h"

using namespace isotone::ui;

namespace {

std::string scratch() {
    return "/tmp/isotone-autostart-" + std::to_string(::getpid());
}

std::string write_file(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path, std::ios::trunc) << text;
    return path;
}

}  // namespace

TEST_CASE("the directory follows the XDG base directory specification") {
    const char* saved = std::getenv("XDG_CONFIG_HOME");
    const std::string keep = saved != nullptr ? saved : "";

    ::setenv("XDG_CONFIG_HOME", "/somewhere/config", 1);
    CHECK(autostart_dir() == "/somewhere/config/autostart");

    // A relative XDG_CONFIG_HOME is invalid and must be ignored, not resolved.
    ::setenv("XDG_CONFIG_HOME", "relative/path", 1);
    ::setenv("HOME", "/home/someone", 1);
    CHECK(autostart_dir() == "/home/someone/.config/autostart");

    ::unsetenv("XDG_CONFIG_HOME");
    CHECK(autostart_dir() == "/home/someone/.config/autostart");

    if (saved != nullptr) ::setenv("XDG_CONFIG_HOME", keep.c_str(), 1);
}

TEST_CASE("the file is a desktop entry, and --tray is part of the command") {
    const std::string plain = autostart_contents("/usr/bin/isotone", false);
    CHECK(plain.find("[Desktop Entry]") == 0);
    CHECK(plain.find("Type=Application") != std::string::npos);
    CHECK(plain.find("Exec=/usr/bin/isotone\n") != std::string::npos);
    // Not pinned to one desktop: the entry is for GNOME, KDE and Cinnamon alike.
    CHECK(plain.find("OnlyShowIn") == std::string::npos);
    CHECK(plain.find("NotShowIn") == std::string::npos);

    const std::string tray = autostart_contents("/usr/bin/isotone", true);
    CHECK(tray.find("Exec=/usr/bin/isotone --tray\n") != std::string::npos);
}

TEST_CASE("a command the desktop would split or expand is quoted and escaped") {
    // The desktop entry specification: arguments are split at spaces, one with
    // a reserved character is quoted whole, and inside the quotes " ` $ and \ take
    // a backslash, which the string escape rule then doubles. A % is a field
    // code unless doubled.
    CHECK(autostart_contents("/home/j/My Apps/isotone", true).find(R"(Exec="/home/j/My Apps/isotone" --tray)" "\n") !=
          std::string::npos);
    CHECK(autostart_contents("/opt/a$b/isotone", false).find(R"(Exec="/opt/a\\$b/isotone")" "\n") != std::string::npos);
    CHECK(autostart_contents(R"(/opt/a"b\c/isotone)", false).find(R"(Exec="/opt/a\\"b\\\\c/isotone")" "\n") !=
          std::string::npos);
    CHECK(autostart_contents("/opt/100%/isotone", false).find("Exec=/opt/100%%/isotone\n") != std::string::npos);
}

TEST_CASE("writing then removing is on then off") {
    const std::string path = autostart_path(scratch());
    std::filesystem::remove_all(scratch());

    CHECK_FALSE(autostart_enabled(path));
    REQUIRE(write_autostart(path, "/opt/isotone/isotone", true) == 0);
    CHECK(autostart_enabled(path));
    CHECK(autostart_command(path) == "/opt/isotone/isotone --tray");

    REQUIRE(remove_autostart(path) == 0);
    CHECK_FALSE(autostart_enabled(path));
    // Turning off what is already off is not a failure.
    CHECK(remove_autostart(path) == 0);

    std::filesystem::remove_all(scratch());
}

TEST_CASE("an entry a desktop switched off reads as off") {
    const std::string dir = scratch();
    std::filesystem::remove_all(dir);
    const std::string path = autostart_path(dir);

    write_file(path, "[Desktop Entry]\nType=Application\nExec=/usr/bin/isotone\nHidden=true\n");
    CHECK_FALSE(autostart_enabled(path));

    write_file(path,
               "[Desktop Entry]\nType=Application\nExec=/usr/bin/isotone\n"
               "X-GNOME-Autostart-enabled=false\n");
    CHECK_FALSE(autostart_enabled(path));

    write_file(path, "[Desktop Entry]\nType=Application\nExec=/usr/bin/isotone\nHidden=false\n");
    CHECK(autostart_enabled(path));

    std::filesystem::remove_all(dir);
}

TEST_CASE("a command with a newline is refused, not written") {
    const std::string path = autostart_path(scratch());
    std::filesystem::remove_all(scratch());

    // It would end the Exec value and write a second key.
    CHECK(write_autostart(path, "/usr/bin/isotone\nX-Evil=1", false) != 0);
    CHECK_FALSE(std::filesystem::exists(path));

    std::filesystem::remove_all(scratch());
}

TEST_CASE("no temporary file is left behind") {
    const std::string path = autostart_path(scratch());
    std::filesystem::remove_all(scratch());

    REQUIRE(write_autostart(path, "/usr/bin/isotone", false) == 0);
    CHECK(std::filesystem::exists(path));
    CHECK_FALSE(std::filesystem::exists(path + ".tmp"));

    std::filesystem::remove_all(scratch());
}

TEST_CASE("in a Flatpak the entry is the host's, and named for the application id") {
    const char* saved_xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string keep_xdg = saved_xdg != nullptr ? saved_xdg : "";
    const char* saved_home = std::getenv("HOME");
    const std::string keep_home = saved_home != nullptr ? saved_home : "";

    ::setenv("HOME", "/home/someone", 1);
    // What a sandbox actually has: XDG_CONFIG_HOME under ~/.var/app, which no
    // desktop reads, and $HOME the real one (measured in the sandbox on the
    // owner's laptop).
    ::setenv("XDG_CONFIG_HOME", "/home/someone/.var/app/io.github.jackwangxyw.Isotone/config", 1);
    ::setenv("FLATPAK_ID", "io.github.jackwangxyw.Isotone", 1);

    CHECK(autostart_dir() == "/home/someone/.config/autostart");
    // The name the Background portal gives the entry it writes.
    CHECK(autostart_path(autostart_dir()) == "/home/someone/.config/autostart/io.github.jackwangxyw.Isotone.desktop");

    // And outside one, the same two are XDG's.
    ::unsetenv("FLATPAK_ID");
    CHECK(autostart_dir() == "/home/someone/.var/app/io.github.jackwangxyw.Isotone/config/autostart");
    // The same name either way: the application ID is what a desktop file is
    // called, in a Flatpak or out of one.
    CHECK(autostart_path(autostart_dir()) ==
          "/home/someone/.var/app/io.github.jackwangxyw.Isotone/config/autostart/io.github.jackwangxyw.Isotone.desktop");

    // An empty FLATPAK_ID is not a Flatpak.
    ::setenv("FLATPAK_ID", "", 1);
    CHECK(autostart_dir() == "/home/someone/.var/app/io.github.jackwangxyw.Isotone/config/autostart");
    ::unsetenv("FLATPAK_ID");

    if (saved_xdg != nullptr) ::setenv("XDG_CONFIG_HOME", keep_xdg.c_str(), 1);
    else ::unsetenv("XDG_CONFIG_HOME");
    if (saved_home != nullptr) ::setenv("HOME", keep_home.c_str(), 1);
}

TEST_CASE("the entry is named for the application id, and every old name is known") {
    // The GlobalShortcuts portal takes the application ID from the systemd unit
    // the desktop started the app in and looks for a desktop file of that name.
    // "isotone.desktop" gives the ID "isotone", which resolves to nothing,
    // because what a package installs is io.github.jackwangxyw.Isotone.desktop.
    CHECK(std::string(kAutostartFileName) == "io.github.jackwangxyw.Isotone.desktop");
    CHECK(autostart_path("/c/autostart") == "/c/autostart/io.github.jackwangxyw.Isotone.desktop");
    // Every name the entry has had, newest first, so one left behind by an older
    // version can be read and removed. io.github.isotone.Isotone was the
    // application ID until 2026-09-21, when it was renamed for the account the
    // repository is actually under; an entry under it starts the app with an ID
    // that now resolves to nothing.
    const std::vector<std::string> legacy = legacy_autostart_paths("/c/autostart");
    REQUIRE(legacy.size() == 2);
    CHECK(legacy[0] == "/c/autostart/io.github.isotone.Isotone.desktop");
    CHECK(legacy[1] == "/c/autostart/isotone.desktop");
    CHECK(legacy_autostart_paths("").empty());
}
