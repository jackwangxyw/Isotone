// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The compatibility backend's file handling. Everything runs in a fresh
// directory under %TEMP%; nothing here knows where a real Equalizer APO lives.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "doctest.h"

#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <atomic>
#include <chrono>
#include <sstream>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "compat_writer.h"
#include "config_files.h"
#include "eapo_install.h"
#include "isotone/biquad.h"
#include "isotone/processor.h"
#include "isotone/speakers.h"
#include "isotone_file.h"
#include "write_coalescer.h"

namespace fs = std::filesystem;
using namespace isotone;
using namespace isotone::compat;

namespace {

struct Sandbox {
    Sandbox() {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        GUID g;
        CoCreateGuid(&g);
        wchar_t name[64];
        StringFromGUID2(g, name, 64);
        dir = fs::path(temp) / (std::wstring(L"isotone-compat-test-") + name);
        fs::create_directories(dir);
    }
    ~Sandbox() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path dir;
};

void put(const fs::path& p, const std::string& bytes) {
    std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string get(const fs::path& p) {
    std::string s;
    REQUIRE(read_file_bytes(p, &s) == ERROR_SUCCESS);
    return s;
}

// The owner's live config.txt, as snapshotted into sim/reference (20 bytes,
// CRLF). Written out literally so CI, which has no sim/, runs the same test.
constexpr char kOwnerConfig[] = "Include: peace.txt\r\n";

std::string block(const char* nl) {
    return std::string(nl) + "# Added by Isotone. Remove these three lines to detach it." + nl +
           "Device: all" + nl + "Include: Isotone.txt" + nl;
}

}  // namespace

// ---------------------------------------------------------------------------
// The guard that keeps tools off a live install

TEST_CASE("a protected root that exists but cannot be identified protects everything") {
    // The guard compares file IDs; a root on a file system that reports none
    // must refuse, not wave every path through. The named-pipe root exists and
    // has no file ID.
    Sandbox s;
    const fs::path pipes = L"\\\\.\\pipe\\";
    REQUIRE(GetFileAttributesW(pipes.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(path_is_inside(s.dir, pipes));
    CHECK_FALSE(path_is_inside(s.dir, s.dir / "no-such-root"));
}

TEST_CASE("a protected directory is recognised however its path is spelled") {
    // The first version compared path text, and the short name
    // C:\PROGRA~1\EqualizerAPO\config walked straight past it onto the owner's
    // live install. Every spelling below must be caught.
    Sandbox s;
    const fs::path prot = s.dir / "Program Files" / "EqualizerAPO" / "config";
    fs::create_directories(prot / "sub");
    fs::create_directories(s.dir / "Program Files" / "EqualizerAPO" / "configX");
    fs::create_directories(s.dir / "other");

    CHECK(path_is_inside(prot, prot));
    CHECK(path_is_inside(prot.wstring() + L"\\", prot));
    CHECK(path_is_inside(prot.wstring() + L"\\\\", prot));
    std::wstring upper = prot.wstring();
    for (wchar_t& c : upper) c = static_cast<wchar_t>(towupper(c));
    CHECK(path_is_inside(upper, prot));
    CHECK(path_is_inside(prot / "sub" / "..", prot));
    CHECK(path_is_inside(prot / "sub", prot));
    CHECK(path_is_inside(prot / "not-created-yet" / "deeper", prot));
    std::wstring forward = prot.wstring();
    for (wchar_t& c : forward) if (c == L'\\') c = L'/';
    CHECK(path_is_inside(forward, prot));

    wchar_t short_name[MAX_PATH] = {};
    const DWORD n = GetShortPathNameW(prot.c_str(), short_name, MAX_PATH);
    if (n > 0 && n < MAX_PATH && _wcsicmp(short_name, prot.c_str()) != 0) {
        std::string shown;
        for (DWORD i = 0; i < n; ++i) shown += static_cast<char>(short_name[i] < 128 ? short_name[i] : '?');
        CAPTURE(shown);
        CHECK(path_is_inside(short_name, prot));
    } else {
        MESSAGE("8.3 names are disabled on this volume; short-name spelling not exercised");
    }

    const fs::path junction = s.dir / "link";
    const std::wstring cmd = L"cmd /c mklink /J \"" + junction.wstring() + L"\" \"" + prot.wstring() + L"\" >nul";
    if (_wsystem(cmd.c_str()) == 0 && fs::exists(junction)) {
        CHECK(path_is_inside(junction, prot));
        CHECK(path_is_inside(junction / "sub", prot));
        RemoveDirectoryW(junction.c_str());   // the junction only, not its target
    } else {
        MESSAGE("could not create a junction; junction spelling not exercised");
    }

    CHECK_FALSE(path_is_inside(s.dir / "other", prot));
    CHECK_FALSE(path_is_inside(s.dir / "Program Files" / "EqualizerAPO" / "configX", prot));
    CHECK_FALSE(path_is_inside(s.dir / "Program Files" / "EqualizerAPO", prot));
    CHECK_FALSE(path_is_inside(prot, s.dir / "does-not-exist"));
}

// ---------------------------------------------------------------------------
// Atomic writes

TEST_CASE("an atomic write creates, replaces, and leaves no temporary file") {
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    REQUIRE(write_file_atomically(f, "first\n") == ERROR_SUCCESS);
    CHECK(get(f) == "first\n");
    REQUIRE(write_file_atomically(f, "second, and longer\n") == ERROR_SUCCESS);
    CHECK(get(f) == "second, and longer\n");
    REQUIRE(write_file_atomically(f, "") == ERROR_SUCCESS);
    CHECK(get(f).empty());
    CHECK_FALSE(fs::exists(s.dir / "Isotone.txt.tmp"));
}

TEST_CASE("a reader holding the file blocks a replace, and the retry waits it out") {
    // Equalizer APO opens config files GENERIC_READ with FILE_SHARE_READ only
    // (FilterEngine::loadConfigFile). Holding the file that way must really
    // block MoveFileExW, or the retry is dead code.
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    REQUIRE(write_file_atomically(f, "old") == ERROR_SUCCESS);

    HANDLE held = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    const DWORD blocked = write_file_atomically(f, "new", 0);
    CAPTURE(blocked);
    CHECK(blocked != ERROR_SUCCESS);
    CHECK_FALSE(fs::exists(s.dir / "Isotone.txt.tmp"));

    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        CloseHandle(held);
    });
    const DWORD waited = write_file_atomically(f, "new", 1000);
    release.join();
    CHECK(waited == ERROR_SUCCESS);
    CHECK(get(f) == "new");
    CHECK_FALSE(fs::exists(s.dir / "Isotone.txt.tmp"));
}

TEST_CASE("an atomic write that cannot land leaves the target as it was") {
    Sandbox s;
    const fs::path f = s.dir / "Isotone.txt";
    REQUIRE(write_file_atomically(f, "keep me") == ERROR_SUCCESS);
    HANDLE held = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    CHECK(write_file_atomically(f, "lost", 50) != ERROR_SUCCESS);
    CloseHandle(held);
    CHECK(get(f) == "keep me");
    CHECK_FALSE(fs::exists(s.dir / "Isotone.txt.tmp"));

    CHECK(write_file_atomically(s.dir / "no-such-dir" / "Isotone.txt", "x") == ERROR_PATH_NOT_FOUND);
    CHECK_FALSE(fs::exists(s.dir / "no-such-dir"));
}

// ---------------------------------------------------------------------------
// The Include line

TEST_CASE("the literal matches the snapshot of the owner's config.txt") {
    const fs::path snapshot = ISOTONE_SIM_REFERENCE "/apo-live/config.txt";
    if (!fs::exists(snapshot)) {
        MESSAGE("no sim/reference in this checkout; literal not cross-checked");
        return;
    }
    CHECK(get(snapshot) == kOwnerConfig);
}

TEST_CASE("attaching to the owner's config appends once and detaches back to the same bytes") {
    Sandbox s;
    const fs::path config = s.dir / "config.txt";
    put(config, kOwnerConfig);

    const AttachResult first = attach_include(s.dir);
    REQUIRE(first.error == ERROR_SUCCESS);
    CHECK(first.appended);
    CHECK(first.before.peace_included);
    CHECK_FALSE(first.before.isotone_included);
    CHECK(get(config) == std::string(kOwnerConfig) + block("\r\n"));
    CHECK(get(s.dir / kConfigBackupName) == kOwnerConfig);

    const ConfigInspection after = inspect_config(s.dir);
    CHECK(after.isotone_included);
    CHECK(after.attached_by_isotone);

    const AttachResult second = attach_include(s.dir);
    REQUIRE(second.error == ERROR_SUCCESS);
    CHECK_FALSE(second.appended);
    CHECK(get(config) == std::string(kOwnerConfig) + block("\r\n"));
    CHECK(get(s.dir / kConfigBackupName) == kOwnerConfig);

    bool removed = false;
    REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
    CHECK(removed);
    CHECK(get(config) == kOwnerConfig);

    REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
    CHECK_FALSE(removed);
    CHECK(get(config) == kOwnerConfig);
}

TEST_CASE("every byte of a busy config survives, whatever its line endings and last line") {
    // Peace, Convolution, VST, a second include, a scoped Device and Channel, a
    // Stage line, a UTF-8 BOM, non-ASCII text, LF endings and no final newline.
    Sandbox s;
    const std::string original =
        "\xEF\xBB\xBF# Peace-managed configuration \xE2\x80\x94 do not edit\n"
        "Include: peace.txt\n"
        "Device: Speakers Realtek\n"
        "Channel: L\n"
        "Convolution: C:\\Impulses\\room.wav\n"
        "VSTPlugin: Library C:\\VST\\plugin.dll ChunkData \"AAAA==\"\n"
        "Stage: post-mix\n"
        "Include: C:\\Users\\someone\\extra.txt\n"
        "Filter 1: ON PK Fc 1000 Hz Gain -3 dB Q 1";
    put(s.dir / "config.txt", original);

    const AttachResult r = attach_include(s.dir);
    REQUIRE(r.error == ERROR_SUCCESS);
    CHECK(r.appended);
    CHECK(r.before.peace_included);
    CHECK(r.before.has_stage_lines);
    CHECK_FALSE(r.before.has_conditionals);
    CHECK(r.before.includes.size() == 2);

    const std::string after = get(s.dir / "config.txt");
    REQUIRE(after.size() == original.size() + block("\n").size());
    CHECK(after.compare(0, original.size(), original) == 0);
    CHECK(after.substr(original.size()) == block("\n"));

    bool removed = false;
    REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
    CHECK(removed);
    CHECK(get(s.dir / "config.txt") == original);
}

TEST_CASE("an Include is recognised the way upstream reads it") {
    Sandbox s;
    // Upstream compares the command case-sensitively, so this line is not an
    // include to Equalizer APO and must not stop Isotone attaching.
    put(s.dir / "config.txt", "include: Isotone.txt\r\n");
    CHECK_FALSE(inspect_config(s.dir).isotone_included);
    CHECK(attach_include(s.dir).appended);

    // A hand-written relative include of our file is ours already.
    put(s.dir / "config.txt", "Preamp: -2 dB\r\n  Include:   .\\ISOTONE.TXT  \r\n");
    const AttachResult r = attach_include(s.dir);
    REQUIRE(r.error == ERROR_SUCCESS);
    CHECK_FALSE(r.appended);
    CHECK(get(s.dir / "config.txt") == "Preamp: -2 dB\r\n  Include:   .\\ISOTONE.TXT  \r\n");

    // But detach will not touch an include it did not write.
    bool removed = true;
    CHECK(detach_include(s.dir, &removed) == ERROR_INVALID_DATA);
    CHECK_FALSE(removed);

    put(s.dir / "config.txt", "If: 1 == 1\r\nEndIf:\r\n");
    CHECK(inspect_config(s.dir).has_conditionals);

    // An include that only some devices reach is not attached for all of them,
    // and attaching again would include the file twice for the ones it does.
    for (const char* scoped : {"Device: Speakers\r\nInclude: Isotone.txt\r\n",
                               "If: sampleRate == 44100\r\nInclude: Isotone.txt\r\nEndIf:\r\n"}) {
        CAPTURE(scoped);
        put(s.dir / "config.txt", scoped);
        const ConfigInspection i = inspect_config(s.dir);
        CHECK_FALSE(i.isotone_included);
        CHECK(i.isotone_included_conditionally);
        const AttachResult refused = attach_include(s.dir);
        CHECK(refused.error != ERROR_SUCCESS);
        CHECK_FALSE(refused.appended);
        CHECK(get(s.dir / "config.txt") == scoped);
    }
    put(s.dir / "config.txt", "Device: Speakers\r\nPreamp: -3 dB\r\nDevice: all\r\nInclude: Isotone.txt\r\n");
    CHECK(inspect_config(s.dir).isotone_included);
    CHECK_FALSE(inspect_config(s.dir).isotone_included_conditionally);
}

TEST_CASE("detach removes the block only while it is still the file's tail") {
    Sandbox s;
    put(s.dir / "config.txt", kOwnerConfig);
    REQUIRE(attach_include(s.dir).appended);

    // An edit above the block leaves it detachable, and the edit stays.
    const std::string edited = std::string("# my note\r\n") + kOwnerConfig;
    put(s.dir / "config.txt", edited + block("\r\n"));
    bool removed = false;
    REQUIRE(detach_include(s.dir, &removed) == ERROR_SUCCESS);
    CHECK(removed);
    CHECK(get(s.dir / "config.txt") == edited);

    // A line added after the block means the user changed it; hands off.
    REQUIRE(attach_include(s.dir).appended);
    const std::string after_block = get(s.dir / "config.txt") + "Preamp: -1 dB\r\n";
    put(s.dir / "config.txt", after_block);
    CHECK(detach_include(s.dir, &removed) == ERROR_INVALID_DATA);
    CHECK_FALSE(removed);
    CHECK(get(s.dir / "config.txt") == after_block);
}

TEST_CASE("an empty config.txt gets CRLF and nothing else") {
    Sandbox s;
    put(s.dir / "config.txt", "");
    REQUIRE(attach_include(s.dir).appended);
    CHECK(get(s.dir / "config.txt") == block("\r\n"));
}

TEST_CASE("missing or malformed config directories fail without creating anything") {
    Sandbox s;
    const fs::path missing = s.dir / "nope";
    CHECK(inspect_config(missing).error == ERROR_PATH_NOT_FOUND);
    CHECK(attach_include(missing).error == ERROR_PATH_NOT_FOUND);
    bool removed = false;
    CHECK(detach_include(missing, &removed) == ERROR_PATH_NOT_FOUND);
    CHECK_FALSE(fs::exists(missing));

    // A directory with no config.txt: not an install; do not invent one.
    CHECK(attach_include(s.dir).error == ERROR_FILE_NOT_FOUND);
    CHECK_FALSE(fs::exists(s.dir / "config.txt"));
    CHECK_FALSE(fs::exists(s.dir / kConfigBackupName));

    fs::create_directory(s.dir / "config.txt");
    CHECK(attach_include(s.dir).error == ERROR_DIRECTORY);

    CompatWriter writer(missing);
    CHECK(writer.load() == ERROR_FILE_NOT_FOUND);   // the directory itself is what is missing
}

// ---------------------------------------------------------------------------
// Isotone.txt

namespace {

constexpr uint32_t kSpeaker714 = 0x2D63F;
constexpr char kStereo[] = "{798436D2-8C71-4834-9248-00CCBAACA00A}";
constexpr char kHeight[] = "{11111111-2222-3333-4444-555555555555}";

Band band(FilterType type, double fc, double gain, double width, WidthMode mode,
          ChannelMask channels = kAllChannels, bool corner = false) {
    Band b;
    b.type = type;
    b.fc = fc;
    b.gain_db = gain;
    b.width = width;
    b.width_mode = mode;
    b.channels = channels;
    b.shelf_corner = corner;
    return b;
}

DeviceConfig stereo_device() {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {2, default_speaker_mask(2)};
    d.state.preamp_db = -6.1;
    d.state.bands.push_back(band(FilterType::LowShelf, 105, 6.4, 0.7, WidthMode::Q));
    d.state.bands.push_back(band(FilterType::Peaking, 1000.5, -3.25, 1.42, WidthMode::Q, 1u << 1));
    d.state.bands.push_back(band(FilterType::HighShelf, 3000, 4, 0.9 * 12, WidthMode::SlopeDb));
    d.state.bands.push_back(band(FilterType::Peaking, 250, 2, 0.5, WidthMode::BandwidthOct));
    d.state.channel_gain_db[0] = -1.5;
    d.state.speakers.delay_ms[1] = 2.5;
    d.state.speakers.inverted = 1u << 0;
    d.state.speakers.swap_left_right = true;
    return d;
}

DeviceConfig height_device() {
    DeviceConfig d;
    d.endpoint_guid = kHeight;
    d.layout = {12, kSpeaker714};
    d.state.bands.push_back(band(FilterType::Peaking, 8000, -6, 2, WidthMode::Q, 1u << 10));
    d.state.bands.push_back(band(FilterType::LowShelf, 80, 3, 0.7, WidthMode::Q, 1u << 3, true));
    d.state.channel_gain_db[7] = 2;
    d.state.mute = true;
    return d;
}

ChannelLayout layout_for(const std::string& guid) {
    if (guid.find("11111111") != std::string::npos) return {12, kSpeaker714};
    return {2, default_speaker_mask(2)};
}

void check_same_state(const EqState& a, const EqState& b) {
    CHECK(a.bypass == b.bypass);
    CHECK(a.mute == b.mute);
    CHECK(format_speaker_setup(a.speakers) == format_speaker_setup(b.speakers));
    CHECK(a.preamp_db == b.preamp_db);
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        CAPTURE(c);
        CHECK(a.channel_gain_db[c] == b.channel_gain_db[c]);
    }
    // format_apo_config writes bands grouped by channel mask, so they come back
    // in group order. A cascade of linear filters has the same response in any
    // order, so compare them as a set.
    REQUIRE(a.bands.size() == b.bands.size());
    const auto sorted = [](std::vector<Band> v) {
        std::sort(v.begin(), v.end(), [](const Band& x, const Band& y) {
            return std::tie(x.channels, x.fc, x.gain_db) < std::tie(y.channels, y.fc, y.gain_db);
        });
        return v;
    };
    const std::vector<Band> sa = sorted(a.bands), sb = sorted(b.bands);
    for (size_t i = 0; i < sa.size(); ++i) {
        CAPTURE(i);
        CHECK(sa[i].type == sb[i].type);
        CHECK(sa[i].fc == sb[i].fc);
        CHECK(sa[i].gain_db == sb[i].gain_db);
        CHECK(sa[i].width == sb[i].width);
        CHECK(sa[i].width_mode == sb[i].width_mode);
        CHECK(sa[i].channels == sb[i].channels);
        CHECK(sa[i].shelf_corner == sb[i].shelf_corner);
        CHECK(sa[i].enabled == sb[i].enabled);
    }
}

}  // namespace

TEST_CASE("Isotone.txt re-parses to the same state for every device") {
    const DeviceConfig a = stereo_device();
    const DeviceConfig b = height_device();
    const std::string text = update_isotone_file(update_isotone_file("", a), b);
    MESSAGE(text);

    const std::vector<ParsedDevice> parsed = parse_isotone_file(text, layout_for);
    REQUIRE(parsed.size() == 2);
    CHECK(parsed[0].endpoint_guid == kStereo);
    CHECK(parsed[1].endpoint_guid == kHeight);
    for (size_t i = 0; i < 2; ++i) {
        CAPTURE(i);
        CHECK(parsed[i].warnings.empty());
        CHECK(parsed[i].unsupported.empty());
    }
    check_same_state(parsed[0].state, a.state);
    check_same_state(parsed[1].state, b.state);

    SUBCASE("bypassed, the curve is kept, and upstream plays no preamp and no bands") {
        DeviceConfig off = a;
        off.state.bypass = true;
        const std::string bypassed = update_isotone_file(text, off);
        const std::vector<ParsedDevice> back = parse_isotone_file(bypassed, layout_for);
        REQUIRE(back.size() == 2);
        CHECK(back[0].warnings.empty());
        check_same_state(back[0].state, off.state);

        // Upstream's view: no band is a command, and the one Preamp left is the
        // trim under its Channel line.
        const std::string block_text = format_device_block(off);
        std::istringstream in(block_text);
        std::string line, previous;
        int filters = 0, preamps = 0;
        while (std::getline(in, line)) {
            if (line.rfind("Filter ", 0) == 0) ++filters;
            if (line.rfind("Preamp:", 0) == 0) {
                ++preamps;
                CHECK(previous == "Channel: L");
            }
            previous = line;
        }
        CHECK(filters == 0);
        CHECK(preamps == 1);
    }

    SUBCASE("a bypassed block written before bypass left the speaker setup on still reads") {
        const std::string old =
            "Device: {798436D2-8C71-4834-9248-00CCBAACA00A}\nChannel: all\n# Isotone: bypass\n"
            "# Preamp: -3 dB\n# Filter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n";
        const std::vector<ParsedDevice> back = parse_isotone_file(old, layout_for);
        REQUIRE(back.size() == 1);
        CHECK(back[0].warnings.empty());
        CHECK(back[0].state.bypass);
        CHECK(back[0].state.preamp_db == -3.0);
        CHECK(back[0].state.bands.size() == 1);
    }
}

TEST_CASE("the file speaks only upstream's commands") {
    const std::string text =
        update_isotone_file(update_isotone_file("", stereo_device()), height_device());
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::string key = line.substr(0, line.find(':'));
        CAPTURE(line);
        CHECK((key == "Device" || key == "Channel" || key == "Preamp" || key == "Copy" ||
               key == "Delay" || key == "Filter" || key.rfind("Filter ", 0) == 0 || key == "If" || key == "EndIf"));
    }
    CHECK(text.find("Device: {798436D2-8C71-4834-9248-00CCBAACA00A}\nChannel: all\n") !=
          std::string::npos);
}

namespace {

// Runs the commands of one Isotone.txt block the way upstream's FilterEngine
// does, for the commands Isotone writes, so the text can be checked against the
// processor without an Equalizer APO install. Semantics as read in upstream at
// the pinned commit:
//   Channel  selects channels by name, or all of them (ChannelFilter).
//   Preamp   gain on the selection (PreampFilter).
//   Filter   one biquad per selected channel, in place (BiQuadFilter).
//   Copy     every target computed from the inputs before any is written; a
//            target that is not a channel becomes a virtual channel starting at
//            zero; channels that are not targets keep their samples
//            (CopyFilter, FilterConfiguration::process).
//   Delay    whole samples, rate * ms / 1000 + 0.5, on the selection (DelayFilter).
//   If       outputChannelCount == N and sampleRate >= X, the forms Isotone
//            writes; lines up to the matching EndIf are skipped when false.
// A channel word is a 1-based number within the device's channel count, or a
// name, with the SL/RL and SR/RR substitutes (ChannelHelper). A Copy source that
// names no channel is added as its factor, a constant (CopyFilter::process).
class UpstreamModel {
public:
    UpstreamModel(const ChannelLayout& layout, double rate) : rate_(rate) {
        names_ = apo_channel_names(layout);
        names_.resize(layout.channels);
    }

    // Whether `expression` holds on this device; only the forms Isotone writes.
    bool holds(const std::string& expression) {
        std::istringstream in(expression);
        std::string name, op;
        double value = 0;
        in >> name >> op >> value;
        if (name == "outputChannelCount" && op == "==") return names_.size() == value;
        if (name == "sampleRate" && op == ">=") return rate_ >= value;
        FAIL("an If the model does not know: " << expression);
        return false;
    }

    // [channel][frame] in, the device channels out.
    std::vector<std::vector<double>> run(const std::string& block, std::vector<std::vector<double>> x) {
        const size_t frames = x[0].size();
        std::vector<long> selection;
        for (size_t c = 0; c < names_.size(); ++c) selection.push_back(static_cast<long>(c));
        const std::vector<long> all = selection;
        int skipping = 0;   // depth of false Ifs
        std::istringstream in(block);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            const size_t colon = line.find(':');
            REQUIRE(colon != std::string::npos);
            const std::string key = line.substr(0, colon);
            const std::string value = line.substr(colon + 1);
            std::istringstream words(value);
            if (key == "If") {
                if (skipping > 0 || !holds(value)) ++skipping;
                continue;
            }
            if (key == "EndIf") {
                if (skipping > 0) --skipping;
                continue;
            }
            if (skipping > 0) continue;
            if (key == "Device") continue;
            if (key == "Channel") {
                selection.clear();
                std::string w;
                while (words >> w) {
                    if (w == "all") selection = all;
                    else if (index(w) >= 0) selection.push_back(index(w));
                }
            } else if (key == "Preamp") {
                double db = 0;
                words >> db;
                const double gain = static_cast<float>(std::pow(10.0, db / 20.0));   // upstream's float gain
                for (long n : selection) for (double& v : x[n]) v *= gain;
            } else if (key == "Filter" || key.rfind("Filter ", 0) == 0) {
                const ApoParseResult r = parse_apo_config(line + "\n");
                REQUIRE(r.state.bands.size() == 1);
                const BiquadCoeffs k = design(r.state.bands[0], rate_);
                for (long n : selection) {
                    double s1 = 0, s2 = 0;
                    for (double& v : x[n]) {
                        const double y = k.b0 * v + s1;
                        s1 = k.b1 * v - k.a1 * y + s2;
                        s2 = k.b2 * v - k.a2 * y;
                        v = y;
                    }
                }
            } else if (key == "Copy") {
                const std::vector<std::vector<double>> input = x;
                std::string assignment;
                while (words >> assignment) {
                    const size_t eq = assignment.find('=');
                    const std::string target = assignment.substr(0, eq);
                    std::vector<double> out(frames, 0.0);
                    std::istringstream sum(assignment.substr(eq + 1));
                    std::string term;
                    while (std::getline(sum, term, '+')) {
                        const size_t star = term.find('*');
                        std::string factor_text, channel;
                        if (star != std::string::npos) {
                            factor_text = term.substr(0, star);
                            channel = term.substr(star + 1);
                        } else if (term == "0" || term.find('.') != std::string::npos) {
                            factor_text = term;
                        } else {
                            channel = term;
                        }
                        const double factor = factor_text.empty() ? 1.0 : std::stod(factor_text);
                        const long source = channel.empty() ? -1 : index(channel);
                        for (size_t f = 0; f < frames; ++f) out[f] += source < 0 ? factor : factor * input[source][f];
                    }
                    long t = index(target);
                    if (t < 0) {
                        virtuals_.push_back(target);
                        x.emplace_back(frames, 0.0);
                        t = static_cast<long>(x.size() - 1);
                    }
                    x[t] = out;
                }
            } else if (key == "Delay") {
                double ms = 0;
                std::string unit;
                words >> ms >> unit;
                REQUIRE(unit == "ms");
                const size_t n = static_cast<size_t>(rate_ * ms / 1000.0 + 0.5);
                for (long c : selection) {
                    std::vector<double>& v = x[c];
                    v.insert(v.begin(), n, 0.0);
                    v.resize(frames);
                }
            } else {
                FAIL("a command the model does not know: " << line);
            }
        }
        x.resize(names_.size());
        return x;
    }

private:
    // -1 for a word that names no channel.
    long index(const std::string& name) {
        if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) {
            const long n = std::stol(name) - 1;
            return n >= 0 && n < static_cast<long>(names_.size()) ? n : -1;
        }
        const auto find = [&](const std::string& w) -> long {
            auto it = std::find(names_.begin(), names_.end(), w);
            if (it != names_.end()) return static_cast<long>(it - names_.begin());
            auto v = std::find(virtuals_.begin(), virtuals_.end(), w);
            return v != virtuals_.end() ? static_cast<long>(names_.size() + (v - virtuals_.begin())) : -1;
        };
        long i = find(name);
        if (i < 0 && name == "SL") i = find("RL");
        if (i < 0 && name == "SR") i = find("RR");
        if (i < 0 && name == "RL") i = find("SL");
        if (i < 0 && name == "RR") i = find("SR");
        return i;
    }

    double rate_;
    std::vector<std::string> names_;
    std::vector<std::string> virtuals_;
};

}  // namespace

TEST_CASE("the text Isotone writes does what the processor does") {
    constexpr double kRate = 48000.0;
    constexpr size_t kFrames = 24000;
    // 5.1 surround: L R C LFE SL SR. Every speaker feature at once, plus bands
    // and a trim, so a difference in any stage or in their order shows up.
    const ChannelLayout layout{6, 0x60F};
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = layout;
    d.state.preamp_db = -4;
    d.state.bands.push_back(band(FilterType::Peaking, 1000, -6, 1.4, WidthMode::Q));
    d.state.bands.push_back(band(FilterType::LowShelf, 120, 5, 0.7, WidthMode::Q, 1u << 3));
    d.state.bands.push_back(band(FilterType::HighShelf, 6000, 3, 0.7, WidthMode::Q, (1u << 0) | (1u << 4)));
    d.state.channel_gain_db[1] = -2.5;
    SpeakerSetup& sp = d.state.speakers;
    sp.swap_left_right = true;
    sp.upmix = Upmix::All;
    sp.bass_management = true;
    sp.crossover_hz = 90;
    sp.lfe_lowpass_hz = 110;
    sp.small_speakers = 0x37;   // L R C SL SR
    sp.inverted = 1u << 4;
    sp.muted = 1u << 5;
    sp.delay_ms[0] = 1.2;
    sp.delay_ms[2] = 0.8;
    sp.lip_sync_ms = 3;

    // A different mixture of bass, mids and treble on every channel.
    std::vector<std::vector<double>> input(6, std::vector<double>(kFrames));
    for (uint32_t c = 0; c < 6; ++c) {
        for (size_t f = 0; f < kFrames; ++f) {
            const double t = static_cast<double>(f) / kRate;
            input[c][f] = 0.3 * std::sin(2 * 3.14159265358979 * (45.0 + 7 * c) * t) +
                          0.2 * std::sin(2 * 3.14159265358979 * (900.0 + 110 * c) * t + c) +
                          0.1 * std::sin(2 * 3.14159265358979 * (7000.0 - 300 * c) * t);
        }
    }

    const std::string block = format_device_block(d);
    MESSAGE(block);
    UpstreamModel model(layout, kRate);
    const std::vector<std::vector<double>> expected = model.run(block, input);

    Processor p;
    p.initialize(kRate, 6, 480, 64, layout.speaker_mask);
    p.set_target(d.state);
    p.reset();
    std::vector<std::vector<float>> buf(6, std::vector<float>(480));
    std::vector<float*> ptr(6);
    double worst = 0.0;
    size_t worst_c = 0, worst_f = 0;
    for (size_t pos = 0; pos < kFrames; pos += 480) {
        for (uint32_t c = 0; c < 6; ++c) {
            for (size_t i = 0; i < 480; ++i) buf[c][i] = static_cast<float>(input[c][pos + i]);
            ptr[c] = buf[c].data();
        }
        p.process(ptr.data(), 480);
        for (uint32_t c = 0; c < 6; ++c) {
            for (size_t i = 0; i < 480; ++i) {
                const double diff = std::abs(buf[c][i] - expected[c][pos + i]);
                if (diff > worst) {
                    worst = diff;
                    worst_c = c;
                    worst_f = pos + i;
                }
            }
        }
    }
    CAPTURE(worst_c);
    CAPTURE(worst_f);
    CHECK(worst < 1e-4);

    // Not vacuous: the stages changed the signal a lot.
    double moved = 0.0;
    for (uint32_t c = 0; c < 6; ++c) {
        for (size_t f = 12000; f < kFrames; ++f) moved = std::max(moved, std::abs(expected[c][f] - input[c][f]));
    }
    CHECK(moved > 0.2);

    // And the settings come back out of the file.
    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return layout; });
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].warnings.empty());
    CHECK(format_speaker_setup(parsed[0].state.speakers) == format_speaker_setup(sp));
    CHECK(parsed[0].state.bands.size() == 3);
}

TEST_CASE("a band beyond the device's Nyquist is written at the frequency the processor designs") {
    DeviceConfig d = stereo_device();
    d.sample_rate = 44100.0;
    d.state.bands.clear();
    d.state.bands.push_back(band(FilterType::Peaking, 30000, -6, 1, WidthMode::Q));
    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return d.layout; });
    REQUIRE(parsed.size() == 1);
    REQUIRE(parsed[0].state.bands.size() == 1);
    CHECK(parsed[0].state.bands[0].fc == doctest::Approx(clamp_fc(30000, 44100.0)));
}

TEST_CASE("updating one device keeps every other byte of the file") {
    DeviceConfig a = stereo_device();
    DeviceConfig b = height_device();
    DeviceConfig c = stereo_device();
    c.endpoint_guid = "{22222222-0000-0000-0000-000000000000}";
    const std::string text = update_isotone_file(update_isotone_file(update_isotone_file("", a), b), c);

    const size_t b_at = text.find("Device: {11111111");
    const size_t c_at = text.find("Device: {22222222");
    REQUIRE(b_at != std::string::npos);
    REQUIRE(c_at != std::string::npos);
    const std::string before_b = text.substr(0, b_at);
    const std::string from_c = text.substr(c_at);

    // Braces and case do not make a different device.
    b.endpoint_guid = "11111111-2222-3333-4444-555555555555";
    b.state.bands[0].gain_db = -9;
    const std::string updated = update_isotone_file(text, b);
    CHECK(updated.substr(0, b_at) == before_b);
    CHECK(updated.size() - updated.find("Device: {22222222") == from_c.size());
    CHECK(updated.substr(updated.find("Device: {22222222")) == from_c);
    CHECK(updated.find("Gain -9 dB") != std::string::npos);
    CHECK(update_isotone_file(updated, b) == updated);

    const std::string removed = remove_device(updated, kHeight);
    CHECK(removed == before_b + from_c);
    CHECK(remove_device(removed, kHeight) == removed);
}

// ---------------------------------------------------------------------------
// Rate limiting

namespace {

struct FakeClock {
    std::chrono::steady_clock::time_point now{};
    void advance(int ms) { now += std::chrono::milliseconds(ms); }
    WriteCoalescer::Clock fn() {
        return [this] { return now; };
    }
};

}  // namespace

TEST_CASE("the first edit goes out at once, then the latest edit per interval") {
    FakeClock clock;
    std::vector<std::string> written;
    WriteCoalescer w([&](const std::string& s) { written.push_back(s); return DWORD{ERROR_SUCCESS}; },
                     std::chrono::milliseconds(33), clock.fn());

    REQUIRE(w.submit("a") == ERROR_SUCCESS);
    CHECK(written == std::vector<std::string>{"a"});

    clock.advance(5);
    w.submit("b");
    clock.advance(5);
    w.submit("c");
    CHECK(written.size() == 1);
    CHECK(w.has_pending());
    CHECK(w.time_until_due() == std::chrono::milliseconds(23));

    clock.advance(22);
    w.poll();
    CHECK(written.size() == 1);
    clock.advance(1);
    w.poll();
    CHECK(written == std::vector<std::string>{"a", "c"});
    CHECK_FALSE(w.has_pending());
}

TEST_CASE("a one-second drag at 1 kHz of edits becomes about 30 writes, ending on the last") {
    FakeClock clock;
    std::vector<std::string> written;
    WriteCoalescer w([&](const std::string& s) { written.push_back(s); return DWORD{ERROR_SUCCESS}; },
                     std::chrono::milliseconds(33), clock.fn());

    for (int i = 0; i < 1000; ++i) {
        w.submit("edit " + std::to_string(i));
        w.poll();
        clock.advance(1);
    }
    // The trailing write lands on the next due poll, with no further edits.
    for (int i = 0; i < 40 && w.has_pending(); ++i) {
        clock.advance(1);
        w.poll();
    }
    CAPTURE(written.size());
    CHECK(written.size() >= 30);
    CHECK(written.size() <= 32);
    CHECK(written.back() == "edit 999");
    CHECK_FALSE(w.has_pending());
}

TEST_CASE("identical content is not rewritten, and a failed write stays pending") {
    FakeClock clock;
    int writes = 0;
    bool fail = false;
    WriteCoalescer w(
        [&](const std::string&) {
            if (fail) return DWORD{ERROR_SHARING_VIOLATION};
            ++writes;
            return DWORD{ERROR_SUCCESS};
        },
        std::chrono::milliseconds(33), clock.fn());

    w.submit("same");
    clock.advance(100);
    w.submit("same");
    CHECK(writes == 1);

    fail = true;
    clock.advance(100);
    CHECK(w.submit("next") == ERROR_SHARING_VIOLATION);
    CHECK(w.has_pending());
    fail = false;
    // A failed attempt starts the interval too, so a failing sink is not hit on
    // every edit or poll.
    int attempts_while_failing = 0;
    WriteCoalescer counted(
        [&](const std::string&) {
            ++attempts_while_failing;
            return DWORD{ERROR_ACCESS_DENIED};
        },
        std::chrono::milliseconds(33), clock.fn());
    counted.submit("a");
    counted.submit("b");
    counted.submit("c");
    counted.poll();
    CHECK(attempts_while_failing == 1);
    clock.advance(34);
    counted.poll();
    CHECK(attempts_while_failing == 2);

    CHECK(w.poll() == ERROR_SUCCESS);   // due: the clock has moved past the interval
    CHECK(writes == 2);
    CHECK_FALSE(w.has_pending());
}

TEST_CASE("the writer coalesces live edits on disk and persists at once") {
    Sandbox s;
    FakeClock clock;
    CompatWriter writer(s.dir, clock.fn());
    REQUIRE(writer.load() == ERROR_SUCCESS);
    CHECK(writer.text().empty());

    DeviceConfig d = stereo_device();
    REQUIRE(writer.apply(d) == ERROR_SUCCESS);
    CHECK(writer.writes() == 1);
    CHECK(get(writer.path()) == writer.text());

    clock.advance(10);
    d.state.preamp_db = -7;
    REQUIRE(writer.apply(d) == ERROR_SUCCESS);
    CHECK(writer.writes() == 1);
    CHECK(writer.has_pending());
    CHECK(get(writer.path()) != writer.text());

    clock.advance(10);
    d.state.preamp_db = -8;
    REQUIRE(writer.persist(d) == ERROR_SUCCESS);
    CHECK(writer.writes() == 2);
    CHECK_FALSE(writer.has_pending());
    CHECK(get(writer.path()) == writer.text());
    CHECK_FALSE(fs::exists(s.dir / "Isotone.txt.tmp"));

    // A second writer on the same directory picks up what is there.
    CompatWriter again(s.dir, clock.fn());
    REQUIRE(again.load() == ERROR_SUCCESS);
    CHECK(again.text() == writer.text());

    // And each keeps the other's devices: a write never reverts a block the
    // other wrote since this one last read the file.
    DeviceConfig other = height_device();
    REQUIRE(again.persist(other) == ERROR_SUCCESS);
    d.state.preamp_db = -9;
    REQUIRE(writer.persist(d) == ERROR_SUCCESS);
    const std::vector<ParsedDevice> both = parse_isotone_file(get(writer.path()), layout_for);
    REQUIRE(both.size() == 2);
    CHECK(both[0].state.preamp_db == -9.0);
    CHECK(both[1].endpoint_guid == kHeight);
}

TEST_CASE("a live edit still pending when the writer goes away is written") {
    Sandbox s;
    FakeClock clock;
    DeviceConfig d = stereo_device();
    {
        CompatWriter writer(s.dir, clock.fn());
        REQUIRE(writer.load() == ERROR_SUCCESS);
        REQUIRE(writer.apply(d) == ERROR_SUCCESS);
        d.state.preamp_db = -11;
        REQUIRE(writer.apply(d) == ERROR_SUCCESS);
        REQUIRE(writer.has_pending());
    }
    const std::vector<ParsedDevice> back = parse_isotone_file(get(s.dir / "Isotone.txt"), layout_for);
    REQUIRE(back.size() == 1);
    CHECK(back[0].state.preamp_db == -11.0);
}

// ---------------------------------------------------------------------------
// Defects found by the 2026-09-13 review

namespace {

std::string utf8_of(const fs::path& p) {
    const std::wstring& w = p.native();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Runs the block for a 5.1 device through the upstream model and the
// processor, returning the worst sample difference.
double model_vs_processor(const DeviceConfig& d) {
    constexpr double kRate = 48000.0;
    constexpr size_t kFrames = 24000;
    const ChannelLayout layout = d.layout;
    std::vector<std::vector<double>> input(layout.channels, std::vector<double>(kFrames));
    for (uint32_t c = 0; c < layout.channels; ++c) {
        for (size_t f = 0; f < kFrames; ++f) {
            const double t = static_cast<double>(f) / kRate;
            input[c][f] = 0.4 * std::sin(2 * 3.14159265358979 * (40.0 + 9 * c) * t) +
                          0.2 * std::sin(2 * 3.14159265358979 * (1500.0 + 70 * c) * t);
        }
    }
    UpstreamModel model(layout, kRate);
    const std::vector<std::vector<double>> expected = model.run(format_device_block(d), input);
    Processor p;
    p.initialize(kRate, layout.channels, 480, 64, layout.speaker_mask);
    p.set_target(d.state);
    p.reset();
    std::vector<std::vector<float>> buf(layout.channels, std::vector<float>(480));
    std::vector<float*> ptr(layout.channels);
    double worst = 0.0;
    for (size_t pos = 0; pos < kFrames; pos += 480) {
        for (uint32_t c = 0; c < layout.channels; ++c) {
            for (size_t i = 0; i < 480; ++i) buf[c][i] = static_cast<float>(input[c][pos + i]);
            ptr[c] = buf[c].data();
        }
        p.process(ptr.data(), 480);
        for (uint32_t c = 0; c < layout.channels; ++c) {
            for (size_t i = 0; i < 480; ++i) worst = std::max(worst, std::abs(buf[c][i] - expected[c][pos + i]));
        }
    }
    return worst;
}

}  // namespace

TEST_CASE("a crossover with three decimals reaches Equalizer APO as written, not 1000 times higher") {
    // Upstream reads "80.125" as Room EQ Wizard's thousands separator. The
    // model parses Filter lines with the same rule, so a crossover written that
    // way would be designed at 80125 Hz and the outputs would disagree.
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = ChannelLayout{6, 0x60F};
    d.state.speakers.bass_management = true;
    d.state.speakers.small_speakers = 0x37;
    d.state.speakers.crossover_hz = 80.125;
    d.state.speakers.lfe_lowpass_hz = 150.25;
    const std::string block = format_device_block(d);
    CAPTURE(block);
    CHECK(block.find("Fc 80.1250 Hz") != std::string::npos);
    CHECK(model_vs_processor(d) < 1e-4);
}

TEST_CASE("mute in Isotone.txt is silence, and reads back as mute") {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = ChannelLayout{6, 0x60F};
    d.state.bands.push_back(band(FilterType::Peaking, 1000, 12, 1, WidthMode::Q));
    d.state.mute = true;
    const std::string block = format_device_block(d);
    CAPTURE(block);
    // Not a -100 dB cut: a gain that underflows upstream's float to zero.
    CHECK(block.find("Preamp: -100 dB\n") == std::string::npos);
    CHECK(block.find("Preamp: -1000 dB\n") != std::string::npos);
    CHECK(model_vs_processor(d) < 1e-6);
    // Silence on whatever layout the device reports when Equalizer APO loads.
    for (const ChannelLayout device : {ChannelLayout{2, 0x3}, ChannelLayout{8, 0x63F}}) {
        std::vector<std::vector<double>> input(device.channels, std::vector<double>(480, 0.5));
        const std::vector<std::vector<double>> out = UpstreamModel(device, 48000.0).run(block, input);
        double loudest = 0.0;
        for (const auto& ch : out) for (double v : ch) loudest = std::max(loudest, std::abs(v));
        CAPTURE(device.channels);
        CHECK(loudest == 0.0);
    }
    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return d.layout; });
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].state.mute);
    CHECK(parsed[0].unsupported.empty());
    CHECK(parsed[0].state.bands.size() == 1);
}

TEST_CASE("a layout given with no speaker mask gets the default one, so speaker features are written") {
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = ChannelLayout{2, 0};
    d.state.speakers.swap_left_right = true;
    const std::string block = format_device_block(d);
    CAPTURE(block);
    CHECK(block.find("Copy: 1=1*2 2=1*1") != std::string::npos);
}

TEST_CASE("a block written for one layout plays no DC on a device that reports another") {
    // Equalizer APO applies Isotone.txt to the format the device has when it
    // loads, which can differ from the one Isotone wrote for. A Copy source
    // naming a channel the device lacks is added as a constant: full-scale DC.
    const ChannelLayout layouts[] = {{2, 0x3},   {3, 0xB},   {3, 0x7},   {4, 0x33},
                                     {4, 0x107}, {6, 0x60F}, {6, 0x3F},  {8, 0x63F}};
    for (const ChannelLayout& written : layouts) {
        DeviceConfig d;
        d.endpoint_guid = kStereo;
        d.layout = written;
        d.state.bands.push_back(band(FilterType::Peaking, 1000, -6, 1, WidthMode::Q));
        SpeakerSetup& sp = d.state.speakers;
        sp.swap_left_right = true;
        sp.swap_front_rear = true;
        sp.upmix = Upmix::All;
        sp.bass_management = true;
        sp.small_speakers = 0xFF;
        sp.inverted = 1u << 1;
        sp.muted = 1u << 2;
        sp.delay_ms[0] = 1.0;
        const std::string block = format_device_block(d);
        CAPTURE(block);
        // On its own layout the text is still what the processor does.
        CHECK(model_vs_processor(d) < 1e-4);
        for (const ChannelLayout& device : layouts) {
            CAPTURE(device.channels);
            CAPTURE(device.speaker_mask);
            constexpr size_t kFrames = 9600;
            std::vector<std::vector<double>> input(device.channels, std::vector<double>(kFrames));
            for (uint32_t c = 0; c < device.channels; ++c) {
                for (size_t f = 0; f < kFrames; ++f) {
                    input[c][f] = 0.5 * std::sin(2 * 3.14159265358979 * (500.0 + 100 * c) * f / 48000.0);
                }
            }
            const std::vector<std::vector<double>> out = UpstreamModel(device, 48000.0).run(block, input);
            for (uint32_t c = 0; c < device.channels; ++c) {
                double mean = 0.0;
                for (size_t f = kFrames / 2; f < kFrames; ++f) mean += out[c][f];
                mean /= static_cast<double>(kFrames / 2);
                CAPTURE(c);
                CHECK(std::abs(mean) < 1e-3);
            }
        }
    }
}

TEST_CASE("bands are guarded by the lowest rate they are stable at") {
    // A band written at 22800 Hz for a 48 kHz device is above Nyquist at
    // 44.1 kHz, where upstream designs it unstable.
    DeviceConfig d;
    d.endpoint_guid = kStereo;
    d.layout = {2, 0x3};
    d.sample_rate = 48000.0;
    d.state.channel_gain_db[0] = -2.0;
    d.state.bands.push_back(band(FilterType::Peaking, 23000, -6, 1, WidthMode::Q));
    d.state.bands.push_back(band(FilterType::Peaking, 1000, -6, 1, WidthMode::Q));
    const std::string block = format_device_block(d);
    CAPTURE(block);
    CHECK(block.find("If: sampleRate >= 48000\n") != std::string::npos);

    std::vector<std::vector<double>> input(2, std::vector<double>(4800));
    for (size_t f = 0; f < 4800; ++f) input[0][f] = input[1][f] = std::sin(2 * 3.14159265358979 * 1000.0 * f / 44100.0);
    // At 44.1 kHz the bands do nothing, and the trim, which has no frequency, stays.
    const std::vector<std::vector<double>> low = UpstreamModel(d.layout, 44100.0).run(block, input);
    double err = 0.0;
    for (size_t f = 0; f < 4800; ++f) {
        err = std::max(err, std::abs(low[1][f] - input[1][f]));
        err = std::max(err, std::abs(low[0][f] - input[0][f] * static_cast<float>(std::pow(10.0, -2.0 / 20.0))));
    }
    CHECK(err < 1e-6);
    // At the rate it was written for it is the processor's curve.
    CHECK(model_vs_processor(d) < 1e-4);

    const auto parsed = parse_isotone_file(update_isotone_file("", d), [&](const std::string&) { return d.layout; });
    REQUIRE(parsed.size() == 1);
    CHECK(parsed[0].warnings.empty());
    CHECK(parsed[0].unsupported.empty());
    CHECK(parsed[0].state.bands.size() == 2);

    // Low bands need no more than a low rate.
    d.state.bands.erase(d.state.bands.begin());
    CHECK(format_device_block(d).find("If: sampleRate >= 2106\n") != std::string::npos);
}

TEST_CASE("bypass turns off the preamp and bands, and nothing else, in Equalizer APO too") {
    DeviceConfig d = stereo_device();
    d.state.bypass = true;
    d.state.mute = false;
    CHECK(model_vs_processor(d) < 1e-4);
    DeviceConfig on = d;
    on.state.bypass = false;
    CHECK(model_vs_processor(on) < 1e-4);
}

TEST_CASE("a second block for the same device is removed on update and reported on read") {
    DeviceConfig a = stereo_device();
    DeviceConfig other = height_device();
    const std::string one = format_device_block(a);
    a.state.preamp_db = -9;
    const std::string stale = format_device_block(a);
    const std::string text = one + "\n" + format_device_block(other) + "\n" + stale;
    const std::vector<ParsedDevice> read = parse_isotone_file(text, layout_for);
    REQUIRE(read.size() == 3);
    CHECK(read[0].warnings.empty());
    CHECK_FALSE(read[2].warnings.empty());

    a.state.preamp_db = -1;
    const std::string updated = update_isotone_file(text, a);
    const std::vector<ParsedDevice> back = parse_isotone_file(updated, layout_for);
    REQUIRE(back.size() == 2);
    CHECK(back[0].state.preamp_db == -1.0);
    CHECK(back[1].endpoint_guid == kHeight);
    CHECK(remove_device(text, kStereo) == format_device_block(other) + "\n");
}

TEST_CASE("a full device ID names the endpoint by its GUID") {
    DeviceConfig d = stereo_device();
    d.endpoint_guid = "{0.0.0.00000000}.{798436D2-8C71-4834-9248-00CCBAACA00A}";
    const std::string text = update_isotone_file("", d);
    CHECK(text.find("Device: {798436D2-8C71-4834-9248-00CCBAACA00A}\n") != std::string::npos);
    DeviceConfig bare = stereo_device();
    bare.state.preamp_db = -2;
    const std::vector<ParsedDevice> back = parse_isotone_file(update_isotone_file(text, bare), layout_for);
    REQUIRE(back.size() == 1);
    CHECK(back[0].state.preamp_db == -2.0);
}

TEST_CASE("a routing section that lost its end marker costs the section, not the curve") {
    const std::string text = "Device: {798436D2-8C71-4834-9248-00CCBAACA00A}\nChannel: all\n# Isotone: routing\n"
                             "If: outputChannelCount == 2\nCopy: 1=1*2 2=1*1\nEndIf:\nChannel: all\n"
                             "Preamp: -3 dB\nFilter 1: ON PK Fc 1000 Hz Gain -6 dB Q 1\n";
    const std::vector<ParsedDevice> back = parse_isotone_file(text, layout_for);
    REQUIRE(back.size() == 1);
    CHECK_FALSE(back[0].warnings.empty());
    CHECK(back[0].state.preamp_db == -3.0);
    CHECK(back[0].state.bands.size() == 1);
}

TEST_CASE("an Include of Isotone.txt by absolute path counts as attached") {
    Sandbox s;
    const std::string absolute = "Include: " + utf8_of(s.dir / "Isotone.txt") + "\r\n";
    put(s.dir / "config.txt", std::string(kOwnerConfig) + absolute);
    CHECK(inspect_config(s.dir).isotone_included);
    const AttachResult r = attach_include(s.dir);
    CHECK(r.error == ERROR_SUCCESS);
    CHECK_FALSE(r.appended);
    CHECK(get(s.dir / "config.txt") == std::string(kOwnerConfig) + absolute);

    // An Isotone.txt somewhere else is a different file.
    Sandbox other;
    put(s.dir / "config.txt", std::string(kOwnerConfig) + "Include: " + utf8_of(other.dir / "Isotone.txt") + "\r\n");
    CHECK_FALSE(inspect_config(s.dir).isotone_included);
}

TEST_CASE("attach and detach refuse a config.txt that is a hard link to another file") {
    // A link planted in a sandbox would otherwise carry the append or the
    // truncation to the file it points at, such as the live config.txt.
    Sandbox s;
    Sandbox elsewhere;
    const fs::path target = elsewhere.dir / "config.txt";
    put(target, kOwnerConfig);
    REQUIRE(CreateHardLinkW((s.dir / "config.txt").c_str(), target.c_str(), nullptr));
    CHECK(attach_include(s.dir).error == ERROR_CANT_ACCESS_FILE);
    CHECK(get(target) == kOwnerConfig);

    put(target, std::string(kOwnerConfig) + block("\r\n"));   // as if attached
    bool removed = false;
    CHECK(detach_include(s.dir, &removed) == ERROR_CANT_ACCESS_FILE);
    CHECK_FALSE(removed);
    CHECK(get(target) == std::string(kOwnerConfig) + block("\r\n"));
}

TEST_CASE("an atomic write does not write through a .tmp name planted as a hard link") {
    Sandbox s;
    const fs::path victim = s.dir / "victim.txt";
    put(victim, "keep me");
    REQUIRE(CreateHardLinkW((s.dir / "Isotone.txt.tmp").c_str(), victim.c_str(), nullptr));
    REQUIRE(write_file_atomically(s.dir / "Isotone.txt", "new content") == ERROR_SUCCESS);
    CHECK(get(victim) == "keep me");
    CHECK(get(s.dir / "Isotone.txt") == "new content");
}

TEST_CASE("detach waits for a writer holding config.txt, then cuts the file it checked") {
    Sandbox s;
    const fs::path config = s.dir / "config.txt";
    put(config, kOwnerConfig);
    REQUIRE(attach_include(s.dir).appended);
    // Another program (Peace, an editor) has the file open for writing.
    HANDLE held = CreateFileW(config.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CloseHandle(held);
    });
    bool removed = false;
    const DWORD e = detach_include(s.dir, &removed);
    release.join();
    CHECK(e == ERROR_SUCCESS);
    CHECK(removed);
    CHECK(get(config) == kOwnerConfig);
}
