// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// isotone_devices. Read-only against this machine's endpoints: nothing here
// changes a device, the registry or a real shared region, or opens a stream.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "doctest.h"

#include <windows.h>
#include <winsvc.h>

#include <mmdeviceapi.h>
#include <objbase.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "device_watcher.h"
#include "devices.h"
#include "engine_probe.h"
#include "isotone/param_block.h"
#include "shared_mapping.h"

using namespace isotone::devices;

namespace {

struct Com {
    Com() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~Com() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    HRESULT hr;
};

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string hex(long value) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(value));
    return buf;
}

// The service's state, or why it could not be read.
std::string service_problem(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr) return "the service manager cannot be opened: " + std::to_string(GetLastError());
    SC_HANDLE service = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    std::string problem;
    SERVICE_STATUS status{};
    if (service == nullptr) {
        problem = narrow(name) + " cannot be opened: " + std::to_string(GetLastError());
    } else if (!QueryServiceStatus(service, &status)) {
        problem = narrow(name) + " status cannot be read: " + std::to_string(GetLastError());
    } else if (status.dwCurrentState != SERVICE_RUNNING) {
        problem = narrow(name) + " is not running (state " + std::to_string(status.dwCurrentState) + ")";
    }
    if (service != nullptr) CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return problem;
}

// Why the tests that need this machine's render endpoints cannot run here, or
// empty when they can. A machine without the audio services (MMDevice
// enumeration is served by AudioEndpointBuilder, sessions by Audiosrv), without
// an MMDeviceEnumerator, or with no render endpoint in any state has nothing to
// compare; GitHub's windows-latest runner can be that machine.
// ISOTONE_DEVICES_TEST_NO_AUDIO forces the skip, so the path is tested (ctest's
// devices_tests_no_audio). An enumeration that fails where the services run is
// not a reason: the test itself reports it. COM is initialised by the caller.
std::string no_audio_reason() {
    if (GetEnvironmentVariableW(L"ISOTONE_DEVICES_TEST_NO_AUDIO", nullptr, 0) > 0)
        return "ISOTONE_DEVICES_TEST_NO_AUDIO is set";
    for (const wchar_t* service : {L"AudioEndpointBuilder", L"Audiosrv"}) {
        if (std::string problem = service_problem(service); !problem.empty()) return problem;
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return "no MMDeviceEnumerator: " + hex(hr);
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &collection);
    enumerator->Release();
    if (FAILED(hr)) return "";
    UINT count = 0;
    hr = collection->GetCount(&count);
    collection->Release();
    return SUCCEEDED(hr) && count == 0 ? "no render endpoints" : "";
}

// ---------------------------------------------------------------------------
// A minimal JSON reader, enough for devicetool's output.

struct Json {
    enum Type { null, boolean, number, string, array, object } type = null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json& operator[](const std::string& key) const {
        static const Json missing;
        const auto it = fields.find(key);
        return it == fields.end() ? missing : it->second;
    }
};

struct JsonParser {
    const std::string& t;
    size_t i = 0;
    bool ok = true;

    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    bool eat(char c) {
        ws();
        if (i < t.size() && t[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    std::string str() {
        std::string out;
        if (!eat('"')) {
            ok = false;
            return out;
        }
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c == '\\' && i < t.size()) {
                const char e = t[i++];
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'u': {
                        // devicetool escapes only control characters this way.
                        c = static_cast<char>(std::stoul(t.substr(i, 4), nullptr, 16));
                        i += 4;
                        break;
                    }
                    default: c = e;
                }
            }
            out += c;
        }
        ok = ok && i < t.size();
        ++i;
        return out;
    }
    Json value() {
        Json v;
        ws();
        if (i >= t.size()) {
            ok = false;
            return v;
        }
        if (t[i] == '{') {
            ++i;
            v.type = Json::object;
            if (eat('}')) return v;
            do {
                std::string key = str();
                if (!eat(':')) ok = false;
                v.fields[key] = value();
            } while (ok && eat(','));
            if (!eat('}')) ok = false;
        } else if (t[i] == '[') {
            ++i;
            v.type = Json::array;
            if (eat(']')) return v;
            do v.items.push_back(value());
            while (ok && eat(','));
            if (!eat(']')) ok = false;
        } else if (t[i] == '"') {
            v.type = Json::string;
            v.s = str();
        } else if (t.compare(i, 4, "true") == 0) {
            v.type = Json::boolean;
            v.b = true;
            i += 4;
        } else if (t.compare(i, 5, "false") == 0) {
            v.type = Json::boolean;
            i += 5;
        } else if (t.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            size_t used = 0;
            try {
                v.n = std::stod(t.substr(i), &used);
            } catch (...) {
                ok = false;
            }
            v.type = Json::number;
            i += used;
        }
        return v;
    }
};

bool parse_json(const std::string& text, Json* out) {
    JsonParser p{text};
    *out = p.value();
    p.ws();
    return p.ok && p.i == text.size();
}

std::wstring devicetool_path() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ISOTONE_DEVICETOOL", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    const std::string compiled = ISOTONE_DEVICETOOL_EXE;
    return std::wstring(compiled.begin(), compiled.end());
}

bool run_devicetool(const std::wstring& exe, const std::wstring& args, Json* out) {
    const std::wstring command = L"\"\"" + exe + L"\" " + args + L"\"";
    FILE* pipe = _wpopen(command.c_str(), L"rb");
    if (pipe == nullptr) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) text.append(buf, n);
    _pclose(pipe);
    return parse_json(text, out);
}

std::vector<std::string> state_flags(DWORD state) {
    std::vector<std::string> flags;
    if (state & DEVICE_STATE_ACTIVE) flags.push_back("active");
    if (state & DEVICE_STATE_DISABLED) flags.push_back("disabled");
    if (state & DEVICE_STATE_NOTPRESENT) flags.push_back("not_present");
    if (state & DEVICE_STATE_UNPLUGGED) flags.push_back("unplugged");
    return flags;
}

}  // namespace

// ---------------------------------------------------------------------------

namespace {

struct Facts {
    bool iso_slot, eapo_slot, record, over, journal;
};

EngineInfo facts(const Facts& f) {
    EngineInfo e;
    e.isoapo_in_slots = f.iso_slot;
    e.equalizerapo_in_slots = f.eapo_slot;
    e.isoapo_record = f.record;
    e.equalizerapo_over_isoapo = f.over;
    e.journal = f.journal;
    return e;
}

std::string state_of(const Facts& f) { return isoapo_state_name(classify_isoapo_state(facts(f))); }

}  // namespace

// Each case is the first branch of devicetool's isoapo_state
// (windows/devicetool/main.cpp lines 1688 to 1705) that the facts reach, and a
// neighbour that differs only in the fact that branch tests.
TEST_CASE("classify_isoapo_state follows devicetool's isoapo_state") {
    //                      iso_slot eapo_slot record over  journal
    SUBCASE("interrupted: the journal wins over everything (1691)") {
        CHECK(state_of({false, false, false, false, true}) == "interrupted");
        CHECK(state_of({true, true, true, true, true}) == "interrupted");
        CHECK(state_of({true, false, false, false, true}) == "interrupted");     // would be unrecorded
        CHECK(state_of({false, false, true, false, true}) == "interrupted");     // would be detached
        CHECK(state_of({false, false, false, false, false}) == "not_installed");
    }
    SUBCASE("unrecorded: IsoAPO in a slot without a record (1692)") {
        CHECK(state_of({true, false, false, false, false}) == "unrecorded");
        CHECK(state_of({true, true, false, true, false}) == "unrecorded");       // before alongside
        CHECK(state_of({true, false, true, false, false}) == "installed");
    }
    SUBCASE("alongside_equalizerapo: both in slots, IsoAPO recorded (1693)") {
        CHECK(state_of({true, true, true, false, false}) == "alongside_equalizerapo");
        CHECK(state_of({true, true, true, true, false}) == "alongside_equalizerapo");   // over does not matter here
        CHECK(state_of({true, false, true, true, false}) == "installed");
    }
    SUBCASE("installed: IsoAPO in a slot, recorded, no Equalizer APO in slots (1695)") {
        CHECK(state_of({true, false, true, false, false}) == "installed");
        CHECK(state_of({true, false, true, true, false}) == "installed");
    }
    SUBCASE("not_installed: IsoAPO in no slot, no record (1699)") {
        CHECK(state_of({false, false, false, false, false}) == "not_installed");
        CHECK(state_of({false, true, false, false, false}) == "not_installed");
        CHECK(state_of({false, true, false, true, false}) == "not_installed");   // Equalizer APO's record alone
        CHECK(state_of({false, false, true, false, false}) == "detached");
    }
    SUBCASE("replaced_by_equalizerapo: record, no slot, Equalizer APO's record names IsoAPO (1700)") {
        CHECK(state_of({false, true, true, true, false}) == "replaced_by_equalizerapo");
        CHECK(state_of({false, false, true, true, false}) == "replaced_by_equalizerapo");   // slots not consulted
        CHECK(state_of({false, true, true, false, false}) == "detached");
    }
    SUBCASE("detached: record, no slot, the rest (1702 to 1704)") {
        CHECK(state_of({false, false, true, false, false}) == "detached");
        CHECK(state_of({false, true, true, false, false}) == "detached");
    }
}

TEST_CASE("classify_backend follows devicetool's backend") {
    // windows/devicetool/main.cpp lines 606 and 676: slots only.
    CHECK(std::string(backend_name(classify_backend(facts({false, false, true, true, true})))) == "none");
    CHECK(std::string(backend_name(classify_backend(facts({true, false, false, false, false})))) == "native");
    CHECK(std::string(backend_name(classify_backend(facts({false, true, false, false, false})))) == "equalizerapo");
    CHECK(std::string(backend_name(classify_backend(facts({true, true, false, false, false})))) == "conflict");
}

// ---------------------------------------------------------------------------

TEST_CASE("canonical_endpoint_guid accepts what the UI and IsoAPO pass") {
    const std::wstring want = L"{798436d2-8c71-4834-9248-00ccbaaca00a}";
    CHECK(isotone::win::canonical_endpoint_guid(L"{798436d2-8c71-4834-9248-00ccbaaca00a}") == want);
    CHECK(isotone::win::canonical_endpoint_guid(L"798436d2-8c71-4834-9248-00ccbaaca00a") == want);
    CHECK(isotone::win::canonical_endpoint_guid(L"{798436D2-8C71-4834-9248-00CCBAACA00A}") == want);
    CHECK(isotone::win::canonical_endpoint_guid(L"798436d2-8C71-4834-9248-00ccBAaca00a") == want);
    CHECK(isotone::win::canonical_endpoint_guid(L"{0.0.0.00000000}.{798436d2-8c71-4834-9248-00ccbaaca00a}") == want);
    CHECK(isotone::win::canonical_endpoint_guid(L"{0.0.1.00000000}.{798436D2-8C71-4834-9248-00CCBAACA00A}") == want);
    CHECK(isotone::win::canonical_endpoint_guid(L"  \t{798436d2-8c71-4834-9248-00ccbaaca00a}\r\n") == want);
    CHECK(isotone::win::canonical_endpoint_guid(L" 798436d2-8c71-4834-9248-00ccbaaca00a ") == want);

    CHECK(isotone::win::canonical_endpoint_guid(L"").empty());
    CHECK(isotone::win::canonical_endpoint_guid(L"   ").empty());
    CHECK(isotone::win::canonical_endpoint_guid(L"junk").empty());
    CHECK(isotone::win::canonical_endpoint_guid(L"{}").empty());
    CHECK(isotone::win::canonical_endpoint_guid(L"{798436d2-8c71-4834-9248-00ccbaaca00}").empty());     // one digit short
    CHECK(isotone::win::canonical_endpoint_guid(L"{798436d2-8c71-4834-9248-00ccbaaca00aa}").empty());   // one too many
    CHECK(isotone::win::canonical_endpoint_guid(L"{798436d2x8c71-4834-9248-00ccbaaca00a}").empty());    // dash replaced
    CHECK(isotone::win::canonical_endpoint_guid(L"{798436g2-8c71-4834-9248-00ccbaaca00a}").empty());    // not hex
    CHECK(isotone::win::canonical_endpoint_guid(L"{798436d2-8c71-4834-9248-00ccbaaca00a").empty());     // no closing brace
    CHECK(isotone::win::canonical_endpoint_guid(L"798436d2-8c71-4834-9248-00ccbaaca00a}").empty());
    CHECK(isotone::win::canonical_endpoint_guid(L"{798436d2-8c71-4834-9248-00ccbaaca00a}}").empty());
    CHECK(isotone::win::canonical_endpoint_guid(L"{79 436d2-8c71-4834-9248-00ccbaaca00a}").empty());    // inner space
    CHECK(isotone::win::canonical_endpoint_guid(L"x.{798436d2-8c71-4834-9248-00ccbaaca00a}").empty());  // prefix not a device ID
    CHECK(isotone::win::canonical_endpoint_guid(L"{0.0.0.00000000}{798436d2-8c71-4834-9248-00ccbaaca00a}").empty());   // no dot
    CHECK(isotone::win::canonical_endpoint_guid(L"{0.0.0.00000000}.{798436d2-8c71-4834-9248-00ccbaaca00a}junk").empty());
    CHECK(isotone::win::canonical_endpoint_guid(L"{\xFF11" L"98436d2-8c71-4834-9248-00ccbaaca00a}").empty());   // fullwidth digit
}

// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> blob_ex(WORD tag, WORD channels, DWORD rate, WORD bits, WORD cb_size = 0) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = tag;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = rate;
    wfx.wBitsPerSample = bits;
    wfx.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    wfx.nAvgBytesPerSec = rate * wfx.nBlockAlign;
    wfx.cbSize = cb_size;
    std::vector<uint8_t> b(sizeof(wfx));
    std::memcpy(b.data(), &wfx, sizeof(wfx));
    return b;
}

std::vector<uint8_t> blob_extensible(WORD channels, DWORD rate, WORD bits, WORD valid, DWORD mask, const GUID& sub) {
    WAVEFORMATEXTENSIBLE ext{};
    ext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ext.Format.nChannels = channels;
    ext.Format.nSamplesPerSec = rate;
    ext.Format.wBitsPerSample = bits;
    ext.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    ext.Format.nAvgBytesPerSec = rate * ext.Format.nBlockAlign;
    ext.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    ext.Samples.wValidBitsPerSample = valid;
    ext.dwChannelMask = mask;
    ext.SubFormat = sub;
    std::vector<uint8_t> b(sizeof(ext));
    std::memcpy(b.data(), &ext, sizeof(ext));
    return b;
}

}  // namespace

TEST_CASE("parse_device_format") {
    SUBCASE("float") {
        const auto b = blob_ex(WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 32);
        const DeviceFormat f = parse_device_format(b.data(), b.size());
        REQUIRE(f.present);
        CHECK(f.error.empty());
        CHECK(f.channels == 2);
        CHECK(f.sample_rate == 48000);
        CHECK(f.bits_per_sample == 32);
        CHECK(f.valid_bits == 32);
        CHECK(f.sample_format == SampleFormat::ieee_float);
        CHECK(f.channel_mask == 0x3);
        CHECK(f.mask_defaulted);
    }
    SUBCASE("24-bit PCM") {
        const auto b = blob_ex(WAVE_FORMAT_PCM, 6, 44100, 24);
        const DeviceFormat f = parse_device_format(b.data(), b.size());
        REQUIRE(f.present);
        CHECK(f.channels == 6);
        CHECK(f.sample_rate == 44100);
        CHECK(f.bits_per_sample == 24);
        CHECK(f.valid_bits == 24);
        CHECK(f.sample_format == SampleFormat::pcm);
        CHECK(f.channel_mask == 0x60F);
        CHECK(f.mask_defaulted);
    }
    SUBCASE("extensible with a mask") {
        const auto b = blob_extensible(8, 96000, 32, 24, 0x63F, KSDATAFORMAT_SUBTYPE_PCM);
        const DeviceFormat f = parse_device_format(b.data(), b.size());
        REQUIRE(f.present);
        CHECK(f.channels == 8);
        CHECK(f.sample_rate == 96000);
        CHECK(f.bits_per_sample == 32);
        CHECK(f.valid_bits == 24);
        CHECK(f.sample_format == SampleFormat::pcm);
        CHECK(f.channel_mask == 0x63F);
        CHECK_FALSE(f.mask_defaulted);
    }
    SUBCASE("extensible with a mask that is not the default for its count") {
        const auto b = blob_extensible(2, 48000, 16, 16, 0x30, KSDATAFORMAT_SUBTYPE_PCM);   // rear pair
        const DeviceFormat f = parse_device_format(b.data(), b.size());
        REQUIRE(f.present);
        CHECK(f.channel_mask == 0x30);
        CHECK_FALSE(f.mask_defaulted);
    }
    SUBCASE("extensible without a mask") {
        const auto b = blob_extensible(2, 48000, 32, 32, 0, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const DeviceFormat f = parse_device_format(b.data(), b.size());
        REQUIRE(f.present);
        CHECK(f.sample_format == SampleFormat::ieee_float);
        CHECK(f.channel_mask == 0x3);
        CHECK(f.mask_defaulted);
    }
    SUBCASE("a count with no default mask") {
        const auto b = blob_extensible(3, 48000, 16, 16, 0, KSDATAFORMAT_SUBTYPE_PCM);
        const DeviceFormat f = parse_device_format(b.data(), b.size());
        REQUIRE(f.present);
        CHECK(f.channel_mask == 0);
        CHECK(f.mask_defaulted);
    }
    SUBCASE("another subformat") {
        const auto b = blob_extensible(2, 48000, 16, 16, 0x3, KSDATAFORMAT_SUBTYPE_ALAW);
        const DeviceFormat f = parse_device_format(b.data(), b.size());
        REQUIRE(f.present);
        CHECK(f.sample_format == SampleFormat::other);
    }
    SUBCASE("trailing bytes past cbSize are ignored") {
        auto b = blob_ex(WAVE_FORMAT_PCM, 2, 48000, 16);
        b.resize(b.size() + 6, 0xAB);
        const DeviceFormat f = parse_device_format(b.data(), b.size());
        REQUIRE(f.present);
        CHECK(f.channels == 2);
    }
    SUBCASE("truncated") {
        const auto b = blob_ex(WAVE_FORMAT_PCM, 2, 48000, 16);
        for (size_t n : {size_t{0}, size_t{8}, size_t{17}}) {
            const DeviceFormat f = parse_device_format(b.data(), n);
            CHECK_FALSE(f.present);
            CHECK_FALSE(f.error.empty());
        }
        CHECK_FALSE(parse_device_format(nullptr, 40).present);
        // An extensible blob cut after its WAVEFORMATEX.
        const auto e = blob_extensible(2, 48000, 32, 32, 0x3, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const DeviceFormat f = parse_device_format(e.data(), sizeof(WAVEFORMATEX));
        CHECK_FALSE(f.present);
        CHECK(f.channels == 0);
        const DeviceFormat g = parse_device_format(e.data(), e.size() - 1);
        CHECK_FALSE(g.present);
    }
    SUBCASE("wrong size") {
        // cbSize claims more than the blob holds.
        const auto b = blob_ex(WAVE_FORMAT_PCM, 2, 48000, 16, 4);
        CHECK_FALSE(parse_device_format(b.data(), b.size()).present);
        // An extensible tag with a cbSize too small for WAVEFORMATEXTENSIBLE,
        // in a blob big enough for one: the mask would be read from bytes the
        // format does not claim.
        auto e = blob_extensible(2, 48000, 32, 32, 0x3, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        for (WORD cb : {WORD{0}, WORD{21}}) {
            std::memcpy(e.data() + offsetof(WAVEFORMATEX, cbSize), &cb, sizeof(cb));
            const DeviceFormat f = parse_device_format(e.data(), e.size());
            CHECK_FALSE(f.present);
            CHECK(f.channel_mask == 0);
            CHECK_FALSE(f.error.empty());
        }
    }
    SUBCASE("no channels or no rate") {
        const auto a = blob_ex(WAVE_FORMAT_PCM, 0, 48000, 16);
        CHECK_FALSE(parse_device_format(a.data(), a.size()).present);
        const auto b = blob_ex(WAVE_FORMAT_PCM, 2, 0, 16);
        CHECK_FALSE(parse_device_format(b.data(), b.size()).present);
    }
}

// ---------------------------------------------------------------------------

namespace {

const PROPERTYKEY kFormatKey = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};
const PROPERTYKEY kFriendlyNameKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kDeviceDescKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
const PROPERTYKEY kInterfaceNameKey = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};
const PROPERTYKEY kAdapterNameKey = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 6};

struct Recorder {
    std::vector<DeviceEvent> events;
};

}  // namespace

TEST_CASE("DeviceNotifier delivers and filters") {
    auto rec = std::make_shared<Recorder>();
    auto* n = new DeviceNotifier([rec](const DeviceEvent& e) { rec->events.push_back(e); });

    const wchar_t id[] = L"{0.0.0.00000000}.{798436d2-8c71-4834-9248-00ccbaaca00a}";
    CHECK(n->OnDeviceAdded(id) == S_OK);
    CHECK(n->OnDeviceRemoved(id) == S_OK);
    CHECK(n->OnDeviceStateChanged(id, DEVICE_STATE_UNPLUGGED) == S_OK);
    CHECK(n->OnDefaultDeviceChanged(eRender, eMultimedia, id) == S_OK);
    CHECK(n->OnDefaultDeviceChanged(eRender, eConsole, nullptr) == S_OK);
    CHECK(n->OnDefaultDeviceChanged(eCapture, eConsole, id) == S_OK);   // filtered
    CHECK(n->OnPropertyValueChanged(id, kFormatKey) == S_OK);
    CHECK(n->OnPropertyValueChanged(id, kFriendlyNameKey) == S_OK);
    CHECK(n->OnPropertyValueChanged(id, kDeviceDescKey) == S_OK);
    CHECK(n->OnPropertyValueChanged(id, kInterfaceNameKey) == S_OK);
    CHECK(n->OnPropertyValueChanged(id, kAdapterNameKey) == S_OK);
    // Filtered: the format key's fmtid with another pid, the name key's fmtid
    // with another pid, and an unrelated key.
    PROPERTYKEY other = kFormatKey;
    other.pid = 1;
    CHECK(n->OnPropertyValueChanged(id, other) == S_OK);
    other = kFriendlyNameKey;
    other.pid = 15;
    CHECK(n->OnPropertyValueChanged(id, other) == S_OK);
    other = kFormatKey;
    other.fmtid.Data1 ^= 1;
    CHECK(n->OnPropertyValueChanged(id, other) == S_OK);

    REQUIRE(rec->events.size() == 10);
    CHECK(rec->events[0].kind == DeviceEventKind::added);
    CHECK(rec->events[0].device_id == id);
    CHECK(rec->events[1].kind == DeviceEventKind::removed);
    CHECK(rec->events[2].kind == DeviceEventKind::state_changed);
    CHECK(rec->events[2].state == DEVICE_STATE_UNPLUGGED);
    CHECK(rec->events[3].kind == DeviceEventKind::default_changed);
    CHECK(rec->events[3].role == eMultimedia);
    CHECK(rec->events[3].device_id == id);
    CHECK(rec->events[4].kind == DeviceEventKind::default_changed);
    CHECK(rec->events[4].role == eConsole);
    CHECK(rec->events[4].device_id.empty());
    CHECK(rec->events[5].kind == DeviceEventKind::format_changed);
    CHECK(rec->events[6].kind == DeviceEventKind::name_changed);
    CHECK(rec->events[7].kind == DeviceEventKind::name_changed);
    CHECK(rec->events[8].kind == DeviceEventKind::name_changed);
    CHECK(rec->events[9].kind == DeviceEventKind::name_changed);

    IUnknown* unk = nullptr;
    CHECK(n->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&unk)) == S_OK);
    IMMNotificationClient* client = nullptr;
    CHECK(n->QueryInterface(__uuidof(IMMNotificationClient), reinterpret_cast<void**>(&client)) == S_OK);
    IMMDeviceEnumerator* wrong = nullptr;
    CHECK(n->QueryInterface(__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&wrong)) == E_NOINTERFACE);
    CHECK(wrong == nullptr);
    CHECK(unk->Release() == 2);
    CHECK(client->Release() == 1);

    // After close: nothing delivered, and the callback (and what it captured) is gone.
    CHECK(rec.use_count() == 2);
    CHECK(n->close() == S_OK);
    CHECK(rec.use_count() == 1);
    n->OnDeviceAdded(id);
    n->OnPropertyValueChanged(id, kFormatKey);
    n->OnDefaultDeviceChanged(eRender, eConsole, id);
    CHECK(rec->events.size() == 10);
    CHECK(n->close() == S_OK);
    CHECK(n->Release() == 0);
}

TEST_CASE("DeviceNotifier close from inside the callback is refused") {
    HRESULT inner = S_OK;
    int calls = 0;
    DeviceNotifier* n = nullptr;
    n = new DeviceNotifier([&](const DeviceEvent&) {
        ++calls;
        inner = n->close();
    });
    n->OnDeviceAdded(L"x");
    CHECK(inner == E_ILLEGAL_METHOD_CALL);
    n->OnDeviceAdded(L"x");   // still open
    CHECK(calls == 2);
    CHECK(n->close() == S_OK);
    n->OnDeviceAdded(L"x");
    CHECK(calls == 2);
    n->Release();
}

TEST_CASE("DeviceNotifier close waits for a callback in progress") {
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
    std::atomic<int> after_close{0};
    std::atomic<bool> closed{false};
    auto* n = new DeviceNotifier([&](const DeviceEvent&) {
        if (closed.load()) ++after_close;
        if (!entered.exchange(true)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            finished = true;
        }
    });
    std::atomic<bool> stop{false};
    std::thread spammer([&] {
        while (!stop.load()) n->OnPropertyValueChanged(L"x", kFormatKey);
    });
    while (!entered.load()) std::this_thread::yield();
    CHECK(n->close() == S_OK);
    closed = true;
    CHECK(finished.load());   // close returned only after the slow callback did
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop = true;
    spammer.join();
    CHECK(after_close.load() == 0);
    n->Release();
}

TEST_CASE("DeviceWatcher registers and unregisters") {
    Com com;
    REQUIRE(SUCCEEDED(com.hr));
    if (const std::string why = no_audio_reason(); !why.empty()) {
        MESSAGE("SKIPPED: " << why);
        return;
    }
    DeviceWatcher w;
    CHECK(w.stop() == S_OK);   // not started
    CHECK(w.start([](const DeviceEvent&) {}) == S_OK);
    CHECK(w.started());
    CHECK(w.start([](const DeviceEvent&) {}) == E_ILLEGAL_STATE_CHANGE);
    CHECK(w.stop() == S_OK);
    CHECK_FALSE(w.started());
    CHECK(w.stop() == S_OK);
    CHECK(w.start([](const DeviceEvent&) {}) == S_OK);   // restartable; the destructor stops it
}

// ---------------------------------------------------------------------------

namespace {

std::wstring unique_prefix() {
    wchar_t buf[96];
    swprintf_s(buf, L"Local\\isotone-devices-test-%lu-%llu-", GetCurrentProcessId(),
               static_cast<unsigned long long>(GetTickCount64()));
    return buf;
}

}  // namespace

TEST_CASE("sample_region, heartbeat_moved and classify_activity") {
    const std::wstring prefix = unique_prefix();
    const std::wstring guid = L"{11111111-2222-3333-4444-555555555555}";

    RegionSample none = sample_region(guid, prefix.c_str());
    CHECK(none.open_error == ERROR_FILE_NOT_FOUND);
    CHECK(sample_region(L"junk", prefix.c_str()).open_error == ERROR_INVALID_PARAMETER);

    isotone::win::SharedMapping host;
    REQUIRE(host.create_or_open(isotone::win::mapping_name(prefix.c_str(), guid)) == ERROR_SUCCESS);
    REQUIRE(host.created());
    isotone::host_publish_format(host.params(), 44100, 6, 0x60F, isotone::HostState::Running);
    // Nonzero, so a sample of no region (heartbeat 0) differs from it.
    for (int i = 0; i < 3; ++i) isotone::host_heartbeat(host.params());

    // Found under the device-ID and upper-case spellings too.
    const RegionSample a = sample_region(L"{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}", prefix.c_str());
    REQUIRE(a.open_error == ERROR_SUCCESS);
    CHECK(a.version == isotone::kParamVersion);
    CHECK(a.sample_rate == 44100);
    CHECK(a.channels == 6);
    CHECK(a.speaker_mask == 0x60F);
    CHECK(a.host_state == static_cast<uint32_t>(isotone::HostState::Running));
    CHECK(a.heartbeat == 3);

    SUBCASE("still") {
        const RegionSample b = sample_region(L"11111111-2222-3333-4444-555555555555", prefix.c_str());
        REQUIRE(b.open_error == ERROR_SUCCESS);
        CHECK_FALSE(heartbeat_moved(a, b));
        CHECK(classify_activity(true, a, b, 0) == EngineActivity::idle);
        CHECK(classify_activity(true, a, b, 1) == EngineActivity::stalled);
        CHECK(classify_activity(true, a, b, -1) == EngineActivity::unknown);
        CHECK(classify_activity(false, a, b, 1) == EngineActivity::not_installed);
    }
    SUBCASE("moving") {
        isotone::host_heartbeat(host.params());
        const RegionSample b = sample_region(guid, prefix.c_str());
        CHECK(heartbeat_moved(a, b));
        CHECK(classify_activity(true, a, b, 0) == EngineActivity::running);
        CHECK(classify_activity(false, a, b, 0) == EngineActivity::running);
        CHECK(classify_activity(true, a, b, -1) == EngineActivity::running);
    }
    SUBCASE("no region") {
        CHECK_FALSE(heartbeat_moved(none, none));
        CHECK_FALSE(heartbeat_moved(none, a));
        CHECK_FALSE(heartbeat_moved(a, none));
        CHECK(classify_activity(true, none, none, 0) == EngineActivity::idle);
        CHECK(classify_activity(true, none, none, 2) == EngineActivity::stalled);
        CHECK(classify_activity(false, none, none, 2) == EngineActivity::not_installed);
        // The region vanished between samples: not running.
        CHECK(classify_activity(true, a, none, 0) == EngineActivity::idle);
    }
    SUBCASE("unreadable") {
        RegionSample bad;
        bad.open_error = ERROR_INVALID_DATA;
        CHECK(classify_activity(true, bad, bad, 1) == EngineActivity::region_unreadable);
        CHECK(classify_activity(false, bad, bad, 1) == EngineActivity::not_installed);
        bad.open_error = ERROR_ACCESS_DENIED;
        CHECK(classify_activity(true, none, bad, 0) == EngineActivity::region_unreadable);
    }
}

TEST_CASE("probe_engine against a test region on a real endpoint") {
    Com com;
    REQUIRE(SUCCEEDED(com.hr));
    if (const std::string why = no_audio_reason(); !why.empty()) {
        MESSAGE("SKIPPED: " << why);
        return;
    }
    std::vector<Endpoint> endpoints;
    REQUIRE(enumerate_render_endpoints(&endpoints) == S_OK);
    const Endpoint* target = nullptr;
    for (const Endpoint& e : endpoints) {
        if (e.state == DEVICE_STATE_ACTIVE && !e.engine.isoapo_in_slots && SUCCEEDED(e.engine.error)) {
            target = &e;
            break;
        }
    }
    if (target == nullptr) {
        MESSAGE("no active render endpoint without IsoAPO; probe_engine not exercised");
        return;
    }
    MESSAGE("probing " << narrow(target->friendly_name) << " " << narrow(target->guid));

    const std::wstring prefix = unique_prefix();
    isotone::win::SharedMapping host;
    REQUIRE(host.create_or_open(isotone::win::mapping_name(prefix.c_str(), target->guid)) == ERROR_SUCCESS);
    isotone::host_publish_format(host.params(), 96000, 8, 0x63F, isotone::HostState::Running);

    EngineProbe p;
    SUBCASE("heartbeat still") {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(probe_engine(target->id, &p, 100, prefix.c_str()) == S_OK);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        CHECK(ms.count() >= 100);
        CHECK(ms.count() < 1000);
        CHECK(p.earlier.open_error == ERROR_SUCCESS);
        CHECK_FALSE(heartbeat_moved(p.earlier, p.later));
        CHECK(p.sessions_error == S_OK);
        CHECK(p.active_sessions >= 0);
        CHECK(p.activity == EngineActivity::not_installed);
    }
    SUBCASE("heartbeat moving") {
        std::atomic<bool> stop{false};
        std::thread beat([&] {
            while (!stop.load()) {
                isotone::host_heartbeat(host.params());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        CHECK(probe_engine(target->guid, &p, 50, prefix.c_str()) == S_OK);
        stop = true;
        beat.join();
        CHECK(heartbeat_moved(p.earlier, p.later));
        CHECK(p.activity == EngineActivity::running);
        CHECK(p.later.sample_rate == 96000);
        CHECK(p.later.channels == 8);
        CHECK(p.later.speaker_mask == 0x63F);
    }
    SUBCASE("no region under another prefix") {
        const std::wstring empty_prefix = unique_prefix() + L"empty-";
        CHECK(probe_engine(target->guid, &p, 10, empty_prefix.c_str()) == S_OK);
        CHECK(p.earlier.open_error == ERROR_FILE_NOT_FOUND);
        CHECK(p.activity == EngineActivity::not_installed);
    }
    CHECK(probe_engine(L"junk", &p, 0, prefix.c_str()) == E_INVALIDARG);
}

TEST_CASE("probe_engine on the real regions (report only)") {
    Com com;
    REQUIRE(SUCCEEDED(com.hr));
    if (const std::string why = no_audio_reason(); !why.empty()) {
        MESSAGE("SKIPPED: " << why);
        return;
    }
    std::vector<Endpoint> endpoints;
    REQUIRE(enumerate_render_endpoints(&endpoints) == S_OK);
    static const char* const names[] = {"not_installed", "idle", "running", "stalled", "region_unreadable", "unknown"};
    for (const Endpoint& e : endpoints) {
        if (e.state != DEVICE_STATE_ACTIVE) continue;
        EngineProbe p;
        const HRESULT hr = probe_engine(e.id, &p);
        char line[256];
        std::snprintf(line, sizeof(line), "hr 0x%08lx activity %s region %lu heartbeat %u->%u sessions %d (0x%08lx) format %u/%u/0x%x",
                      static_cast<unsigned long>(hr), names[static_cast<int>(p.activity)], p.later.open_error,
                      p.earlier.heartbeat, p.later.heartbeat, p.active_sessions,
                      static_cast<unsigned long>(p.sessions_error), p.later.sample_rate, p.later.channels,
                      p.later.speaker_mask);
        MESSAGE(narrow(e.friendly_name) << " " << narrow(e.guid) << ": " << line);
        CHECK(hr == S_OK);
    }
}

// ---------------------------------------------------------------------------

TEST_CASE("ISOTONE_DEVICES_TEST_NO_AUDIO forces the skip") {
    if (GetEnvironmentVariableW(L"ISOTONE_DEVICES_TEST_NO_AUDIO", nullptr, 0) == 0) return;
    Com com;
    REQUIRE(SUCCEEDED(com.hr));
    CHECK(no_audio_reason() == "ISOTONE_DEVICES_TEST_NO_AUDIO is set");
}

TEST_CASE("read_engine and read_render_endpoint errors") {
    Com com;
    REQUIRE(SUCCEEDED(com.hr));
    CHECK(read_engine(L"junk").error == E_INVALIDARG);
    CHECK(read_engine(L"{00000000-0000-0000-0000-000000000001}").error == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    CHECK_FALSE(read_device_format(L"junk").present);
    if (const std::string why = no_audio_reason(); !why.empty()) {
        MESSAGE("SKIPPED read_render_endpoint and count_active_sessions: " << why);
        return;
    }
    Endpoint e;
    CHECK(read_render_endpoint(L"{00000000-0000-0000-0000-000000000001}", &e) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    CHECK(read_render_endpoint(L"junk", &e) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    int active = 7;
    CHECK(count_active_sessions(L"junk", &active) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
    CHECK(active == -1);
}

TEST_CASE("endpoints, formats and engines match isotone-devicetool") {
    const std::wstring tool = devicetool_path();
    if (GetFileAttributesW(tool.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MESSAGE("SKIPPED: no isotone-devicetool at " << narrow(tool));
        return;
    }
    MESSAGE("devicetool: " << narrow(tool));
    Com com;
    REQUIRE(SUCCEEDED(com.hr));
    if (const std::string why = no_audio_reason(); !why.empty()) {
        MESSAGE("SKIPPED: " << why);
        return;
    }

    std::vector<Endpoint> mine;
    REQUIRE(enumerate_render_endpoints(&mine) == S_OK);
    std::map<std::wstring, const Endpoint*> by_guid;
    for (const Endpoint& e : mine) {
        CHECK_MESSAGE(e.error.empty(), narrow(e.id) << ": " << e.error);
        CHECK(e.guid == isotone::win::canonical_endpoint_guid(e.id));
        CHECK(by_guid.emplace(e.guid, &e).second);
    }

    Json list;
    REQUIRE(run_devicetool(tool, L"list", &list));
    REQUIRE(list["ok"].b);
    size_t render = 0;
    size_t compared_formats = 0;
    for (const Json& d : list["endpoints"].items) {
        if (d["flow"].s != "render") continue;
        ++render;
        const std::string guid = d["guid"].s;
        INFO("endpoint " << guid << " " << d["connection"].s);
        const auto it = by_guid.find(isotone::win::canonical_endpoint_guid(std::wstring(guid.begin(), guid.end())));
        REQUIRE_MESSAGE(it != by_guid.end(), "not enumerated");
        const Endpoint& e = *it->second;

        CHECK(narrow(e.id) == d["id"].s);
        CHECK(narrow(e.device_name) == d["name"].s);
        CHECK(narrow(e.connection_name) == d["connection"].s);
        std::vector<std::string> flags;
        for (const Json& f : d["state"]["flags"].items) flags.push_back(f.s);
        CHECK(state_flags(e.state) == flags);
        CHECK(e.engine.error == S_OK);
        CHECK(std::string(backend_name(e.engine.backend)) == d["backend"].s);

        Json status;
        REQUIRE(run_devicetool(tool, L"status " + std::wstring(guid.begin(), guid.end()), &status));
        REQUIRE(status["ok"].b);
        const Json& iso = status["isoapo"];
        CHECK(std::string(backend_name(e.engine.backend)) == status["backend"].s);
        CHECK(std::string(isoapo_state_name(e.engine.isoapo_state)) == iso["state"].s);
        CHECK(e.engine.isoapo_in_slots == iso["in_slots"].b);
        CHECK(e.engine.equalizerapo_in_slots == status["equalizerapo"]["in_slots"].b);
        CHECK(e.engine.isoapo_record == (iso["record"].type != Json::null));
        CHECK(e.engine.equalizerapo_over_isoapo == iso["equalizerapo_over_isoapo"].b);
        CHECK(e.engine.journal == (iso["interrupted_command"].type != Json::null));
        CHECK((e.engine.isoapo_state == IsoApoState::detached) == iso["detached"].b);
        CHECK((e.engine.isoapo_state == IsoApoState::unrecorded) == iso["unrecorded"].b);
        MESSAGE("backend " << d["backend"].s << ", isoapo " << iso["state"].s);

        const Json& dev = status["device"];
        if (dev.type == Json::null) {
            // devicetool loads no format for a not-present endpoint.
            if (e.format.present) {
                MESSAGE("not present; library format " << e.format.channels << " ch " << e.format.sample_rate);
            } else {
                MESSAGE("not present; library: " << e.format.error);
            }
            continue;
        }
        CHECK(narrow(e.device_name) == dev["name"].s);
        CHECK(narrow(e.connection_name) == dev["connection"].s);
        CHECK(e.default_multimedia == dev["default_device"].b);
        CHECK(e.engine.enhancements_disabled == dev["enhancements_disabled"].b);
        REQUIRE_MESSAGE(e.format.present, e.format.error);
        CHECK(e.format.channels == dev["channels"].n);
        CHECK(e.format.sample_rate == dev["sample_rate"].n);
        const unsigned long tool_mask = std::stoul(dev["channel_mask"].s, nullptr, 16);
        if (e.format.mask_defaulted) {
            // devicetool falls back to PKEY_AudioEndpoint_PhysicalSpeakers, then 0.
            MESSAGE("mask defaulted to 0x" << std::hex << e.format.channel_mask << "; devicetool 0x" << tool_mask);
        } else {
            CHECK(e.format.channel_mask == tool_mask);
        }
        ++compared_formats;
    }
    CHECK(render == mine.size());
    MESSAGE(render << " render endpoints compared, " << compared_formats << " with a format");
}
