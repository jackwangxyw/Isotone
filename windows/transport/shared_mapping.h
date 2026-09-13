// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The per-device shared region on Windows (plan 5.3): a named file mapping
// holding the ParamBlock and the audio ring.
//
// The APO creates `Global\IsoAPO.{endpoint-guid}` and the UI opens it. Creating
// a Global\ object requires SeCreateGlobalPrivilege, which services hold and an
// ordinary desktop process does not, so the roles cannot be the other way round.

#pragma once

#include <windows.h>

#include <string>

#include "isotone/param_block.h"

namespace isotone::win {

// SYSTEM and LocalService (audiodg) full control; Authenticated Users read and
// write, so the UI can open it without elevation. Protected, so nothing is
// inherited.
inline constexpr wchar_t kMappingSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GRGW;;;AU)";

// "<object_namespace>IsoAPO.{guid}" with the GUID lower-cased. Kernel object
// names are case-sensitive, and the two sides learn the GUID in different
// cases: PKEY_AudioEndpoint_GUID, which the APO reads, is upper case, while
// IMMDevice::GetId, which the UI sees, is lower case.
std::wstring mapping_name(const wchar_t* object_namespace, const std::wstring& endpoint_guid);

class SharedMapping {
public:
    SharedMapping() = default;
    SharedMapping(const SharedMapping&) = delete;
    SharedMapping& operator=(const SharedMapping&) = delete;
    ~SharedMapping() { close(); }

    // Host side. Creates the region with kMappingSddl and initialises it, or
    // opens it if another instance or an earlier engine run already made it.
    // Returns a Win32 error code.
    // `seed` fills the parameter block of a region this call creates, before
    // any other process can see the region as valid.
    DWORD create_or_open(const std::wstring& name, void (*seed)(ParamBlock* block, void* context) = nullptr,
                         void* context = nullptr);

    // Client side. Opens an existing region; ERROR_FILE_NOT_FOUND means no
    // engine has created it yet.
    DWORD open(const std::wstring& name);

    void close();

    bool is_open() const { return view_ != nullptr; }
    bool created() const { return created_; }
    ParamBlock* params() const { return region_params(view_); }
    AudioRingHeader* ring() const { return region_ring(view_); }

private:
    DWORD map_existing();

    HANDLE handle_  = nullptr;
    void*  view_    = nullptr;
    bool   created_ = false;
};

}  // namespace isotone::win
