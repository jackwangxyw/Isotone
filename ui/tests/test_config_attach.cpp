// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The Attach dialog's preview and attach, on a sandbox copy of a config.txt with
// a Peace include. Never the installed Equalizer APO's directory.

#include "doctest.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "config_attach.h"
#include "config_files.h"
#include "eapo_install.h"

using namespace isotone::ui;
namespace fs = std::filesystem;

namespace {

constexpr char kConfig[] = "Preamp: -3 dB\r\nInclude: peace.txt\r\nGraphicEQ: 25 0; 40 -1.5; 100 0\r\n";
constexpr char kBlock[] = "# Added by Isotone. Remove these three lines to detach it.\r\nDevice: all\r\nInclude: Isotone.txt\r\n";

struct Sandbox {
    fs::path dir;
    Sandbox() {
        dir = fs::temp_directory_path() /
              ("isotone-attach-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(dir);
        REQUIRE_FALSE(isotone::compat::is_live_install_path(dir));
        write(kConfig);
    }
    ~Sandbox() {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
    void write(const std::string& bytes) const {
        std::ofstream out(dir / "config.txt", std::ios::binary);
        out << bytes;
    }
    std::string read() const {
        std::ifstream in(dir / "config.txt", std::ios::binary);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }
};

}  // namespace

TEST_CASE("the attach preview lists config.txt's lines, marks the Peace include and shows the block") {
    Sandbox box;
    const AttachPreview p = preview_attach(box.dir);
    REQUIRE(p.error == ERROR_SUCCESS);
    REQUIRE(p.lines.size() == 3);
    CHECK(p.lines[0] == "Preamp: -3 dB");
    CHECK(p.lines[1] == "Include: peace.txt");
    CHECK(p.lines[2] == "GraphicEQ: 25 0; 40 -1.5; 100 0");
    REQUIRE(p.peace.size() == 3);
    CHECK_FALSE(p.peace[0]);
    CHECK(p.peace[1]);
    CHECK_FALSE(p.peace[2]);
    CHECK(p.added == std::vector<std::string>{"# Added by Isotone. Remove these three lines to detach it.", "Device: all",
                                              "Include: Isotone.txt"});
    CHECK_FALSE(p.attached);
    // The preview wrote nothing in the directory.
    CHECK(box.read() == kConfig);
    CHECK_FALSE(fs::exists(box.dir / "config.txt.isotone-backup"));
}

TEST_CASE("the preview's block follows what config.txt leaves open") {
    Sandbox box;
    box.write("If: sampleRate == 48000\nInclude: Peace.txt");   // no line break at the end
    const AttachPreview p = preview_attach(box.dir);
    REQUIRE(p.error == ERROR_SUCCESS);
    CHECK(p.lines.size() == 2);
    CHECK(p.peace == std::vector<bool>{false, true});
    CHECK(p.added == std::vector<std::string>{"# Added by Isotone. Remove these four lines and the line break before them to detach it.",
                                              "Device: all", "EndIf:", "Include: Isotone.txt"});
}

TEST_CASE("attach keeping Peace appends the block once") {
    Sandbox box;
    AttachOutcome a = attach_config(box.dir, false);
    CHECK(a.error == ERROR_SUCCESS);
    CHECK(a.appended);
    CHECK_FALSE(a.peace_removed);
    CHECK(box.read() == std::string(kConfig) + kBlock);
    CHECK(preview_attach(box.dir).attached);

    a = attach_config(box.dir, false);
    CHECK(a.error == ERROR_SUCCESS);
    CHECK_FALSE(a.appended);
    CHECK(box.read() == std::string(kConfig) + kBlock);
}

TEST_CASE("attach removing Peace drops its include, then appends the block, once") {
    Sandbox box;
    AttachOutcome a = attach_config(box.dir, true);
    CHECK(a.error == ERROR_SUCCESS);
    CHECK(a.appended);
    CHECK(a.peace_removed);
    const std::string expected = std::string("Preamp: -3 dB\r\nGraphicEQ: 25 0; 40 -1.5; 100 0\r\n") + kBlock;
    CHECK(box.read() == expected);
    CHECK_FALSE(isotone::compat::inspect_config(box.dir).peace_included);

    a = attach_config(box.dir, true);
    CHECK(a.error == ERROR_SUCCESS);
    CHECK_FALSE(a.appended);
    CHECK_FALSE(a.peace_removed);
    CHECK(box.read() == expected);
}

TEST_CASE("only Include lines naming peace.txt are removed") {
    CHECK(without_peace_includes("Include: peace.txt\n") == "");
    CHECK(without_peace_includes("A: 1\nInclude:  C:\\Peace\\PEACE.txt \nB: 2") == "A: 1\nB: 2");
    CHECK(without_peace_includes("A: 1\r\nInclude: peace.txt") == "A: 1\r\n");
    CHECK(without_peace_includes("# Include: peace.txt\n") == "# Include: peace.txt\n");
    CHECK(without_peace_includes("include: peace.txt\n") == "include: peace.txt\n");   // keys are case-sensitive
    CHECK(without_peace_includes("Include: peace.txt.bak\n") == "Include: peace.txt.bak\n");
    CHECK(without_peace_includes("Include: Isotone.txt\r\n") == "Include: Isotone.txt\r\n");
}

TEST_CASE("a directory without config.txt reports the error and attaches nothing") {
    Sandbox box;
    fs::remove(box.dir / "config.txt");
    CHECK(preview_attach(box.dir).error == ERROR_FILE_NOT_FOUND);
    CHECK(attach_config(box.dir, true).error == ERROR_FILE_NOT_FOUND);
    CHECK_FALSE(fs::exists(box.dir / "config.txt"));
}
