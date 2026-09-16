// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// An Equalizer APO write that fails because something else holds Isotone.txt is
// tried again: without that the edit was lost until the next commit (found in
// the surround package's notes, and as a CI flake in compat's own stress test,
// 2026-09-15).

#include "doctest.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "devicelink.h"

using namespace isotone;
using namespace isotone::ui;

namespace {

std::filesystem::path scratch_dir(const wchar_t* name) {
    wchar_t temp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    REQUIRE(n > 0);
    const std::filesystem::path dir =
        std::filesystem::path(temp) / (std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

EqState one_band(double gain_db) {
    EqState s;
    Band b;
    b.id = 1;
    b.fc = 1000;
    b.gain_db = gain_db;
    b.width = 1;
    s.bands.push_back(b);
    return s;
}

// Holds a file open without sharing delete, as a reader that blocks a replace does.
struct Holder {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Holder(const std::filesystem::path& path) {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    bool open() const { return h != INVALID_HANDLE_VALUE; }
    void release() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    ~Holder() { release(); }
};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST_CASE("an Equalizer APO write blocked by another reader is tried again, not lost") {
    const std::filesystem::path dir = scratch_dir(L"isotone-compat-retry");
    const std::filesystem::path file = dir / "Isotone.txt";
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-0000000000aa}";
    {
        DeviceLink link(L"Local\\unused.", dir.wstring());
        link.set_target(OutputTarget{guid, Backend::equalizer_apo, OutputLayout{2, 0x3, 48000}});

        // A first edit creates the file.
        link.commit(one_band(-2.0));
        for (int i = 0; i < 200 && !std::filesystem::exists(file); ++i) Sleep(10);
        REQUIRE(std::filesystem::exists(file));
        for (int i = 0; i < 200 && link.last_compat_error() != ERROR_SUCCESS; ++i) Sleep(10);
        REQUIRE(link.last_compat_error() == ERROR_SUCCESS);

        // Held open with no delete sharing, the replace cannot happen: the write
        // fails, and the retry keeps trying while it is held.
        Holder holder(file);
        REQUIRE(holder.open());
        link.commit(one_band(-7.5));
        Sleep(400);
        CHECK(read_text(file).find("Gain -7.5 dB") == std::string::npos);

        // Let go: the edit lands without another commit.
        holder.release();
        std::string text;
        for (int i = 0; i < 300; ++i) {
            text = read_text(file);
            if (text.find("Gain -7.5 dB") != std::string::npos) break;
            Sleep(10);
        }
        CAPTURE(text);
        CHECK(text.find("Gain -7.5 dB") != std::string::npos);
        CHECK(link.last_compat_error() == ERROR_SUCCESS);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a write that stays blocked stops being retried and keeps its error") {
    const std::filesystem::path dir = scratch_dir(L"isotone-compat-retry3");
    const std::filesystem::path file = dir / "Isotone.txt";
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-0000000000cc}";
    {
        DeviceLink link(L"Local\\\\unused.", dir.wstring(), 300);   // give up after 300 ms, not 10 s
        link.set_target(OutputTarget{guid, Backend::equalizer_apo, OutputLayout{2, 0x3, 48000}});
        link.commit(one_band(-1.0));
        for (int i = 0; i < 200 && !std::filesystem::exists(file); ++i) Sleep(10);
        REQUIRE(std::filesystem::exists(file));
        for (int i = 0; i < 200 && link.last_compat_error() != ERROR_SUCCESS; ++i) Sleep(10);

        Holder holder(file);
        REQUIRE(holder.open());
        link.commit(one_band(-9.0));
        for (int i = 0; i < 200 && link.last_compat_error() == ERROR_SUCCESS; ++i) Sleep(10);
        REQUIRE(link.last_compat_error() != ERROR_SUCCESS);
        Sleep(600);   // past the give-up time
        const uint64_t settled = link.compat_writes();
        Sleep(600);
        CHECK(link.compat_writes() == settled);   // no longer trying
        CHECK(link.last_compat_error() != ERROR_SUCCESS);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a newer edit replaces the one being retried") {
    const std::filesystem::path dir = scratch_dir(L"isotone-compat-retry2");
    const std::filesystem::path file = dir / "Isotone.txt";
    const std::wstring guid = L"{8f4d2a10-0000-4000-8000-0000000000bb}";
    {
        DeviceLink link(L"Local\\unused.", dir.wstring());
        link.set_target(OutputTarget{guid, Backend::equalizer_apo, OutputLayout{2, 0x3, 48000}});
        link.commit(one_band(-1.0));
        for (int i = 0; i < 200 && !std::filesystem::exists(file); ++i) Sleep(10);
        REQUIRE(std::filesystem::exists(file));

        for (int i = 0; i < 200 && link.last_compat_error() != ERROR_SUCCESS; ++i) Sleep(10);
        const uint64_t before = link.compat_writes();

        Holder holder(file);
        REQUIRE(holder.open());
        link.commit(one_band(-3.0));
        for (int i = 0; i < 200 && link.last_compat_error() == ERROR_SUCCESS; ++i) Sleep(10);
        REQUIRE(link.last_compat_error() != ERROR_SUCCESS);   // held: the write failed and is being retried
        const uint64_t failed = link.compat_writes();
        link.commit(one_band(-4.5));   // the newer edit replaces the one being retried
        holder.release();
        std::string text;
        for (int i = 0; i < 300; ++i) {
            text = read_text(file);
            if (text.find("Gain -4.5 dB") != std::string::npos) break;
            Sleep(10);
        }
        CAPTURE(text);
        CHECK(text.find("Gain -4.5 dB") != std::string::npos);
        CHECK(text.find("Gain -3 dB") == std::string::npos);
        // The stale edit is never written: every write reloads every Equalizer APO
        // device, so the retry carries the newest state, not the one it started with.
        CHECK(link.compat_writes() - before == failed - before + 1);
    }
    std::filesystem::remove_all(dir);
}
