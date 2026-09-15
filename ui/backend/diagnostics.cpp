// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "diagnostics.h"

#include <windows.h>

#include <mmdeviceapi.h>

#include <cstdio>
#include <filesystem>

namespace isotone::ui {

namespace {

constexpr wchar_t kIsoApoServer[] = L"CLSID\\{BAF30F18-9FA2-4E55-97D9-007CEA179824}\\InprocServer32";
constexpr wchar_t kEqualizerApoKey[] = L"SOFTWARE\\EqualizerAPO";
constexpr wchar_t kEqualizerApoUninstall[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\EqualizerAPO";
constexpr wchar_t kAudioKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio";

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool read_string(HKEY root, const wchar_t* key, const wchar_t* name, std::wstring* out) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return false;
    std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS)
        return false;
    buffer.resize(wcslen(buffer.c_str()));
    *out = buffer;
    return true;
}

std::string file_version(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &length) || info == nullptr || length < sizeof(VS_FIXEDFILEINFO)) return {};
    return version_text(HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS),
                        LOWORD(info->dwFileVersionLS));
}

std::string outputs_text(int n) { return std::to_string(n) + (n == 1 ? " output" : " outputs"); }

const char* state_text(DWORD state) {
    switch (state) {
        case DEVICE_STATE_ACTIVE: return "active";
        case DEVICE_STATE_DISABLED: return "disabled";
        case DEVICE_STATE_NOTPRESENT: return "not present";
        case DEVICE_STATE_UNPLUGGED: return "unplugged";
    }
    return "unknown state";
}

}  // namespace

std::string version_text(unsigned major, unsigned minor, unsigned patch, unsigned build) {
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (build != 0) s += "." + std::to_string(build);
    return s;
}

EngineSummary read_engine_summary(const std::vector<isotone::devices::Endpoint>& endpoints) {
    EngineSummary s;
    std::wstring dll;
    if (read_string(HKEY_CLASSES_ROOT, kIsoApoServer, nullptr, &dll) && !dll.empty()) {
        s.isoapo_registered = true;
        s.isoapo_dll = utf8(dll);
        s.isoapo_version = file_version(dll);
    }

    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kEqualizerApoKey, 0, KEY_READ, &h) == ERROR_SUCCESS) {
        RegCloseKey(h);
        s.equalizerapo_installed = true;
        std::wstring install, display;
        if (read_string(HKEY_LOCAL_MACHINE, kEqualizerApoKey, L"InstallPath", &install))
            s.equalizerapo_version = file_version((std::filesystem::path(install) / L"EqualizerAPO.dll").wstring());
        if (s.equalizerapo_version.empty() && read_string(HKEY_LOCAL_MACHINE, kEqualizerApoUninstall, L"DisplayVersion", &display))
            s.equalizerapo_version = utf8(display);
    }

    for (const isotone::devices::Endpoint& e : endpoints) {
        if (e.state != DEVICE_STATE_ACTIVE) continue;
        if (e.engine.backend == isotone::devices::Backend::native && e.engine.isoapo_state == isotone::devices::IsoApoState::installed)
            ++s.isoapo_outputs;
        if (e.engine.backend == isotone::devices::Backend::equalizerapo) ++s.equalizerapo_outputs;
    }

    DWORD value = 0, bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kAudioKey, L"DisableProtectedAudioDG", RRF_RT_REG_DWORD, nullptr, &value, &bytes) ==
            ERROR_SUCCESS &&
        value == 1) {
        s.protected_audio = ProtectedAudio::disabled;
    }
    return s;
}

std::string isoapo_row(const EngineSummary& s) {
    if (!s.isoapo_registered) return "Not installed";
    return s.isoapo_version.empty() ? outputs_text(s.isoapo_outputs) : s.isoapo_version + " \xC2\xB7 " + outputs_text(s.isoapo_outputs);
}

std::string equalizerapo_row(const EngineSummary& s) {
    if (!s.equalizerapo_installed) return "Not installed";
    return s.equalizerapo_version.empty() ? outputs_text(s.equalizerapo_outputs)
                                          : s.equalizerapo_version + " \xC2\xB7 " + outputs_text(s.equalizerapo_outputs);
}

std::string diagnostics_text(const DiagnosticsInput& in) {
    std::string t;
    t += "Isotone " + in.app_version + "\n";
    t += "Qt " + in.qt_version + "\n";
    t += in.windows + "\n\n";

    const EngineSummary& e = in.engines;
    if (!e.isoapo_registered) {
        t += "IsoAPO: not registered\n";
    } else {
        t += "IsoAPO: " + (e.isoapo_version.empty() ? std::string("registered, no version") : e.isoapo_version) + ", " +
             outputs_text(e.isoapo_outputs) + "\n";
        t += "IsoAPO DLL: " + e.isoapo_dll + "\n";
    }
    if (!e.equalizerapo_installed) {
        t += "Equalizer APO: not installed\n";
    } else {
        t += "Equalizer APO: " + (e.equalizerapo_version.empty() ? std::string("installed, no version") : e.equalizerapo_version) +
             ", " + outputs_text(e.equalizerapo_outputs) + "\n";
    }
    t += std::string("Protected audio: ") + (e.protected_audio == ProtectedAudio::disabled ? "disabled" : "enabled") + "\n\n";

    t += "Render endpoints\n";
    for (const isotone::devices::Endpoint& d : in.endpoints) {
        t += utf8(d.friendly_name) + " " + utf8(d.guid) + "\n";
        t += std::string("  ") + state_text(d.state) + (d.default_console ? ", default" : "") + "\n";
        if (!d.error.empty()) t += "  error: " + d.error + "\n";
        if (d.engine.error != S_OK) {
            char hr[32];
            std::snprintf(hr, sizeof(hr), "0x%08lX", static_cast<unsigned long>(d.engine.error));
            t += std::string("  engine error ") + hr + "\n";
        } else {
            t += std::string("  engine ") + isotone::devices::backend_name(d.engine.backend) + ", IsoAPO " +
                 isotone::devices::isoapo_state_name(d.engine.isoapo_state) +
                 (d.engine.enhancements_disabled ? ", enhancements off" : "") + "\n";
        }
        if (!d.format.present) {
            t += "  format: " + d.format.error + "\n";
            continue;
        }
        const char* kind = d.format.sample_format == isotone::devices::SampleFormat::pcm          ? "PCM"
                           : d.format.sample_format == isotone::devices::SampleFormat::ieee_float ? "float"
                                                                                                  : "other";
        char line[160];
        std::snprintf(line, sizeof(line), "  %u ch, %u Hz, %u-bit", d.format.channels, d.format.sample_rate, d.format.bits_per_sample);
        t += line;
        if (d.format.valid_bits != d.format.bits_per_sample) t += " (" + std::to_string(d.format.valid_bits) + " valid)";
        std::snprintf(line, sizeof(line), " %s, mask 0x%X%s\n", kind, d.format.channel_mask, d.format.mask_defaulted ? " (defaulted)" : "");
        t += line;
    }
    return t;
}

}  // namespace isotone::ui
