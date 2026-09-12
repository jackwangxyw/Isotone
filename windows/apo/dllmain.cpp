// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// COM plumbing for IsoAPO: class factory, the four exported entry points, and
// self-registration. Registration writes to HKLM, so it must be run elevated,
// normally by regsvr32.
//
// Nothing here may throw across a COM boundary (plan 5.3), so every entry point
// is wrapped.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <new>
#include <string>

#include "isoapo.h"

namespace {

HINSTANCE g_module = nullptr;
long g_lockCount = 0;

std::wstring guid_string(REFGUID guid) {
    wchar_t buffer[64] = {};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

bool write_string(HKEY root, const std::wstring& subkey, const wchar_t* name,
                  const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LSTATUS status =
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool register_clsid(REFGUID clsid, const wchar_t* description, const std::wstring& dll) {
    const std::wstring base = L"SOFTWARE\\Classes\\CLSID\\" + guid_string(clsid);
    if (!write_string(HKEY_LOCAL_MACHINE, base, L"", description)) {
        return false;
    }
    if (!write_string(HKEY_LOCAL_MACHINE, base + L"\\InprocServer32", L"", dll)) {
        return false;
    }
    // "Both" lets the audio engine create the object on whichever apartment it
    // is using, which is what every shipping APO does.
    return write_string(HKEY_LOCAL_MACHINE, base + L"\\InprocServer32", L"ThreadingModel",
                        L"Both");
}

void unregister_clsid(REFGUID clsid) {
    const std::wstring base = L"SOFTWARE\\Classes\\CLSID\\" + guid_string(clsid);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, (base + L"\\InprocServer32").c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, base.c_str());
}

class ClassFactory : public IClassFactory {
public:
    explicit ClassFactory(REFCLSID clsid) : clsid_(clsid) {}

    HRESULT __stdcall QueryInterface(const IID& iid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG __stdcall AddRef() override { return InterlockedIncrement(&refCount_); }

    ULONG __stdcall Release() override {
        const long count = InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>(count);
    }

    HRESULT __stdcall CreateInstance(IUnknown* outer, const IID& iid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        // Aggregation is only legal when the caller asks for IUnknown.
        if (outer != nullptr && iid != __uuidof(IUnknown)) {
            return CLASS_E_NOAGGREGATION;
        }

        IsoApo* apo = new (std::nothrow) IsoApo(outer);
        if (apo == nullptr) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = apo->NonDelegatingQueryInterface(iid, ppv);
        apo->NonDelegatingRelease();
        return hr;
    }

    HRESULT __stdcall LockServer(BOOL lock) override {
        if (lock) {
            InterlockedIncrement(&g_lockCount);
        } else {
            InterlockedDecrement(&g_lockCount);
        }
        return S_OK;
    }

private:
    long   refCount_ = 1;
    CLSID  clsid_;
};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void* /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    return (IsoApo::instanceCount == 0 && g_lockCount == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(const CLSID& clsid, const IID& iid, void** ppv) {
    if (clsid != ISOAPO_POST_MIX_GUID && clsid != ISOAPO_PRE_MIX_GUID) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    ClassFactory* factory = new (std::nothrow) ClassFactory(clsid);
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(iid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(g_module, path, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // RegisterAPO fails if the CLSID is already registered, which makes a second
    // regsvr32 return failure even though the machine is already in exactly the
    // state being asked for. Clearing first makes registration idempotent, so
    // reinstalling or upgrading in place works.
    UnregisterAPO(ISOAPO_POST_MIX_GUID);
    UnregisterAPO(ISOAPO_PRE_MIX_GUID);

    HRESULT hr = RegisterAPO(&static_cast<const APO_REG_PROPERTIES&>(
        IsoApo::regPostMixProperties));
    if (FAILED(hr)) {
        return hr;
    }
    hr = RegisterAPO(&static_cast<const APO_REG_PROPERTIES&>(
        IsoApo::regPreMixProperties));
    if (FAILED(hr)) {
        UnregisterAPO(ISOAPO_POST_MIX_GUID);
        return hr;
    }

    if (!register_clsid(ISOAPO_POST_MIX_GUID, L"IsoAPO Post-Mix Class", path) ||
        !register_clsid(ISOAPO_PRE_MIX_GUID, L"IsoAPO Pre-Mix Class", path)) {
        unregister_clsid(ISOAPO_POST_MIX_GUID);
        unregister_clsid(ISOAPO_PRE_MIX_GUID);
        UnregisterAPO(ISOAPO_POST_MIX_GUID);
        UnregisterAPO(ISOAPO_PRE_MIX_GUID);
        return E_ACCESSDENIED;
    }
    return S_OK;
}

STDAPI DllUnregisterServer() {
    unregister_clsid(ISOAPO_POST_MIX_GUID);
    unregister_clsid(ISOAPO_PRE_MIX_GUID);
    UnregisterAPO(ISOAPO_POST_MIX_GUID);
    UnregisterAPO(ISOAPO_PRE_MIX_GUID);
    return S_OK;
}
