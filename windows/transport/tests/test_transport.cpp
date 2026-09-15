// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The transport's naming and saved-state file, without IsoAPO or the registry.
// isotone-apo-selftest checks the same through the APO; these run in ctest.

#include "doctest.h"

#include <windows.h>

#include <cwctype>
#include <string>

#include "isotone/param_block.h"
#include "isotone/types.h"
#include "persisted_state.h"
#include "shared_mapping.h"

using namespace isotone;

namespace {

// A directory under %TEMP% that is removed with everything in it.
struct TempDir {
    std::wstring path;
    TempDir() {
        wchar_t base[MAX_PATH];
        const DWORD n = GetTempPathW(MAX_PATH, base);
        REQUIRE(n > 0);
        path = std::wstring(base) + L"isotone-transport-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64());
    }
    ~TempDir() {
        const std::wstring devices = path + L"\\devices";
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW((devices + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((devices + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(devices.c_str());
        RemoveDirectoryW(path.c_str());
    }
};

// For CAPTURE: the GUID forms are ASCII.
std::string narrow(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) out += static_cast<char>(c);
    return out;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

}  // namespace

TEST_CASE("every form of an endpoint GUID names the same region and saved file") {
    // IsoAPO has PKEY_AudioEndpoint_GUID, braced upper case; the UI and the tools
    // may pass a bare GUID or the full IMMDevice::GetId string. A raw string in
    // the name gave a region that never existed (review 2026-09-13).
    const std::wstring upper = L"{8F4D2A10-AAAA-BBBB-CCCC-DDDDEEEEFFFF}";
    const std::wstring bare = upper.substr(1, 36);
    const std::wstring want_name = L"Local\\IsoAPO." + lower(upper);
    const std::wstring want_file = L"C:\\dir\\" + lower(upper) + L".bin";
    for (const std::wstring& form : {upper, lower(upper), bare, lower(bare), L"{0.0.0.00000000}." + lower(upper),
                                     L"{0.0.1.00000000}." + upper, L" \t" + upper + L"\r\n"}) {
        CAPTURE(narrow(form));
        CHECK(win::mapping_name(L"Local\\", form) == want_name);
        CHECK(win::persisted_state_path(L"C:\\dir", form) == want_file);
    }
    for (const std::wstring& junk : {std::wstring(L"not-a-guid"), std::wstring(L"{0.0.0.00000000}"),
                                     L"{0.0.0.00000000}." + bare, bare + L"0", std::wstring()}) {
        CAPTURE(narrow(junk));
        CHECK(win::mapping_name(L"Local\\", junk).empty());
        CHECK(win::persisted_state_path(L"C:\\dir", junk).empty());
    }
}

TEST_CASE("a block with no header is saved as one IsoAPO reads") {
    // to_param_block fills parameters only. Without the stamp the file was read
    // back Invalid, and IsoAPO started such a device flat (review 2026-09-13).
    TempDir tmp;
    EqState s;
    Band b;
    b.type = FilterType::Peaking;
    b.fc = 1000.0;
    b.gain_db = -6.0;
    b.width = 1.0;
    s.bands.push_back(b);
    s.layout_channels = 8;
    s.layout_speaker_mask = 0x63F;
    ParamBlock raw{};
    REQUIRE(to_param_block(s, &raw));
    raw.hdr.channels = 2;
    raw.hdr.speaker_mask = 0x3;

    const std::wstring path = win::persisted_state_path(tmp.path + L"\\devices", L"{8F4D2A10-1111-2222-3333-444455556666}");
    REQUIRE(!path.empty());
    CHECK(win::write_persisted_state(path, raw) == ERROR_SUCCESS);
    ParamBlock back{};
    REQUIRE(win::read_persisted_state(path, &back) == win::PersistedRead::Loaded);
    CHECK(back.band_count == 1);
    CHECK(back.bands[0].gain_db == -6.0f);
    // The layout the state was written for is kept; the host's format is not.
    CHECK(back.layout_channels == 8);
    CHECK(back.layout_speaker_mask == 0x63F);
    CHECK(back.hdr.channels == 0);
    CHECK(back.hdr.speaker_mask == 0);
}
