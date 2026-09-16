// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Settings without Qt: the spectrum's resolutions and its decay, launch at
// sign-in against a test key, and the diagnostics text.

#include "doctest.h"

#include <windows.h>

#include <mmdeviceapi.h>

#include <cmath>
#include <vector>

#include "diagnostics.h"
#include "spectrum.h"
#include "startup_registration.h"

using namespace isotone::ui;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;

std::vector<float> sine(double hz, double amplitude, size_t frames) {
    std::vector<float> out(frames);
    for (size_t i = 0; i < frames; ++i)
        out[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
    return out;
}

// A test key of its own under HKCU, removed when the test ends.
struct TestKey {
    std::wstring path = L"Software\\Isotone-tests\\Run-" + std::to_wstring(GetCurrentProcessId());
    ~TestKey() {
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\Isotone-tests");   // only when no other run's key is in it
    }
};

constexpr size_t kN = SpectrumAnalyzer::kFftSize;

}  // namespace

TEST_CASE("each resolution reads a sine at its level, in its bin") {
    // 750 Hz is bin 64 at 4096, 128 at 8192 and 256 at 16384.
    for (size_t n : {size_t{4096}, size_t{8192}, size_t{16384}}) {
        CAPTURE(n);
        SpectrumAnalyzer a;
        a.set_fft_size(n);
        CHECK(a.fft_size() == n);
        CHECK(a.bin_db().size() == n / 2 + 1);
        const std::vector<float> s = sine(750.0, 0.5, n * 2);
        a.push(s.data(), s.size(), 1);
        a.update(kRate, 2.0);
        const size_t bin = 64 * n / 4096;
        CHECK(a.bin_db()[bin] == doctest::Approx(20.0 * std::log10(0.5)).epsilon(0.001));
        CHECK(a.bin_db()[bin + 8] < a.bin_db()[bin] - 40.0);
        const double freqs[] = {750.0 / 1.01, 750.0, 750.0 * 1.01};
        double out[3];
        a.levels_at(freqs, 3, out);
        CHECK(out[1] == doctest::Approx(-6.02).epsilon(0.01));
    }
}

TEST_CASE("fewer samples than the resolution read as the floor") {
    SpectrumAnalyzer a;
    a.set_fft_size(16384);
    const std::vector<float> s = sine(750.0, 0.5, 12000);
    a.push(s.data(), s.size(), 1);
    a.update(kRate, 2.0);
    CHECK(a.bin_db()[256] == SpectrumAnalyzer::kFloorDb);
}

TEST_CASE("the release setting sets how fast the level falls") {
    SpectrumAnalyzer a;
    a.set_release_ms(100.0);
    const std::vector<float> s = sine(170 * kRate / kN, 0.5, kN * 2);
    a.push(s.data(), s.size(), 1);
    a.update(kRate, 2.0);
    std::vector<float> quiet(kN, 0.0f);
    a.push(quiet.data(), quiet.size(), 1);
    a.update(kRate, 1.0 / 60.0);
    // In power, e^(-t / release): 10 log10(e) * 16.7 / 100 = 0.72 dB.
    CHECK(a.bin_db()[170] == doctest::Approx(-6.02 - 0.7238).epsilon(0.002));
}

TEST_CASE("a source that stops sending falls at its release, it does not stand still") {
    // The owner, 2026-09-15: stopping the music faded the curve for a moment and
    // then dropped it. A player that stops closes its stream, so no frames arrive
    // at all: the analyzer kept reading the same history, and the display hid the
    // curve outright once its timeout tripped.
    SpectrumAnalyzer a;
    const std::vector<float> s = sine(170 * kRate / kN, 0.5, kN * 2);
    a.push(s.data(), s.size(), 1);
    a.update(kRate, 2.0);
    REQUIRE(a.loudest_db() == doctest::Approx(-6.02).epsilon(0.001));

    SpectrumAnalyzer stuck = a;
    for (int i = 0; i < 60; ++i) stuck.update(kRate, 1.0 / 60.0);
    CHECK(stuck.loudest_db() == doctest::Approx(-6.02).epsilon(0.001));   // nothing pushed: it stands still

    const size_t frame = static_cast<size_t>(kRate / 60.0);
    double previous = a.loudest_db();
    for (int i = 0; i < 60; ++i) {   // a second of the silence the source is not sending
        a.push_silence(frame);
        a.update(kRate, 1.0 / 60.0);
        CHECK(a.loudest_db() <= previous);   // falling, never a step back up
        previous = a.loudest_db();
    }
    CHECK(previous < -15.0);    // well down after a second (measured -19.6)
    CHECK(previous > -100.0);   // and not dropped to the floor at once
    for (int i = 0; i < 60 * 12; ++i) {
        a.push_silence(frame);
        a.update(kRate, 1.0 / 60.0);
    }
    CHECK(a.loudest_db() < -100.0);   // and it gets there, so the display can stop drawing it
}

TEST_CASE("launch at sign-in writes, reads back and removes the Run value") {
    TestKey key;
    CHECK_FALSE(read_run_value(key.path, L"Isotone").has_value());

    const std::wstring exe = L"C:\\Program Files\\Isotone\\isotone.exe";
    CHECK(run_command(exe, false) == L"\"C:\\Program Files\\Isotone\\isotone.exe\"");
    CHECK(run_command(exe, true) == L"\"C:\\Program Files\\Isotone\\isotone.exe\" --tray");

    CHECK(write_run_value(key.path, L"Isotone", run_command(exe, true)) == ERROR_SUCCESS);
    REQUIRE(read_run_value(key.path, L"Isotone").has_value());
    CHECK(*read_run_value(key.path, L"Isotone") == run_command(exe, true));

    // A REG_SZ, as Windows reads Run values.
    HKEY h = nullptr;
    REQUIRE(RegOpenKeyExW(HKEY_CURRENT_USER, key.path.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS);
    DWORD type = 0;
    CHECK(RegQueryValueExW(h, L"Isotone", nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS);
    CHECK(type == REG_SZ);
    RegCloseKey(h);

    CHECK(write_run_value(key.path, L"Isotone", run_command(exe, false)) == ERROR_SUCCESS);
    CHECK(*read_run_value(key.path, L"Isotone") == run_command(exe, false));

    CHECK(remove_run_value(key.path, L"Isotone") == ERROR_SUCCESS);
    CHECK_FALSE(read_run_value(key.path, L"Isotone").has_value());
    // Removing what is not there is not an error.
    CHECK(remove_run_value(key.path, L"Isotone") == ERROR_SUCCESS);
}

TEST_CASE("the diagnostics text has every section") {
    DiagnosticsInput in;
    in.app_version = "0.1.0";
    in.qt_version = "6.11.2";
    in.windows = "Windows 11 Home 10.0.26200.1234";
    in.engines.isoapo_registered = true;
    in.engines.isoapo_version = "";
    in.engines.isoapo_outputs = 1;
    in.engines.equalizerapo_installed = true;
    in.engines.equalizerapo_version = "1.4.2";
    in.engines.equalizerapo_outputs = 3;
    in.engines.protected_audio = ProtectedAudio::disabled;

    isotone::devices::Endpoint e;
    e.guid = L"{798436d2-8c71-4834-9248-00ccbaaca00a}";
    e.friendly_name = L"CABLE Input (VB-Audio Virtual Cable)";
    e.state = DEVICE_STATE_ACTIVE;
    e.default_console = true;
    e.format.present = true;
    e.format.channels = 2;
    e.format.sample_rate = 48000;
    e.format.bits_per_sample = 24;
    e.format.valid_bits = 24;
    e.format.sample_format = isotone::devices::SampleFormat::pcm;
    e.format.channel_mask = 0x3;
    e.engine.backend = isotone::devices::Backend::native;
    e.engine.isoapo_state = isotone::devices::IsoApoState::installed;
    isotone::devices::Endpoint unplugged;
    unplugged.guid = L"{00000000-0000-0000-0000-000000000001}";
    unplugged.friendly_name = L"Headset";
    unplugged.state = DEVICE_STATE_UNPLUGGED;
    unplugged.format.error = "no format";
    in.endpoints = {e, unplugged};

    const std::string text = diagnostics_text(in);
    CAPTURE(text);
    CHECK(text.find("Isotone 0.1.0") != std::string::npos);
    CHECK(text.find("Qt 6.11.2") != std::string::npos);
    CHECK(text.find("Windows 11 Home 10.0.26200.1234") != std::string::npos);
    CHECK(text.find("IsoAPO: registered, no version, 1 output") != std::string::npos);
    CHECK(text.find("Equalizer APO: 1.4.2, 3 outputs") != std::string::npos);
    CHECK(text.find("Protected audio: disabled") != std::string::npos);
    CHECK(text.find("Render endpoints") != std::string::npos);
    CHECK(text.find("CABLE Input (VB-Audio Virtual Cable) {798436d2-8c71-4834-9248-00ccbaaca00a}") != std::string::npos);
    CHECK(text.find("active, default") != std::string::npos);
    CHECK(text.find("engine native, IsoAPO installed") != std::string::npos);
    CHECK(text.find("2 ch, 48000 Hz, 24-bit PCM, mask 0x3") != std::string::npos);
    CHECK(text.find("Headset {00000000-0000-0000-0000-000000000001}") != std::string::npos);
    CHECK(text.find("unplugged") != std::string::npos);
    CHECK(text.find("format: no format") != std::string::npos);

    SUBCASE("engines not there") {
        in.engines = EngineSummary{};
        const std::string none = diagnostics_text(in);
        CHECK(none.find("IsoAPO: not registered") != std::string::npos);
        CHECK(none.find("Equalizer APO: not installed") != std::string::npos);
        CHECK(none.find("Protected audio: enabled") != std::string::npos);
    }
}

TEST_CASE("the About rows") {
    EngineSummary s;
    CHECK(isoapo_row(s) == "Not installed");
    s.isoapo_registered = true;
    CHECK(isoapo_row(s) == "0 outputs");
    s.isoapo_outputs = 2;
    s.isoapo_version = "0.1.0";
    CHECK(isoapo_row(s) == "0.1.0 \xC2\xB7 2 outputs");
    s.isoapo_outputs = 1;
    CHECK(isoapo_row(s) == "0.1.0 \xC2\xB7 1 output");
    CHECK(equalizerapo_row(s) == "Not installed");
    s.equalizerapo_installed = true;
    s.equalizerapo_version = "1.4.2";
    s.equalizerapo_outputs = 1;
    CHECK(equalizerapo_row(s) == "1.4.2 \xC2\xB7 1 output");
}

TEST_CASE("a file version reads as major.minor.patch, without a zero build") {
    CHECK(version_text(1, 4, 2, 0) == "1.4.2");
    CHECK(version_text(1, 4, 2, 7) == "1.4.2.7");
    CHECK(version_text(0, 1, 0, 0) == "0.1.0");
}
