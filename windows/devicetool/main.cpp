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
//   isotone-devicetool repair    [--mode mfx|efx|gfx] [--dry-run]
//   isotone-devicetool test      <endpoint>
//
// <endpoint> is an endpoint GUID, with or without braces, or a full device ID
// such as {0.0.0.00000000}.{guid}. One JSON object on stdout. Exit 0 on
// success, 1 on failure or refusal, 2 on bad arguments.
//
// --dry-run runs upstream's install and uninstall logic unchanged with every
// registry write intercepted (RegistryDryRun), prints the writes, and needs no
// elevation. Without it, install/uninstall/repair require an elevated process.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <mmdeviceapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <userenv.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "DeviceAPOInfo.h"
#include "dry_run.h"
#include "helpers/RegistryHelper.h"

using isotone::devicetool::DryRunRegistry;
using isotone::devicetool::OperationLog;
using isotone::devicetool::RegistryOperation;
using isotone::devicetool::ScopedDryRun;
using isotone::devicetool::ScopedLog;

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

int fail(const std::string& command, const std::string& reason, const std::string& extra = "") {
    std::printf("{\"command\":%s,\"ok\":false,\"reason\":%s%s}\n", quote(command).c_str(),
                quote(reason).c_str(), extra.c_str());
    return 1;
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  isotone-devicetool list\n"
                 "  isotone-devicetool status    <endpoint>\n"
                 "  isotone-devicetool install   <endpoint> [--mode mfx|efx|gfx] [--replace-equalizerapo] [--dry-run]\n"
                 "  isotone-devicetool uninstall <endpoint> [--dry-run]\n"
                 "  isotone-devicetool repair    [--mode mfx|efx|gfx] [--dry-run]\n"
                 "  isotone-devicetool test      <endpoint>\n"
                 "  isotone-devicetool roundtrip <endpoint> [--mode mfx|efx|gfx] [--replace-equalizerapo\n"
                 "                               [--simulate-equalizerapo PRE,POST[,unhosted|unregistered]]]\n"
                 "Without --mode: the mode of Equalizer APO's post-mix slot where it is on the endpoint,\n"
                 "otherwise the mode upstream's install would pick.\n");
    return 2;
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
DWORD value_type(const std::wstring& key, const std::wstring& name, bool* present) {
    *present = false;
    if (!RegistryHelper::keyExists(key)) return REG_NONE;
    HKEY h = RegistryHelper::openKey(key, KEY_QUERY_VALUE | KEY_WOW64_64KEY);
    DWORD type = REG_NONE;
    const LSTATUS status = RegQueryValueExW(h, name.c_str(), nullptr, &type, nullptr, nullptr);
    RegCloseKey(h);
    *present = status == ERROR_SUCCESS;
    return type;
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
int resolve_endpoint(const std::string& arg, Endpoint* out) {
    std::string s = arg;
    const size_t open = s.rfind('{');
    if (open != std::string::npos) {
        const size_t close = s.find('}', open);
        if (close == std::string::npos) return 2;
        s = s.substr(open, close - open + 1);
    } else {
        s = "{" + s + "}";
    }
    std::wstring w(s.begin(), s.end());
    GUID guid;
    if (s.size() != 38 || FAILED(CLSIDFromString(w.c_str(), &guid))) return 2;
    for (wchar_t& c : w) c = static_cast<wchar_t>(towlower(c));

    const std::wstring render = std::wstring(kMMDevices) + L"\\Render\\" + w;
    const std::wstring capture = std::wstring(kMMDevices) + L"\\Capture\\" + w;
    if (RegistryHelper::keyExists(render)) {
        *out = {w, false, render};
    } else if (RegistryHelper::keyExists(capture)) {
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
    std::string json;
};

Registration read_registration() {
    Registration r;
    const std::wstring clsid = RegistryHelper::getGuidString(ISOAPO_POST_MIX_GUID);
    r.apo_registered = RegistryHelper::keyExists(std::wstring(kApoRegistration) + L"\\" + clsid);
    try_read_string(std::wstring(kClsid) + L"\\" + clsid + L"\\InprocServer32", L"", &r.dll);
    if (!r.dll.empty()) {
        r.dll_exists = GetFileAttributesW(r.dll.c_str()) != INVALID_FILE_ATTRIBUTES;
        // audiodg hosts the APO as LocalService, which cannot read user
        // profiles; a DLL there fails the whole endpoint with E_ACCESSDENIED.
        std::wstring lower = r.dll;
        for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
        wchar_t profiles[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        std::wstring root = L"c:\\users\\";
        if (GetProfilesDirectoryW(profiles, &n)) {
            root = profiles;
            for (wchar_t& c : root) c = static_cast<wchar_t>(towlower(c));
            if (root.back() != L'\\') root += L'\\';
        }
        r.dll_under_user_profile = lower.rfind(root, 0) == 0;
    }
    r.json = std::string("{\"clsid\":") + quote(clsid) + ",\"apo_registered\":" +
             boolean(r.apo_registered) + ",\"dll\":" + (r.dll.empty() ? "null" : quote(r.dll)) +
             ",\"dll_exists\":" + boolean(r.dll_exists) +
             ",\"dll_under_user_profile\":" + boolean(r.dll_under_user_profile) + "}";
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
    const bool detached = iso.exists && !slots.isoapo;
    const bool unrecorded = slots.isoapo && !iso.exists;
    const char* backend = slots.isoapo && slots.equalizerapo ? "conflict"
                          : slots.isoapo                    ? "native"
                          : slots.equalizerapo              ? "equalizerapo"
                                                            : "none";
    if (slots.isoapo && slots.equalizerapo)
        warnings.push_back("IsoAPO and Equalizer APO are both on this endpoint");
    if (detached)
        warnings.push_back("IsoAPO has an install record but is in no effect slot: detached, probably by a driver update; run repair");
    if (unrecorded)
        warnings.push_back("IsoAPO is in an effect slot with no install record (installed by the retired install.ps1); restore FxProperties from that script's .reg backup, then install with devicetool");
    if (!load_error.empty() && load_error != "not_present")
        warnings.push_back("upstream load failed: " + load_error);

    std::wstring config_path;
    const bool eapo_installed = RegistryHelper::keyExists(EQUALIZERAPO_REGPATH);
    const bool have_config = try_read_string(EQUALIZERAPO_REGPATH, L"ConfigPath", &config_path);

    const Registration reg = read_registration();
    bool protected_audiodg_disabled = false;
    try {
        protected_audiodg_disabled = DeviceAPOInfo::checkProtectedAudioDG(false);
    } catch (RegistryException&) {
    }

    std::string recommended = "null";
    if (load_error.empty() && !info.isInstalled())
        recommended = quote(mode_name(info.getCurrentInstallState().installMode));

    std::printf(
        "{\"command\":\"status\",\"ok\":true,\"guid\":%s,\"id\":%s,\"flow\":\"%s\",\"device\":%s,"
        "\"backend\":\"%s\",\"effect_slots\":%s,\"effect_lists\":%s,\"processing_modes\":%s,"
        "\"isoapo\":{\"in_slots\":%s,\"record\":%s,\"detached\":%s,\"unrecorded\":%s,\"registration\":%s},"
        "\"equalizerapo\":{\"in_slots\":%s,\"installed\":%s,\"config_path\":%s,\"record\":%s},"
        "\"upstream_recommended_mode\":%s,\"protected_audiodg_disabled\":%s,\"warnings\":%s}\n",
        quote(e.guid).c_str(), quote(device_id(e)).c_str(), e.input ? "capture" : "render",
        device.c_str(), backend, slots.json.c_str(), slots.lists_json.c_str(), modes.c_str(),
        boolean(slots.isoapo),
        iso.json.c_str(), boolean(detached), boolean(unrecorded), reg.json.c_str(),
        boolean(slots.equalizerapo), boolean(eapo_installed),
        have_config ? quote(config_path).c_str() : "null", eapo.json.c_str(), recommended.c_str(),
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

// ---------------------------------------------------------------------------
// What upstream's install changes outside the effect slots and its uninstall
// never puts back: it deletes the "disable all enhancements" flag, and adds a
// processing-mode list for each slot it writes. Isotone records both in the
// install record and restores them on uninstall.

const wchar_t* const kDisableEnhancements = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";
const wchar_t* const kRecordEnhancements  = L"Isotone.DisableEnhancements";      // decimal, only if the flag was set
const wchar_t* const kRecordModesAbsent   = L"Isotone.ProcessingModesAbsent";    // mode value names, '|' separated

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
        x.enhancements = RegistryHelper::readDWORDValue(fx, kDisableEnhancements);
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

// ---------------------------------------------------------------------------
// Replacing Equalizer APO

struct Replacement {
    int removed = 0;              // slots left empty
    int restored = 0;             // slots given back the APO Equalizer APO was running there
    std::wstring child;           // what IsoAPO took over from Equalizer APO's post-mix instance
};

// With --replace-equalizerapo, after upstream's install. Equalizer APO's classes
// leave the effect slots, so the endpoint has one EQ, and everything Equalizer
// APO was running keeps running exactly once:
//  - a slot IsoAPO did not take gets back the APO Equalizer APO was hosting
//    there (Equalizer APO's record: the slot's original value, and the same
//    CLSID as its PreMixChild or PostMixChild), or is emptied;
//  - the slot IsoAPO took: if Equalizer APO was there hosting an APO, IsoAPO
//    hosts it now.
// Only APOs Equalizer APO was actually hosting count, so an APO the user had
// switched off in Equalizer APO ("use original APO" unticked) stays off, and a
// stale record for slots Equalizer APO no longer holds does nothing. Upstream's
// install has already recorded every slot's value, and uninstall writes each
// back.
Replacement remove_equalizerapo(const Endpoint& e) {
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring eapo_record = eapo_record_key(e);
    const std::wstring iso_record = iso_record_key(e);
    std::wstring eapo_pre, eapo_post;
    try_read_string(eapo_record, L"PreMixChild", &eapo_pre);
    try_read_string(eapo_record, L"PostMixChild", &eapo_post);
    const auto hosted = [&](const std::wstring& clsid) {
        return is_other_apo(clsid) && (same_clsid(clsid, eapo_pre) || same_clsid(clsid, eapo_post));
    };

    Replacement r;
    for (const SlotName& slot : kEffectSlots) {
        std::wstring clsid;
        if (!try_read_string(fx, slot.value, &clsid)) continue;
        std::wstring original;
        try_read_string(eapo_record, slot.value, &original);
        if (is_equalizerapo(clsid)) {
            if (hosted(original)) {
                RegistryHelper::writeValue(fx, slot.value, original);
                ++r.restored;
            } else {
                RegistryHelper::deleteValue(fx, slot.value);
                ++r.removed;
            }
        } else if (std::string(classify(clsid)) == "isoapo_post_mix") {
            std::wstring replaced;
            try_read_string(iso_record, slot.value, &replaced);
            std::wstring current_child;
            try_read_string(iso_record, L"PostMixChild", &current_child);
            if (is_equalizerapo(replaced) && hosted(original) && current_child.empty()) {
                RegistryHelper::writeValue(iso_record, L"PostMixChild", original);
                r.child = original;
            }
        }
    }
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

// Throws RegistryException.
Replacement install_isoapo(DeviceAPOInfo& info, const Endpoint& e, DeviceAPOInfo::InstallMode mode,
                           bool replace_eapo) {
    const Extras extras = extras_on_endpoint(e);
    select_post_mix_only(&info, mode);
    info.install();
    record_extras(e, extras);
    return replace_eapo ? remove_equalizerapo(e) : Replacement{};
}

struct Uninstalled {
    bool fx_properties_missing = false;
    int equalizerapo_dropped = 0;
};

// Throws RegistryException.
Uninstalled uninstall_isoapo(const Endpoint& e) {
    Uninstalled u;
    const std::wstring fx = e.key + L"\\FxProperties";
    const std::wstring record = iso_record_key(e);
    if (!RegistryHelper::keyExists(fx)) {
        // A driver change removed FxProperties: there is nothing to restore, and
        // upstream's uninstall would fail deleting a key that is not there.
        u.fx_properties_missing = true;
        if (RegistryHelper::keyExists(record)) RegistryHelper::deleteKey(record);
        return u;
    }
    const Extras extras = recorded_extras(e);
    DeviceAPOInfo info;
    if (!info.load(e.guid)) throw RegistryException(L"endpoint is not present");
    info.uninstall();
    restore_extras(e, extras);
    u.equalizerapo_dropped = drop_unregistered_equalizerapo(e);
    return u;
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
                 !reg.dll.empty() && !reg.dll_under_user_profile) + "]";
}

// Every value an install can touch, as it was, so a failed install can be put
// back.
struct Snapshot {
    struct Value {
        std::wstring key, name;
        bool present = false;
        DWORD type = REG_NONE;
        std::wstring text;
        std::vector<std::wstring> multi;
        unsigned long dword = 0;
    };
    std::vector<Value> values;
    bool record_existed = false;
};

Snapshot snapshot_endpoint(const Endpoint& e) {
    Snapshot s;
    const std::wstring fx = e.key + L"\\FxProperties";
    std::vector<const wchar_t*> names;
    for (const SlotName& slot : kEffectSlots) names.push_back(slot.value);
    for (const SlotName& m : kModeSlots) names.push_back(m.value);
    names.push_back(kDisableEnhancements);
    for (const wchar_t* name : names) {
        Snapshot::Value v;
        v.key = fx;
        v.name = name;
        v.type = value_type(fx, name, &v.present);
        try {
            if (v.present && v.type == REG_SZ) v.text = RegistryHelper::readValue(fx, name);
            else if (v.present && v.type == REG_MULTI_SZ) v.multi = RegistryHelper::readMultiValue(fx, name);
            else if (v.present && v.type == REG_DWORD) v.dword = RegistryHelper::readDWORDValue(fx, name);
        } catch (RegistryException&) {
            v.present = false;
        }
        s.values.push_back(v);
    }
    s.record_existed = RegistryHelper::keyExists(iso_record_key(e));
    return s;
}

// Best effort: every value goes back even if one of them cannot.
void roll_back(const Endpoint& e, const Snapshot& s) {
    for (const Snapshot::Value& v : s.values) {
        try {
            bool now = false;
            value_type(v.key, v.name, &now);
            if (!v.present) {
                if (now) RegistryHelper::deleteValue(v.key, v.name);
            } else if (v.type == REG_SZ) {
                RegistryHelper::writeValue(v.key, v.name, v.text);
            } else if (v.type == REG_MULTI_SZ) {
                RegistryHelper::writeMultiValue(v.key, v.name, v.multi);
            } else if (v.type == REG_DWORD) {
                RegistryHelper::writeDWORDValue(v.key, v.name, v.dword);
            }
        } catch (RegistryException&) {
        }
    }
    try {
        if (!s.record_existed && RegistryHelper::keyExists(iso_record_key(e))) {
            RegistryHelper::deleteKey(iso_record_key(e));
        }
    } catch (RegistryException&) {
    }
}

std::string replacement_json(const Replacement& r) {
    return std::string("\"equalizerapo_slots_removed\":") + std::to_string(r.removed) +
           ",\"equalizerapo_slots_restored_to_original\":" + std::to_string(r.restored) +
           ",\"child_from_equalizerapo\":" + (r.child.empty() ? "null" : quote(r.child));
}

int cmd_install(const Endpoint& e, std::optional<DeviceAPOInfo::InstallMode> mode, bool replace_eapo, bool dry_run) {
    if (!dry_run && !is_elevated()) return fail("install", "needs an elevated process; use --dry-run to preview");
    if (e.input) return fail("install", "capture endpoints are not supported: IsoAPO is an output EQ");

    DeviceAPOInfo info;
    try {
        if (!info.load(e.guid)) return fail("install", "endpoint is not present");
    } catch (RegistryException& ex) {
        return fail("install", utf8(ex.getMessage()));
    }

    const Slots slots = read_slots(e);
    const Record record = read_record(kIsoChildApos, e.guid);
    const std::wstring backups = backup_directory();
    if (slots.isoapo && record.exists) {
        if (!(replace_eapo && slots.equalizerapo)) {
            std::printf("{\"command\":\"install\",\"ok\":true,\"dry_run\":%s,\"already_installed\":true,\"operations\":[]}\n",
                        boolean(dry_run));
            return 0;
        }
        // Installed before Equalizer APO was to be replaced: do only that.
        DryRunRegistry dry;
        OperationLog log;
        Replacement replaced;
        try {
            if (dry_run) {
                ScopedDryRun scope(&dry);
                replaced = remove_equalizerapo(e);
            } else {
                ScopedLog scope(&log);
                replaced = remove_equalizerapo(e);
            }
        } catch (RegistryException& ex) {
            return fail("install", utf8(ex.getMessage()), ",\"operations\":" + operations_json(log.operations()));
        }
        std::printf("{\"command\":\"install\",\"ok\":true,\"dry_run\":%s,\"already_installed\":true,%s,\"operations\":%s}\n",
                    boolean(dry_run), replacement_json(replaced).c_str(),
                    operations_json(dry_run ? dry.operations() : log.operations()).c_str());
        return 0;
    }
    if (slots.isoapo)
        return fail("install", "IsoAPO is in an effect slot with no install record (installed by the retired install.ps1); restore the endpoint's FxProperties from that script's .reg backup first");
    if (record.exists)
        return fail("install", "an install record exists but IsoAPO is in no slot (detached); run repair");
    if (slots.equalizerapo && !replace_eapo)
        return fail("install", "Equalizer APO is installed on this endpoint; a device may be on one backend only (pass --replace-equalizerapo to replace it)");

    const Registration reg = read_registration();
    std::vector<std::string> failed;
    const std::string checks = install_checks_json(reg, &failed);
    if (!dry_run && !failed.empty()) return fail("install", "registration checks failed", ",\"checks\":" + checks);

    const DeviceAPOInfo::InstallMode chosen = mode ? *mode : default_mode(info, e);
    select_post_mix_only(&info, chosen);
    const DeviceAPOInfo::InstallState selected = info.getSelectedInstallState();
    const std::wstring child = selected.useOriginalAPOPostMix ? info.getOriginalAPOPostMix() : L"";

    std::vector<std::string> warnings;
    if (!child.empty())
        warnings.push_back("IsoAPO hosts " + utf8(child) + ", the APO this install replaces, and runs it before its own processing");
    warnings.insert(warnings.end(), slots.warnings.begin(), slots.warnings.end());

    DryRunRegistry dry;
    OperationLog log;
    Replacement replaced;
    if (dry_run) {
        try {
            ScopedDryRun scope(&dry);
            replaced = install_isoapo(info, e, chosen, replace_eapo);
        } catch (RegistryException& ex) {
            return fail("install", utf8(ex.getMessage()), ",\"operations\":" + operations_json(dry.operations()));
        }
    } else {
        if (!use_backup_directory(backups)) return fail("install", "cannot use backup directory " + utf8(backups));
        const Snapshot before = snapshot_endpoint(e);
        try {
            ScopedLog scope(&log);
            replaced = install_isoapo(info, e, chosen, replace_eapo);
        } catch (RegistryException& ex) {
            // Leave the endpoint as it was, not half installed with a record
            // that makes every later command refuse.
            OperationLog undo;
            {
                ScopedLog scope(&undo);
                roll_back(e, before);
            }
            return fail("install", utf8(ex.getMessage()),
                        ",\"rolled_back\":true,\"operations\":" + operations_json(log.operations()) +
                            ",\"rollback_operations\":" + operations_json(undo.operations()));
        }
    }

    std::string verified = "null", eapo_left = "null";
    if (!dry_run) {
        const Slots after = read_slots(e);
        verified = boolean(after.isoapo);
        eapo_left = boolean(after.equalizerapo);
    }

    // A dry run whose registration checks fail shows what would be written, but
    // is not ok: the real install would refuse.
    const bool ok = failed.empty();
    std::printf("{\"command\":\"install\",\"ok\":%s,\"dry_run\":%s,%s\"mode\":%s,\"child\":%s,"
                "\"backup_directory\":%s,\"checks\":%s,%s,\"operations\":%s,"
                "\"verified_in_slot\":%s,\"equalizerapo_still_in_slots\":%s,\"warnings\":%s}\n",
                boolean(ok), boolean(dry_run), ok ? "" : "\"reason\":\"registration checks failed; a real install would refuse\",",
                quote(mode_name(chosen)).c_str(),
                child.empty() ? "null" : quote(child).c_str(), quote(backups).c_str(), checks.c_str(),
                replacement_json(replaced).c_str(),
                operations_json(dry_run ? dry.operations() : log.operations()).c_str(),
                verified.c_str(), eapo_left.c_str(), string_array(warnings).c_str());
    return ok ? 0 : 1;
}

int cmd_uninstall(const Endpoint& e, bool dry_run) {
    if (!dry_run && !is_elevated()) return fail("uninstall", "needs an elevated process; use --dry-run to preview");

    const Slots slots = read_slots(e);
    const Record record = read_record(kIsoChildApos, e.guid);
    if (!record.exists) {
        if (slots.isoapo)
            return fail("uninstall", "IsoAPO is in an effect slot but has no install record (the retired install.ps1 installed it), so the original values are unknown here; restore FxProperties from that script's .reg backup");
        std::printf("{\"command\":\"uninstall\",\"ok\":true,\"dry_run\":%s,\"not_installed\":true,\"operations\":[]}\n",
                    boolean(dry_run));
        return 0;
    }

    DryRunRegistry dry;
    OperationLog log;
    Uninstalled u;
    try {
        if (dry_run) {
            ScopedDryRun scope(&dry);
            u = uninstall_isoapo(e);
        } else {
            ScopedLog scope(&log);
            u = uninstall_isoapo(e);
        }
    } catch (RegistryException& ex) {
        return fail("uninstall", utf8(ex.getMessage()), ",\"operations\":" + operations_json(log.operations()));
    }
    std::printf("{\"command\":\"uninstall\",\"ok\":true,\"dry_run\":%s,\"record\":%s,\"fx_properties_missing\":%s,"
                "\"unregistered_equalizerapo_slots\":%d,\"operations\":%s}\n",
                boolean(dry_run), record.json.c_str(), boolean(u.fx_properties_missing), u.equalizerapo_dropped,
                operations_json(dry_run ? dry.operations() : log.operations()).c_str());
    return 0;
}

// Detached: forget the stale record, then install again against the APOs the
// driver now declares. Throws RegistryException.
void reattach(const Endpoint& e, std::optional<DeviceAPOInfo::InstallMode> mode) {
    uninstall_isoapo(e);
    DeviceAPOInfo info;
    if (!info.load(e.guid)) throw RegistryException(L"endpoint is not present");
    install_isoapo(info, e, mode ? *mode : default_mode(info, e), false);
}

int cmd_repair(std::optional<DeviceAPOInfo::InstallMode> mode, bool dry_run) {
    if (!dry_run && !is_elevated()) return fail("repair", "needs an elevated process; use --dry-run to preview");

    std::vector<std::wstring> guids;
    try {
        guids = RegistryHelper::enumSubKeys(std::wstring(kMMDevices) + L"\\Render");
    } catch (RegistryException& ex) {
        return fail("repair", utf8(ex.getMessage()));
    }
    // Upstream's install writes its .reg backup to the working directory.
    const std::wstring backups = backup_directory();
    if (!dry_run && !use_backup_directory(backups)) return fail("repair", "cannot use backup directory " + utf8(backups));

    std::string devices = "[";
    bool first = true, all_ok = true;
    for (const std::wstring& g : guids) {
        Endpoint e{g, false, std::wstring(kMMDevices) + L"\\Render\\" + g};
        const Record record = read_record(kIsoChildApos, g);
        const Slots slots = read_slots(e);
        if (!record.exists || slots.isoapo) continue;

        DryRunRegistry dry;
        OperationLog log;
        std::string error;
        if (slots.equalizerapo) {
            // The one-backend rule install applies: Equalizer APO came back (its
            // Device Selector, or a driver reinstall), and putting IsoAPO next to
            // it would run two EQs.
            error = "Equalizer APO is on this endpoint again; run install --replace-equalizerapo to replace it";
            all_ok = false;
        } else {
            try {
                ScopedDryRun scope(dry_run ? &dry : nullptr);
                ScopedLog logged(dry_run ? nullptr : &log);
                reattach(e, mode);
            } catch (RegistryException& ex) {
                error = utf8(ex.getMessage());
                all_ok = false;
            }
        }
        devices += std::string(first ? "" : ",") + "{\"guid\":" + quote(g) + ",\"ok\":" +
                   boolean(error.empty()) + (error.empty() ? "" : ",\"error\":" + quote(error)) +
                   ",\"operations\":" + operations_json(dry_run ? dry.operations() : log.operations()) + "}";
        first = false;
    }
    std::printf("{\"command\":\"repair\",\"ok\":%s,\"dry_run\":%s,\"mode\":%s,\"repaired\":%s]}\n",
                boolean(all_ok), boolean(dry_run), mode ? quote(mode_name(*mode)).c_str() : "\"upstream\"",
                devices.c_str());
    return all_ok ? 0 : 1;
}

struct Simulation {
    std::wstring pre, post;         // vendor APOs Equalizer APO replaced, pre-mix and post-mix
    bool hosted = true;             // false: "use original APO" was off in Equalizer APO
    bool eapo_unregistered = false; // Equalizer APO is uninstalled after IsoAPO replaced it
};

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
                enh = type == REG_DWORD && present ? std::to_wstring(RegistryHelper::readDWORDValue(fx, kDisableEnhancements))
                                                   : L"present";
            }
        } catch (RegistryException&) {
        }
        v.push_back(enh);
        return v;
    };
    const auto slots_json = [&](const std::vector<std::wstring>& v) {
        std::string out = "{";
        for (size_t i = 0; i < v.size(); ++i)
            out += std::string(i ? "," : "") + quote(std::string(kEffectSlots[i].label)) + ":" + quote(v[i]);
        return out + "}";
    };

    DryRunRegistry dry;
    ScopedDryRun scope(&dry);
    const std::wstring eapo_record = eapo_record_key(e);
    const std::wstring iso_record = iso_record_key(e);
    if (sim) {
        try {
            RegistryHelper::createKey(eapo_record);
            RegistryHelper::writeValue(eapo_record, L"PreMixChild", sim->hosted ? sim->pre : L"");
            RegistryHelper::writeValue(eapo_record, L"PostMixChild", sim->hosted ? sim->post : L"");
            for (const SlotName& slot : kEffectSlots) {
                std::wstring clsid;
                if (!try_read_string(fx, slot.value, &clsid) || !is_equalizerapo(clsid)) continue;
                const bool pre_slot = std::string(slot.label) == "LFX" || std::string(slot.label) == "SFX";
                const std::wstring& vendor = pre_slot ? sim->pre : sim->post;
                if (!vendor.empty()) RegistryHelper::writeValue(eapo_record, slot.value, vendor);
            }
        } catch (RegistryException& ex) {
            return fail("roundtrip", utf8(ex.getMessage()));
        }
    }
    const std::vector<std::wstring> before = slot_values();
    const std::vector<std::wstring> extras_before = extra_values();
    const std::wstring post = RegistryHelper::getGuidString(ISOAPO_POST_MIX_GUID);
    const auto holds_isoapo = [&](const std::vector<std::wstring>& v) {
        return std::find(v.begin(), v.end(), post) != v.end();
    };

    std::vector<std::wstring> installed, after_direct, detached, repaired, after;
    std::vector<std::wstring> extras_after_direct, extras_after;
    std::wstring child_after_install;
    DeviceAPOInfo::InstallMode chosen = DeviceAPOInfo::INSTALL_SFX_MFX;
    bool reloaded_installed = false, detach_seen = false;
    try {
        // Install and uninstall directly.
        DeviceAPOInfo first;
        if (!first.load(e.guid)) return fail("roundtrip", "endpoint is not present");
        if (first.isInstalled()) return fail("roundtrip", "IsoAPO is already in a slot on this endpoint");
        chosen = mode ? *mode : default_mode(first, e);
        install_isoapo(first, e, chosen, replace_eapo);
        installed = slot_values();
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
        DeviceAPOInfo again;
        again.load(e.guid);
        install_isoapo(again, e, chosen, replace_eapo);
        const std::vector<std::wstring> reinstalled = slot_values();
        // A driver update rewrites FxProperties with its own APOs: put the
        // slots back as they were before the install, leaving the record.
        for (size_t i = 0; i < before.size(); ++i) {
            if (reinstalled[i] == before[i]) continue;
            if (before[i] == L"(absent)") RegistryHelper::deleteValue(fx, kEffectSlots[i].value);
            else RegistryHelper::writeValue(fx, kEffectSlots[i].value, before[i]);
        }
        detached = slot_values();
        DeviceAPOInfo third;
        third.load(e.guid);
        detach_seen = !third.isInstalled() && RegistryHelper::keyExists(iso_record);

        reattach(e, mode);
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

    const bool unregistered = sim && sim->eapo_unregistered;
    const bool restored = unregistered
                              ? true
                              : before == after_direct && before == after && extras_before == extras_after_direct &&
                                    extras_before == extras_after;
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
                    (unregistered || (detach_seen && !holds_isoapo(detached) && holds_isoapo(repaired))) &&
                    (!replace_eapo || !holds_eapo(installed)) && handed_over && no_dangling_eapo &&
                    survives_missing_fx_properties;
    std::printf("{\"command\":\"roundtrip\",\"ok\":%s,\"mode\":%s,\"replace_equalizerapo\":%s,"
                "\"simulated\":%s,\"isoapo_child_after_install\":%s,\"vendor_placements\":%s,\"handed_over\":%s,"
                "\"before\":%s,\"after_install\":%s,\"after_uninstall\":%s,"
                "\"reload_sees_install\":%s,\"after_simulated_driver_update\":%s,\"detach_seen\":%s,"
                "\"after_repair\":%s,\"after_repair_and_uninstall\":%s,\"restored\":%s,"
                "\"survives_missing_fx_properties\":%s,\"no_dangling_equalizerapo\":%s,\"operations\":%s}\n",
                boolean(ok), quote(mode_name(chosen)).c_str(), boolean(replace_eapo),
                sim ? (sim->eapo_unregistered ? "\"hosted, then Equalizer APO uninstalled\""
                           : sim->hosted          ? "\"hosted\""
                                                  : "\"not hosted\"")
                    : "null",
                quote(child_after_install).c_str(), placements.c_str(), boolean(handed_over), slots_json(before).c_str(),
                slots_json(installed).c_str(), slots_json(after_direct).c_str(), boolean(reloaded_installed),
                slots_json(detached).c_str(), boolean(detach_seen), slots_json(repaired).c_str(),
                slots_json(after).c_str(), boolean(restored), boolean(survives_missing_fx_properties),
                boolean(no_dangling_eapo), operations_json(dry.operations()).c_str());
    return ok ? 0 : 1;
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

}  // namespace

// wmain: arguments arrive as UTF-16 and are turned into UTF-8, so a non-ASCII
// argument cannot put invalid UTF-8 into the JSON output.
int wmain(int argc, wchar_t** wargv) {
    std::vector<std::string> argv_utf8;
    for (int i = 0; i < argc; ++i) argv_utf8.push_back(utf8(std::wstring(wargv[i])));

    std::vector<std::string> args;
    bool dry_run = false, replace_eapo = false;
    std::optional<std::string> mode_text;
    std::optional<std::string> simulation_text;
    for (int i = 1; i < argc; ++i) {
        const std::string& a = argv_utf8[i];
        if (a == "--dry-run") dry_run = true;
        else if (a == "--replace-equalizerapo") replace_eapo = true;
        else if (a == "--mode" && i + 1 < argc) mode_text = argv_utf8[++i];
        else if (a == "--simulate-equalizerapo" && i + 1 < argc) simulation_text = argv_utf8[++i];
        else if (a.rfind("--", 0) == 0) return usage();
        else args.push_back(a);
    }
    if (args.empty()) return usage();
    std::optional<DeviceAPOInfo::InstallMode> mode;
    if (mode_text) {
        DeviceAPOInfo::InstallMode parsed;
        if (!parse_mode(*mode_text, &parsed)) return usage();
        mode = parsed;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return fail(args[0], "CoInitializeEx failed");
    }

    const std::string& command = args[0];
    // --simulate-equalizerapo PRE,POST[,unhosted]: only for the dry-run roundtrip
    // with --replace-equalizerapo. Either CLSID may be empty.
    std::optional<Simulation> sim;
    if (simulation_text) {
        if (command != "roundtrip" || !replace_eapo) return usage();
        std::vector<std::wstring> parts;
        const std::wstring text(simulation_text->begin(), simulation_text->end());
        size_t start = 0;
        while (start <= text.size()) {
            const size_t comma = text.find(L',', start);
            parts.push_back(text.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start));
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
        if (parts.size() < 2 || parts.size() > 3 ||
            (parts.size() == 3 && parts[2] != L"unhosted" && parts[2] != L"unregistered")) {
            return usage();
        }
        Simulation s;
        s.pre = parts[0];
        s.post = parts[1];
        s.hosted = parts.size() == 2 || parts[2] == L"unregistered";
        s.eapo_unregistered = parts.size() == 3 && parts[2] == L"unregistered";
        GUID g;
        if ((s.pre.empty() && s.post.empty()) || (!s.pre.empty() && FAILED(CLSIDFromString(s.pre.c_str(), &g))) ||
            (!s.post.empty() && FAILED(CLSIDFromString(s.post.c_str(), &g)))) {
            return usage();
        }
        sim = s;
    }
    if (command == "list" && args.size() == 1) return cmd_list();
    if (command == "repair" && args.size() == 1) return cmd_repair(mode, dry_run);

    const bool needs_endpoint = command == "status" || command == "install" ||
                                command == "uninstall" || command == "test" ||
                                command == "roundtrip";
    if (!needs_endpoint || args.size() != 2) return usage();

    Endpoint endpoint;
    const int found = resolve_endpoint(args[1], &endpoint);
    if (found == 2) return usage();
    if (found == 1) return fail(command, "no render or capture endpoint with GUID " + args[1]);

    if (command == "status") return cmd_status(endpoint);
    if (command == "install") return cmd_install(endpoint, mode, replace_eapo, dry_run);
    if (command == "uninstall") return cmd_uninstall(endpoint, dry_run);
    if (command == "roundtrip") return cmd_roundtrip(endpoint, mode, replace_eapo, sim);
    return cmd_test(endpoint);
}
