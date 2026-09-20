// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone-devicetool: device registration for IsoAPO (plan 5.2), wrapping
// upstream Equalizer APO's DeviceAPOInfo and RegistryHelper rather than
// reimplementing them.
//
//   isotone-devicetool list
//   isotone-devicetool status    <endpoint>
//   isotone-devicetool install   <endpoint> [--mode mfx|efx|gfx] [--replace-equalizerapo] [--dry-run]
//   isotone-devicetool uninstall <endpoint> [--dry-run]
//   isotone-devicetool repair    [<endpoint>] [--mode mfx|efx|gfx] [--dry-run]
//   isotone-devicetool test      <endpoint>
//   isotone-devicetool enable-enhancements <endpoint> [--dry-run]
//   isotone-devicetool restart-audio [--dry-run]
//   isotone-devicetool layouts   <endpoint>
//   isotone-devicetool set-layout <endpoint> --layout stereo|2.1|5.1|7.1 [--dry-run]
//   isotone-devicetool roundtrip <endpoint> [...]
//   isotone-devicetool serve     --pipe <name> --parent <pid>
// Every command also takes --output <absolute path>.
//
// <endpoint> is an endpoint GUID, with or without braces, or a full device ID
// such as {0.0.0.00000000}.{guid}. One JSON object on stdout, or in the
// --output file. Exit 0 on success, 1 on failure or refusal, 2 on bad
// arguments, 3 when the command needs an elevated process, 4 when another
// devicetool run held the machine lock for too long.
//
// --dry-run runs upstream's install and uninstall logic unchanged with every
// registry write intercepted (RegistryDryRun), prints the writes, and needs no
// elevation. Without it, install/uninstall/repair/enable-enhancements require
// an elevated process; so does restart-audio, whose dry run only reads the
// service's state.
//
// serve runs commands for the UI in one elevated process, so Windows asks for
// approval once: see cmd_serve.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <aclapi.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <sddl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <userenv.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "DeviceAPOInfo.h"
#include "acl.h"
#include "dry_run.h"
#include "helpers/PrecisionTimer.h"
#include "helpers/RegistryHelper.h"
#include "speaker_layout.h"
#include "helpers/ServiceHelper.h"

using isotone::devicetool::dacl_grants;
using isotone::devicetool::DryRunRegistry;
using isotone::devicetool::OperationLog;
using isotone::devicetool::RegistryOperation;
using isotone::devicetool::ScopedDryRun;
using isotone::devicetool::ScopedLog;
using isotone::devicetool::SimulatedKill;

namespace {

// ---------------------------------------------------------------------------
// Registry locations. The value names are the ones upstream DeviceAPOInfo.cpp
// uses; see the registry-traps notes in docs/decisions.md for why the
// {d04e05a6-...} and {d3993a3f-...} keys must never be confused.

const wchar_t* const kMMDevices = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio";
const wchar_t* const kApoRegistration = L"HKEY_CLASSES_ROOT\\AudioEngine\\AudioProcessingObjects";
const wchar_t* const kClsid = L"HKEY_CLASSES_ROOT\\CLSID";
const wchar_t* const kIsoChildApos = APP_REGPATH L"\\Child APOs";
const wchar_t* const kEapoChildApos = EQUALIZERAPO_REGPATH L"\\Child APOs";

struct SlotName {
    const char* label;
    const wchar_t* value;
};
const SlotName kEffectSlots[] = {
    {"LFX", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1"},
    {"GFX", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2"},
    {"SFX", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5"},
    {"MFX", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6"},
    {"EFX", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7"},
};
// REG_MULTI_SZ lists of additional effects (upstream's multiSfx/multiMfx/
// multiEfx value names). Upstream only checks whether they exist when choosing
// an install mode and never changes them, so their APOs keep running.
// ,19 and ,20 are PKEY_CompositeFX_Offload_StreamEffectClsid and
// _ModeEffectClsid (Windows SDK), the lists for hardware-offloaded streams;
// Realtek drivers fill them. Reported only, never written.
const SlotName kEffectLists[] = {
    {"SFX_LIST", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13"},
    {"MFX_LIST", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14"},
    {"EFX_LIST", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15"},
    {"SFX_OFFLOAD_LIST", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},19"},
    {"MFX_OFFLOAD_LIST", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},20"},
};
const SlotName kModeSlots[] = {
    {"SFX", L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},5"},
    {"MFX", L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},6"},
    {"EFX", L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7"},
};

// ---------------------------------------------------------------------------
// JSON

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}
std::string quote(const std::wstring& w) { return quote(utf8(w)); }
const char* boolean(bool b) { return b ? "true" : "false"; }

std::string string_array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? "," : "") + quote(items[i]);
    return out + "]";
}

std::string operations_json(const std::vector<RegistryOperation>& ops) {
    std::string out = "[";
    for (size_t i = 0; i < ops.size(); ++i) {
        const RegistryOperation& o = ops[i];
        out += std::string(i ? "," : "") + "{\"op\":" + quote(o.operation) + ",\"key\":" +
               quote(o.key) + ",\"value\":" + quote(o.valuename) + ",\"data\":" + quote(o.data) + "}";
    }
    return out + "]";
}

constexpr int kExitFailed = 1;
constexpr int kExitBadArguments = 2;
constexpr int kExitNotElevated = 3;
constexpr int kExitBusy = 4;

int fail(const std::string& command, const std::string& reason, const std::string& extra = "") {
    std::printf("{\"command\":%s,\"ok\":false,\"reason\":%s%s}\n", quote(command).c_str(),
                quote(reason).c_str(), extra.c_str());
    return kExitFailed;
}

// A failure the caller tells apart by its exit code and `error` field, not its text.
std::string failure_json(const char* error, const std::string& command, const std::string& reason) {
    return std::string("{\"command\":") + (command.empty() ? "null" : quote(command)) + ",\"ok\":false,\"error\":\"" +
           error + "\",\"reason\":" + quote(reason) + "}";
}
int fail_with(int exit_code, const char* error, const std::string& command, const std::string& reason) {
    std::printf("%s\n", failure_json(error, command, reason).c_str());
    return exit_code;
}

int usage(const std::string& command, const std::string& reason) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  isotone-devicetool list\n"
                 "  isotone-devicetool status    <endpoint>\n"
                 "  isotone-devicetool install   <endpoint> [--mode mfx|efx|gfx] [--replace-equalizerapo] [--dry-run]\n"
                 "  isotone-devicetool uninstall <endpoint> [--dry-run]\n"
                 "  isotone-devicetool repair    [<endpoint>] [--mode mfx|efx|gfx] [--dry-run]\n"
                 "  isotone-devicetool test      <endpoint>\n"
                 "  isotone-devicetool machine-install   --dll <absolute path> [--dry-run]\n"
                 "  isotone-devicetool machine-uninstall [--remove-data] [--dry-run]\n"
                 "  isotone-devicetool enable-enhancements <endpoint> [--dry-run]\n"
                 "  isotone-devicetool restart-audio [--dry-run]\n"
                 "  isotone-devicetool layouts   <endpoint>\n"
                 "  isotone-devicetool set-layout <endpoint> --layout stereo|2.1|5.1|7.1 [--dry-run]\n"
                 "  isotone-devicetool roundtrip <endpoint> [--mode mfx|efx|gfx] [--replace-equalizerapo\n"
                 "                               [--simulate-equalizerapo PRE,POST[,unhosted|unregistered][,deleted|fallback]]]\n"
                 "  isotone-devicetool serve     --pipe <name> --parent <pid>\n"
                 "Every command takes --output <absolute path>: the JSON goes to that new file.\n"
                 "<endpoint>: {guid}, guid, {0.0.0.00000000}.{guid} (render) or {0.0.1.00000000}.{guid} (capture).\n"
                 "Without --mode: the mode of Equalizer APO's post-mix slot where it is on the endpoint,\n"
                 "otherwise the mode upstream's install would pick.\n");
    return fail_with(kExitBadArguments, "bad_arguments", command, reason);
}

// ---------------------------------------------------------------------------
// Registry reads that must not throw

bool try_read_string(const std::wstring& key, const std::wstring& name, std::wstring* out) {
    try {
        if (!RegistryHelper::keyExists(key) || !RegistryHelper::valueExists(key, name)) return false;
        *out = RegistryHelper::readValue(key, name);
        return true;
    } catch (RegistryException&) {
        return false;
    }
}

// Type of a value, or REG_NONE with *present false when it does not exist.
// During a dry run, what the dry run wrote counts: RegistryHelper has no typed
// read of its own, so these reads would otherwise see only the real registry,
// and a slot the dry run filled would read as empty.
DWORD value_type(const std::wstring& key, const std::wstring& name, bool* present) {
    *present = false;
    if (const auto* dry = dynamic_cast<const DryRunRegistry*>(RegistryHelper::dryRun)) {
        unsigned long type = REG_NONE;
        if (dry->valueType(key, name, &type, present)) return *present ? type : REG_NONE;
    }
    if (!RegistryHelper::keyExists(key)) return REG_NONE;
    HKEY h = nullptr;
    try {
        h = RegistryHelper::openKey(key, KEY_QUERY_VALUE | KEY_WOW64_64KEY);
    } catch (RegistryException&) {
        return REG_NONE;   // a key that exists only in the dry run has no other values
    }
    DWORD type = REG_NONE;
    const LSTATUS status = RegQueryValueExW(h, name.c_str(), nullptr, &type, nullptr, nullptr);
    RegCloseKey(h);
    *present = status == ERROR_SUCCESS;
    return type;
}

// A REG_DWORD, including one a dry run has written.
unsigned long read_dword(const std::wstring& key, const std::wstring& name) {
    std::wstring text;
    if (RegistryHelper::dryRun != nullptr && RegistryHelper::dryRun->readValue(key, name, &text)) {
        return std::wcstoul(text.c_str(), nullptr, 10);
    }
    return RegistryHelper::readDWORDValue(key, name);
}

const char* type_name(DWORD type) {
    switch (type) {
        case REG_SZ: return "REG_SZ";
        case REG_EXPAND_SZ: return "REG_EXPAND_SZ";
        case REG_MULTI_SZ: return "REG_MULTI_SZ";
        case REG_DWORD: return "REG_DWORD";
        case REG_BINARY: return "REG_BINARY";
        default: return "other";
    }
}

bool guid_equals(const std::wstring& text, const GUID& guid) {
    GUID parsed;
    return SUCCEEDED(CLSIDFromString(text.c_str(), &parsed)) && parsed == guid;
}

const char* classify(const std::wstring& clsid) {
    if (guid_equals(clsid, ISOAPO_PRE_MIX_GUID)) return "isoapo_pre_mix";
    if (guid_equals(clsid, ISOAPO_POST_MIX_GUID)) return "isoapo_post_mix";
    if (guid_equals(clsid, EQUALIZERAPO_PRE_MIX_GUID)) return "equalizerapo_pre_mix";
    if (guid_equals(clsid, EQUALIZERAPO_POST_MIX_GUID)) return "equalizerapo_post_mix";
    return "other";
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// ---------------------------------------------------------------------------
// Endpoints

struct Endpoint {
    std::wstring guid;   // lower case, in braces: the MMDevices subkey name
    bool input = false;
    std::wstring key;    // full MMDevices key path
};

// 2 = malformed argument, 1 = well-formed but no such endpoint, 0 = found.
// Accepted: {guid}, guid, and the full device IDs {0.0.0.00000000}.{guid}
// (render only) and {0.0.1.00000000}.{guid} (capture only). Nothing else
// around the GUID.
int resolve_endpoint(const std::string& arg, Endpoint* out) {
    std::string s = arg;
    int flow = -1;   // -1 either, 0 render, 1 capture
    const std::string render_prefix = "{0.0.0.00000000}.", capture_prefix = "{0.0.1.00000000}.";
    if (s.rfind(render_prefix, 0) == 0) {
        flow = 0;
        s = s.substr(render_prefix.size());
    } else if (s.rfind(capture_prefix, 0) == 0) {
        flow = 1;
        s = s.substr(capture_prefix.size());
    }
    if (s.size() == 36) s = "{" + s + "}";
    if (s.size() != 38 || s.front() != '{' || s.back() != '}') return 2;
    for (size_t i = 1; i < 37; ++i) {
        const bool dash = i == 9 || i == 14 || i == 19 || i == 24;
        if (dash ? s[i] != '-' : !std::isxdigit(static_cast<unsigned char>(s[i]))) return 2;
    }
    std::wstring w(s.begin(), s.end());
    GUID guid;
    if (FAILED(CLSIDFromString(w.c_str(), &guid))) return 2;
    for (wchar_t& c : w) c = static_cast<wchar_t>(towlower(c));

    const std::wstring render = std::wstring(kMMDevices) + L"\\Render\\" + w;
    const std::wstring capture = std::wstring(kMMDevices) + L"\\Capture\\" + w;
    if (flow != 1 && RegistryHelper::keyExists(render)) {
        *out = {w, false, render};
    } else if (flow != 0 && RegistryHelper::keyExists(capture)) {
        *out = {w, true, capture};
    } else {
        return 1;
    }
    return 0;
}

std::string device_id(const Endpoint& e) {
    return utf8((e.input ? L"{0.0.1.00000000}." : L"{0.0.0.00000000}.") + e.guid);
}

std::string state_json(unsigned long state) {
    // Upstream: "disabled devices do not actually use DEVICE_STATE_DISABLED in
    // registry but 0x10000000". Such an endpoint also keeps the ACTIVE bit
    // (Stereo Mix here reads 0x10000001) while the MMDevice API does not
    // enumerate it as active, so it is reported as disabled only.
    std::vector<std::string> flags;
    const bool disabled = (state & DEVICE_STATE_DISABLED) || (state & 0x10000000);
    if ((state & DEVICE_STATE_ACTIVE) && !disabled) flags.push_back("active");
    if (disabled) flags.push_back("disabled");
    if (state & DEVICE_STATE_NOTPRESENT) flags.push_back("not_present");
    if (state & DEVICE_STATE_UNPLUGGED) flags.push_back("unplugged");
    char hex[16];
    std::snprintf(hex, sizeof(hex), "0x%08lx", state);
    return std::string("{\"raw\":") + quote(std::string(hex)) + ",\"flags\":" + string_array(flags) + "}";
}

struct Slots {
    std::string json;
    std::string lists_json;
    bool isoapo = false;
    bool equalizerapo = false;
    std::vector<std::string> warnings;
};

Slots read_slots(const Endpoint& e) {
    Slots s;
    const std::wstring fx = e.key + L"\\FxProperties";
    std::map<std::wstring, std::vector<std::string>> seen;   // clsid -> slot labels
    s.json = "[";
    bool first = true;
    for (const SlotName& slot : kEffectSlots) {
        bool present = false;
        const DWORD type = value_type(fx, slot.value, &present);
        std::string entry = std::string("{\"slot\":") + quote(std::string(slot.label)) +
                            ",\"value_name\":" + quote(std::wstring(slot.value)) +
                            ",\"present\":" + boolean(present);
        if (present && type == REG_SZ) {
            std::wstring clsid;
            try_read_string(fx, slot.value, &clsid);
            const std::string kind = classify(clsid);
            entry += ",\"clsid\":" + quote(clsid) + ",\"kind\":" + quote(kind);
            if (kind.rfind("isoapo", 0) == 0) s.isoapo = true;
            if (kind.rfind("equalizerapo", 0) == 0) s.equalizerapo = true;
            std::wstring upper = clsid;
            for (wchar_t& c : upper) c = static_cast<wchar_t>(towupper(c));
            seen[upper].push_back(slot.label);
        } else if (present) {
            entry += std::string(",\"type\":\"") + type_name(type) + "\",\"kind\":\"not_a_clsid\"";
            s.warnings.push_back(std::string(slot.label) + " effect slot is " + type_name(type) +
                                 ", not REG_SZ");
        }
        s.json += (first ? "" : ",") + entry + "}";
        first = false;
    }
    s.json += "]";

    s.lists_json = "[";
    first = true;
    for (const SlotName& list : kEffectLists) {
        bool present = false;
        const DWORD type = value_type(fx, list.value, &present);
        if (!present) continue;
        std::string entry = std::string("{\"slot\":") + quote(std::string(list.label)) +
                            ",\"value_name\":" + quote(std::wstring(list.value)) +
                            ",\"type\":\"" + type_name(type) + "\"";
        if (type == REG_MULTI_SZ) {
            std::string items = "[";
            try {
                bool first_item = true;
                for (const std::wstring& clsid : RegistryHelper::readMultiValue(fx, list.value)) {
                    const std::string kind = classify(clsid);
                    if (kind.rfind("isoapo", 0) == 0) s.isoapo = true;
                    if (kind.rfind("equalizerapo", 0) == 0) s.equalizerapo = true;
                    std::wstring upper = clsid;
                    for (wchar_t& c : upper) c = static_cast<wchar_t>(towupper(c));
                    seen[upper].push_back(list.label);
                    items += std::string(first_item ? "" : ",") + "{\"clsid\":" + quote(clsid) +
                             ",\"kind\":" + quote(kind) + "}";
                    first_item = false;
                }
            } catch (RegistryException&) {
            }
            entry += ",\"effects\":" + items + "]";
        }
        s.lists_json += (first ? "" : ",") + entry + "}";
        first = false;
    }
    s.lists_json += "]";

    for (const auto& [clsid, labels] : seen) {
        if (labels.size() > 1) {
            std::string joined;
            for (const std::string& l : labels) joined += (joined.empty() ? "" : ", ") + l;
            s.warnings.push_back("CLSID " + utf8(clsid) + " occupies " + joined +
                                 "; an APO in two slots can process the audio twice");
        }
    }
    return s;
}

std::string processing_modes_json(const Endpoint& e, std::vector<std::string>* warnings) {
    const std::wstring fx = e.key + L"\\FxProperties";
    std::string out = "[";
    bool first = true;
    for (const SlotName& slot : kModeSlots) {
        bool present = false;
        const DWORD type = value_type(fx, slot.value, &present);
        std::string entry = std::string("{\"slot\":") + quote(std::string(slot.label)) +
                            ",\"present\":" + boolean(present);
        if (present) {
            entry += std::string(",\"type\":\"") + type_name(type) + "\"";
            if (type == REG_MULTI_SZ) {
                std::vector<std::string> modes;
                try {
                    for (const std::wstring& m : RegistryHelper::readMultiValue(fx, slot.value))
                        modes.push_back(utf8(m));
                } catch (RegistryException&) {
                }
                entry += ",\"modes\":" + string_array(modes);
            } else {
                // The trap the stage 1b install fell into: a CLSID written here
                // disables the endpoint's effects.
                warnings->push_back(std::string(slot.label) +
                                    " processing-modes value is not REG_MULTI_SZ");
            }
        }
        out += (first ? "" : ",") + entry + "}";
        first = false;
    }
    return out + "]";
}

// An install record under <base>\Child APOs\{guid}, as upstream writes it.
struct Record {
    bool exists = false;
    bool legacy = false;   // upstream's version-0 form: a value named by the device
    std::string json = "null";
};

Record read_record(const std::wstring& base, const std::wstring& guid) {
    Record r;
    const std::wstring key = base + L"\\" + guid;
    if (RegistryHelper::keyExists(key)) {
        r.exists = true;
        std::string json = "{";
        const auto field = [&](const char* name, const wchar_t* value) {
            std::wstring v;
            const bool have = try_read_string(key, value, &v);
            json += std::string(json.size() > 1 ? "," : "") + quote(std::string(name)) + ":" +
                    (have ? quote(v) : "null");
        };
        field("version", L"Version");
        field("pre_mix_child", L"PreMixChild");
        field("post_mix_child", L"PostMixChild");
        field("allow_silent_buffer_modification", L"AllowSilentBufferModification");
        json += ",\"original_slots\":{";
        bool first = true;
        for (const SlotName& slot : kEffectSlots) {
            std::wstring v;
            const bool have = try_read_string(key, slot.value, &v);
            json += std::string(first ? "" : ",") + quote(std::string(slot.label)) + ":" +
                    (have ? quote(v) : "null");
            first = false;
        }
        r.json = json + "}}";
    } else {
        std::wstring v;
        if (try_read_string(base, guid, &v)) {
            r.exists = true;
            r.legacy = true;
            r.json = "{\"legacy_child\":" + quote(v) + "}";
        }
    }
    return r;
}

std::string mode_name(DeviceAPOInfo::InstallMode mode) {
    switch (mode) {
        case DeviceAPOInfo::INSTALL_LFX_GFX: return "LFX_GFX";
        case DeviceAPOInfo::INSTALL_SFX_MFX: return "SFX_MFX";
        case DeviceAPOInfo::INSTALL_SFX_EFX: return "SFX_EFX";
    }
    return "unknown";
}

// Registration of the post-mix class, which is the one devicetool installs.
struct Registration {
    bool apo_registered = false;   // AudioEngine\AudioProcessingObjects, what Windows resolves
    std::wstring dll;
    bool dll_exists = false;
    bool dll_under_user_profile = false;
    // LOCAL SERVICE may read and execute the DLL; empty when that could not be
    // determined (no DLL, or the access check failed).
    std::optional<bool> dll_local_service_can_load;
    std::string json;
};

// Whether LOCAL SERVICE (S-1-5-19), the account audiodg hosts APOs under, may
// read and execute `path`, by an access check against the file's security
// descriptor for that account and the groups Authz gives it. A DLL moved into
// Program Files keeps its profile ACL, and one on another drive can carry any
// ACL; either way audiodg's load fails and the whole endpoint returns
// E_ACCESSDENIED.
std::optional<bool> local_service_can_load(const std::wstring& path) {
    try {
        const ACCESS_MASK granted = RegistryHelper::getFileAccessForUser(path, SECURITY_LOCAL_SERVICE_RID);
        const ACCESS_MASK needed = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
        return (granted & needed) == needed;
    } catch (RegistryException&) {
        return std::nullopt;
    }
}

// audiodg hosts the APO as LocalService, which cannot read user profiles; a DLL
// there fails the whole endpoint with E_ACCESSDENIED. machine-install refuses
// such a path before registering it, and status reports one already registered.
bool under_user_profile(const std::wstring& path) {
    std::wstring lower = path;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    wchar_t profiles[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    std::wstring root = L"c:\\users\\";
    if (GetProfilesDirectoryW(profiles, &n)) {
        root = profiles;
        for (wchar_t& c : root) c = static_cast<wchar_t>(towlower(c));
        if (root.back() != L'\\') root += L'\\';
    }
    return lower.rfind(root, 0) == 0;
}

Registration read_registration() {
    Registration r;
    const std::wstring clsid = RegistryHelper::getGuidString(ISOAPO_POST_MIX_GUID);
    r.apo_registered = RegistryHelper::keyExists(std::wstring(kApoRegistration) + L"\\" + clsid);
    try_read_string(std::wstring(kClsid) + L"\\" + clsid + L"\\InprocServer32", L"", &r.dll);
    if (!r.dll.empty()) {
        r.dll_exists = GetFileAttributesW(r.dll.c_str()) != INVALID_FILE_ATTRIBUTES;
        r.dll_under_user_profile = under_user_profile(r.dll);
        if (r.dll_exists) r.dll_local_service_can_load = local_service_can_load(r.dll);
    }
    r.json = std::string("{\"clsid\":") + quote(clsid) + ",\"apo_registered\":" +
             boolean(r.apo_registered) + ",\"dll\":" + (r.dll.empty() ? "null" : quote(r.dll)) +
             ",\"dll_exists\":" + boolean(r.dll_exists) +
             ",\"dll_under_user_profile\":" + boolean(r.dll_under_user_profile) +
             ",\"dll_local_service_can_load\":" +
             (r.dll_local_service_can_load ? boolean(*r.dll_local_service_can_load) : "null") + "}";
    return r;
}

// ---------------------------------------------------------------------------
// Commands

int cmd_list() {
    std::string out = "{\"command\":\"list\",\"ok\":true,\"endpoints\":[";
    bool first = true;
    for (bool input : {false, true}) {
        const std::wstring base = std::wstring(kMMDevices) + (input ? L"\\Capture" : L"\\Render");
        std::vector<std::wstring> guids;
        try {
            guids = RegistryHelper::enumSubKeys(base);
        } catch (RegistryException& e) {
            return fail("list", utf8(e.getMessage()));
        }
        for (const std::wstring& g : guids) {
            Endpoint e{g, input, base + L"\\" + g};
            unsigned long state = 0;
            bool have_state = true;
            try {
                state = RegistryHelper::readDWORDValue(e.key, L"DeviceState");
            } catch (RegistryException&) {
                have_state = false;
            }
            std::wstring name, connection;
            try_read_string(e.key + L"\\Properties", L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", &name);
            try_read_string(e.key + L"\\Properties", L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", &connection);
            const Slots slots = read_slots(e);
            const char* backend = slots.isoapo && slots.equalizerapo ? "conflict"
                                  : slots.isoapo                    ? "native"
                                  : slots.equalizerapo              ? "equalizerapo"
                                                                    : "none";
            out += std::string(first ? "" : ",") + "{\"guid\":" + quote(g) + ",\"id\":" +
                   quote(device_id(e)) + ",\"flow\":" + (input ? "\"capture\"" : "\"render\"") +
                   ",\"name\":" + quote(name) + ",\"connection\":" + quote(connection) +
                   ",\"state\":" + (have_state ? state_json(state) : "null") +
                   ",\"backend\":\"" + backend + "\"}";
            first = false;
        }
    }
    std::printf("%s]}\n", out.c_str());
    return 0;
}

struct KnownMode {
    std::optional<DeviceAPOInfo::InstallMode> mode;
    const char* source = nullptr;   // "record", "effect_slot", "equalizerapo_record"
};

// IsoAPO's state on an endpoint, as status reports it, with the commands that
// move it on.
struct IsoState {
    const char* name;
    std::vector<std::string> remedies;
};

KnownMode install_mode_of(const Endpoint& e);
IsoState isoapo_state(const Endpoint& e);
bool equalizerapo_over_isoapo(const Endpoint& e);
std::string interrupted_command(const Endpoint& e);
std::optional<DeviceAPOInfo::InstallMode> recorded_mode(const Endpoint& e);
DeviceAPOInfo::InstallMode default_mode(DeviceAPOInfo& info, const Endpoint& e);

int cmd_status(const Endpoint& e) {
    std::vector<std::string> warnings;
    Slots slots = read_slots(e);
    warnings.insert(warnings.end(), slots.warnings.begin(), slots.warnings.end());
    const std::string modes = processing_modes_json(e, &warnings);

    DeviceAPOInfo info;
    std::string device = "null", load_error;
    try {
        if (info.load(e.guid)) {
            char mask[16];
            std::snprintf(mask, sizeof(mask), "0x%lx", info.getChannelMask());
            device = "{\"name\":" + quote(info.getDeviceName()) + ",\"connection\":" +
                     quote(info.getConnectionName()) + ",\"default_device\":" +
                     boolean(info.isDefaultDevice()) + ",\"disabled\":" + boolean(info.isDisabled()) +
                     ",\"unplugged\":" + boolean(info.isUnplugged()) + ",\"channels\":" +
                     std::to_string(info.getChannelCount()) + ",\"sample_rate\":" +
                     std::to_string(info.getSampleRate()) + ",\"channel_mask\":" + quote(std::string(mask)) +
                     ",\"enhancements_disabled\":" + boolean(info.isEnhancementsDisabled()) + "}";
        } else {
            load_error = "not_present";
        }
    } catch (RegistryException& ex) {
        load_error = utf8(ex.getMessage());
    }

    const Record iso = read_record(kIsoChildApos, e.guid);
    const Record eapo = read_record(kEapoChildApos, e.guid);
    const IsoState state = isoapo_state(e);
    const std::string state_name = state.name;
    const bool detached = state_name == "detached";
    const bool unrecorded = state_name == "unrecorded";
    const bool over = equalizerapo_over_isoapo(e);
    const KnownMode known = install_mode_of(e);
    const std::string interrupted = interrupted_command(e);
    const char* backend = slots.isoapo && slots.equalizerapo ? "conflict"
                          : slots.isoapo                    ? "native"
                          : slots.equalizerapo              ? "equalizerapo"
                                                            : "none";
    if (!interrupted.empty())
        warnings.push_back("a devicetool " + interrupted + " on this endpoint was interrupted; the next install, uninstall or repair first puts the endpoint back as it was before it");
    if (state_name == "alongside_equalizerapo")
        warnings.push_back(over ? "Equalizer APO was installed on this endpoint after IsoAPO, and both are in effect slots; install --replace-equalizerapo removes Equalizer APO, uninstall removes IsoAPO"
                                : "IsoAPO and Equalizer APO are both on this endpoint");
    if (state_name == "replaced_by_equalizerapo")
        warnings.push_back("Equalizer APO's Device Selector took IsoAPO's slot on this endpoint; install --replace-equalizerapo puts IsoAPO back, uninstall keeps Equalizer APO");
    if (detached && slots.equalizerapo)
        warnings.push_back("IsoAPO has an install record but is in no effect slot, and Equalizer APO is on the endpoint; install --replace-equalizerapo replaces it, uninstall keeps it");
    else if (detached && known.mode)
        warnings.push_back("IsoAPO has an install record but is in no effect slot: detached, probably by a driver update; run repair");
    else if (detached)
        warnings.push_back("IsoAPO has an install record but is in no effect slot: detached, probably by a driver update; the record predates Isotone.InstallMode, so run repair with --mode");
    if (unrecorded)
        warnings.push_back("IsoAPO is in an effect slot with no install record (installed by the retired install.ps1); restore FxProperties from that script's .reg backup, then install with devicetool");
    if (!load_error.empty() && load_error != "not_present")
        warnings.push_back("upstream load failed: " + load_error);

    std::wstring config_path, uninstaller;
    const bool eapo_installed = RegistryHelper::keyExists(EQUALIZERAPO_REGPATH);
    const bool have_config = try_read_string(EQUALIZERAPO_REGPATH, L"ConfigPath", &config_path);
    // Equalizer APO's own uninstaller, for the UI to run once IsoAPO has
    // replaced it everywhere it is wanted.
    const bool have_uninstaller = try_read_string(
        L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\EqualizerAPO", L"UninstallString",
        &uninstaller);

    const Registration reg = read_registration();
    bool protected_audiodg_disabled = false;
    try {
        protected_audiodg_disabled = DeviceAPOInfo::checkProtectedAudioDG(false);
    } catch (RegistryException&) {
    }

    std::string recommended = "null", default_install = "null";
    if (load_error.empty() && !info.isInstalled()) {
        recommended = quote(mode_name(info.getCurrentInstallState().installMode));
        if (!e.input) default_install = quote(mode_name(default_mode(info, e)));
    }
    const std::optional<DeviceAPOInfo::InstallMode> recorded = recorded_mode(e);
    const std::string install_mode =
        std::string("{\"recorded\":") + (recorded ? quote(mode_name(*recorded)) : "null") +
        ",\"effective\":" + (known.mode ? quote(mode_name(*known.mode)) : "null") +
        ",\"source\":" + (known.source ? quote(std::string(known.source)) : "null") + "}";

    std::printf(
        "{\"command\":\"status\",\"ok\":true,\"guid\":%s,\"id\":%s,\"flow\":\"%s\",\"device\":%s,"
        "\"backend\":\"%s\",\"effect_slots\":%s,\"effect_lists\":%s,\"processing_modes\":%s,"
        "\"isoapo\":{\"state\":%s,\"remedies\":%s,\"in_slots\":%s,\"record\":%s,\"install_mode\":%s,"
        "\"detached\":%s,\"unrecorded\":%s,\"equalizerapo_over_isoapo\":%s,\"interrupted_command\":%s,\"registration\":%s},"
        "\"equalizerapo\":{\"in_slots\":%s,\"installed\":%s,\"config_path\":%s,\"uninstaller\":%s,\"record\":%s},"
        "\"default_install_mode\":%s,\"upstream_recommended_mode\":%s,\"protected_audiodg_disabled\":%s,\"warnings\":%s}\n",
        quote(e.guid).c_str(), quote(device_id(e)).c_str(), e.input ? "capture" : "render",
        device.c_str(), backend, slots.json.c_str(), slots.lists_json.c_str(), modes.c_str(),
        quote(state_name).c_str(), string_array(state.remedies).c_str(), boolean(slots.isoapo),
        iso.json.c_str(), install_mode.c_str(), boolean(detached), boolean(unrecorded), boolean(over),
        interrupted.empty() ? "null" : quote(interrupted).c_str(), reg.json.c_str(),
        boolean(slots.equalizerapo), boolean(eapo_installed),
        have_config ? quote(config_path).c_str() : "null", have_uninstaller ? quote(uninstaller).c_str() : "null",
        eapo.json.c_str(), default_install.c_str(), recommended.c_str(),
        boolean(protected_audiodg_disabled), string_array(warnings).c_str());
    return 0;
}

bool parse_mode(const std::string& text, DeviceAPOInfo::InstallMode* mode) {
    if (text == "mfx") *mode = DeviceAPOInfo::INSTALL_SFX_MFX;
    else if (text == "efx") *mode = DeviceAPOInfo::INSTALL_SFX_EFX;
    else if (text == "gfx") *mode = DeviceAPOInfo::INSTALL_LFX_GFX;
    else return false;
    return true;
}

// Post-mix only: IsoAPO filters in every slot it occupies, and the stage 1b
// measurement showed two slots applying the EQ twice.
void select_post_mix_only(DeviceAPOInfo* info, DeviceAPOInfo::InstallMode mode) {
    DeviceAPOInfo::InstallState& s = info->getSelectedInstallState();
    s = info->getCurrentInstallState();
    s.installMode = mode;
    s.installPreMix = false;
    s.installPostMix = true;
    s.useOriginalAPOPreMix = false;
    // The APO being replaced becomes the child, unless it is Equalizer APO:
    // wrapping another EQ would apply both.
    const std::wstring original = info->getOriginalAPOPostMix();
    s.useOriginalAPOPostMix = !original.empty() && classify(original) == std::string("other");
}

// A CLSID that is neither Equalizer APO's nor IsoAPO's: a vendor or Windows APO.
bool is_other_apo(const std::wstring& value) {
    GUID guid;
    return !value.empty() && value[0] == L'{' && SUCCEEDED(CLSIDFromString(value.c_str(), &guid)) &&
           std::string(classify(value)) == "other";
}

bool same_clsid(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool is_equalizerapo(const std::wstring& clsid) {
    return std::string(classify(clsid)).rfind("equalizerapo", 0) == 0;
}

bool apo_registered(const std::wstring& clsid) {
    return RegistryHelper::keyExists(std::wstring(kApoRegistration) + L"\\" + clsid);
}

std::wstring iso_record_key(const Endpoint& e) { return std::wstring(kIsoChildApos) + L"\\" + e.guid; }
std::wstring eapo_record_key(const Endpoint& e) { return std::wstring(kEapoChildApos) + L"\\" + e.guid; }

bool is_isoapo(const std::wstring& clsid) {
    return std::string(classify(clsid)).rfind("isoapo", 0) == 0;
}

// Equalizer APO's Device Selector ticked this endpoint after IsoAPO was
// installed on it. Upstream's load maps only Equalizer APO's own classes to
// !VALUE, so its install records IsoAPO's class as the original of the slot it
// took (and as its child when "use original APO" is on). IsoAPO's record is
// then the only one that says what the device had before either EQ.
bool equalizerapo_over_isoapo(const Endpoint& e) {
    const std::wstring record = eapo_record_key(e);
    if (!RegistryHelper::keyExists(record)) return false;
    std::wstring v;
    for (const SlotName& slot : kEffectSlots) {
        if (try_read_string(record, slot.value, &v) && is_isoapo(v)) return true;
    }
    for (const wchar_t* child : {L"PreMixChild", L"PostMixChild"}) {
        if (try_read_string(record, child, &v) && is_isoapo(v)) return true;
    }
    return false;
}

// The slot each install mode writes the post-mix class to, and the slots it
// deletes (upstream DeviceAPOInfo::install).
const char* post_slot_label(DeviceAPOInfo::InstallMode mode) {
    return mode == DeviceAPOInfo::INSTALL_LFX_GFX ? "GFX" : mode == DeviceAPOInfo::INSTALL_SFX_MFX ? "MFX" : "EFX";
}
bool mode_deletes(DeviceAPOInfo::InstallMode mode, const std::string& label) {
    return mode == DeviceAPOInfo::INSTALL_LFX_GFX ? (label == "SFX" || label == "MFX" || label == "EFX")
                                                  : (label == "LFX" || label == "GFX");
}
std::optional<DeviceAPOInfo::InstallMode> mode_of_post_slot(const std::string& label) {
    if (label == "GFX") return DeviceAPOInfo::INSTALL_LFX_GFX;
    if (label == "MFX") return DeviceAPOInfo::INSTALL_SFX_MFX;
    if (label == "EFX") return DeviceAPOInfo::INSTALL_SFX_EFX;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// What upstream's install changes outside the effect slots and its uninstall
// never puts back: it deletes the "disable all enhancements" flag, and adds a
// processing-mode list for each slot it writes. Isotone records both in the
// install record and restores them on uninstall.

const wchar_t* const kDisableEnhancements = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";
const wchar_t* const kRecordEnhancements  = L"Isotone.DisableEnhancements";      // decimal, only if the flag was set
const wchar_t* const kRecordModesAbsent   = L"Isotone.ProcessingModesAbsent";    // mode value names, '|' separated
const wchar_t* const kRecordInstallMode   = L"Isotone.InstallMode";              // mfx, efx or gfx
// FxProperties' title, which upstream's install writes.
const wchar_t* const kFxTitle = L"{b725f130-47ef-101a-a5f1-02608c9eebac},10";
// Every value an install record holds besides the slots: upstream's, then Isotone's.
const wchar_t* const kRecordValues[] = {
    L"PreMixChild", L"PostMixChild", L"AllowSilentBufferModification", L"DisableAutomaticAdjustment", L"Version",
    kRecordEnhancements, kRecordModesAbsent, kRecordInstallMode,
};

struct Extras {
    bool enhancements_present = false;
    unsigned long enhancements = 0;
    std::vector<std::wstring> modes_absent;
};

Extras extras_on_endpoint(const Endpoint& e) {
    Extras x;
    const std::wstring fx = e.key + L"\\FxProperties";
    if (!RegistryHelper::keyExists(fx)) {
        for (const SlotName& m : kModeSlots) x.modes_absent.push_back(m.value);
        return x;
    }
    bool present = false;
    if (value_type(fx, kDisableEnhancements, &present) == REG_DWORD && present) {
        x.enhancements_present = true;
        x.enhancements = read_dword(fx, kDisableEnhancements);
    }
    for (const SlotName& m : kModeSlots) {
        if (!RegistryHelper::valueExists(fx, m.value)) x.modes_absent.push_back(m.value);
    }
    return x;
}

void record_extras(const Endpoint& e, const Extras& x) {
    const std::wstring record = iso_record_key(e);
    if (x.enhancements_present) {
        RegistryHelper::writeValue(record, kRecordEnhancements, std::to_wstring(x.enhancements));
    }
    std::wstring joined;
    for (const std::wstring& m : x.modes_absent) joined += (joined.empty() ? L"" : L"|") + m;
    RegistryHelper::writeValue(record, kRecordModesAbsent, joined);
}

Extras recorded_extras(const Endpoint& e) {
    Extras x;
    const std::wstring record = iso_record_key(e);
    std::wstring v;
    if (try_read_string(record, kRecordEnhancements, &v)) {
        x.enhancements_present = true;
        x.enhancements = std::wcstoul(v.c_str(), nullptr, 10);
    }
    if (try_read_string(record, kRecordModesAbsent, &v)) {
        size_t start = 0;
        while (start <= v.size()) {
            const size_t bar = v.find(L'|', start);
            const std::wstring part = v.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start);
            if (!part.empty()) x.modes_absent.push_back(part);
            if (bar == std::wstring::npos) break;
            start = bar + 1;
        }
    }
    return x;
}

void restore_extras(const Endpoint& e, const Extras& x) {
    const std::wstring fx = e.key + L"\\FxProperties";
    if (!RegistryHelper::keyExists(fx)) return;
    if (x.enhancements_present) {
        RegistryHelper::writeDWORDValue(fx, kDisableEnhancements, x.enhancements);
    }
    for (const std::wstring& m : x.modes_absent) {
        if (RegistryHelper::valueExists(fx, m)) RegistryHelper::deleteValue(fx, m);
    }
}

// Enhancements are off, as upstream's DeviceAPOInfo::load reads the flag: a
// REG_DWORD other than 0. A flag that cannot be read reads as not set, as
// try_read_string reads a value.
bool enhancements_disabled(const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    bool present = false;
    try {
        return value_type(fx, kDisableEnhancements, &present) == REG_DWORD && present &&
               read_dword(fx, kDisableEnhancements) != 0;
    } catch (RegistryException&) {
        return false;
    }
}

// enable-enhancements: what upstream's install does to the flag ("force-enable
// enhancements", DeviceAPOInfo::install), deleting it whatever its value.
// Returns whether it was there. The install record is left alone: its
// Isotone.DisableEnhancements is the flag as it was before IsoAPO, which
// uninstall puts back, and turning enhancements on later does not change what
// the device had then. Throws RegistryException.
bool enable_enhancements(const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    if (!RegistryHelper::keyExists(fx) || !RegistryHelper::valueExists(fx, kDisableEnhancements)) return false;
    RegistryHelper::deleteValue(fx, kDisableEnhancements);
    return true;
}

// ---------------------------------------------------------------------------
// Replacing Equalizer APO

struct Replacement {
    int removed = 0;              // slots left empty
    int restored = 0;             // slots given back the APO Equalizer APO was running there
    std::wstring child;           // what IsoAPO took over from Equalizer APO's post-mix instance
    bool record_taken_over = false;
    bool took_back = false;       // Equalizer APO had been installed over IsoAPO; its install was undone
};

// Choosing IsoAPO removes Equalizer APO (the owner's decision), so its install
// record for the endpoint goes too, and IsoAPO's record takes what it says each
// slot held before Equalizer APO:
//  - a slot where IsoAPO's record names an Equalizer APO class it replaced;
//  - a slot IsoAPO's record saw empty that Equalizer APO's record says held an
//    APO. Equalizer APO's install deletes the slots its mode does not use
//    (SFX_EFX deletes LFX and GFX), and its record is the only copy of what
//    they held.
// Uninstalling IsoAPO then restores the device as it was before either EQ, as
// Equalizer APO's own uninstall would. Equalizer APO's uninstaller only acts on
// endpoints where its classes are in the slots (DeviceSelector /u,
// DeviceAPOInfo::isInstalled), so it never sees this endpoint again either way.
bool isoapo_replaced_equalizerapo(const Endpoint& e) {
    if (!RegistryHelper::keyExists(eapo_record_key(e))) return false;
    for (const SlotName& slot : kEffectSlots) {
        std::wstring replaced;
        if (try_read_string(iso_record_key(e), slot.value, &replaced) && is_equalizerapo(replaced)) return true;
    }
    return false;
}

// Throws RegistryException.
bool take_over_equalizerapo_record(const Endpoint& e) {
    if (!isoapo_replaced_equalizerapo(e)) return false;
    const std::wstring eapo_record = eapo_record_key(e);
    const std::wstring iso_record = iso_record_key(e);
    for (const SlotName& slot : kEffectSlots) {
        std::wstring replaced;
        const bool recorded = try_read_string(iso_record, slot.value, &replaced);
        std::wstring original;
        const bool had_apo = try_read_string(eapo_record, slot.value, &original) && is_other_apo(original);
        if (recorded && is_equalizerapo(replaced)) {
            RegistryHelper::writeValue(iso_record, slot.value, had_apo ? original : std::wstring(APOGUID_NOVALUE));
        } else if (had_apo && (!recorded || replaced == APOGUID_NOVALUE)) {
            RegistryHelper::writeValue(iso_record, slot.value, original);
        }
    }
    RegistryHelper::deleteKey(eapo_record);
    if (RegistryHelper::keyExists(kEapoChildApos) && RegistryHelper::keyEmpty(kEapoChildApos))
        RegistryHelper::deleteKey(kEapoChildApos);
    return true;
}

// With --replace-equalizerapo, after upstream's install. Equalizer APO's classes
// leave the effect slots, so the endpoint has one EQ, and everything Equalizer
// APO was running keeps running exactly once, where it ran: inside the
// Equalizer APO instance that hosted it.
//  - a slot IsoAPO did not take, holding Equalizer APO's pre-mix or post-mix
//    class, gets that instance's child (PreMixChild or PostMixChild), or is
//    emptied;
//  - the slot IsoAPO took, if it held Equalizer APO's post-mix class: IsoAPO
//    hosts that instance's child now.
// The child is what the instance ran, whichever slot's original it came from:
// upstream's getOriginalAPOPostMix falls back to the GFX original (MFX for
// LFX_GFX) when the instance's own slot and SFX were empty, and Equalizer APO's
// install has deleted that slot. An APO the user had switched off in Equalizer
// APO ("use original APO" unticked) is no child and stays off. Upstream's
// install has already recorded every slot's value, and uninstall writes each
// back.
Replacement remove_equalizerapo(const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring eapo_record = eapo_record_key(e);
    const std::wstring iso_record = iso_record_key(e);
    std::wstring eapo_pre, eapo_post;
    try_read_string(eapo_record, L"PreMixChild", &eapo_pre);
    try_read_string(eapo_record, L"PostMixChild", &eapo_post);

    Replacement r;
    for (const SlotName& slot : kEffectSlots) {
        std::wstring clsid;
        if (!try_read_string(fx, slot.value, &clsid)) continue;
        const std::string kind = classify(clsid);
        if (kind == "equalizerapo_pre_mix" || kind == "equalizerapo_post_mix") {
            const std::wstring& child = kind == "equalizerapo_pre_mix" ? eapo_pre : eapo_post;
            if (is_other_apo(child)) {
                RegistryHelper::writeValue(fx, slot.value, child);
                ++r.restored;
            } else {
                RegistryHelper::deleteValue(fx, slot.value);
                ++r.removed;
            }
        } else if (kind == "isoapo_post_mix") {
            std::wstring replaced;
            try_read_string(iso_record, slot.value, &replaced);
            std::wstring current_child;
            try_read_string(iso_record, L"PostMixChild", &current_child);
            if (std::string(classify(replaced)) == "equalizerapo_post_mix" && is_other_apo(eapo_post) &&
                current_child.empty()) {
                RegistryHelper::writeValue(iso_record, L"PostMixChild", eapo_post);
                r.child = eapo_post;
            }
        }
    }
    r.record_taken_over = take_over_equalizerapo_record(e);
    return r;
}

// After upstream's uninstall: a slot given back to Equalizer APO while Equalizer
// APO is no longer registered would name a class that does not exist. It gets
// the APO Equalizer APO had replaced, when that is registered, or is emptied.
int drop_unregistered_equalizerapo(const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    if (!RegistryHelper::keyExists(fx)) return 0;
    int changed = 0;
    for (const SlotName& slot : kEffectSlots) {
        std::wstring clsid;
        if (!try_read_string(fx, slot.value, &clsid) || !is_equalizerapo(clsid) || apo_registered(clsid)) continue;
        std::wstring original;
        if (try_read_string(eapo_record_key(e), slot.value, &original) && is_other_apo(original) &&
            apo_registered(original)) {
            RegistryHelper::writeValue(fx, slot.value, original);
        } else {
            RegistryHelper::deleteValue(fx, slot.value);
        }
        ++changed;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Install and uninstall: upstream's code, and what Isotone adds around it.

// The mode an install used, as recorded, so a repair puts IsoAPO back in the
// same slot. Without it a repair after a replacement picks upstream's mode,
// which can be a slot never measured on that device.
std::optional<DeviceAPOInfo::InstallMode> recorded_mode(const Endpoint& e) {
    std::wstring text;
    DeviceAPOInfo::InstallMode mode;
    if (!try_read_string(iso_record_key(e), kRecordInstallMode, &text) ||
        !parse_mode(utf8(text), &mode)) {
        return std::nullopt;
    }
    return mode;
}

// The mode IsoAPO's install on this endpoint used: recorded since the pre-UI
// review; for an older record, the slot IsoAPO's post-mix class is in now, or
// the slot Equalizer APO's record says it took from IsoAPO. Otherwise unknown,
// and nothing guesses: an install mode decides which slots are deleted, so a
// guess could move IsoAPO to a slot never measured on the device.
KnownMode install_mode_of(const Endpoint& e) {
    if (const auto recorded = recorded_mode(e)) return {recorded, "record"};
    const std::wstring fx = e.key + L"\\FxProperties";
    std::optional<DeviceAPOInfo::InstallMode> found;
    int slots = 0;
    for (const SlotName& slot : kEffectSlots) {
        std::wstring clsid;
        if (try_read_string(fx, slot.value, &clsid) && std::string(classify(clsid)) == "isoapo_post_mix") {
            found = mode_of_post_slot(slot.label);
            ++slots;
        }
    }
    if (slots == 1 && found) return {found, "effect_slot"};
    if (slots > 1) return {};
    for (const SlotName& slot : kEffectSlots) {
        std::wstring original;
        if (try_read_string(eapo_record_key(e), slot.value, &original) &&
            std::string(classify(original)) == "isoapo_post_mix") {
            if (const auto m = mode_of_post_slot(slot.label)) return {m, "equalizerapo_record"};
        }
    }
    return {};
}

// A vendor APO an Equalizer APO instance hosts, where that instance sits in a
// slot `mode` deletes: upstream's install for SFX_MFX or SFX_EFX deletes LFX
// and GFX, and for LFX_GFX deletes SFX, MFX and EFX. Replacing Equalizer APO
// with such a mode would drop the vendor APO without a trace. Empty when there
// is none.
std::wstring hosted_apo_in_deleted_slot(const Endpoint& e, DeviceAPOInfo::InstallMode mode) {
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring eapo_record = eapo_record_key(e);
    std::wstring pre, post;
    try_read_string(eapo_record, L"PreMixChild", &pre);
    try_read_string(eapo_record, L"PostMixChild", &post);
    for (const SlotName& slot : kEffectSlots) {
        std::wstring clsid;
        if (!mode_deletes(mode, slot.label) || !try_read_string(fx, slot.value, &clsid)) continue;
        const std::string kind = classify(clsid);
        if (kind == "equalizerapo_pre_mix" && is_other_apo(pre)) return pre;
        if (kind == "equalizerapo_post_mix" && is_other_apo(post)) return post;
    }
    return L"";
}

// Throws RegistryException.
Replacement install_isoapo(DeviceAPOInfo& info, const Endpoint& e, DeviceAPOInfo::InstallMode mode,
                           bool replace_eapo) {
    const Extras extras = extras_on_endpoint(e);
    select_post_mix_only(&info, mode);
    info.install();
    record_extras(e, extras);
    RegistryHelper::writeValue(iso_record_key(e), kRecordInstallMode,
                               mode == DeviceAPOInfo::INSTALL_SFX_MFX   ? L"mfx"
                               : mode == DeviceAPOInfo::INSTALL_SFX_EFX ? L"efx"
                                                                        : L"gfx");
    return replace_eapo ? remove_equalizerapo(e) : Replacement{};
}

struct Uninstalled {
    bool fx_properties_missing = false;
    bool detached = false;
    bool under_equalizerapo = false;
    int equalizerapo_dropped = 0;
};

// What upstream's DeviceAPOInfo::uninstall writes, taken from the install record
// alone. Upstream's load refuses an endpoint that is not present (a USB DAC or
// headset removed for good), so its uninstall cannot run there, but the slots
// and the record are still in the registry. On a detached endpoint (IsoAPO in
// no slot) the slots are the driver's since the record was written, and
// upstream's uninstall would write back only what it reads there now, so only
// the record goes. Throws RegistryException.
void uninstall_from_record(const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring record = iso_record_key(e);
    std::wstring first;
    if (!read_slots(e).isoapo) {
        // Detached: leave the slots.
    } else if (try_read_string(record, kEffectSlots[0].value, &first) && first == APOGUID_NOKEY) {
        if (RegistryHelper::keyExists(fx)) RegistryHelper::deleteKey(fx);
    } else {
        for (const SlotName& slot : kEffectSlots) {
            std::wstring original;
            if (!try_read_string(record, slot.value, &original)) continue;
            if (original == APOGUID_NOVALUE) {
                if (RegistryHelper::valueExists(fx, slot.value)) RegistryHelper::deleteValue(fx, slot.value);
            } else if (!original.empty()) {
                RegistryHelper::writeValue(fx, slot.value, original);
            }
        }
    }
    if (RegistryHelper::keyExists(record)) RegistryHelper::deleteKey(record);
    if (RegistryHelper::keyExists(kIsoChildApos) && RegistryHelper::keyEmpty(kIsoChildApos))
        RegistryHelper::deleteKey(kIsoChildApos);
}

const char* mode_slot_label(const std::wstring& mode_value_name) {
    for (const SlotName& m : kModeSlots) {
        if (_wcsicmp(m.value, mode_value_name.c_str()) == 0) return m.label;
    }
    return "";
}

// Uninstall where Equalizer APO was installed over IsoAPO. Equalizer APO was not
// asked to go, so it stays and keeps working; IsoAPO leaves every slot; and
// Equalizer APO's record is corrected so that its own uninstall later restores
// the device as it was before either EQ:
//  - a slot original naming IsoAPO becomes what IsoAPO's record says was there;
//  - a slot IsoAPO's install deleted, which Equalizer APO therefore recorded
//    empty, gets IsoAPO's original back;
//  - a child naming IsoAPO becomes the APO IsoAPO hosted (or none).
// A slot IsoAPO still holds gets its original back directly. The disable-
// enhancements flag stays deleted (Equalizer APO's install force-enables
// enhancements too), and a processing-mode list stays where Equalizer APO's
// class is, since its install adds one only when none was there.
// Throws RegistryException.
void uninstall_under_equalizerapo(const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring iso_record = iso_record_key(e);
    const std::wstring eapo_record = eapo_record_key(e);
    const KnownMode known = install_mode_of(e);
    const Extras extras = recorded_extras(e);
    for (const SlotName& slot : kEffectSlots) {
        std::wstring before_isoapo, eapo_original, current;
        const bool have_iso = try_read_string(iso_record, slot.value, &before_isoapo);
        const std::wstring before_both = have_iso && is_other_apo(before_isoapo) ? before_isoapo : APOGUID_NOVALUE;
        if (try_read_string(eapo_record, slot.value, &eapo_original)) {
            if (is_isoapo(eapo_original)) {
                RegistryHelper::writeValue(eapo_record, slot.value, before_both);
            } else if (eapo_original == APOGUID_NOVALUE && is_other_apo(before_both) && known.mode &&
                       mode_deletes(*known.mode, slot.label)) {
                RegistryHelper::writeValue(eapo_record, slot.value, before_both);
            }
        }
        if (try_read_string(fx, slot.value, &current) && is_isoapo(current)) {
            if (is_other_apo(before_both)) RegistryHelper::writeValue(fx, slot.value, before_both);
            else RegistryHelper::deleteValue(fx, slot.value);
        }
    }
    std::wstring hosted, child;
    try_read_string(iso_record, L"PostMixChild", &hosted);
    for (const wchar_t* name : {L"PreMixChild", L"PostMixChild"}) {
        if (try_read_string(eapo_record, name, &child) && is_isoapo(child)) {
            RegistryHelper::writeValue(eapo_record, name,
                                       std::wstring(name) == L"PostMixChild" && is_other_apo(hosted) ? hosted : L"");
        }
    }
    if (RegistryHelper::keyExists(fx)) {
        for (const std::wstring& m : extras.modes_absent) {
            std::wstring clsid;
            bool eapo_there = false;
            for (const SlotName& slot : kEffectSlots) {
                if (std::string(slot.label) == mode_slot_label(m) && try_read_string(fx, slot.value, &clsid))
                    eapo_there = is_equalizerapo(clsid);
            }
            if (!eapo_there && RegistryHelper::valueExists(fx, m)) RegistryHelper::deleteValue(fx, m);
        }
    }
    if (RegistryHelper::keyExists(iso_record)) RegistryHelper::deleteKey(iso_record);
    if (RegistryHelper::keyExists(kIsoChildApos) && RegistryHelper::keyEmpty(kIsoChildApos))
        RegistryHelper::deleteKey(kIsoChildApos);
}

// Throws RegistryException.
Uninstalled uninstall_isoapo(const Endpoint& e) {
    Uninstalled u;
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring record = iso_record_key(e);
    if (equalizerapo_over_isoapo(e)) {
        u.under_equalizerapo = true;
        u.detached = !read_slots(e).isoapo;
        u.fx_properties_missing = !RegistryHelper::keyExists(fx);
        uninstall_under_equalizerapo(e);
        u.equalizerapo_dropped = drop_unregistered_equalizerapo(e);
        return u;
    }
    if (!RegistryHelper::keyExists(fx)) {
        // A driver change removed FxProperties: there is nothing to restore, and
        // upstream's uninstall would fail deleting a key that is not there.
        u.fx_properties_missing = true;
        if (RegistryHelper::keyExists(record)) RegistryHelper::deleteKey(record);
        return u;
    }
    const Extras extras = recorded_extras(e);
    // Detached: a driver update rewrote FxProperties, so the enhancements flag
    // and processing-mode lists there are the driver's, and what the record
    // says about the old ones no longer applies. Upstream's uninstall already
    // leaves the slots alone in that case (load reads their current values).
    u.detached = !read_slots(e).isoapo;
    DeviceAPOInfo info;
    if (info.load(e.guid)) {
        info.uninstall();
    } else {
        uninstall_from_record(e);
    }
    if (!u.detached) restore_extras(e, extras);
    u.equalizerapo_dropped = drop_unregistered_equalizerapo(e);
    return u;
}

// --replace-equalizerapo where Equalizer APO was installed over IsoAPO: undo
// Equalizer APO's install as its own uninstall would, writing each slot's
// recorded original back, with IsoAPO's class in the one slot `mode` puts it
// in. Every slot then holds what it held while IsoAPO was installed alone, so
// IsoAPO hosts what it hosted then (its record is untouched), and IsoAPO's
// record, which saw the device before either EQ, stays right for uninstall.
// Equalizer APO's record goes. Throws RegistryException.
Replacement take_back_from_equalizerapo(const Endpoint& e, DeviceAPOInfo::InstallMode mode) {
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring eapo_record = eapo_record_key(e);
    const std::wstring isoapo = RegistryHelper::getGuidString(ISOAPO_POST_MIX_GUID);
    Replacement r;
    r.took_back = true;
    for (const SlotName& slot : kEffectSlots) {
        std::wstring original, current;
        const bool has = try_read_string(fx, slot.value, &current);
        if (!try_read_string(eapo_record, slot.value, &original)) {
            // No original recorded: leave the slot, unless Equalizer APO is in it.
            if (!has || !is_equalizerapo(current)) continue;
            original = APOGUID_NOVALUE;
        }
        std::wstring target;   // empty: no value
        if (is_isoapo(original)) {
            if (std::string(slot.label) == post_slot_label(mode)) target = isoapo;
        } else if (is_other_apo(original)) {
            target = original;
        }
        if (target.empty()) {
            if (has) {
                RegistryHelper::deleteValue(fx, slot.value);
                ++r.removed;
            }
        } else if (!has || current != target) {
            RegistryHelper::writeValue(fx, slot.value, target);
            if (target != isoapo) ++r.restored;
        }
    }
    RegistryHelper::deleteKey(eapo_record);
    if (RegistryHelper::keyExists(kEapoChildApos) && RegistryHelper::keyEmpty(kEapoChildApos))
        RegistryHelper::deleteKey(kEapoChildApos);
    r.record_taken_over = true;
    return r;
}

// The install mode to use when --mode is not given. Where Equalizer APO is on
// the endpoint, the mode of the slot its post-mix class is in: that slot is
// known to run on this device (it is EFX on this machine's Realtek outputs,
// MFX on the virtual cables). Elsewhere, the mode upstream's own install picks,
// which on Windows 8.1 and later is SFX/EFX unless the driver declares only
// LFX/GFX or the device is a combined Bluetooth one.
DeviceAPOInfo::InstallMode default_mode(DeviceAPOInfo& info, const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    const struct { const char* label; DeviceAPOInfo::InstallMode mode; } kPostSlots[] = {
        {"GFX", DeviceAPOInfo::INSTALL_LFX_GFX},
        {"MFX", DeviceAPOInfo::INSTALL_SFX_MFX},
        {"EFX", DeviceAPOInfo::INSTALL_SFX_EFX},
    };
    for (const auto& p : kPostSlots) {
        for (const SlotName& slot : kEffectSlots) {
            std::wstring clsid;
            if (std::string(slot.label) == p.label && try_read_string(fx, slot.value, &clsid) &&
                std::string(classify(clsid)) == "equalizerapo_post_mix") {
                return p.mode;
            }
        }
    }
    return info.getCurrentInstallState().installMode;
}

// Where upstream's install writes its .reg backup: the working directory.
std::wstring backup_directory() {
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\IsoAPO\\backups";
        CoTaskMemFree(base);
    }
    return dir;
}

bool use_backup_directory(const std::wstring& backups) {
    CreateDirectoryW((backups.substr(0, backups.rfind(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(backups.c_str(), nullptr);
    return SetCurrentDirectoryW(backups.c_str()) != 0;
}

std::string install_checks_json(const Registration& reg, std::vector<std::string>* failed) {
    const auto check = [&](const char* name, bool ok) {
        if (!ok) failed->push_back(name);
        return std::string("{\"check\":") + quote(std::string(name)) + ",\"ok\":" + boolean(ok) + "}";
    };
    return "[" + check("post-mix CLSID registered under AudioEngine\\AudioProcessingObjects", reg.apo_registered) +
           "," + check("CLSID InprocServer32 names a DLL that exists", reg.dll_exists) + "," +
           check("DLL is not under a user profile (audiodg runs as LocalService)",
                 !reg.dll.empty() && !reg.dll_under_user_profile) + "," +
           check("LOCAL SERVICE can read and execute the DLL (audiodg runs as LocalService)",
                 reg.dll_local_service_can_load.value_or(false)) + "]";
}

// Every key and value an install, uninstall or repair can touch, as it was, so
// a command that fails part way can put the endpoint back: FxProperties (which
// upstream's install can create), the slots, mode lists, enhancements flag and
// title in it, and both install records with their parent keys (the Equalizer
// APO record is deleted by a replacement). Not covered: upstream's install takes
// ownership of the endpoint key and grants Administrators full control when
// FxProperties is missing, which no command undoes.
struct Snapshot {
    struct Key {
        std::wstring key;
        bool existed = false;
        bool whole = true;   // made since: removed with what is in it; false for a parent shared by endpoints
        bool operator==(const Key&) const = default;
    };
    struct Value {
        std::wstring key, name;
        bool present = false;
        DWORD type = REG_NONE;
        std::wstring text;
        std::vector<std::wstring> multi;
        unsigned long dword = 0;
        bool operator==(const Value&) const = default;
    };
    std::vector<Key> keys;       // parents before children
    std::vector<Value> values;
    bool operator==(const Snapshot&) const = default;
};

Snapshot snapshot_endpoint(const Endpoint& e) {
    Snapshot s;
    const std::wstring fx = e.key + L"\\FxProperties";
    const auto key = [&](const std::wstring& k, bool whole = true) {
        s.keys.push_back({k, RegistryHelper::keyExists(k), whole});
    };
    const auto value = [&](const std::wstring& k, const wchar_t* name) {
        Snapshot::Value v;
        v.key = k;
        v.name = name;
        try {
            if (RegistryHelper::keyExists(k)) {
                v.type = value_type(k, name, &v.present);
                if (v.present && v.type == REG_SZ) v.text = RegistryHelper::readValue(k, name);
                else if (v.present && v.type == REG_MULTI_SZ) v.multi = RegistryHelper::readMultiValue(k, name);   // through the dry run's hook
                else if (v.present && v.type == REG_DWORD) v.dword = read_dword(k, name);
            }
        } catch (RegistryException&) {
            v.present = false;
        }
        if (!v.present) v.type = REG_NONE;
        s.values.push_back(v);
    };

    key(fx);
    for (const SlotName& slot : kEffectSlots) value(fx, slot.value);
    for (const SlotName& m : kModeSlots) value(fx, m.value);
    value(fx, kDisableEnhancements);
    value(fx, kFxTitle);
    for (const auto& [parent, record] : {std::pair{std::wstring(kIsoChildApos), iso_record_key(e)},
                                         std::pair{std::wstring(kEapoChildApos), eapo_record_key(e)}}) {
        key(parent, false);
        key(record);
        for (const SlotName& slot : kEffectSlots) value(record, slot.value);
        for (const wchar_t* name : kRecordValues) value(record, name);
    }
    return s;
}

std::string error_text(const std::function<void()>& step);

struct RollbackFailure {
    std::string step;
    std::string error;
};

// Best effort: every key and value goes back even if one of them cannot. Returns
// the steps that failed; none means the endpoint is as the snapshot was.
std::vector<RollbackFailure> roll_back(const Snapshot& s) {
    std::vector<RollbackFailure> failures;
    const auto attempt = [&](const std::wstring& what, const std::function<void()>& step) {
        const std::string error = error_text(step);
        if (!error.empty()) failures.push_back({utf8(what), error});
    };
    // Keys removed since come back (parents first), with their values; keys
    // made since go (children first, and a shared parent only once empty).
    for (const Snapshot::Key& k : s.keys) {
        attempt(L"create key " + k.key, [&] {
            if (k.existed && !RegistryHelper::keyExists(k.key)) RegistryHelper::createKey(k.key);
        });
    }
    for (const Snapshot::Value& v : s.values) {
        attempt(L"restore value " + v.key + L"\\" + v.name, [&] {
            if (!RegistryHelper::keyExists(v.key)) return;
            if (!v.present) {
                if (RegistryHelper::valueExists(v.key, v.name)) RegistryHelper::deleteValue(v.key, v.name);
            } else if (v.type == REG_SZ) {
                RegistryHelper::writeValue(v.key, v.name, v.text);
            } else if (v.type == REG_MULTI_SZ) {
                RegistryHelper::writeMultiValue(v.key, v.name, v.multi);
            } else if (v.type == REG_DWORD) {
                RegistryHelper::writeDWORDValue(v.key, v.name, v.dword);
            }
        });
    }
    for (auto k = s.keys.rbegin(); k != s.keys.rend(); ++k) {
        attempt(L"delete key " + k->key, [&] {
            if (!k->existed && RegistryHelper::keyExists(k->key) && (k->whole || RegistryHelper::keyEmpty(k->key))) {
                RegistryHelper::deleteKey(k->key);
            }
        });
    }
    return failures;
}

// Runs `step`; returns what it threw as text, or empty if it returned. Catches
// everything, so no failure leaves a command without its JSON or its rollback.
std::string error_text(const std::function<void()>& step) {
    std::string message;
    try {
        step();
        return "";
    } catch (RegistryException& ex) {
        message = utf8(ex.getMessage());
    } catch (DeviceException& ex) {
        message = utf8(ex.getMessage());
    } catch (ServiceException& ex) {
        message = utf8(ex.getMessage());
    } catch (std::exception& ex) {
        message = ex.what();
    } catch (...) {
        message = "unknown exception";
    }
    return message.empty() ? "registry error" : message;
}

std::string rollback_failures_json(const std::vector<RollbackFailure>& failures) {
    std::string out = "[";
    for (size_t i = 0; i < failures.size(); ++i) {
        out += std::string(i ? "," : "") + "{\"step\":" + quote(failures[i].step) + ",\"error\":" +
               quote(failures[i].error) + "}";
    }
    return out + "]";
}

// ---------------------------------------------------------------------------
// The journal: an interrupted command must not look like a driver update.
//
// Before its first write to an endpoint, a command records the snapshot of
// everything it can touch under HKLM\SOFTWARE\IsoAPO\Pending\{guid}; its last
// write deletes that key. A key still there means the command was cut short
// (killed, power lost), and the next command that changes the endpoint first
// puts it back from the snapshot. Without it, a kill after upstream's install
// wrote the record but before it wrote IsoAPO's slot reads as detached, and
// repair would reinstall over slots the install had already deleted.

const wchar_t* const kPending = APP_REGPATH L"\\Pending";

std::wstring journal_key(const Endpoint& e) { return std::wstring(kPending) + L"\\" + e.guid; }

std::wstring escape_field(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\n') out += L"\\n";
        else out += c;
    }
    return out;
}

std::wstring unescape_field(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            const wchar_t n = s[++i];
            out += n == L't' ? L'\t' : n == L'n' ? L'\n' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

// One line per key ("K", existed, whole, path) and value ("V", key, name,
// present, type, text, dword, then one field per REG_MULTI_SZ item).
std::wstring serialize(const Snapshot& s) {
    std::wstring out;
    for (const Snapshot::Key& k : s.keys) {
        out += L"K\t" + std::to_wstring(k.existed) + L"\t" + std::to_wstring(k.whole) + L"\t" + escape_field(k.key) + L"\n";
    }
    for (const Snapshot::Value& v : s.values) {
        out += L"V\t" + escape_field(v.key) + L"\t" + escape_field(v.name) + L"\t" + std::to_wstring(v.present) + L"\t" +
               std::to_wstring(v.type) + L"\t" + escape_field(v.text) + L"\t" + std::to_wstring(v.dword);
        for (const std::wstring& item : v.multi) out += L"\t" + escape_field(item);
        out += L"\n";
    }
    return out;
}

bool deserialize(const std::wstring& text, Snapshot* out) {
    Snapshot s;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) return false;
        std::vector<std::wstring> f;
        size_t p = start;
        for (;;) {
            const size_t tab = text.find(L'\t', p);
            if (tab == std::wstring::npos || tab > end) {
                f.push_back(unescape_field(text.substr(p, end - p)));
                break;
            }
            f.push_back(unescape_field(text.substr(p, tab - p)));
            p = tab + 1;
        }
        start = end + 1;
        if (f[0] == L"K" && f.size() == 4) {
            s.keys.push_back({f[3], f[1] == L"1", f[2] == L"1"});
        } else if (f[0] == L"V" && f.size() >= 7) {
            Snapshot::Value v;
            v.key = f[1];
            v.name = f[2];
            v.present = f[3] == L"1";
            v.type = std::wcstoul(f[4].c_str(), nullptr, 10);
            v.text = f[5];
            v.dword = std::wcstoul(f[6].c_str(), nullptr, 10);
            v.multi.assign(f.begin() + 7, f.end());
            s.values.push_back(v);
        } else {
            return false;
        }
    }
    *out = s;
    return true;
}

// Throws RegistryException.
void write_journal(const Endpoint& e, const char* command, const Snapshot& before) {
    const std::wstring key = journal_key(e);
    RegistryHelper::createKey(kPending);
    RegistryHelper::createKey(key);
    RegistryHelper::writeValue(key, L"Snapshot", serialize(before));
    // Last: a journal without it was cut off before the command wrote anything else.
    RegistryHelper::writeValue(key, L"Command", wide(command));
}

// Throws RegistryException.
void clear_journal(const Endpoint& e) {
    const std::wstring key = journal_key(e);
    if (RegistryHelper::keyExists(key)) RegistryHelper::deleteKey(key);
    if (RegistryHelper::keyExists(kPending) && RegistryHelper::keyEmpty(kPending)) RegistryHelper::deleteKey(kPending);
}

// The command a journal left for this endpoint says was interrupted, or empty.
std::string interrupted_command(const Endpoint& e) {
    std::wstring command;
    if (!RegistryHelper::keyExists(journal_key(e))) return "";
    return try_read_string(journal_key(e), L"Command", &command) ? utf8(command) : "(journal incomplete)";
}

// Puts back an endpoint a command was interrupted on, from its journal. Returns
// the command undone, or empty when there was none. Throws RegistryException if
// the journal cannot be read or a step of the undo fails; the journal then
// stays, for the next run to try again.
std::string recover_interrupted(const Endpoint& e) {
    const std::wstring key = journal_key(e);
    if (!RegistryHelper::keyExists(key)) {
        // A run cut off between creating the parent and the journal left it empty.
        if (RegistryHelper::keyExists(kPending) && RegistryHelper::keyEmpty(kPending)) RegistryHelper::deleteKey(kPending);
        return "";
    }
    std::wstring command, text;
    if (!try_read_string(key, L"Command", &command)) {
        clear_journal(e);   // cut off while being written: nothing else was
        return "";
    }
    Snapshot before;
    if (!try_read_string(key, L"Snapshot", &text) || !deserialize(text, &before))
        throw RegistryException(L"the journal of an interrupted " + command + L" at " + key + L" cannot be read");
    const std::vector<RollbackFailure> failures = roll_back(before);
    if (!failures.empty()) {
        throw RegistryException(L"undoing the interrupted " + command + L" failed at " + wide(failures[0].step) +
                                L": " + wide(failures[0].error));
    }
    clear_journal(e);
    return utf8(command);
}

// `command` between a journal written from `before` and the journal's removal.
void journaled(const Endpoint& e, const char* name, const Snapshot& before, const std::function<void()>& command) {
    write_journal(e, name, before);
    command();
    clear_journal(e);
}

// Runs `command`, journaled, with its writes logged; if it throws, rolls the
// endpoint back to `before` and reports both sets of operations and any rollback
// step that failed. Returns the error, or empty.
std::string with_rollback(const Endpoint& e, const Snapshot& before, OperationLog* log, std::string* rollback_json,
                          const char* name, const std::function<void()>& command) {
    std::string error;
    {
        ScopedLog scope(log);
        error = error_text([&] { journaled(e, name, before, command); });
    }
    if (error.empty()) return "";
    OperationLog undo;
    std::vector<RollbackFailure> failures;
    {
        ScopedLog scope(&undo);
        failures = roll_back(before);
        // The journal stays when the endpoint could not be put back, so the next
        // run tries again.
        if (failures.empty()) {
            const std::string cleared = error_text([&] { clear_journal(e); });
            if (!cleared.empty()) failures.push_back({"clear journal " + utf8(journal_key(e)), cleared});
        }
    }
    *rollback_json = std::string(",\"rolled_back\":") + boolean(failures.empty()) +
                     ",\"rollback_failed\":" + rollback_failures_json(failures) +
                     ",\"rollback_operations\":" + operations_json(undo.operations());
    return error;
}

// ---------------------------------------------------------------------------
// What each command does in each state an endpoint can be in.


IsoState isoapo_state(const Endpoint& e) {
    const Slots slots = read_slots(e);
    const bool record = read_record(kIsoChildApos, e.guid).exists;
    if (RegistryHelper::keyExists(journal_key(e))) return {"interrupted", {"repair"}};
    if (slots.isoapo) {
        IsoState s = !record              ? IsoState{"unrecorded", {}}
                     : slots.equalizerapo ? IsoState{"alongside_equalizerapo", {"install --replace-equalizerapo", "uninstall"}}
                     : isoapo_replaced_equalizerapo(e) ? IsoState{"installed", {"install --replace-equalizerapo"}}
                                                       : IsoState{"installed", {}};
        // With IsoAPO in a slot, install does not touch the flag again and
        // repair skips the endpoint: enhancements turned off since have this
        // command only.
        if (enhancements_disabled(e)) s.remedies.push_back("enable-enhancements");
        return s;
    }
    if (!record) return {"not_installed", {}};
    if (equalizerapo_over_isoapo(e))
        return {"replaced_by_equalizerapo", {"install --replace-equalizerapo", "uninstall"}};
    if (slots.equalizerapo) return {"detached", {"install --replace-equalizerapo", "uninstall"}};
    if (install_mode_of(e).mode) return {"detached", {"repair", "uninstall"}};
    return {"detached", {"repair --mode", "uninstall"}};
}

enum class InstallAction { Install, AlreadyInstalled, ReplaceOnly, TakeBack, ReattachReplacing, Refuse };
struct InstallPlan {
    InstallAction action = InstallAction::Refuse;
    std::string refusal;
};

// What install does on this endpoint, from the state IsoAPO and Equalizer APO
// are in. Every state has a way forward: none refuses one flag while the
// command it names refuses back.
InstallPlan plan_install(const Endpoint& e, bool replace_eapo) {
    const Slots slots = read_slots(e);
    const bool record = read_record(kIsoChildApos, e.guid).exists;
    const bool over = equalizerapo_over_isoapo(e);
    if (slots.isoapo && record) {
        if (replace_eapo && over) return {InstallAction::TakeBack, ""};
        // Also when an earlier replacement left Equalizer APO's record behind.
        if (replace_eapo && (slots.equalizerapo || isoapo_replaced_equalizerapo(e))) return {InstallAction::ReplaceOnly, ""};
        return {InstallAction::AlreadyInstalled, ""};
    }
    if (slots.isoapo)
        return {InstallAction::Refuse, "IsoAPO is in an effect slot with no install record (installed by the retired install.ps1); restore the endpoint's FxProperties from that script's .reg backup first"};
    if (record && over) {
        if (replace_eapo) return {InstallAction::TakeBack, ""};
        return {InstallAction::Refuse, "Equalizer APO was installed over IsoAPO on this endpoint; pass --replace-equalizerapo to go back to IsoAPO, or uninstall to keep Equalizer APO"};
    }
    if (record && slots.equalizerapo) {
        if (replace_eapo) return {InstallAction::ReattachReplacing, ""};
        return {InstallAction::Refuse, "IsoAPO is detached and Equalizer APO is on this endpoint; pass --replace-equalizerapo to replace it, or uninstall to keep Equalizer APO"};
    }
    if (record) return {InstallAction::Refuse, "an install record exists but IsoAPO is in no slot (detached); run repair"};
    if (slots.equalizerapo && !replace_eapo)
        return {InstallAction::Refuse, "Equalizer APO is installed on this endpoint; a device may be on one backend only (pass --replace-equalizerapo to replace it)"};
    return {InstallAction::Install, ""};
}

struct RepairPlan {
    bool skip = true;
    std::optional<DeviceAPOInfo::InstallMode> mode;   // set when repair reattaches
    std::string refusal;
};

// What repair does on one endpoint: nothing unless IsoAPO is detached; refuses
// where Equalizer APO is on the endpoint (the one-backend rule), and where the
// slot IsoAPO had cannot be known and no --mode is given.
RepairPlan plan_repair(const Endpoint& e, std::optional<DeviceAPOInfo::InstallMode> mode) {
    RepairPlan p;
    if (!read_record(kIsoChildApos, e.guid).exists || read_slots(e).isoapo) return p;
    p.skip = false;
    if (equalizerapo_over_isoapo(e)) {
        p.refusal = "Equalizer APO was installed over IsoAPO on this endpoint; run install --replace-equalizerapo to go back to IsoAPO, or uninstall to keep Equalizer APO";
    } else if (read_slots(e).equalizerapo) {
        // Equalizer APO came back (a driver reinstall, or its Device Selector
        // after a driver update), and putting IsoAPO next to it would run two EQs.
        p.refusal = "Equalizer APO is on this endpoint again; run install --replace-equalizerapo to replace it, or uninstall to keep Equalizer APO";
    } else if (mode) {
        p.mode = mode;
    } else if (const KnownMode known = install_mode_of(e); known.mode) {
        p.mode = known.mode;
    } else {
        p.refusal = "the install record predates Isotone.InstallMode, so the slot IsoAPO had cannot be told; pass --mode mfx, efx or gfx";
    }
    return p;
}

std::string replacement_json(const Replacement& r) {
    return std::string("\"equalizerapo_slots_removed\":") + std::to_string(r.removed) +
           ",\"equalizerapo_slots_restored_to_original\":" + std::to_string(r.restored) +
           ",\"child_from_equalizerapo\":" + (r.child.empty() ? "null" : quote(r.child)) +
           ",\"equalizerapo_record_taken_over\":" + boolean(r.record_taken_over) +
           ",\"took_back_from_equalizerapo\":" + boolean(r.took_back);
}

// A dry run routes every read and write of the command through `dry`; a real run
// logs its writes to `log`. The command's own code is the same either way.
struct CommandScope {
    DryRunRegistry dry;
    OperationLog log;
    bool dry_run;
    std::optional<ScopedDryRun> dry_scope;
    std::optional<ScopedLog> log_scope;
    explicit CommandScope(bool dry) : dry_run(dry) {
        if (dry_run) dry_scope.emplace(&this->dry);
        else log_scope.emplace(&log);
    }
    const std::vector<RegistryOperation>& operations() const { return dry_run ? dry.operations() : log.operations(); }
    std::string operations_field() const { return ",\"operations\":" + operations_json(operations()); }
    // Runs `command` journaled: in a dry run as it is, for real rolled back on
    // failure. Returns the error, or empty.
    std::string run(const Endpoint& e, const char* name, std::string* rollback_json,
                    const std::function<void()>& command) {
        const Snapshot before = snapshot_endpoint(e);
        if (dry_run) return error_text([&] { journaled(e, name, before, command); });
        return with_rollback(e, before, &log, rollback_json, name, command);
    }
};

// Whether any operation changed the endpoint's own keys (its FxProperties, or
// the ownership and ACL of its key), as opposed to only the install records.
bool endpoint_changed(const Endpoint& e, const std::vector<RegistryOperation>& ops) {
    std::wstring prefix = e.key;
    for (wchar_t& c : prefix) c = static_cast<wchar_t>(towlower(c));
    return std::any_of(ops.begin(), ops.end(), [&](const RegistryOperation& o) {
        std::wstring key = o.key;
        for (wchar_t& c : key) c = static_cast<wchar_t>(towlower(c));
        return key.rfind(prefix, 0) == 0;
    });
}

// Detached: forget the stale record, then install again against the APOs the
// driver now declares, in `mode`. Throws RegistryException.
Replacement reattach(const Endpoint& e, DeviceAPOInfo::InstallMode mode, bool replace_eapo) {
    uninstall_isoapo(e);
    DeviceAPOInfo info;
    if (!info.load(e.guid)) throw RegistryException(L"endpoint is not present");
    return install_isoapo(info, e, mode, replace_eapo);
}

int cmd_install(const Endpoint& e, std::optional<DeviceAPOInfo::InstallMode> mode, bool replace_eapo, bool dry_run) {
    if (e.input) return fail("install", "capture endpoints are not supported: IsoAPO is an output EQ");

    CommandScope scope(dry_run);
    std::string undone;
    if (const std::string error = error_text([&] { undone = recover_interrupted(e); }); !error.empty())
        return fail("install", error, scope.operations_field());

    DeviceAPOInfo info;
    if (const std::string error = error_text([&] {
            if (!info.load(e.guid)) throw RegistryException(L"endpoint is not present");
        });
        !error.empty()) {
        return fail("install", error, scope.operations_field());
    }

    const IsoState state = isoapo_state(e);
    const InstallPlan plan = plan_install(e, replace_eapo);
    if (plan.action == InstallAction::Refuse)
        return fail("install", plan.refusal, ",\"state\":" + quote(std::string(state.name)) + scope.operations_field());

    // IsoAPO stays in the slot it has; --mode cannot move it.
    const bool keeps_slot = plan.action == InstallAction::AlreadyInstalled ||
                            plan.action == InstallAction::ReplaceOnly || plan.action == InstallAction::TakeBack;
    const KnownMode known = install_mode_of(e);
    std::optional<DeviceAPOInfo::InstallMode> chosen;
    if (keeps_slot) {
        if (known.mode && mode && *mode != *known.mode)
            return fail("install", std::string("IsoAPO is in ") + post_slot_label(*known.mode) +
                                       " on this endpoint; --mode cannot move it (uninstall, then install)");
        chosen = known.mode ? known.mode : mode;
        if (plan.action == InstallAction::TakeBack && !chosen)
            return fail("install", "the slot IsoAPO had on this endpoint cannot be told from its records; pass --mode mfx, efx or gfx");
    } else {
        chosen = mode ? *mode : default_mode(info, e);
    }

    const Slots slots = read_slots(e);
    const std::wstring backups = backup_directory();
    const bool writes_slot = plan.action == InstallAction::Install || plan.action == InstallAction::TakeBack ||
                             plan.action == InstallAction::ReattachReplacing;
    const bool runs_upstream_install = plan.action == InstallAction::Install || plan.action == InstallAction::ReattachReplacing;
    std::vector<std::string> failed;
    std::string checks = "[]";
    if (writes_slot) {
        checks = install_checks_json(read_registration(), &failed);
        if (!dry_run && !failed.empty()) return fail("install", "registration checks failed", ",\"checks\":" + checks);
    }
    if (runs_upstream_install && replace_eapo) {
        const std::wstring lost = hosted_apo_in_deleted_slot(e, *chosen);
        if (!lost.empty())
            return fail("install", "this mode deletes the slot where Equalizer APO hosts " + utf8(lost) +
                                       ", which would be lost; install without --mode, in the slots Equalizer APO uses");
    }

    std::vector<std::string> warnings;
    if (plan.action == InstallAction::Install) {
        select_post_mix_only(&info, *chosen);
        const std::wstring original = info.getSelectedInstallState().useOriginalAPOPostMix ? info.getOriginalAPOPostMix() : L"";
        if (!original.empty())
            warnings.push_back("IsoAPO hosts " + utf8(original) + ", the APO this install replaces, and runs it before its own processing");
    }
    warnings.insert(warnings.end(), slots.warnings.begin(), slots.warnings.end());

    Replacement replaced;
    if (plan.action != InstallAction::AlreadyInstalled) {
        if (!dry_run && runs_upstream_install && !use_backup_directory(backups))
            return fail("install", "cannot use backup directory " + utf8(backups));
        // Leave the endpoint as it was on failure, not half installed with a
        // record that makes every later command refuse.
        std::string rollback;
        const std::string error = scope.run(e, "install", &rollback, [&] {
            switch (plan.action) {
                case InstallAction::ReplaceOnly: replaced = remove_equalizerapo(e); break;
                case InstallAction::TakeBack: replaced = take_back_from_equalizerapo(e, *chosen); break;
                case InstallAction::ReattachReplacing: replaced = reattach(e, *chosen, true); break;
                case InstallAction::Install: replaced = install_isoapo(info, e, *chosen, replace_eapo); break;
                default: break;
            }
        });
        if (!error.empty()) return fail("install", error, rollback + scope.operations_field());
    }

    std::string verified = "null", eapo_left = "null";
    if (!dry_run) {
        const Slots after = read_slots(e);
        verified = boolean(after.isoapo);
        eapo_left = boolean(after.equalizerapo);
    }
    std::wstring child;
    try_read_string(iso_record_key(e), L"PostMixChild", &child);

    // A dry run whose registration checks fail shows what would be written, but
    // is not ok: the real install would refuse.
    const bool ok = failed.empty();
    std::printf("{\"command\":\"install\",\"ok\":%s,\"dry_run\":%s,%s\"already_installed\":%s,\"state_before\":%s,"
                "\"undid_interrupted\":%s,\"mode\":%s,\"child\":%s,"
                "\"backup_directory\":%s,\"checks\":%s,%s,\"fx_properties_changed\":%s,\"operations\":%s,"
                "\"verified_in_slot\":%s,\"equalizerapo_still_in_slots\":%s,\"warnings\":%s}\n",
                boolean(ok), boolean(dry_run), ok ? "" : "\"reason\":\"registration checks failed; a real install would refuse\",",
                boolean(plan.action == InstallAction::AlreadyInstalled || plan.action == InstallAction::ReplaceOnly),
                quote(std::string(state.name)).c_str(), undone.empty() ? "null" : quote(undone).c_str(),
                chosen ? quote(mode_name(*chosen)).c_str() : "null",
                child.empty() ? "null" : quote(child).c_str(), quote(backups).c_str(), checks.c_str(),
                replacement_json(replaced).c_str(), boolean(endpoint_changed(e, scope.operations())),
                operations_json(scope.operations()).c_str(),
                verified.c_str(), eapo_left.c_str(), string_array(warnings).c_str());
    return ok ? 0 : kExitFailed;
}

int cmd_uninstall(const Endpoint& e, bool dry_run) {
    CommandScope scope(dry_run);
    std::string undone;
    if (const std::string error = error_text([&] { undone = recover_interrupted(e); }); !error.empty())
        return fail("uninstall", error, scope.operations_field());

    const Slots slots = read_slots(e);
    const Record record = read_record(kIsoChildApos, e.guid);
    const IsoState state = isoapo_state(e);
    if (!record.exists) {
        if (slots.isoapo)
            return fail("uninstall", "IsoAPO is in an effect slot but has no install record (the retired install.ps1 installed it), so the original values are unknown here; restore FxProperties from that script's .reg backup");
        std::printf("{\"command\":\"uninstall\",\"ok\":true,\"dry_run\":%s,\"not_installed\":true,\"undid_interrupted\":%s,"
                    "\"fx_properties_changed\":%s,\"operations\":%s}\n",
                    boolean(dry_run), undone.empty() ? "null" : quote(undone).c_str(),
                    boolean(endpoint_changed(e, scope.operations())), operations_json(scope.operations()).c_str());
        return 0;
    }

    Uninstalled u;
    std::string rollback;
    const std::string error = scope.run(e, "uninstall", &rollback, [&] { u = uninstall_isoapo(e); });
    if (!error.empty()) return fail("uninstall", error, rollback + scope.operations_field());
    std::printf("{\"command\":\"uninstall\",\"ok\":true,\"dry_run\":%s,\"state_before\":%s,\"undid_interrupted\":%s,"
                "\"record\":%s,\"fx_properties_missing\":%s,\"detached\":%s,\"equalizerapo_kept\":%s,"
                "\"unregistered_equalizerapo_slots\":%d,\"fx_properties_changed\":%s,\"operations\":%s}\n",
                boolean(dry_run), quote(std::string(state.name)).c_str(), undone.empty() ? "null" : quote(undone).c_str(),
                record.json.c_str(), boolean(u.fx_properties_missing), boolean(u.detached),
                boolean(u.under_equalizerapo), u.equalizerapo_dropped,
                boolean(endpoint_changed(e, scope.operations())), operations_json(scope.operations()).c_str());
    return 0;
}

// Every render endpoint, or only `only` when given (the UI repairs the output it
// shows, so another endpoint's refusal or recorded mode does not decide it).
int cmd_repair(const std::optional<Endpoint>& only, std::optional<DeviceAPOInfo::InstallMode> mode, bool dry_run) {
    if (only && only->input) return fail("repair", "capture endpoints are not supported: IsoAPO is an output EQ");
    std::vector<std::wstring> guids;
    if (only) {
        guids.push_back(only->guid);
    } else {
        try {
            guids = RegistryHelper::enumSubKeys(std::wstring(kMMDevices) + L"\\Render");
        } catch (RegistryException& ex) {
            return fail("repair", utf8(ex.getMessage()));
        }
    }
    // Upstream's install writes its .reg backup to the working directory.
    const std::wstring backups = backup_directory();
    if (!dry_run && !use_backup_directory(backups)) return fail("repair", "cannot use backup directory " + utf8(backups));

    std::string devices = "[";
    bool first = true, all_ok = true, changed = false;
    for (const std::wstring& g : guids) {
        Endpoint e{g, false, std::wstring(kMMDevices) + L"\\Render\\" + g};
        CommandScope scope(dry_run);
        std::string undone, rollback;
        std::string error = error_text([&] { undone = recover_interrupted(e); });
        RepairPlan plan;
        if (error.empty()) plan = plan_repair(e, mode);
        if (error.empty() && plan.skip && undone.empty()) continue;
        if (error.empty() && !plan.skip) {
            if (!plan.refusal.empty()) {
                error = plan.refusal;
            } else {
                // A repair whose install fails after its uninstall succeeded would
                // leave no IsoAPO and no record, and later repairs would skip the
                // endpoint: put the detached install back as it was instead.
                error = scope.run(e, "repair", &rollback, [&] { reattach(e, *plan.mode, false); });
            }
        }
        all_ok &= error.empty();
        changed |= endpoint_changed(e, scope.operations());
        devices += std::string(first ? "" : ",") + "{\"guid\":" + quote(g) + ",\"ok\":" + boolean(error.empty()) +
                   ",\"mode\":" + (plan.mode ? quote(mode_name(*plan.mode)) : "null") +
                   ",\"undid_interrupted\":" + (undone.empty() ? "null" : quote(undone)) +
                   (error.empty() ? "" : ",\"error\":" + quote(error)) + rollback + scope.operations_field() + "}";
        first = false;
    }
    std::printf("{\"command\":\"repair\",\"ok\":%s,\"dry_run\":%s,\"mode\":%s,\"fx_properties_changed\":%s,\"repaired\":%s]}\n",
                boolean(all_ok), boolean(dry_run), mode ? quote(mode_name(*mode)).c_str() : "null", boolean(changed),
                devices.c_str());
    return all_ok ? 0 : kExitFailed;
}

// One value deleted or none, so no journal: a run cut short has either written
// it or not. An interrupted command's journal is still undone first, as the
// other registry-changing commands do, or undoing it later would put the flag
// back from its snapshot.
int cmd_enable_enhancements(const Endpoint& e, bool dry_run) {
    if (e.input) return fail("enable-enhancements", "capture endpoints are not supported: IsoAPO is an output EQ");
    CommandScope scope(dry_run);
    std::string undone;
    if (const std::string error = error_text([&] { undone = recover_interrupted(e); }); !error.empty())
        return fail("enable-enhancements", error, scope.operations_field());
    const bool was_disabled = enhancements_disabled(e);
    bool changed = false;
    if (const std::string error = error_text([&] { changed = enable_enhancements(e); }); !error.empty())
        return fail("enable-enhancements", error, scope.operations_field());
    std::printf("{\"command\":\"enable-enhancements\",\"ok\":true,\"dry_run\":%s,\"guid\":%s,\"undid_interrupted\":%s,"
                "\"enhancements_were_disabled\":%s,\"changed\":%s,\"operations\":%s}\n",
                boolean(dry_run), quote(e.guid).c_str(), undone.empty() ? "null" : quote(undone).c_str(),
                boolean(was_disabled), boolean(changed), operations_json(scope.operations()).c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// restart-audio: what Equalizer APO's Device Selector does after it changes
// devices (DeviceSelector/DeviceTestThread.cpp), with upstream's ServiceHelper:
// the running services that depend on AudioSrv stop first, AudioSrv starts
// again before them, and 30 seconds is the limit for the whole restart.

const wchar_t* const kAudioService = L"AudioSrv";   // upstream's spelling

const char* service_state_name(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED: return "stopped";
        case SERVICE_START_PENDING: return "start_pending";
        case SERVICE_STOP_PENDING: return "stop_pending";
        case SERVICE_RUNNING: return "running";
        case SERVICE_CONTINUE_PENDING: return "continue_pending";
        case SERVICE_PAUSE_PENDING: return "pause_pending";
        case SERVICE_PAUSED: return "paused";
    }
    return "unknown";
}

struct ServiceView {
    std::string error;                 // empty when read
    DWORD state = 0;
    std::vector<std::string> restarts;   // in the order upstream stops them: active dependents, then the service
};

// With query rights only, so a dry run needs no elevation: the state, and the
// services upstream's restartService would restart (the active dependents only
// when the service is running).
ServiceView view_service(const wchar_t* name) {
    ServiceView v;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) {
        v.error = "OpenSCManager failed (error " + std::to_string(GetLastError()) + ")";
        return v;
    }
    SC_HANDLE service = OpenServiceW(scm, name, SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0, count = 0;
    if (service == nullptr) {
        v.error = "OpenService " + utf8(name) + " failed (error " + std::to_string(GetLastError()) + ")";
    } else if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed)) {
        v.error = "QueryServiceStatusEx failed (error " + std::to_string(GetLastError()) + ")";
    } else {
        v.state = status.dwCurrentState;
        // A first call that succeeds found no dependents (upstream's getActiveDependentServices).
        if (v.state == SERVICE_RUNNING && !EnumDependentServicesW(service, SERVICE_ACTIVE, nullptr, 0, &needed, &count)) {
            const DWORD first_error = GetLastError();
            std::vector<BYTE> buffer(needed);
            const LPENUM_SERVICE_STATUSW list = reinterpret_cast<LPENUM_SERVICE_STATUSW>(buffer.data());
            if (first_error != ERROR_MORE_DATA || !EnumDependentServicesW(service, SERVICE_ACTIVE, list, needed, &needed, &count)) {
                v.error = "EnumDependentServices failed (error " +
                          std::to_string(first_error != ERROR_MORE_DATA ? first_error : GetLastError()) + ")";
                count = 0;
            }
            for (DWORD i = 0; i < count; ++i) v.restarts.push_back(utf8(list[i].lpServiceName));
        }
        v.restarts.push_back(utf8(name));
    }
    if (service != nullptr) CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return v;
}

int cmd_restart_audio(bool dry_run) {
    const ServiceView before = view_service(kAudioService);
    if (!before.error.empty()) return fail("restart-audio", before.error, std::string(",\"dry_run\":") + boolean(dry_run));
    const std::string facts = std::string(",\"dry_run\":") + boolean(dry_run) + ",\"service\":" + quote(std::wstring(kAudioService)) +
                              ",\"state_before\":\"" + service_state_name(before.state) + "\",\"restarts\":" +
                              string_array(before.restarts);
    if (dry_run) {
        std::printf("{\"command\":\"restart-audio\",\"ok\":true%s,\"state_after\":null,\"seconds\":null}\n", facts.c_str());
        return 0;
    }
    PrecisionTimer timer;
    timer.start();
    const std::string error = error_text([] { ServiceHelper::restartService(kAudioService); });
    const double seconds = timer.stop();
    const ServiceView after = view_service(kAudioService);
    char elapsed[32];
    std::snprintf(elapsed, sizeof(elapsed), "%.3f", seconds);
    const std::string outcome = facts + ",\"state_after\":" +
                                (after.error.empty() ? quote(std::string(service_state_name(after.state))) : "null") +
                                ",\"seconds\":" + elapsed;
    if (!error.empty()) return fail("restart-audio", error, outcome);
    std::printf("{\"command\":\"restart-audio\",\"ok\":true%s}\n", outcome.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// The machine-wide half of an install: what the installer does once for the
// machine rather than once per endpoint (plan section 8, the Setup/ row).
//
//   the COM class      HKLM\SOFTWARE\Classes\CLSID\{IsoAPO} and
//                      AudioEngine\AudioProcessingObjects. Written by the DLL's
//                      own DllRegisterServer rather than reimplemented here:
//                      that is where RegisterAPO and the APO_REG_PROPERTIES
//                      live, and two copies of it would drift.
//   protected audio    DisableProtectedAudioDG=1, without which audiodg loads
//                      no unsigned APO at all.
//   the data directory %ProgramData%\IsoAPO\{devices,backups}, with an ACL
//                      audiodg can read and any account's app can write.
//
// A dry run writes nothing anywhere. It cannot call DllRegisterServer, which
// goes at the registry directly and has no dry run to install, so it reports
// every precondition it can check and says the call would follow.

const wchar_t* const kAudioSettingsKey =
    L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio";
const wchar_t* const kProtectedAudioValue = L"DisableProtectedAudioDG";
const wchar_t* const kIsotoneKey = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\IsoAPO";
// 1 when this install set DisableProtectedAudioDG, so machine-uninstall knows
// whether the value is ours to remove. Equalizer APO sets the same value, and
// on a machine that had it first this reads 0 and the uninstall leaves it.
const wchar_t* const kProtectedAudioOursValue = L"ProtectedAudioDGSetByIsotone";

std::wstring program_data_dir() {
    wchar_t* base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    return dir.empty() ? dir : dir + L"\\IsoAPO";
}

// "NT SERVICE\<service>" as an SDDL SID string, empty when it cannot be
// resolved. Derived by the LSA from the service name, so it is the same on
// every machine; resolved here rather than written out, because a wrong literal
// would be an ACE that silently grants nothing.
std::wstring service_sid(const wchar_t* service) {
    const std::wstring account = std::wstring(L"NT SERVICE\\") + service;
    DWORD sid_size = 0, domain_size = 0;
    SID_NAME_USE use{};
    LookupAccountNameW(nullptr, account.c_str(), nullptr, &sid_size, nullptr, &domain_size, &use);
    if (sid_size == 0) return {};
    std::vector<BYTE> sid(sid_size);
    std::vector<wchar_t> domain(domain_size == 0 ? 1 : domain_size);
    if (!LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sid_size, domain.data(), &domain_size, &use)) {
        return {};
    }
    wchar_t* text = nullptr;
    if (!ConvertSidToStringSidW(sid.data(), &text)) return {};
    std::wstring out = text;
    LocalFree(text);
    return out;
}

// SYSTEM and Administrators full control; LOCAL SERVICE, which audiodg runs as,
// and the Windows Audio service read and execute; Authenticated Users modify.
//
// Modify rather than write, and Authenticated Users rather than the creator,
// because the app runs unelevated as whoever is logged in and replaces the state
// file with MoveFileEx, which needs DELETE on the file that is already there.
// ProgramData's inherited ACL gives Users read and create only, so until this is
// set "a file one Windows account writes cannot be replaced by another"
// (docs/ui-spec.md). Protected (D:P), so those inherited ACEs do not apply as
// well, and inherited by what is created inside (OICI).
std::wstring data_dir_sddl() {
    std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;LS)(A;OICI;0x1301bf;;;AU)";
    const std::wstring audiosrv = service_sid(L"Audiosrv");
    if (!audiosrv.empty()) sddl += L"(A;OICI;0x1200a9;;;" + audiosrv + L")";
    return sddl;
}

// Creates `dir` if it is not there and puts `sddl`'s DACL on it, replacing
// whatever it had. Empty on success, else what went wrong.
std::string apply_data_dir_acl(const std::wstring& dir, const std::wstring& sddl) {
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return "cannot create " + utf8(dir) + " (error " + std::to_string(GetLastError()) + ")";
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return "cannot build the security descriptor (error " + std::to_string(GetLastError()) + ")";
    }
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    const bool got = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present;
    const DWORD set =
        got ? SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, dacl, nullptr)
            : ERROR_INVALID_PARAMETER;
    LocalFree(sd);
    if (set != ERROR_SUCCESS) {
        return "cannot set the ACL on " + utf8(dir) + " (error " + std::to_string(set) + ")";
    }
    return {};
}

// The DACL a directory actually carries, so a caller can compare it with what
// was asked for. Empty when it cannot be read.
std::wstring read_dacl(const std::wstring& dir) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              nullptr, nullptr, &sd) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t* text = nullptr;
    ULONG length = 0;
    const bool ok = ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1,
                                                                         DACL_SECURITY_INFORMATION, &text, &length);
    std::wstring out;
    if (ok && text != nullptr) out = text;
    if (text != nullptr) LocalFree(text);
    LocalFree(sd);
    return out;
}

// Calls the named entry point in the DLL at `path`. Empty on success.
std::string call_dll_entry(const std::wstring& path, const char* entry) {
    // LOAD_WITH_ALTERED_SEARCH_PATH so the DLL's own directory is searched for
    // what it imports, which is how audiodg loads it through CoLoadLibrary.
    const HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr) {
        return std::string("cannot load the DLL (error ") + std::to_string(GetLastError()) + ")";
    }
    using EntryFn = HRESULT(__stdcall*)();
    const auto fn = reinterpret_cast<EntryFn>(GetProcAddress(module, entry));
    if (fn == nullptr) {
        FreeLibrary(module);
        return std::string("the DLL does not export ") + entry;
    }
    const HRESULT hr = fn();
    FreeLibrary(module);
    if (FAILED(hr)) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
        return std::string(entry) + " returned " + buffer;
    }
    return {};
}

std::string check_json(const char* name, bool ok, std::vector<std::string>* failed) {
    if (!ok) failed->push_back(name);
    return std::string("{\"check\":") + quote(std::string(name)) + ",\"ok\":" + boolean(ok) + "}";
}

int cmd_machine_install(const std::wstring& dll, bool dry_run) {
    // Everything this would do, worked out before anything is written, so a run
    // that stops at a bad DLL still reports the whole plan rather than only the
    // step it failed at. Recorded as ours only when this run is what sets
    // DisableProtectedAudioDG, so machine-uninstall leaves alone a value
    // Equalizer APO put there first.
    bool protected_audio_present = false;
    const DWORD type = value_type(kAudioSettingsKey, kProtectedAudioValue, &protected_audio_present);
    const bool already =
        protected_audio_present && type == REG_DWORD && read_dword(kAudioSettingsKey, kProtectedAudioValue) == 1;
    const std::wstring data_dir = program_data_dir();
    const std::wstring sddl = data_dir_sddl();
    const std::string plan = std::string("\"command\":\"machine-install\",\"dry_run\":") + boolean(dry_run) +
                             ",\"dll\":" + quote(dll) + ",\"protected_audiodg_already_disabled\":" + boolean(already) +
                             ",\"data_dir\":" + (data_dir.empty() ? "null" : quote(data_dir)) + ",\"acl\":" +
                             quote(sddl);

    // The DLL: a path audiodg cannot load would register cleanly and then fail
    // every endpoint with E_ACCESSDENIED.
    const bool absolute = dll.size() >= 3 && dll[1] == L':' && dll[2] == L'\\';
    std::vector<std::string> failed;
    std::string checks = "[";
    checks += check_json("the path is a local absolute path", absolute, &failed);
    checks += "," + check_json("the DLL exists",
                               absolute && GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES, &failed);
    checks += "," + check_json("the DLL is not under a user profile (audiodg runs as LocalService)",
                               absolute && !under_user_profile(dll), &failed);
    const std::optional<bool> loadable = absolute ? local_service_can_load(dll) : std::optional<bool>(false);
    checks += "," + check_json("LOCAL SERVICE can read and execute the DLL", loadable.value_or(false), &failed);
    checks += "]";
    const std::string facts = plan + ",\"checks\":" + checks;
    if (!failed.empty()) {
        return fail("machine-install", "the DLL cannot be registered: " + failed.front(), "," + facts);
    }

    if (dry_run) {
        std::printf("{%s,\"ok\":true,\"would\":[\"call DllRegisterServer in the DLL\",%s%s]}\n", facts.c_str(),
                    already ? "" : "\"set DisableProtectedAudioDG=1\",",
                    "\"create the data directory and set its ACL\"");
        return 0;
    }

    if (data_dir.empty()) return fail("machine-install", "cannot find ProgramData", "," + facts);

    const std::string registered = call_dll_entry(dll, "DllRegisterServer");
    if (!registered.empty()) return fail("machine-install", registered, "," + facts);

    if (!already) {
        const std::string wrote = error_text([] {
            RegistryHelper::writeDWORDValue(kAudioSettingsKey, kProtectedAudioValue, 1);
        });
        if (!wrote.empty()) return fail("machine-install", "DisableProtectedAudioDG: " + wrote, "," + facts);
    }
    const std::string recorded = error_text([&] {
        RegistryHelper::writeDWORDValue(kIsotoneKey, kProtectedAudioOursValue, already ? 0 : 1);
    });
    if (!recorded.empty()) return fail("machine-install", "the install record: " + recorded, "," + facts);

    for (const std::wstring& dir : {data_dir, data_dir + L"\\devices", data_dir + L"\\backups"}) {
        const std::string error = apply_data_dir_acl(dir, sddl);
        if (!error.empty()) return fail("machine-install", error, "," + facts);
    }

    // What the machine actually holds now, not what was asked for.
    const Registration after = read_registration();
    std::vector<std::string> after_failed;
    std::string done = "[";
    done += check_json("the post-mix CLSID is registered", after.apo_registered, &after_failed);
    done += "," + check_json("InprocServer32 names the DLL that was installed",
                             !after.dll.empty() && _wcsicmp(after.dll.c_str(), dll.c_str()) == 0, &after_failed);
    bool present_now = false;
    value_type(kAudioSettingsKey, kProtectedAudioValue, &present_now);
    done += "," + check_json("DisableProtectedAudioDG is 1",
                             present_now && read_dword(kAudioSettingsKey, kProtectedAudioValue) == 1, &after_failed);
    done += "," + check_json("the data directory carries the ACL",
                             dacl_grants(read_dacl(data_dir + L"\\devices"), sddl), &after_failed);
    done += "]";
    const std::string outcome = facts + ",\"after\":" + done + ",\"registration\":" + after.json;
    if (!after_failed.empty()) {
        return fail("machine-install", "installed, but " + after_failed.front() + " is not true", "," + outcome);
    }
    std::printf("{%s,\"ok\":true}\n", outcome.c_str());
    return 0;
}

// Every render endpoint IsoAPO is on, by its install record or by an effect
// slot. The installer would otherwise have to read `list`'s JSON to find them,
// and NSIS is a bad place to put a JSON reader.
std::vector<std::wstring> endpoints_with_isoapo() {
    std::vector<std::wstring> out;
    std::vector<std::wstring> guids;
    try {
        guids = RegistryHelper::enumSubKeys(std::wstring(kMMDevices) + L"\\Render");
    } catch (RegistryException&) {
        return out;
    }
    for (const std::wstring& guid : guids) {
        const Endpoint e{guid, false, std::wstring(kMMDevices) + L"\\Render\\" + guid};
        bool found = false;
        // A broken endpoint must not stop the rest being cleaned up.
        error_text([&] { found = read_record(kIsoChildApos, guid).exists || read_slots(e).isoapo; });
        if (found) out.push_back(guid);
    }
    return out;
}

int cmd_machine_uninstall(bool remove_data, bool dry_run) {
    const Registration before = read_registration();
    const std::wstring data_dir = program_data_dir();
    // Only what this install set, and only while nothing else needs it:
    // Equalizer APO sets the same value and stops working without it.
    bool ours_present = false;
    value_type(kIsotoneKey, kProtectedAudioOursValue, &ours_present);
    const bool ours = ours_present && read_dword(kIsotoneKey, kProtectedAudioOursValue) == 1;
    const bool equalizerapo = RegistryHelper::keyExists(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\EqualizerAPO");
    const bool restore_protected_audio = ours && !equalizerapo;
    // Taken off every endpoint before the class is unregistered: leaving a slot
    // pointing at a CLSID that no longer resolves is how an endpoint ends up
    // with no audio at all.
    const std::vector<std::wstring> outputs = endpoints_with_isoapo();
    std::string outputs_json = "[";
    for (size_t i = 0; i < outputs.size(); ++i) outputs_json += (i ? "," : "") + quote(outputs[i]);
    outputs_json += "]";
    const std::string facts = std::string("\"command\":\"machine-uninstall\",\"dry_run\":") + boolean(dry_run) +
                              ",\"outputs\":" + outputs_json +
                              ",\"dll\":" + (before.dll.empty() ? "null" : quote(before.dll)) +
                              ",\"protected_audiodg_set_by_isotone\":" + boolean(ours) +
                              ",\"equalizerapo_installed\":" + boolean(equalizerapo) +
                              ",\"restore_protected_audio\":" + boolean(restore_protected_audio) +
                              ",\"remove_data\":" + boolean(remove_data) + ",\"data_dir\":" +
                              (data_dir.empty() ? "null" : quote(data_dir));

    if (dry_run) {
        std::string would = "[";
        if (!outputs.empty()) {
            would += quote("take IsoAPO off " + std::to_string(outputs.size()) +
                           (outputs.size() == 1 ? " output" : " outputs")) + ",";
        }
        would += "\"call DllUnregisterServer in the DLL\"";
        if (restore_protected_audio) would += ",\"remove DisableProtectedAudioDG\"";
        else if (ours) would += ",\"leave DisableProtectedAudioDG: Equalizer APO is installed and needs it\"";
        else would += ",\"leave DisableProtectedAudioDG: this install did not set it\"";
        would += remove_data ? ",\"delete the data directory and every saved state in it\"]"
                             : ",\"leave the data directory and the saved state in it\"]";
        std::printf("{%s,\"ok\":true,\"would\":%s}\n", facts.c_str(), would.c_str());
        return 0;
    }

    // The endpoints first, through the same journalled, rolled-back path
    // `uninstall` uses; the class is only unregistered once no slot names it.
    for (const std::wstring& guid : outputs) {
        const Endpoint e{guid, false, std::wstring(kMMDevices) + L"\\Render\\" + guid};
        CommandScope scope(dry_run);
        std::string rollback;
        if (const std::string error = error_text([&] { recover_interrupted(e); }); !error.empty()) {
            return fail("machine-uninstall", utf8(guid) + ": " + error, "," + facts);
        }
        if (!read_record(kIsoChildApos, guid).exists) continue;   // a slot with no record: `uninstall` explains why
        const std::string error = scope.run(e, "uninstall", &rollback, [&] { uninstall_isoapo(e); });
        if (!error.empty()) return fail("machine-uninstall", utf8(guid) + ": " + error, "," + rollback + facts);
    }

    if (!before.dll.empty() && before.dll_exists) {
        const std::string error = call_dll_entry(before.dll, "DllUnregisterServer");
        if (!error.empty()) return fail("machine-uninstall", error, "," + facts);
    }
    if (restore_protected_audio) {
        const std::string error =
            error_text([] { RegistryHelper::deleteValue(kAudioSettingsKey, kProtectedAudioValue); });
        if (!error.empty()) return fail("machine-uninstall", "DisableProtectedAudioDG: " + error, "," + facts);
    }
    if (ours_present) {
        error_text([] { RegistryHelper::deleteValue(kIsotoneKey, kProtectedAudioOursValue); });
    }
    if (remove_data && !data_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(data_dir), ec);
        if (ec) return fail("machine-uninstall", "cannot delete " + utf8(data_dir) + ": " + ec.message(), "," + facts);
    }

    const Registration after = read_registration();
    std::vector<std::string> after_failed;
    std::string done = "[" + check_json("the post-mix CLSID is no longer registered", !after.apo_registered,
                                        &after_failed);
    bool present_now = false;
    value_type(kAudioSettingsKey, kProtectedAudioValue, &present_now);
    done += "," + check_json("DisableProtectedAudioDG is as it should be",
                             restore_protected_audio ? !present_now : true, &after_failed);
    done += "," + check_json("no output is left with IsoAPO in a slot", endpoints_with_isoapo().empty(),
                             &after_failed);
    done += "]";
    const std::string outcome = facts + ",\"after\":" + done + ",\"registration\":" + after.json;
    if (!after_failed.empty()) {
        return fail("machine-uninstall", "uninstalled, but " + after_failed.front() + " is not true", "," + outcome);
    }
    std::printf("{%s,\"ok\":true}\n", outcome.c_str());
    return 0;
}

struct Simulation {
    std::wstring pre, post;         // vendor APOs Equalizer APO replaced, pre-mix and post-mix
    bool hosted = true;             // false: "use original APO" was off in Equalizer APO
    bool eapo_unregistered = false; // Equalizer APO is uninstalled after IsoAPO replaced it
    // Equalizer APO's record also names vendor APOs in the slots it does not
    // hold, as its install writes when its mode deletes slots that had APOs.
    bool deleted_slots = false;
    // Equalizer APO's record names the vendor APOs only in the slots upstream's
    // getOriginalAPOPreMix/PostMix fall back to (LFX and GFX under SFX modes,
    // SFX and MFX under LFX_GFX), which its install deleted; its own slots were
    // empty.
    bool fallback = false;
};

// ---------------------------------------------------------------------------
// Simulations for roundtrip. Each runs inside a dry run the caller installed.

std::vector<std::wstring> slot_values_of(const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    std::vector<std::wstring> v;
    for (const SlotName& slot : kEffectSlots) {
        std::wstring value = L"(absent)";
        try {
            if (RegistryHelper::keyExists(fx) && RegistryHelper::valueExists(fx, slot.value))
                value = RegistryHelper::readValue(fx, slot.value);
        } catch (RegistryException&) {
            value = L"(unreadable)";
        }
        v.push_back(value);
    }
    return v;
}

std::string records_of(const Endpoint& e) {
    return read_record(kIsoChildApos, e.guid).json + read_record(kEapoChildApos, e.guid).json;
}

// How many times `vendor` runs on the endpoint: in an effect slot itself, and as
// the child of each IsoAPO or Equalizer APO instance in a slot (an Equalizer APO
// hosting IsoAPO runs IsoAPO's child too).
int vendor_runs(const Endpoint& e, const std::wstring& vendor) {
    const std::wstring fx = e.key + L"\\FxProperties";
    std::wstring iso_child, eapo_pre, eapo_post;
    try_read_string(iso_record_key(e), L"PostMixChild", &iso_child);
    try_read_string(eapo_record_key(e), L"PreMixChild", &eapo_pre);
    try_read_string(eapo_record_key(e), L"PostMixChild", &eapo_post);
    int runs = 0;
    for (const SlotName& slot : kEffectSlots) {
        std::wstring clsid;
        if (!try_read_string(fx, slot.value, &clsid)) continue;
        const std::string kind = classify(clsid);
        if (same_clsid(clsid, vendor)) ++runs;
        if (kind == "isoapo_post_mix" && same_clsid(iso_child, vendor)) ++runs;
        for (const auto& [eapo_kind, child] : {std::pair{"equalizerapo_pre_mix", eapo_pre}, std::pair{"equalizerapo_post_mix", eapo_post}}) {
            if (kind != eapo_kind) continue;
            if (same_clsid(child, vendor)) ++runs;
            if (is_isoapo(child) && same_clsid(iso_child, vendor)) ++runs;
        }
    }
    return runs;
}

// Equalizer APO's Device Selector ticking the endpoint, as upstream writes it at
// the vendored commit: DeviceAPOInfo::load (only Equalizer APO's own classes
// become !VALUE; anything else, IsoAPO included, is recorded as the original),
// getOriginalAPOPreMix/PostMix with their fallbacks, and install in `mode` with
// both classes and "use original APO" on or off, under Equalizer APO's GUIDs and
// record path.
void simulate_equalizerapo_install(const Endpoint& e, DeviceAPOInfo::InstallMode mode, bool hosted) {
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring record = eapo_record_key(e);
    const bool had_fx = RegistryHelper::keyExists(fx);
    std::wstring o[5];
    for (int i = 0; i < 5; ++i) {
        std::wstring v;
        o[i] = !had_fx ? APOGUID_NOKEY
               : try_read_string(fx, kEffectSlots[i].value, &v) && !is_equalizerapo(v) ? v
                                                                                     : APOGUID_NOVALUE;
    }
    const auto original = [&](bool pre_mix) {
        std::wstring g;
        if (mode == DeviceAPOInfo::INSTALL_LFX_GFX) {
            g = pre_mix ? o[0] : o[1];
            if (o[0] == APOGUID_NOVALUE && o[1] == APOGUID_NOVALUE) g = pre_mix ? o[2] : o[3];
        } else if (mode == DeviceAPOInfo::INSTALL_SFX_MFX) {
            g = pre_mix ? o[2] : o[3];
            if (o[2] == APOGUID_NOVALUE && o[3] == APOGUID_NOVALUE) g = pre_mix ? o[0] : o[1];
        } else {
            g = pre_mix ? o[2] : o[4];
            if (o[2] == APOGUID_NOVALUE && o[4] == APOGUID_NOVALUE) g = pre_mix ? o[0] : o[1];
        }
        return g == APOGUID_NOKEY || g == APOGUID_NOVALUE ? std::wstring() : g;
    };
    RegistryHelper::createKey(kEapoChildApos);
    RegistryHelper::createKey(record);
    if (!had_fx) RegistryHelper::createKey(fx);
    for (int i = 0; i < 5; ++i) RegistryHelper::writeValue(record, kEffectSlots[i].value, o[i]);
    RegistryHelper::writeValue(record, L"PreMixChild", hosted ? original(true) : L"");
    RegistryHelper::writeValue(record, L"PostMixChild", hosted ? original(false) : L"");
    RegistryHelper::writeValue(record, L"AllowSilentBufferModification", L"false");
    if (RegistryHelper::valueExists(record, L"DisableAutomaticAdjustment"))
        RegistryHelper::deleteValue(record, L"DisableAutomaticAdjustment");
    RegistryHelper::writeValue(record, L"Version", L"2");
    const std::wstring pre = RegistryHelper::getGuidString(EQUALIZERAPO_PRE_MIX_GUID);
    const std::wstring post = RegistryHelper::getGuidString(EQUALIZERAPO_POST_MIX_GUID);
    const auto remove = [&](int i) {
        if (RegistryHelper::valueExists(fx, kEffectSlots[i].value)) RegistryHelper::deleteValue(fx, kEffectSlots[i].value);
    };
    const auto put = [&](int i, const std::wstring& clsid, int mode_list) {
        RegistryHelper::writeValue(fx, kEffectSlots[i].value, clsid);
        if (mode_list >= 0 && !RegistryHelper::valueExists(fx, kModeSlots[mode_list].value))
            RegistryHelper::writeMultiValue(fx, kModeSlots[mode_list].value, L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}");
    };
    if (mode == DeviceAPOInfo::INSTALL_LFX_GFX) {
        put(0, pre, -1);
        put(1, post, -1);
        remove(2);
        remove(3);
        remove(4);
    } else {
        remove(0);
        remove(1);
        put(2, pre, 0);
        if (mode == DeviceAPOInfo::INSTALL_SFX_MFX) put(3, post, 1);
        else put(4, post, 2);
    }
    if (RegistryHelper::valueExists(fx, kDisableEnhancements)) RegistryHelper::deleteValue(fx, kDisableEnhancements);
}

// Equalizer APO's own uninstall (DeviceAPOInfo::uninstall with its record).
void simulate_equalizerapo_uninstall(const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring record = eapo_record_key(e);
    if (!RegistryHelper::keyExists(record)) return;
    std::wstring first;
    if (try_read_string(record, kEffectSlots[0].value, &first) && first == APOGUID_NOKEY) {
        if (RegistryHelper::keyExists(fx)) RegistryHelper::deleteKey(fx);
    } else {
        for (const SlotName& slot : kEffectSlots) {
            std::wstring original;
            if (!try_read_string(record, slot.value, &original)) continue;
            if (original == APOGUID_NOVALUE) {
                if (RegistryHelper::valueExists(fx, slot.value)) RegistryHelper::deleteValue(fx, slot.value);
            } else if (!original.empty()) {
                RegistryHelper::writeValue(fx, slot.value, original);
            }
        }
    }
    RegistryHelper::deleteKey(record);
    if (RegistryHelper::keyExists(kEapoChildApos) && RegistryHelper::keyEmpty(kEapoChildApos))
        RegistryHelper::deleteKey(kEapoChildApos);
}

// Collects the names of the expectations that failed.
struct Expectations {
    std::vector<std::string> failed;
    void operator()(bool ok, const std::string& what) {
        if (!ok) failed.push_back(what);
    }
    bool ok() const { return failed.empty(); }
};

// Equalizer APO's Device Selector ticks the endpoint while IsoAPO is installed on
// it (`installed`, a dry run holding that state). From there:
//  - install --replace-equalizerapo must leave the slots and IsoAPO's child as
//    IsoAPO's install left them, Equalizer APO's record gone, and uninstall must
//    then give back `device` (the slots before either EQ);
//  - uninstall must keep Equalizer APO in its slots with a record that names no
//    IsoAPO, and Equalizer APO's own uninstall must then give back `device`.
// `vendor`, when set, is the APO IsoAPO hosts: it must run once after the
// replacement, and after the uninstall as often as Equalizer APO was hosting
// IsoAPO (or once, where IsoAPO kept its slot and the vendor goes back there).
std::string check_retick(const Endpoint& e, const DryRunRegistry& installed, const std::vector<std::wstring>& device,
                         DeviceAPOInfo::InstallMode isoapo_mode, DeviceAPOInfo::InstallMode eapo_mode, bool hosted,
                         const std::wstring& vendor, bool* ok) {
    Expectations expect;
    DryRunRegistry state = installed;
    ScopedDryRun scope(&state);
    std::string state_name;
    const std::string error = error_text([&] {
        const std::vector<std::wstring> as_installed = slot_values_of(e);
        std::wstring child;
        try_read_string(iso_record_key(e), L"PostMixChild", &child);
        const std::string iso_record = read_record(kIsoChildApos, e.guid).json;
        simulate_equalizerapo_install(e, eapo_mode, hosted);

        const bool in_slot = read_slots(e).isoapo;
        std::wstring eapo_child;
        try_read_string(eapo_record_key(e), L"PostMixChild", &eapo_child);
        state_name = isoapo_state(e).name;
        expect(state_name == (in_slot ? "alongside_equalizerapo" : "replaced_by_equalizerapo"), "state");
        expect(plan_install(e, true).action == InstallAction::TakeBack, "install --replace-equalizerapo takes back");
        expect(plan_install(e, false).action == (in_slot ? InstallAction::AlreadyInstalled : InstallAction::Refuse),
               "install without the flag");
        expect(plan_repair(e, std::nullopt).skip == in_slot, "repair skips only an installed IsoAPO");
        if (!in_slot) expect(!plan_repair(e, std::nullopt).refusal.empty(), "repair refuses");
        const KnownMode known = install_mode_of(e);
        expect(known.mode && *known.mode == isoapo_mode, "install mode known");

        {
            DryRunRegistry replaced = state;
            ScopedDryRun replaced_scope(&replaced);
            take_back_from_equalizerapo(e, known.mode ? *known.mode : isoapo_mode);
            std::wstring child_after;
            try_read_string(iso_record_key(e), L"PostMixChild", &child_after);
            expect(slot_values_of(e) == as_installed, "replace: slots as IsoAPO's install left them");
            expect(child_after == child, "replace: IsoAPO hosts what it hosted");
            expect(!RegistryHelper::keyExists(eapo_record_key(e)), "replace: Equalizer APO's record gone");
            expect(read_record(kIsoChildApos, e.guid).json == iso_record, "replace: IsoAPO's record unchanged");
            expect(isoapo_state(e).name == std::string("installed"), "replace: state installed");
            if (!vendor.empty()) expect(vendor_runs(e, vendor) == 1, "replace: vendor runs once");
            uninstall_isoapo(e);
            expect(slot_values_of(e) == device, "replace, uninstall: the device as before either EQ");
            expect(!RegistryHelper::keyExists(iso_record_key(e)) && !RegistryHelper::keyExists(eapo_record_key(e)),
                   "replace, uninstall: no record left");
            if (!vendor.empty()) expect(vendor_runs(e, vendor) == 1, "replace, uninstall: vendor runs once");
        }
        {
            DryRunRegistry removed = state;
            ScopedDryRun removed_scope(&removed);
            const std::vector<std::wstring> with_eapo = slot_values_of(e);
            uninstall_isoapo(e);
            const std::vector<std::wstring> after = slot_values_of(e);
            const std::wstring fx = e.key + L"\\FxProperties";
            bool eapo_kept = true, isoapo_gone = true, lists_kept = true;
            for (size_t i = 0; i < after.size(); ++i) {
                if (is_equalizerapo(with_eapo[i])) {
                    eapo_kept &= after[i] == with_eapo[i];
                    for (const SlotName& m : kModeSlots) {
                        if (std::string(m.label) == kEffectSlots[i].label) lists_kept &= RegistryHelper::valueExists(fx, m.value);
                    }
                }
                isoapo_gone &= !is_isoapo(after[i]);
            }
            expect(eapo_kept, "uninstall: Equalizer APO stays in its slots");
            expect(lists_kept, "uninstall: processing-mode lists stay where Equalizer APO is");
            expect(isoapo_gone, "uninstall: IsoAPO in no slot");
            expect(!RegistryHelper::keyExists(iso_record_key(e)), "uninstall: IsoAPO's record gone");
            expect(RegistryHelper::keyExists(eapo_record_key(e)) && !equalizerapo_over_isoapo(e),
                   "uninstall: Equalizer APO's record names no IsoAPO");
            if (!vendor.empty())
                expect(vendor_runs(e, vendor) == (in_slot || is_isoapo(eapo_child) ? 1 : 0),
                       "uninstall: vendor runs as Equalizer APO hosted IsoAPO");
            simulate_equalizerapo_uninstall(e);
            expect(slot_values_of(e) == device, "uninstall, Equalizer APO's uninstall: the device as before either EQ");
        }
    });
    expect(error.empty(), "threw: " + error);
    *ok = expect.ok();
    return std::string("{\"equalizerapo_mode\":") + quote(mode_name(eapo_mode)) + ",\"hosted\":" + boolean(hosted) +
           ",\"vendor\":" + (vendor.empty() ? "null" : quote(vendor)) + ",\"state\":" + quote(state_name) +
           ",\"ok\":" + boolean(*ok) + ",\"failed\":" + string_array(expect.failed) + "}";
}

// Kills `command` at each of its writes in turn, each in its own copy of `start`.
// After each kill the next command's recovery must give back the endpoint and
// both records exactly as they were before `command`, and leave no journal.
std::string check_kills(const Endpoint& e, const DryRunRegistry& start, const char* name,
                        const std::function<void()>& command, bool* ok) {
    size_t writes = 0;
    std::string error, completed_records;
    Snapshot completed;
    {
        DryRunRegistry full = start;
        ScopedDryRun scope(&full);
        const size_t before = full.operations().size();
        error = error_text([&] { journaled(e, name, snapshot_endpoint(e), command); });
        writes = full.operations().size() - before;
        completed = snapshot_endpoint(e);
        completed_records = records_of(e);
    }
    size_t recovered = 0;
    std::vector<std::string> failed;
    for (size_t k = 0; error.empty() && k < writes; ++k) {
        DryRunRegistry run = start;
        ScopedDryRun scope(&run);
        const Snapshot before = snapshot_endpoint(e);
        const std::string records = records_of(e);
        run.kill_at(run.operations().size() + k);
        bool killed = false;
        try {
            journaled(e, name, before, command);
        } catch (SimulatedKill&) {
            killed = true;
        }
        run.kill_at(static_cast<size_t>(-1));
        // The journal is complete after its first four writes, and its removal
        // is the command's last write but one (the parent key follows): killed
        // after that, the command had finished, and finished is what must stay.
        const bool finished = k >= 4 && !RegistryHelper::keyExists(journal_key(e));
        const bool shows_interrupted = k < 4 || finished || isoapo_state(e).name == std::string("interrupted");
        const std::string recovery = error_text([&] { recover_interrupted(e); });
        const bool back = killed && shows_interrupted && recovery.empty() &&
                          (finished ? snapshot_endpoint(e) == completed && records_of(e) == completed_records
                                    : snapshot_endpoint(e) == before && records_of(e) == records) &&
                          !RegistryHelper::keyExists(kPending);
        if (back) ++recovered;
        else failed.push_back("write " + std::to_string(k) + (recovery.empty() ? "" : ": " + recovery));
    }
    *ok = error.empty() && writes > 0 && recovered == writes;
    return std::string("{\"command\":") + quote(std::string(name)) + ",\"kill_points\":" + std::to_string(writes) +
           ",\"recovered\":" + std::to_string(recovered) + ",\"ok\":" + boolean(*ok) +
           (error.empty() ? "" : ",\"error\":" + quote(error)) + ",\"failed\":" + string_array(failed) + "}";
}

// enable-enhancements and the remedy status offers for it, in copies of `start`
// with the flag at 1, at 0 and absent, IsoAPO in a slot (its class put in MFX
// with a record, where it is in none) and in none, and an interrupted command.
std::string check_enhancements(const Endpoint& e, const DryRunRegistry& start, bool* ok) {
    Expectations expect;
    const std::wstring fx = e.key + L"\\FxProperties";
    const auto offered = [&] {
        const IsoState s = isoapo_state(e);
        return std::find(s.remedies.begin(), s.remedies.end(), "enable-enhancements") != s.remedies.end();
    };
    const auto set_flag = [&](int flag) {   // -1: absent
        if (flag >= 0) RegistryHelper::writeDWORDValue(fx, kDisableEnhancements, static_cast<unsigned long>(flag));
        else if (RegistryHelper::valueExists(fx, kDisableEnhancements)) RegistryHelper::deleteValue(fx, kDisableEnhancements);
    };
    const std::string error = error_text([&] {
        for (bool in_slot : {true, false}) {
            DryRunRegistry state = start;
            ScopedDryRun scope(&state);
            if (RegistryHelper::keyExists(journal_key(e))) RegistryHelper::deleteKey(journal_key(e));
            if (!RegistryHelper::keyExists(fx)) {
                // As upstream's install makes it.
                RegistryHelper::takeOwnership(e.key);
                RegistryHelper::makeWritable(e.key);
                RegistryHelper::createKey(fx);
            }
            if (in_slot && !read_slots(e).isoapo) {
                RegistryHelper::writeValue(fx, kEffectSlots[3].value, RegistryHelper::getGuidString(ISOAPO_POST_MIX_GUID));
                RegistryHelper::createKey(kIsoChildApos);
                RegistryHelper::createKey(iso_record_key(e));
            }
            for (const SlotName& slot : kEffectSlots) {
                std::wstring clsid;
                if (!in_slot && try_read_string(fx, slot.value, &clsid) && is_isoapo(clsid)) RegistryHelper::deleteValue(fx, slot.value);
            }
            for (int flag : {1, 0, -1}) {
                DryRunRegistry flagged = state;
                ScopedDryRun flagged_scope(&flagged);
                set_flag(flag);
                const std::string name = std::string(in_slot ? "IsoAPO in a slot" : "IsoAPO in no slot") + ", flag " +
                                         (flag < 0 ? "absent" : std::to_string(flag));
                expect(enhancements_disabled(e) == (flag == 1), name + ": read as upstream reads it");
                expect(offered() == (in_slot && flag == 1), name + ": status offers enable-enhancements");
                const size_t before = flagged.operations().size();
                const bool changed = enable_enhancements(e);
                const std::vector<RegistryOperation> ops(flagged.operations().begin() + static_cast<std::ptrdiff_t>(before),
                                                         flagged.operations().end());
                expect(changed == (flag >= 0) && ops.size() == (flag >= 0 ? 1u : 0u), name + ": deletes the flag when present, and nothing else");
                expect(ops.empty() || (ops[0].operation == L"delete value" && _wcsicmp(ops[0].key.c_str(), fx.c_str()) == 0 &&
                                       _wcsicmp(ops[0].valuename.c_str(), kDisableEnhancements) == 0),
                       name + ": the write is the flag's deletion");
                expect(!RegistryHelper::valueExists(fx, kDisableEnhancements) && !enhancements_disabled(e) && !offered(),
                       name + ": enhancements on after");
                expect(!enable_enhancements(e) && flagged.operations().size() == before + ops.size(), name + ": a second run writes nothing");
            }
            if (in_slot) {
                // An interrupted command: repair is the remedy, whatever the flag.
                DryRunRegistry interrupted = state;
                ScopedDryRun interrupted_scope(&interrupted);
                set_flag(1);
                RegistryHelper::createKey(kPending);
                RegistryHelper::createKey(journal_key(e));
                expect(!offered() && isoapo_state(e).name == std::string("interrupted"), "interrupted: repair, not enable-enhancements");
            }
        }
    });
    expect(error.empty(), "threw: " + error);
    *ok = expect.ok();
    return std::string("{\"ok\":") + boolean(*ok) + ",\"failed\":" + string_array(expect.failed) + "}";
}

// A device with no Equalizer APO on it (its own uninstall run, or its classes
// deleted where it left no record), with `vendor` in the slot `mode` writes
// IsoAPO to and `deleted_vendor` in a slot `mode` deletes, when given, and
// optionally no processing-mode lists (every endpoint on the test machine has
// all three, so what install records as absent is otherwise never exercised).
void prepare_device(const Endpoint& e, DeviceAPOInfo::InstallMode mode, const std::wstring& vendor,
                    const std::wstring& deleted_vendor, bool without_mode_lists) {
    const std::wstring fx = e.key + L"\\FxProperties";
    simulate_equalizerapo_uninstall(e);
    for (const SlotName& slot : kEffectSlots) {
        std::wstring clsid;
        if (try_read_string(fx, slot.value, &clsid) && is_equalizerapo(clsid)) RegistryHelper::deleteValue(fx, slot.value);
    }
    if (!RegistryHelper::keyExists(fx)) return;
    for (const SlotName& m : kModeSlots) {
        if (without_mode_lists && RegistryHelper::valueExists(fx, m.value)) RegistryHelper::deleteValue(fx, m.value);
    }
    for (const SlotName& slot : kEffectSlots) {
        if (!vendor.empty() && std::string(slot.label) == post_slot_label(mode)) RegistryHelper::writeValue(fx, slot.value, vendor);
    }
    const char* deleted_label = mode == DeviceAPOInfo::INSTALL_LFX_GFX ? "SFX" : "LFX";
    for (const SlotName& slot : kEffectSlots) {
        if (!deleted_vendor.empty() && std::string(slot.label) == deleted_label)
            RegistryHelper::writeValue(fx, slot.value, deleted_vendor);
    }
}

// Microsoft's WM audio APOs, registered on every Windows install this was run on,
// stand in for vendor APOs.
// A vendor APO for a simulation: one of two WM audio GFX APOs, whichever is in
// no slot of the endpoint already, so each run counts only the one placed.
std::wstring simulated_vendor(const Endpoint& e) {
    const std::vector<std::wstring> slots = slot_values_of(e);
    for (const wchar_t* candidate : {L"{13AB3EBD-137E-4903-9D89-60BE8277FD17}", L"{637C490D-EEE3-4C0A-973F-371958802DA2}"}) {
        if (std::none_of(slots.begin(), slots.end(), [&](const std::wstring& v) { return same_clsid(v, candidate); }))
            return candidate;
    }
    return L"";
}
const wchar_t* const kSimulatedDeletedVendor = L"{C9453E73-8C5C-4463-9984-AF8BAB2F5447}";   // WM audio LFX APO

const DeviceAPOInfo::InstallMode kModes[] = {DeviceAPOInfo::INSTALL_LFX_GFX, DeviceAPOInfo::INSTALL_SFX_MFX,
                                             DeviceAPOInfo::INSTALL_SFX_EFX};

// Every Device Selector re-tick case against `installed`: three Equalizer APO
// modes, "use original APO" on and off.
std::string retick_matrix(const Endpoint& e, const DryRunRegistry& installed, const std::vector<std::wstring>& device,
                          DeviceAPOInfo::InstallMode isoapo_mode, const std::wstring& vendor, bool* ok) {
    std::string out;
    *ok = true;
    for (DeviceAPOInfo::InstallMode eapo_mode : kModes) {
        for (bool hosted : {true, false}) {
            bool case_ok = false;
            out += std::string(out.empty() ? "" : ",") +
                   check_retick(e, installed, device, isoapo_mode, eapo_mode, hosted, vendor, &case_ok);
            *ok &= case_ok;
        }
    }
    return out;
}

// Roundtrip on an endpoint IsoAPO is already installed on (CABLE Input on the
// owner's machine): everything that can be checked from the install as it is.
int cmd_roundtrip_installed(const Endpoint& e) {
    // Every check below runs in dry runs of its own; this one catches anything
    // written outside them.
    DryRunRegistry outer;
    ScopedDryRun outer_scope(&outer);
    Expectations expect;
    std::string retick, kills, legacy, enhancements = "null";
    const std::string error = error_text([&] {
        DryRunRegistry installed;
        bool enhancements_ok = false;
        enhancements = check_enhancements(e, installed, &enhancements_ok);
        expect(enhancements_ok, "enable-enhancements cases");
        std::vector<std::wstring> device;
        std::wstring child;
        KnownMode known;
        std::optional<DeviceAPOInfo::InstallMode> recorded;
        {
            ScopedDryRun scope(&installed);
            expect(isoapo_state(e).name == std::string("installed"), "state installed");
            known = install_mode_of(e);
            recorded = recorded_mode(e);
            try_read_string(iso_record_key(e), L"PostMixChild", &child);
        }
        expect(known.mode.has_value(), "install mode known");
        {
            DryRunRegistry removed = installed;
            ScopedDryRun scope(&removed);
            uninstall_isoapo(e);
            device = slot_values_of(e);
            expect(!read_slots(e).isoapo && !RegistryHelper::keyExists(iso_record_key(e)), "uninstall removes IsoAPO and its record");
        }
        if (known.mode) {
            bool ok = false;
            retick = retick_matrix(e, installed, device, *known.mode, is_other_apo(child) ? child : L"", &ok);
            expect(ok, "Device Selector re-tick cases");

            // A driver update detaches IsoAPO: repair must put it back where it
            // was, or refuse without --mode when the record cannot say where.
            DryRunRegistry detached = installed;
            ScopedDryRun scope(&detached);
            for (const SlotName& slot : kEffectSlots) {
                std::wstring clsid;
                if (try_read_string(e.key + L"\\FxProperties", slot.value, &clsid) && is_isoapo(clsid))
                    RegistryHelper::deleteValue(e.key + L"\\FxProperties", slot.value);
            }
            const RepairPlan plain = plan_repair(e, std::nullopt);
            const RepairPlan with_mode = plan_repair(e, known.mode);
            expect(recorded ? (plain.mode == recorded) : !plain.refusal.empty(),
                   recorded ? "repair reuses the recorded mode" : "repair refuses a legacy record without --mode");
            expect(with_mode.mode == known.mode && with_mode.refusal.empty(), "repair with --mode");
            expect(std::string(isoapo_state(e).name) == "detached", "detached state");
            legacy = std::string("{\"recorded_mode\":") + (recorded ? quote(mode_name(*recorded)) : "null") +
                     ",\"repair_without_mode\":" + (plain.refusal.empty() ? quote(mode_name(*plain.mode)) : quote(plain.refusal)) + "}";

            bool kill_ok = false;
            kills = check_kills(e, installed, "uninstall", [&] { uninstall_isoapo(e); }, &kill_ok);
            expect(kill_ok, "kills during uninstall");
        }
    });
    expect(error.empty(), "threw: " + error);
    std::printf("{\"command\":\"roundtrip\",\"ok\":%s,\"already_installed\":true,\"failed\":%s,\"legacy_repair\":%s,"
                "\"device_selector_retick\":[%s],\"kills\":[%s],\"enable_enhancements\":%s}\n",
                boolean(expect.ok()), string_array(expect.failed).c_str(), legacy.empty() ? "null" : legacy.c_str(),
                retick.c_str(), kills.c_str(), enhancements.c_str());
    return expect.ok() ? 0 : kExitFailed;
}

// Always a dry run, against one DryRunRegistry, checking that uninstall
// restores what install changed, twice: directly after an install, and after
// a simulated driver update and a repair. With replace_eapo the install also
// removes Equalizer APO. A Simulation first makes Equalizer APO's install
// record, in the dry run only, say that it replaced vendor APOs in the slots it
// holds (and hosts them, or not): the case no test machine may have.
int cmd_roundtrip(const Endpoint& e, std::optional<DeviceAPOInfo::InstallMode> mode, bool replace_eapo,
                  const std::optional<Simulation>& sim) {
    if (e.input) return fail("roundtrip", "capture endpoints are not supported: IsoAPO is an output EQ");
    const std::wstring fx = e.key + L"\\FxProperties";
    const auto slot_values = [&]() {
        std::vector<std::wstring> v;
        for (const SlotName& slot : kEffectSlots) {
            std::wstring value = L"(absent)";
            try {
                if (RegistryHelper::keyExists(fx) && RegistryHelper::valueExists(fx, slot.value))
                    value = RegistryHelper::readValue(fx, slot.value);
            } catch (RegistryException&) {
                value = L"(unreadable)";
            }
            v.push_back(value);
        }
        return v;
    };
    // The processing-mode lists and the enhancements flag, as upstream's
    // uninstall alone would leave them changed.
    const auto extra_values = [&]() {
        std::vector<std::wstring> v;
        for (const SlotName& m : kModeSlots) {
            v.push_back(RegistryHelper::keyExists(fx) && RegistryHelper::valueExists(fx, m.value) ? L"present" : L"absent");
        }
        bool present = false;
        const DWORD type = value_type(fx, kDisableEnhancements, &present);
        std::wstring enh = L"absent";
        try {
            if (RegistryHelper::keyExists(fx) && RegistryHelper::valueExists(fx, kDisableEnhancements)) {
                enh = type == REG_DWORD && present ? std::to_wstring(read_dword(fx, kDisableEnhancements))
                                                   : L"present";
            }
        } catch (RegistryException&) {
        }
        v.push_back(enh);
        return v;
    };
    const auto extras_json = [&](const std::vector<std::wstring>& v) {
        std::string out = "[";
        for (size_t i = 0; i < v.size(); ++i) out += std::string(i ? "," : "") + quote(v[i]);
        return out + "]";
    };
    const auto slots_json = [&](const std::vector<std::wstring>& v) {
        std::string out = "{";
        for (size_t i = 0; i < v.size(); ++i)
            out += std::string(i ? "," : "") + quote(std::string(kEffectSlots[i].label)) + ":" + quote(v[i]);
        return out + "}";
    };

    if (read_slots(e).isoapo && read_record(kIsoChildApos, e.guid).exists) {
        if (mode || replace_eapo) return fail("roundtrip", "IsoAPO is installed on this endpoint; roundtrip checks the install as it is and takes no --mode or --replace-equalizerapo");
        return cmd_roundtrip_installed(e);
    }

    DryRunRegistry dry;
    ScopedDryRun scope(&dry);
    const std::wstring eapo_record = eapo_record_key(e);
    const std::wstring iso_record = iso_record_key(e);
    if (sim && !read_slots(e).equalizerapo) {
        // What Equalizer APO hosts only means something where it is installed.
        return fail("roundtrip", "--simulate-equalizerapo needs Equalizer APO in a slot on this endpoint");
    }
    if (sim) {
        try {
            RegistryHelper::createKey(eapo_record);
            RegistryHelper::writeValue(eapo_record, L"PreMixChild", sim->hosted ? sim->pre : L"");
            RegistryHelper::writeValue(eapo_record, L"PostMixChild", sim->hosted ? sim->post : L"");
            bool eapo_in_lfx_gfx = false;
            for (const SlotName& slot : kEffectSlots) {
                std::wstring clsid;
                const std::string label = slot.label;
                if ((label == "LFX" || label == "GFX") && try_read_string(fx, slot.value, &clsid) && is_equalizerapo(clsid))
                    eapo_in_lfx_gfx = true;
            }
            for (const SlotName& slot : kEffectSlots) {
                std::wstring clsid;
                const std::string label = slot.label;
                const bool occupied = try_read_string(fx, slot.value, &clsid);
                const bool pre_slot = label == "LFX" || label == "SFX";
                const std::wstring& vendor = pre_slot ? sim->pre : sim->post;
                if (sim->fallback) {
                    const bool fallback_slot = eapo_in_lfx_gfx ? (label == "SFX" || label == "MFX") : (label == "LFX" || label == "GFX");
                    if (fallback_slot && !occupied && !vendor.empty()) RegistryHelper::writeValue(eapo_record, slot.value, vendor);
                    continue;
                }
                if (occupied ? !is_equalizerapo(clsid) : !sim->deleted_slots) continue;
                if (!vendor.empty()) RegistryHelper::writeValue(eapo_record, slot.value, vendor);
            }
        } catch (RegistryException& ex) {
            return fail("roundtrip", utf8(ex.getMessage()));
        }
    }
    const std::vector<std::wstring> before = slot_values();
    const std::vector<std::wstring> extras_before = extra_values();
    // What uninstall must restore. Without replacing Equalizer APO, the slots as
    // they were. Replacing it, the device as it was before Equalizer APO: its
    // record's original for each slot it held, or nothing.
    std::vector<std::wstring> expected_after = before;
    bool had_eapo_record = false;
    try {
        had_eapo_record = RegistryHelper::keyExists(eapo_record);
        for (size_t i = 0; replace_eapo && i < before.size(); ++i) {
            if (!is_equalizerapo(before[i]) && before[i] != L"(absent)") continue;
            std::wstring original;
            const bool had_apo = try_read_string(eapo_record, kEffectSlots[i].value, &original) && is_other_apo(original);
            if (is_equalizerapo(before[i])) expected_after[i] = had_apo ? original : L"(absent)";
            else if (had_apo) expected_after[i] = original;
        }
    } catch (RegistryException& ex) {
        return fail("roundtrip", utf8(ex.getMessage()));
    }
    const std::wstring post = RegistryHelper::getGuidString(ISOAPO_POST_MIX_GUID);
    const auto holds_isoapo = [&](const std::vector<std::wstring>& v) {
        return std::find(v.begin(), v.end(), post) != v.end();
    };

    std::vector<std::wstring> installed, after_direct, before_again, detached, repaired, after;
    std::vector<std::wstring> extras_after_direct, extras_after_update, extras_after;
    std::wstring child_after_install;
    DeviceAPOInfo::InstallMode chosen = DeviceAPOInfo::INSTALL_SFX_MFX;
    bool reloaded_installed = false, detach_seen = false, eapo_record_left = false;
    try {
        // Install and uninstall directly.
        DeviceAPOInfo first;
        if (!first.load(e.guid)) return fail("roundtrip", "endpoint is not present");
        if (first.isInstalled()) return fail("roundtrip", "IsoAPO is already in a slot on this endpoint");
        chosen = mode ? *mode : default_mode(first, e);
        if (replace_eapo) {
            const std::wstring lost = hosted_apo_in_deleted_slot(e, chosen);
            if (!lost.empty())
                return fail("roundtrip", "this mode deletes the slot where Equalizer APO hosts " + utf8(lost) +
                                             "; install refuses it");
        }
        install_isoapo(first, e, chosen, replace_eapo);
        installed = slot_values();
        eapo_record_left = RegistryHelper::keyExists(eapo_record);
        try_read_string(iso_record, L"PostMixChild", &child_after_install);
        DeviceAPOInfo second;
        second.load(e.guid);
        reloaded_installed = second.isInstalled();
        if (sim && sim->eapo_unregistered) {
            // Equalizer APO's own uninstaller runs: its classes are unregistered.
            for (const GUID& g : {EQUALIZERAPO_PRE_MIX_GUID, EQUALIZERAPO_POST_MIX_GUID}) {
                const std::wstring k = std::wstring(kApoRegistration) + L"\\" + RegistryHelper::getGuidString(g);
                if (RegistryHelper::keyExists(k)) RegistryHelper::deleteKey(k);
            }
        }
        uninstall_isoapo(e);
        after_direct = slot_values();
        extras_after_direct = extra_values();
        if (sim && sim->eapo_unregistered) {
            throw RegistryException(L"");   // the rest assumes Equalizer APO is still there
        }

        // Install again, lose it to a simulated driver update, repair, uninstall.
        // After a replacement the first uninstall left no Equalizer APO, so this
        // cycle starts from, and must return to, the slots as they are now.
        before_again = slot_values();
        DeviceAPOInfo again;
        again.load(e.guid);
        install_isoapo(again, e, chosen, replace_eapo);
        const std::vector<std::wstring> reinstalled = slot_values();
        // A driver update rewrites FxProperties with its own APOs: put the
        // slots back as they were before the install, leaving the record.
        for (size_t i = 0; i < before_again.size(); ++i) {
            if (reinstalled[i] == before_again[i]) continue;
            if (before_again[i] == L"(absent)") RegistryHelper::deleteValue(fx, kEffectSlots[i].value);
            else RegistryHelper::writeValue(fx, kEffectSlots[i].value, before_again[i]);
        }
        // It declares processing modes of its own, too. Uninstall and repair
        // must leave those as the driver wrote them.
        for (const SlotName& m : kModeSlots) {
            RegistryHelper::writeMultiValue(fx, m.value, std::vector<std::wstring>{
                L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}", L"{9E90EA20-B493-4FD1-A1A8-7E1361A956CF}"});
        }
        extras_after_update = extra_values();
        detached = slot_values();
        DeviceAPOInfo third;
        third.load(e.guid);
        detach_seen = !third.isInstalled() && RegistryHelper::keyExists(iso_record);

        // Repair's own reattach, in the mode it would pick. Without
        // --replace-equalizerapo on an endpoint Equalizer APO is on, the repair
        // command refuses (the one-backend rule, checked separately); the
        // reattach itself is still checked here.
        const std::optional<DeviceAPOInfo::InstallMode> repair_mode = mode ? mode : install_mode_of(e).mode;
        if (!repair_mode) throw RegistryException(L"repair cannot tell the install mode");
        reattach(e, *repair_mode, false);
        repaired = slot_values();

        uninstall_isoapo(e);
        after = slot_values();
        extras_after = extra_values();
    } catch (RegistryException& ex) {
        if (!(sim && sim->eapo_unregistered && ex.getMessage().empty())) {
            return fail("roundtrip", utf8(ex.getMessage()));
        }
    }

    // A driver change that removes FxProperties while IsoAPO is installed: the
    // uninstall must still succeed and remove the record. A separate dry run.
    bool survives_missing_fx_properties = false;
    {
        DryRunRegistry lost;
        ScopedDryRun lost_scope(&lost);
        try {
            DeviceAPOInfo info;
            if (info.load(e.guid) && !info.isInstalled()) {
                install_isoapo(info, e, chosen, false);
                RegistryHelper::deleteKey(fx);
                const Uninstalled u = uninstall_isoapo(e);
                survives_missing_fx_properties = u.fx_properties_missing && !RegistryHelper::keyExists(iso_record);
            }
        } catch (RegistryException&) {
        }
    }

    // The uninstall for an endpoint that is no longer present works from the
    // record alone: it must leave the slots as upstream's uninstall does.
    bool record_only_uninstall_restores = false;
    {
        DryRunRegistry gone;
        ScopedDryRun gone_scope(&gone);
        try {
            DeviceAPOInfo info;
            if (info.load(e.guid) && !info.isInstalled()) {
                const std::vector<std::wstring> start = slot_values();
                install_isoapo(info, e, chosen, false);
                uninstall_from_record(e);
                record_only_uninstall_restores = slot_values() == start && !RegistryHelper::keyExists(iso_record);
            }
        } catch (RegistryException&) {
        }
    }

    // A command that fails part way rolls back: after an install, and after an
    // uninstall, the rollback must give back exactly what was there. Separate
    // dry runs.
    bool rollback_restores = false;
    {
        DryRunRegistry undone;
        ScopedDryRun undone_scope(&undone);
        try {
            DeviceAPOInfo info;
            if (info.load(e.guid) && !info.isInstalled()) {
                const Snapshot start = snapshot_endpoint(e);
                // Read independently of the snapshot, so what it leaves out shows.
                const std::string records = read_record(kIsoChildApos, e.guid).json + read_record(kEapoChildApos, e.guid).json;
                install_isoapo(info, e, chosen, replace_eapo);
                const Snapshot installed_state = snapshot_endpoint(e);
                roll_back(start);
                const bool install_undone = snapshot_endpoint(e) == start &&
                                            read_record(kIsoChildApos, e.guid).json + read_record(kEapoChildApos, e.guid).json == records;
                DeviceAPOInfo again;
                again.load(e.guid);
                install_isoapo(again, e, chosen, replace_eapo);
                const Snapshot reinstalled = snapshot_endpoint(e);
                uninstall_isoapo(e);
                roll_back(reinstalled);
                rollback_restores = install_undone && start != installed_state && snapshot_endpoint(e) == reinstalled;
            }
        } catch (RegistryException&) {
        }
    }

    // The states after install: Equalizer APO's Device Selector ticking the
    // endpoint again, a driver update, legacy records, interrupted runs, failed
    // rollbacks, missing keys and mode lists. Each in dry runs of its own, from a
    // device prepared without Equalizer APO (with a vendor APO in IsoAPO's slot
    // and another in a slot the mode deletes, or with none).
    Expectations more;
    std::string retick_json, kills_json, enhancements_json = "null";
    const std::string more_error = error_text([&] {
        const std::wstring fx_key = e.key + L"\\FxProperties";
        {
            bool enhancements_ok = false;
            enhancements_json = check_enhancements(e, DryRunRegistry(), &enhancements_ok);
            more(enhancements_ok, "enable-enhancements cases");
        }
        const auto restore_driver_slot = [&](const std::vector<std::wstring>& device) {
            // A driver update: IsoAPO's slot back to what the driver declares.
            for (size_t i = 0; i < device.size(); ++i) {
                std::wstring clsid;
                if (!try_read_string(fx_key, kEffectSlots[i].value, &clsid) || !is_isoapo(clsid)) continue;
                if (device[i] == L"(absent)") RegistryHelper::deleteValue(fx_key, kEffectSlots[i].value);
                else RegistryHelper::writeValue(fx_key, kEffectSlots[i].value, device[i]);
            }
        };
        const auto install_here = [&](bool replacing) {
            DeviceAPOInfo info;
            if (!info.load(e.guid)) throw RegistryException(L"endpoint is not present");
            install_isoapo(info, e, chosen, replacing);
        };
        const std::wstring simulated = simulated_vendor(e);
        for (const std::wstring& vendor : {std::wstring(), simulated}) {
            DryRunRegistry prepared, installed;
            std::vector<std::wstring> device;
            {
                ScopedDryRun s(&prepared);
                prepare_device(e, chosen, vendor, vendor.empty() ? L"" : kSimulatedDeletedVendor, !vendor.empty());
                device = slot_values_of(e);
            }
            installed = prepared;
            {
                ScopedDryRun s(&installed);
                install_here(false);
                std::wstring child;
                try_read_string(iso_record_key(e), L"PostMixChild", &child);
                more(child == vendor, "the prepared install hosts the APO in its slot");
            }
            bool ok = false;
            retick_json += std::string(retick_json.empty() ? "" : ",") +
                           retick_matrix(e, installed, device, chosen, vendor, &ok);
            more(ok, vendor.empty() ? "Device Selector re-tick cases" : "Device Selector re-tick cases, IsoAPO hosting a vendor APO");
            if (vendor.empty()) {
                // A detached install on an endpoint Equalizer APO was then
                // installed on: install --replace-equalizerapo reattaches with
                // the replacement, where it used to send the user to repair and
                // repair sent them back.
                DryRunRegistry d = installed;
                ScopedDryRun s(&d);
                restore_driver_slot(device);
                simulate_equalizerapo_install(e, chosen, true);
                more(plan_install(e, true).action == InstallAction::ReattachReplacing, "detached under Equalizer APO: install --replace-equalizerapo reattaches");
                more(plan_install(e, false).action == InstallAction::Refuse && !plan_repair(e, std::nullopt).refusal.empty(),
                     "detached under Equalizer APO: install without the flag and repair refuse");
                reattach(e, chosen, true);
                more(read_slots(e).isoapo && !read_slots(e).equalizerapo, "detached under Equalizer APO: replaced");
                uninstall_isoapo(e);
                more(slot_values_of(e) == device, "detached under Equalizer APO: uninstall gives back the device");

                // A legacy record (no Isotone.InstallMode) on a detached endpoint:
                // repair must refuse without --mode, not pick upstream's slot.
                DryRunRegistry l = installed;
                ScopedDryRun ls(&l);
                RegistryHelper::deleteValue(iso_record_key(e), kRecordInstallMode);
                more(install_mode_of(e).mode == chosen, "legacy record: mode from the slot while installed");
                restore_driver_slot(device);
                more(!plan_repair(e, std::nullopt).refusal.empty(), "legacy record: repair refuses without --mode");
                more(plan_repair(e, chosen).mode == chosen, "legacy record: repair with --mode");
                const IsoState legacy_state = isoapo_state(e);
                more(std::find(legacy_state.remedies.begin(), legacy_state.remedies.end(), "repair --mode") !=
                         legacy_state.remedies.end(), "legacy record: status asks for --mode");

                // A driver update, then uninstall from the record alone (the
                // endpoint gone): the driver's slots stay.
                DryRunRegistry g = installed;
                ScopedDryRun gs(&g);
                for (size_t i = 0; i < std::size(kEffectSlots); ++i) {
                    std::wstring clsid;
                    if (try_read_string(fx_key, kEffectSlots[i].value, &clsid) && is_isoapo(clsid))
                        RegistryHelper::writeValue(fx_key, kEffectSlots[i].value, simulated);
                }
                const std::vector<std::wstring> driver_slots = slot_values_of(e);
                uninstall_from_record(e);
                more(slot_values_of(e) == driver_slots && !RegistryHelper::keyExists(iso_record_key(e)),
                     "detached, record-only uninstall leaves the driver's slots");
                continue;
            }

            // Kills at every write of each command, from the states it runs in.
            bool k1 = false, k2 = false, k3 = false, k4 = false, k5 = false;
            kills_json += check_kills(e, prepared, "install", [&] { install_here(false); }, &k1);
            kills_json += "," + check_kills(e, installed, "uninstall", [&] { uninstall_isoapo(e); }, &k2);
            DryRunRegistry detached_state = installed;
            {
                ScopedDryRun s(&detached_state);
                restore_driver_slot(device);
            }
            kills_json += "," + check_kills(e, detached_state, "repair", [&] { reattach(e, chosen, false); }, &k3);
            DryRunRegistry under = installed;
            {
                ScopedDryRun s(&under);
                simulate_equalizerapo_install(e, chosen, true);
            }
            kills_json += "," + check_kills(e, under, "install", [&] { take_back_from_equalizerapo(e, chosen); }, &k4);
            kills_json += "," + check_kills(e, under, "uninstall", [&] { uninstall_isoapo(e); }, &k5);
            more(k1 && k2 && k3 && k4 && k5, "every kill recovered");
            if (replace_eapo && read_slots(e).equalizerapo) {
                bool k6 = false;
                kills_json += "," + check_kills(e, DryRunRegistry(), "install", [&] { install_here(true); }, &k6);
                more(k6, "every kill of a replacing install recovered");
            }

            // A rollback step that fails is reported, and so is a failure that is
            // not a RegistryException.
            {
                DryRunRegistry f = prepared;
                ScopedDryRun s(&f);
                const Snapshot before = snapshot_endpoint(e);
                const size_t at = f.operations().size() + 4;   // the first write after the journal's four
                f.fail_at(at);
                f.fail_at(at);   // and the rollback's first write, which takes the same index
                OperationLog log;
                std::string rollback;
                const std::string failed = with_rollback(e, before, &log, &rollback, "install", [&] { install_here(false); });
                more(!failed.empty() && rollback.find("\"rolled_back\":false") != std::string::npos &&
                         rollback.find("\"rollback_failed\":[{") != std::string::npos &&
                         RegistryHelper::keyExists(journal_key(e)),
                     "a failed rollback step is reported and the journal kept");
            }
            {
                DryRunRegistry f = prepared;
                ScopedDryRun s(&f);
                const Snapshot before = snapshot_endpoint(e);
                OperationLog log;
                std::string rollback, failed;
                try {
                    failed = with_rollback(e, before, &log, &rollback, "install", [&] {
                        install_here(false);
                        throw std::runtime_error("injected");
                    });
                } catch (...) {
                    failed = "escaped";
                }
                more(failed == "injected" && rollback.find("\"rolled_back\":true") != std::string::npos &&
                         snapshot_endpoint(e) == before && !RegistryHelper::keyExists(kPending),
                     "a non-registry exception is rolled back");
            }
        }

        // FxProperties missing: the endpoint key refuses Administrators subkey
        // creation, so upstream's install takes ownership first.
        {
            DryRunRegistry m;
            ScopedDryRun s(&m);
            if (RegistryHelper::keyExists(fx_key)) RegistryHelper::deleteKey(fx_key);
            install_here(false);
            bool took = false, granted = false;
            for (const RegistryOperation& o : m.operations()) {
                took |= o.operation == L"take ownership for Administrators" && _wcsicmp(o.key.c_str(), e.key.c_str()) == 0;
                granted |= o.operation == L"grant Administrators KEY_ALL_ACCESS" && _wcsicmp(o.key.c_str(), e.key.c_str()) == 0;
            }
            more(took && granted, "missing FxProperties: the dry run shows upstream taking ownership");
        }

        // Processing-mode lists: absent before install, their creation is rolled
        // back; a driver's lists that differ from the registry's are put back
        // exactly, read through the dry run.
        {
            DryRunRegistry a;
            ScopedDryRun s(&a);
            for (const SlotName& m : kModeSlots) {
                if (RegistryHelper::valueExists(fx_key, m.value)) RegistryHelper::deleteValue(fx_key, m.value);
            }
            const Snapshot before = snapshot_endpoint(e);
            install_here(false);
            const Snapshot installed_state = snapshot_endpoint(e);
            bool created = false;
            for (const SlotName& m : kModeSlots) created |= RegistryHelper::valueExists(fx_key, m.value);
            DryRunRegistry u = a;
            {
                ScopedDryRun us(&u);
                uninstall_isoapo(e);
                bool none = true;
                for (const SlotName& m : kModeSlots) none &= !RegistryHelper::valueExists(fx_key, m.value);
                more(none, "absent mode lists: uninstall removes the ones install created");
            }
            more(roll_back(before).empty(), "absent mode lists: rollback steps");
            bool none = true;
            for (const SlotName& m : kModeSlots) none &= !RegistryHelper::valueExists(fx_key, m.value);
            // LFX_GFX writes no mode list.
            more(created == (chosen != DeviceAPOInfo::INSTALL_LFX_GFX) && none && snapshot_endpoint(e) == before &&
                     installed_state != before,
                 "absent mode lists: rollback of the install removes them");
        }
        {
            DryRunRegistry b;
            ScopedDryRun s(&b);
            const std::vector<std::wstring> lists = {L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}", L"{9E90EA20-B493-4FD1-A1A8-7E1361A956CF}"};
            for (const SlotName& m : kModeSlots) RegistryHelper::writeMultiValue(fx_key, m.value, lists);
            const Snapshot before = snapshot_endpoint(e);
            for (const SlotName& m : kModeSlots) RegistryHelper::deleteValue(fx_key, m.value);
            roll_back(before);
            bool exact = true;
            for (const SlotName& m : kModeSlots) {
                std::vector<std::wstring> read;
                exact &= b.readMultiValue(fx_key, m.value, &read) && read == lists;
            }
            more(exact, "mode lists: rollback restores their exact contents");
        }
    });
    more(more_error.empty(), "threw: " + more_error);

    const bool unregistered = sim && sim->eapo_unregistered;
    const bool restored = unregistered
                              ? expected_after == after_direct && extras_before == extras_after_direct
                              : expected_after == after_direct && extras_before == extras_after_direct &&
                                    before_again == after && extras_after_update == extras_after;
    // Replacing Equalizer APO takes its record over; nothing of it may be left.
    const bool record_taken_over = !replace_eapo || !had_eapo_record || !eapo_record_left;
    // With Equalizer APO gone, nothing may name its classes after the uninstall.
    const bool no_dangling_eapo =
        !unregistered || std::none_of(after_direct.begin(), after_direct.end(), [](const std::wstring& c) {
            return std::string(classify(c)).rfind("equalizerapo", 0) == 0;
        });
    const auto holds_eapo = [&](const std::vector<std::wstring>& v) {
        return std::any_of(v.begin(), v.end(), [](const std::wstring& c) { return is_equalizerapo(c); });
    };
    // With a simulation: each vendor APO Equalizer APO hosted runs exactly once
    // after the install (in a slot, or hosted by IsoAPO); one it did not host
    // runs nowhere.
    bool handed_over = true;
    std::string placements = "{";
    if (sim) {
        for (const std::wstring* vendor : {&sim->pre, &sim->post}) {
            if (vendor->empty()) continue;
            int count = same_clsid(child_after_install, *vendor) ? 1 : 0;
            for (const std::wstring& v : installed) count += same_clsid(v, *vendor) ? 1 : 0;
            handed_over &= count == (sim->hosted ? 1 : 0);
            placements += std::string(placements.size() > 1 ? "," : "") + quote(*vendor) + ":" + std::to_string(count);
        }
    }
    placements += "}";
    const bool ok = restored && holds_isoapo(installed) && reloaded_installed &&
                    (unregistered || (detach_seen && !holds_isoapo(detached) && holds_isoapo(repaired) &&
                                      std::find(repaired.begin(), repaired.end(), post) - repaired.begin() ==
                                          std::find(installed.begin(), installed.end(), post) - installed.begin())) &&
                    (!replace_eapo || !holds_eapo(installed)) && handed_over && no_dangling_eapo &&
                    record_taken_over && survives_missing_fx_properties && record_only_uninstall_restores &&
                    rollback_restores && more.ok();
    std::printf("{\"command\":\"roundtrip\",\"ok\":%s,\"mode\":%s,\"replace_equalizerapo\":%s,"
                "\"simulated\":%s,\"isoapo_child_after_install\":%s,\"vendor_placements\":%s,\"handed_over\":%s,"
                "\"before\":%s,\"after_install\":%s,\"after_uninstall\":%s,"
                "\"reload_sees_install\":%s,\"after_simulated_driver_update\":%s,\"detach_seen\":%s,"
                "\"after_repair\":%s,\"after_repair_and_uninstall\":%s,\"expected_after_uninstall\":%s,\"restored\":%s,"
                "\"equalizerapo_record_taken_over\":%s,\"extras_before\":%s,\"extras_after_uninstall\":%s,"
                "\"extras_after_simulated_driver_update\":%s,\"extras_after_repair_and_uninstall\":%s,"
                "\"survives_missing_fx_properties\":%s,\"record_only_uninstall_restores\":%s,"
                "\"rollback_restores\":%s,\"no_dangling_equalizerapo\":%s,\"failed\":%s,\"device_selector_retick\":[%s],"
                "\"kills\":[%s],\"enable_enhancements\":%s,\"operations\":%s}\n",
                boolean(ok), quote(mode_name(chosen)).c_str(), boolean(replace_eapo),
                sim ? (sim->eapo_unregistered ? "\"hosted, then Equalizer APO uninstalled\""
                           : sim->hosted          ? "\"hosted\""
                                                  : "\"not hosted\"")
                    : "null",
                quote(child_after_install).c_str(), placements.c_str(), boolean(handed_over), slots_json(before).c_str(),
                slots_json(installed).c_str(), slots_json(after_direct).c_str(), boolean(reloaded_installed),
                slots_json(detached).c_str(), boolean(detach_seen), slots_json(repaired).c_str(),
                slots_json(after).c_str(), slots_json(expected_after).c_str(), boolean(restored),
                boolean(record_taken_over), extras_json(extras_before).c_str(), extras_json(extras_after_direct).c_str(),
                extras_json(extras_after_update).c_str(), extras_json(extras_after).c_str(),
                boolean(survives_missing_fx_properties), boolean(record_only_uninstall_restores),
                boolean(rollback_restores), boolean(no_dangling_eapo), string_array(more.failed).c_str(),
                retick_json.c_str(), kills_json.c_str(), enhancements_json.c_str(), operations_json(dry.operations()).c_str());
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Speaker layouts (windows/devices/speaker_layout.h): the Speakers view's picker.

std::string hresult_hex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(hr));
    return buf;
}

std::string format_json(const isotone::devices::DeviceFormat& f) {
    if (!f.present) return "{\"present\":false,\"error\":" + quote(f.error) + "}";
    char mask[16];
    std::snprintf(mask, sizeof(mask), "0x%lx", static_cast<unsigned long>(f.channel_mask));
    const char* kind = f.sample_format == isotone::devices::SampleFormat::pcm          ? "pcm"
                       : f.sample_format == isotone::devices::SampleFormat::ieee_float ? "float"
                                                                                       : "other";
    return "{\"present\":true,\"channels\":" + std::to_string(f.channels) + ",\"sample_rate\":" +
           std::to_string(f.sample_rate) + ",\"bits\":" + std::to_string(f.bits_per_sample) + ",\"valid_bits\":" +
           std::to_string(f.valid_bits) + ",\"sample_format\":\"" + kind + "\",\"channel_mask\":" +
           quote(std::string(mask)) + ",\"mask_defaulted\":" + boolean(f.mask_defaulted) + "}";
}

isotone::devices::DeviceFormat extensible_format(const WAVEFORMATEXTENSIBLE& f) {
    return isotone::devices::parse_device_format(reinterpret_cast<const uint8_t*>(&f), sizeof(f));
}

// What the library's documented failures mean, for the reason field.
std::string layout_failure(HRESULT hr, const std::string& detail) {
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_READY)) return "the endpoint is not active";
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return "no such render endpoint";
    if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) return "the endpoint's current format cannot be the base of a layout";
    if (hr == isotone::devices::kCurrentFormatRefused)
        return "the output refuses its own format in exclusive mode, so its layouts cannot be told";
    if (!detail.empty()) return detail;
    return "failed with " + hresult_hex(hr);
}

int cmd_layouts(const Endpoint& e) {
    if (e.input) return fail("layouts", "capture endpoints have no speaker layout here");
    std::vector<isotone::devices::SpeakerLayout> supported;
    const HRESULT hr = isotone::devices::supported_speaker_layouts(e.guid, &supported);
    if (FAILED(hr)) return fail("layouts", layout_failure(hr, ""), ",\"hresult\":" + quote(hresult_hex(hr)));
    isotone::devices::Endpoint info;
    const HRESULT read = isotone::devices::read_render_endpoint(e.guid, &info);
    if (FAILED(read)) return fail("layouts", layout_failure(read, ""), ",\"hresult\":" + quote(hresult_hex(read)));
    isotone::devices::SpeakerLayout current{};
    const bool known = isotone::devices::current_speaker_layout(info.format, &current);
    std::vector<std::string> names;
    for (isotone::devices::SpeakerLayout l : supported) names.push_back(isotone::devices::speaker_layout_spec(l)->name);
    std::printf("{\"command\":\"layouts\",\"ok\":true,\"guid\":%s,\"format\":%s,\"current\":%s,\"supported\":%s}\n",
                quote(e.guid).c_str(), format_json(info.format).c_str(),
                known ? quote(std::string(isotone::devices::speaker_layout_spec(current)->name)).c_str() : "null",
                string_array(names).c_str());
    return 0;
}

// --dry-run makes every check set_speaker_layout makes (check_speaker_layout)
// and sets nothing.
int cmd_set_layout(const Endpoint& e, isotone::devices::SpeakerLayout layout, bool dry_run) {
    if (e.input) return fail("set-layout", "capture endpoints have no speaker layout here");
    isotone::devices::LayoutChange change;
    const HRESULT hr = dry_run ? isotone::devices::check_speaker_layout(e.guid, layout, &change)
                               : isotone::devices::set_speaker_layout(e.guid, layout, &change);
    const bool built = change.requested.endpoint.Format.nChannels != 0;
    const std::string requested = built ? "{\"endpoint\":" + format_json(extensible_format(change.requested.endpoint)) +
                                              ",\"mix\":" + format_json(extensible_format(change.requested.mix)) + "}"
                                        : "null";
    const auto maybe = [](const isotone::devices::DeviceFormat& f, bool filled) {
        return filled ? format_json(f) : std::string("null");
    };
    const std::string facts =
        std::string(",\"dry_run\":") + boolean(dry_run) + ",\"guid\":" + quote(e.guid) + ",\"layout\":" +
        quote(std::string(isotone::devices::speaker_layout_spec(layout)->name)) + ",\"hresult\":" +
        quote(hresult_hex(hr)) + ",\"device_id\":" + quote(change.device_id) + ",\"set_called\":" +
        boolean(change.set_called) + ",\"before\":" + maybe(change.before, !change.device_id.empty()) +
        ",\"policy_before\":" + maybe(change.policy_before, change.policy_before.present) + ",\"mix_before\":" +
        maybe(change.mix_before, change.mix_before.present) + ",\"requested\":" + requested + ",\"after\":" +
        maybe(change.after, change.set_called) + ",\"mix_after\":" + maybe(change.mix_after, change.set_called) +
        ",\"property_after\":" + maybe(change.property_after, change.set_called);
    if (FAILED(hr)) return fail("set-layout", layout_failure(hr, change.error), facts);
    std::printf("{\"command\":\"set-layout\",\"ok\":true%s}\n", facts.c_str());
    return 0;
}

int cmd_test(const Endpoint& e) {
    DeviceAPOInfo info;
    try {
        if (!info.load(e.guid)) return fail("test", "endpoint is not present");
        if (info.isDisabled() || info.isUnplugged())
            return fail("test", "endpoint is disabled or unplugged; upstream skips the test for those");
        info.testAPOInstallation();
    } catch (RegistryException& ex) {
        return fail("test", utf8(ex.getMessage()));
    } catch (DeviceException& ex) {
        return fail("test", utf8(ex.getMessage()));
    }
    std::printf("{\"command\":\"test\",\"ok\":true,\"guid\":%s,\"audio_client_initialized\":true}\n",
                quote(e.guid).c_str());
    return 0;
}

// --output: the JSON goes to a file the caller names and reads once the process
// has exited. An elevated run started with ShellExecuteEx "runas" cannot have
// its stdout redirected to its unelevated caller. The caller chose the path, so
// the file is created new (never an existing file, and so never a link or hard
// link someone planted), and must end up exactly at the path given: a junction
// or symbolic link on the way would let the caller steer an elevated process
// into creating a file where the caller may not. A file that fails the check is
// deleted again. Returns empty, or why the path cannot be used.
std::string redirect_output(const std::wstring& path) {
    if (PathIsRelativeW(path.c_str())) return "--output needs an absolute path";
    if (path.size() > 2 && path.find(L':', 2) != std::wstring::npos) return "--output may not name an alternate data stream";
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(path.c_str(), n, full.data(), nullptr);
    full.resize(n);
    const size_t slash = full.rfind(L'\\');
    if (n == 0 || slash == std::wstring::npos || slash + 1 == full.size()) return "--output does not name a file";
    const std::wstring parent = full.substr(0, slash + 1), name = full.substr(slash + 1);
    // The long form of the directory, so an 8.3 name is not taken for a link.
    n = GetLongPathNameW(parent.c_str(), nullptr, 0);
    if (n == 0) return "the --output directory does not exist";
    std::wstring long_parent(n, L'\0');
    n = GetLongPathNameW(parent.c_str(), long_parent.data(), n);
    long_parent.resize(n);
    if (long_parent.empty() || long_parent.back() != L'\\') long_parent += L'\\';
    const std::wstring expected = long_parent + name;

    const HANDLE file = CreateFileW(full.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
                   ? "the --output file already exists"
                   : "cannot create the --output file (error " + std::to_string(error) + ")";
    }
    std::wstring final_path(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file, final_path.data(), static_cast<DWORD>(final_path.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    final_path.resize(length < final_path.size() ? length : 0);
    if (final_path.rfind(L"\\\\?\\UNC\\", 0) == 0) final_path = L"\\\\" + final_path.substr(8);
    else if (final_path.rfind(L"\\\\?\\", 0) == 0) final_path = final_path.substr(4);
    BY_HANDLE_FILE_INFORMATION info{};
    const bool one_link = GetFileInformationByHandle(file, &info) && info.nNumberOfLinks == 1;
    if (final_path.empty() || _wcsicmp(final_path.c_str(), expected.c_str()) != 0 || !one_link) {
        FILE_DISPOSITION_INFO dispose{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &dispose, sizeof(dispose));
        CloseHandle(file);
        return "the --output path goes through a link or reparse point; name a plain path";
    }
    const int fd = _open_osfhandle(static_cast<intptr_t>(reinterpret_cast<INT_PTR>(file)), _O_WRONLY | _O_TEXT);
    if (fd < 0) {
        CloseHandle(file);
        return "cannot use the --output file";
    }
    std::fflush(stdout);
    const bool redirected = _dup2(fd, _fileno(stdout)) == 0;
    _close(fd);
    return redirected ? "" : "cannot use the --output file";
}

// Serialises the registry-changing commands of every elevated devicetool run on
// the machine: two runs interleaving snapshots, writes and rollbacks on one
// endpoint could each put back a state the other had half written.
//
// The mutex is in a private namespace whose boundary requires the
// Administrators SID, rather than under Global\: any logged-on user can create
// a Global\ name first and hold it, blocking every install, while only a process
// whose token has Administrators enabled (an elevated run) can create or open
// this namespace. The namespace and the mutex grant only Administrators and
// SYSTEM. Each run keeps its namespace handle until it exits, so the namespace,
// and the mutex in it, stay findable while any run is alive.
class MachineLock {
public:
    // Empty once held, or why not; *busy when another run held it for `timeout_ms`.
    std::string acquire(DWORD timeout_ms, bool* busy, const std::wstring& boundary_sid = L"S-1-5-32-544") {
        *busy = false;
        PSID sid = nullptr;
        if (!ConvertStringSidToSidW(boundary_sid.c_str(), &sid)) return "lock: bad boundary SID";
        boundary_ = CreateBoundaryDescriptorW(L"Isotone", 0);
        const bool added = boundary_ != nullptr && AddSIDToBoundaryDescriptor(&boundary_, sid);
        LocalFree(sid);
        if (!added) return "lock: cannot create the boundary descriptor (error " + std::to_string(GetLastError()) + ")";
        const std::wstring sddl = L"D:P(A;;GA;;;" + boundary_sid + L")(A;;GA;;;SY)";
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
            return "lock: cannot build the security descriptor";
        SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
        DWORD error = 0;
        for (int attempt = 0; attempt < 20 && namespace_ == nullptr; ++attempt) {
            namespace_ = CreatePrivateNamespaceW(&sa, boundary_, L"Isotone.devicetool");
            if (namespace_ != nullptr) break;
            error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) break;
            // Exists: open it. If the last run holding it exited in between, the
            // open fails and the next attempt creates it.
            namespace_ = OpenPrivateNamespaceW(boundary_, L"Isotone.devicetool");
            if (namespace_ == nullptr) error = GetLastError();
        }
        if (namespace_ == nullptr) {
            LocalFree(sd);
            return "lock: cannot create or open the private namespace (error " + std::to_string(error) + ")";
        }
        mutex_ = CreateMutexW(&sa, FALSE, L"Isotone.devicetool\\registry");
        error = GetLastError();
        LocalFree(sd);
        if (mutex_ == nullptr) return "lock: cannot create the mutex (error " + std::to_string(error) + ")";
        const DWORD waited = WaitForSingleObject(mutex_, timeout_ms);
        // Abandoned: a run holding it was killed. Its journal, not the lock,
        // says what it left, and this run's command undoes that first.
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
            held_ = true;
            return "";
        }
        if (waited == WAIT_TIMEOUT) {
            *busy = true;
            return "another devicetool run held the machine lock for " + std::to_string(timeout_ms) + " ms";
        }
        return "lock: wait failed (error " + std::to_string(GetLastError()) + ")";
    }
    ~MachineLock() {
        if (held_) ReleaseMutex(mutex_);
        if (mutex_ != nullptr) CloseHandle(mutex_);
        if (namespace_ != nullptr) ClosePrivateNamespace(namespace_, 0);
        if (boundary_ != nullptr) DeleteBoundaryDescriptor(boundary_);
    }

private:
    HANDLE boundary_ = nullptr;
    HANDLE namespace_ = nullptr;
    HANDLE mutex_ = nullptr;
    bool held_ = false;
};

const DWORD kLockTimeoutMs = 60000;

// ---------------------------------------------------------------------------
// serve: one elevated process for the life of the UI, so Windows asks for
// approval once (the owner's decision, 2026-09-14).
//
// The UI creates the pipe \\.\pipe\<name> (its first instance, open to the UI's
// user and SYSTEM only) and starts `serve --pipe <name> --parent <its pid>`
// elevated (session.h). serve connects, refuses unless the pipe's server is
// --parent, then answers one request at a time until the pipe breaks, which is
// the UI closing it or exiting:
//   request:  the command and its arguments, UTF-8, separated by U+001F, ending in \n
//   response: {"exit":<code>,"result":<the command's JSON object, or null>}\n
// The command runs as a child isotone-devicetool with the same arguments, which
// inherits the elevated token, so every check, journal, lock and exit code is
// the command's own. Its stdout comes back through an anonymous pipe rather than
// an --output file: a file in the user's temp directory could be swapped or
// linked by an unelevated process between the child writing it and serve
// reading and deleting it; a pipe has no path.

const char* const kServedCommands[] = {"list", "status", "test", "install", "uninstall", "repair",
                                       "enable-enhancements", "restart-audio", "layouts", "set-layout"};
const size_t kMaxRequest = 65536;

// One argument as CommandLineToArgvW and the CRT read it back: quoted, with the
// backslashes before a quote, or before the closing quote, doubled.
std::wstring quote_argument(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

// Why the request cannot run, or empty.
std::string refuse_request(const std::vector<std::string>& fields) {
    const std::string& command = fields[0];
    if (std::find(std::begin(kServedCommands), std::end(kServedCommands), command) == std::end(kServedCommands))
        return "serve runs list, status, test, install, uninstall, repair, enable-enhancements and restart-audio, not " +
               (command.empty() ? std::string("an empty command") : command);
    if (std::find(fields.begin(), fields.end(), "--output") != fields.end())
        return "serve returns the output itself; a request takes no --output";
    return "";
}

struct ChildRun {
    std::string error;   // empty when the child ran
    DWORD exit = 0;
    std::string output;
};

ChildRun run_child(const std::vector<std::wstring>& args) {
    ChildRun r;
    std::wstring self(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (length == 0 || length >= self.size()) {
        r.error = "GetModuleFileName failed (error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    self.resize(length);
    std::wstring command_line = quote_argument(self);
    for (const std::wstring& a : args) command_line += L" " + quote_argument(a);

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) {
        r.error = "CreatePipe failed (error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write;   // no stdin, and no stderr: usage() text is not part of the result
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(self.c_str(), command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    const DWORD start_error = GetLastError();
    CloseHandle(write);   // the child's copy is the only writer left, so the read ends when it exits
    if (!started) {
        CloseHandle(read);
        r.error = "CreateProcess failed (error " + std::to_string(start_error) + ")";
        return r;
    }
    char buffer[4096];
    DWORD n = 0;
    while (ReadFile(read, buffer, sizeof(buffer), &n, nullptr) && n > 0) r.output.append(buffer, n);
    CloseHandle(read);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &r.exit);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return r;
}

int cmd_serve(const std::string& pipe_name, const std::string& parent_text) {
    // A name only: no separator, so \\.\pipe\ cannot be left through "..".
    if (pipe_name.empty() || pipe_name.size() > 200 || pipe_name.find_first_of("\\/") != std::string::npos ||
        pipe_name == "." || pipe_name == "..")
        return usage("serve", "--pipe is a pipe name: not . or .., and without \\ or /");
    if (parent_text.empty() || parent_text.size() > 10 ||
        !std::all_of(parent_text.begin(), parent_text.end(), [](unsigned char c) { return std::isdigit(c) != 0; }) ||
        std::stoull(parent_text) == 0 || std::stoull(parent_text) > 0xFFFFFFFFull)
        return usage("serve", "--parent is a process ID");
    const DWORD parent = static_cast<DWORD>(std::stoull(parent_text));

    // Identification only: the UI, which runs unelevated, may learn who connected
    // but cannot impersonate this elevated process.
    const std::wstring path = L"\\\\.\\pipe\\" + wide(pipe_name);
    HANDLE pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return fail("serve", "cannot open " + utf8(path) + " (error " + std::to_string(GetLastError()) + ")");
    ULONG server = 0;
    if (!GetNamedPipeServerProcessId(pipe, &server) || server != parent) {
        CloseHandle(pipe);
        return fail("serve", "the pipe's server is process " + std::to_string(server) + ", not --parent " + parent_text);
    }

    size_t requests = 0;
    std::string pending;
    for (;;) {
        size_t newline;
        while ((newline = pending.find('\n')) == std::string::npos) {
            if (pending.size() > kMaxRequest) {
                CloseHandle(pipe);
                return fail("serve", "a request is longer than " + std::to_string(kMaxRequest) + " bytes");
            }
            char buffer[4096];
            DWORD n = 0;
            if (!ReadFile(pipe, buffer, sizeof(buffer), &n, nullptr)) {
                const DWORD error = GetLastError();
                CloseHandle(pipe);
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                    std::printf("{\"command\":\"serve\",\"ok\":true,\"requests\":%zu}\n", requests);
                    return 0;
                }
                return fail("serve", "reading the pipe failed (error " + std::to_string(error) + ")");
            }
            pending.append(buffer, n);
        }
        const std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        ++requests;

        std::vector<std::string> fields;
        for (size_t start = 0;;) {
            const size_t separator = line.find('\x1f', start);
            fields.push_back(line.substr(start, separator == std::string::npos ? std::string::npos : separator - start));
            if (separator == std::string::npos) break;
            start = separator + 1;
        }
        std::vector<std::wstring> args;
        std::string refusal = refuse_request(fields);
        for (const std::string& f : fields) {
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, f.data(), static_cast<int>(f.size()), nullptr, 0);
            if (!f.empty() && length == 0) refusal = "a request is not UTF-8";
            args.push_back(wide(f));
        }

        std::string response;
        if (!refusal.empty()) {
            response = "{\"exit\":" + std::to_string(kExitBadArguments) + ",\"result\":" +
                       failure_json("bad_arguments", fields[0], refusal) + "}\n";
        } else {
            ChildRun child = run_child(args);
            if (!child.error.empty()) {
                CloseHandle(pipe);
                return fail("serve", child.error);
            }
            // The child prints one JSON object on one line; the CRT's text mode ends it in \r\n.
            while (!child.output.empty() && std::isspace(static_cast<unsigned char>(child.output.back()))) child.output.pop_back();
            const bool object = !child.output.empty() && child.output.front() == '{' && child.output.back() == '}' &&
                                child.output.find_first_of("\r\n") == std::string::npos;
            response = "{\"exit\":" + std::to_string(child.exit) + ",\"result\":" + (object ? child.output : "null") + "}\n";
        }
        for (size_t written = 0; written < response.size();) {
            DWORD n = 0;
            if (!WriteFile(pipe, response.data() + written, static_cast<DWORD>(response.size() - written), &n, nullptr)) {
                const DWORD error = GetLastError();
                CloseHandle(pipe);
                if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED) {
                    std::printf("{\"command\":\"serve\",\"ok\":true,\"requests\":%zu}\n", requests);
                    return 0;
                }
                return fail("serve", "writing the pipe failed (error " + std::to_string(error) + ")");
            }
            written += n;
        }
    }
}

int dispatch(int argc, wchar_t** wargv) {
    std::vector<std::string> argv_utf8;
    for (int i = 0; i < argc; ++i) argv_utf8.push_back(utf8(std::wstring(wargv[i])));

    // --output first, so every later error goes where the caller reads.
    for (int i = 1; i < argc; ++i) {
        if (argv_utf8[i] != "--output") continue;
        if (i + 1 >= argc) return usage("", "--output needs a path");
        const std::string error = redirect_output(wargv[i + 1]);
        if (!error.empty()) return usage("", error);
        break;
    }

    std::vector<std::string> args, flags;
    bool dry_run = false, replace_eapo = false, remove_data = false;
    std::optional<std::string> mode_text;
    std::optional<std::string> simulation_text;
    std::string pipe_text, parent_text;
    std::optional<std::string> layout_text;
    std::optional<std::string> dll_text;
    int outputs = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string& a = argv_utf8[i];
        const bool takes_value = a == "--mode" || a == "--simulate-equalizerapo" || a == "--output" || a == "--pipe" || a == "--parent" || a == "--layout" ||
                               a == "--dll";
        if (takes_value && i + 1 >= argc) return usage("", a + " needs a value");
        if (a == "--dry-run") dry_run = true;
        else if (a == "--replace-equalizerapo") replace_eapo = true;
        else if (a == "--mode") mode_text = argv_utf8[++i];
        else if (a == "--simulate-equalizerapo") simulation_text = argv_utf8[++i];
        else if (a == "--pipe") pipe_text = argv_utf8[++i];
        else if (a == "--parent") parent_text = argv_utf8[++i];
        else if (a == "--layout") layout_text = argv_utf8[++i];
        else if (a == "--dll") dll_text = argv_utf8[++i];
        else if (a == "--remove-data") remove_data = true;
        else if (a == "--output") { ++i; ++outputs; continue; }
        else if (a.rfind("--", 0) == 0) return usage("", "unknown flag " + a);
        else { args.push_back(a); continue; }
        flags.push_back(a);
    }
    if (args.empty()) return usage("", "no command");
    const std::string& command = args[0];
    if (outputs > 1) return usage(command, "--output given more than once");

    // The flags each command takes; anything else is refused, not ignored.
    const std::map<std::string, std::vector<std::string>> accepted = {
        {"list", {}},
        {"status", {}},
        {"test", {}},
        {"install", {"--mode", "--replace-equalizerapo", "--dry-run"}},
        {"uninstall", {"--dry-run"}},
        {"repair", {"--mode", "--dry-run"}},
        {"roundtrip", {"--mode", "--replace-equalizerapo", "--simulate-equalizerapo"}},
        {"machine-install", {"--dll", "--dry-run"}},
        {"machine-uninstall", {"--remove-data", "--dry-run"}},
        {"enable-enhancements", {"--dry-run"}},
        {"restart-audio", {"--dry-run"}},
        {"layouts", {}},
        {"set-layout", {"--layout", "--dry-run"}},
        {"serve", {"--pipe", "--parent"}},
    };
    const auto found_command = accepted.find(command);
    if (found_command == accepted.end()) return usage(command, "unknown command " + command);
    for (const std::string& f : flags) {
        if (std::count(flags.begin(), flags.end(), f) > 1) return usage(command, f + " given more than once");
        if (std::find(found_command->second.begin(), found_command->second.end(), f) == found_command->second.end())
            return usage(command, command + " does not take " + f);
    }
    // repair: one endpoint, or none for every render endpoint.
    const bool takes_endpoint = command != "list" && command != "restart-audio" && command != "serve" &&
                                command != "machine-install" && command != "machine-uninstall" &&
                                (command != "repair" || args.size() == 2);
    if (args.size() != (takes_endpoint ? 2u : 1u))
        return usage(command, command == "repair"      ? "repair takes one endpoint or none"
                              : takes_endpoint ? command + " takes one endpoint"
                                               : command + " takes no endpoint");
    if (command == "serve") {
        if (pipe_text.empty() || parent_text.empty()) return usage(command, "serve needs --pipe and --parent");
        return cmd_serve(pipe_text, parent_text);
    }

    isotone::devices::SpeakerLayout layout{};
    if (command == "set-layout") {
        if (!layout_text) return usage(command, "set-layout needs --layout");
        if (!isotone::devices::parse_speaker_layout(*layout_text, &layout))
            return usage(command, "--layout is stereo, 2.1, 5.1 or 7.1");
    }

    std::optional<DeviceAPOInfo::InstallMode> mode;
    if (mode_text) {
        DeviceAPOInfo::InstallMode parsed;
        if (!parse_mode(*mode_text, &parsed)) return usage(command, "--mode is mfx, efx or gfx");
        mode = parsed;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return fail(command, "CoInitializeEx failed");
    }

    // --simulate-equalizerapo PRE,POST[,unhosted][,unregistered][,deleted|fallback]:
    // only for the dry-run roundtrip with --replace-equalizerapo. Either CLSID
    // may be empty.
    std::optional<Simulation> sim;
    if (simulation_text) {
        if (!replace_eapo) return usage(command, "--simulate-equalizerapo needs --replace-equalizerapo");
        std::vector<std::wstring> parts;
        const std::wstring text(simulation_text->begin(), simulation_text->end());
        size_t start = 0;
        while (start <= text.size()) {
            const size_t comma = text.find(L',', start);
            parts.push_back(text.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
        if (parts.size() < 2) return usage(command, "--simulate-equalizerapo needs PRE,POST");
        Simulation s;
        s.pre = parts[0];
        s.post = parts[1];
        for (size_t k = 2; k < parts.size(); ++k) {
            if (parts[k] == L"unhosted") s.hosted = false;
            else if (parts[k] == L"unregistered") s.eapo_unregistered = true;
            else if (parts[k] == L"deleted") s.deleted_slots = true;
            else if (parts[k] == L"fallback") s.fallback = true;
            else return usage(command, "unknown --simulate-equalizerapo option " + utf8(parts[k]));
        }
        if ((s.eapo_unregistered && !s.hosted) || (s.deleted_slots && s.fallback))
            return usage(command, "--simulate-equalizerapo options conflict");
        GUID g;
        if ((s.pre.empty() && s.post.empty()) || (!s.pre.empty() && FAILED(CLSIDFromString(s.pre.c_str(), &g))) ||
            (!s.post.empty() && FAILED(CLSIDFromString(s.post.c_str(), &g)))) {
            return usage(command, "--simulate-equalizerapo CLSIDs are malformed");
        }
        sim = s;
    }

    Endpoint endpoint;
    if (takes_endpoint) {
        const int found = resolve_endpoint(args[1], &endpoint);
        if (found == 2) return usage(command, "malformed endpoint " + args[1]);
        if (found == 1) return fail(command, "no endpoint with ID " + args[1]);
    }

    // The registry-changing commands, and the audio service restart: an elevated
    // process, one run at a time.
    MachineLock lock;
    if ((command == "install" || command == "uninstall" || command == "repair" || command == "enable-enhancements" ||
         command == "restart-audio" || command == "machine-install" || command == "machine-uninstall") &&
        !dry_run) {
        if (!is_elevated())
            return fail_with(kExitNotElevated, "not_elevated", command, "needs an elevated process; use --dry-run to preview");
        bool busy = false;
        const std::string error = lock.acquire(kLockTimeoutMs, &busy);
        if (busy) return fail_with(kExitBusy, "busy", command, error);
        if (!error.empty()) return fail(command, error);
    }

    if (command == "machine-install") {
        if (!dll_text) return usage(command, "machine-install needs --dll");
        return cmd_machine_install(wide(*dll_text), dry_run);
    }
    if (command == "machine-uninstall") return cmd_machine_uninstall(remove_data, dry_run);
    if (command == "list") return cmd_list();
    if (command == "repair") return cmd_repair(takes_endpoint ? std::optional<Endpoint>(endpoint) : std::nullopt, mode, dry_run);
    if (command == "status") return cmd_status(endpoint);
    if (command == "install") return cmd_install(endpoint, mode, replace_eapo, dry_run);
    if (command == "uninstall") return cmd_uninstall(endpoint, dry_run);
    if (command == "roundtrip") return cmd_roundtrip(endpoint, mode, replace_eapo, sim);
    if (command == "enable-enhancements") return cmd_enable_enhancements(endpoint, dry_run);
    if (command == "restart-audio") return cmd_restart_audio(dry_run);
    if (command == "layouts") return cmd_layouts(endpoint);
    if (command == "set-layout") return cmd_set_layout(endpoint, layout, dry_run);
    return cmd_test(endpoint);
}

}  // namespace

// wmain: arguments arrive as UTF-16 and are turned into UTF-8, so a non-ASCII
// argument cannot put invalid UTF-8 into the JSON output.
int wmain(int argc, wchar_t** wargv) {
    const int code = dispatch(argc, wargv);
    // With --output, the file is complete on disk and closed before the process
    // exits, which is when the caller reads it.
    std::fflush(stdout);
    _commit(_fileno(stdout));
    std::fclose(stdout);
    return code;
}
