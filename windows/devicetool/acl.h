// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Comparing a DACL with the one that was asked for.
//
// Header-only so the tests can reach it: devicetool_tests runs the tool as a
// subprocess and links none of main.cpp, and this is the one piece of that file
// worth testing directly rather than through a command.

#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace isotone::devicetool {

// Whether the DACL `got` grants exactly what `want` asked for.
//
// Not a string comparison. Windows does not hand back the string that was set:
// it adds AI (auto-inherited) to the flags and returns the ACEs in an order of
// its own. Measured 2026-09-19, setting
//   D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;LS)(A;OICI;0x1301bf;;;AU)(...)
// reads back as
//   D:PAI(A;OICI;0x1301bf;;;AU)(A;OICI;FA;;;SY)(A;OICI;0x1200a9;;;LS)(A;OICI;FA;;;BA)(...)
// which is the same ACL. A real install applied the ACL correctly and then
// failed its own verification over that difference.
//
// So the ACEs are compared as a set, and the one flag that matters is checked
// on its own: P, without which ProgramData's inherited "Users may read and
// create" applies as well, which is what the ACL exists to stop.
inline bool dacl_grants(const std::wstring& got, const std::wstring& want) {
    const auto parse = [](const std::wstring& sddl, std::wstring* flags) {
        std::vector<std::wstring> aces;
        const size_t start = sddl.rfind(L"D:", 0) == 0 ? 2 : 0;
        const size_t first = sddl.find(L'(', start);
        *flags = sddl.substr(start, (first == std::wstring::npos ? sddl.size() : first) - start);
        size_t i = first;
        while (i != std::wstring::npos && i < sddl.size() && sddl[i] == L'(') {
            const size_t end = sddl.find(L')', i);
            if (end == std::wstring::npos) break;
            aces.push_back(sddl.substr(i, end - i + 1));
            i = end + 1;
        }
        std::sort(aces.begin(), aces.end());
        return aces;
    };
    std::wstring got_flags, want_flags;
    const std::vector<std::wstring> got_aces = parse(got, &got_flags);
    const std::vector<std::wstring> want_aces = parse(want, &want_flags);
    if (got_aces.empty() || got_aces != want_aces) return false;
    return got_flags.find(L'P') != std::wstring::npos;
}

}  // namespace isotone::devicetool
