// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "device_watcher.h"

#include <objbase.h>

#include <thread>

namespace isotone::devices {
namespace {

const PROPERTYKEY kDeviceFormat = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kDeviceDesc = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
const PROPERTYKEY kInterfaceFriendlyName = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};
const PROPERTYKEY kAdapterName = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 6};

bool same_key(const PROPERTYKEY& a, const PROPERTYKEY& b) { return a.pid == b.pid && a.fmtid == b.fmtid; }

// Depth of DeviceNotifier callbacks running on this thread, so close() can
// refuse to wait for the callback it is called from.
thread_local int t_delivering = 0;

}  // namespace

HRESULT DeviceNotifier::close() {
    if (t_delivering > 0) return E_ILLEGAL_METHOD_CALL;
    // A delivery counts itself in before it reads closed_, and this reads the
    // count after setting closed_: every delivery either sees closed_ or is
    // counted here and waited for.
    closed_.store(true);
    while (in_flight_.load() != 0) std::this_thread::yield();
    callback_ = nullptr;
    return S_OK;
}

void DeviceNotifier::deliver(DeviceEventKind kind, LPCWSTR device_id, DWORD state, ERole role) {
    in_flight_.fetch_add(1);
    ++t_delivering;
    if (!closed_.load() && callback_) {
        DeviceEvent event;
        event.kind = kind;
        if (device_id != nullptr) event.device_id = device_id;
        event.state = state;
        event.role = role;
        callback_(event);
    }
    --t_delivering;
    in_flight_.fetch_sub(1);
}

ULONG DeviceNotifier::AddRef() { return refs_.fetch_add(1) + 1; }

ULONG DeviceNotifier::Release() {
    const ULONG n = refs_.fetch_sub(1) - 1;
    if (n == 0) delete this;
    return n;
}

HRESULT DeviceNotifier::QueryInterface(REFIID iid, void** out) {
    if (out == nullptr) return E_POINTER;
    if (iid == IID_IUnknown || iid == __uuidof(IMMNotificationClient)) {
        *out = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

HRESULT DeviceNotifier::OnDeviceStateChanged(LPCWSTR device_id, DWORD new_state) {
    deliver(DeviceEventKind::state_changed, device_id, new_state);
    return S_OK;
}

HRESULT DeviceNotifier::OnDeviceAdded(LPCWSTR device_id) {
    deliver(DeviceEventKind::added, device_id);
    return S_OK;
}

HRESULT DeviceNotifier::OnDeviceRemoved(LPCWSTR device_id) {
    deliver(DeviceEventKind::removed, device_id);
    return S_OK;
}

HRESULT DeviceNotifier::OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR default_device_id) {
    if (flow == eRender) deliver(DeviceEventKind::default_changed, default_device_id, 0, role);
    return S_OK;
}

HRESULT DeviceNotifier::OnPropertyValueChanged(LPCWSTR device_id, const PROPERTYKEY key) {
    if (same_key(key, kDeviceFormat)) {
        deliver(DeviceEventKind::format_changed, device_id);
    } else if (same_key(key, kFriendlyName) || same_key(key, kDeviceDesc) || same_key(key, kInterfaceFriendlyName) ||
               same_key(key, kAdapterName)) {
        deliver(DeviceEventKind::name_changed, device_id);
    }
    return S_OK;
}

HRESULT DeviceWatcher::start(DeviceEventCallback callback) {
    if (notifier_ != nullptr) return E_ILLEGAL_STATE_CHANGE;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) {
        enumerator_ = nullptr;
        return hr;
    }
    notifier_ = new DeviceNotifier(std::move(callback));
    // Register and Unregister take no reference (their documentation); the
    // reference `new` gave is held until after Unregister.
    hr = enumerator_->RegisterEndpointNotificationCallback(notifier_);
    if (FAILED(hr)) {
        notifier_->Release();
        enumerator_->Release();
        notifier_ = nullptr;
        enumerator_ = nullptr;
    }
    return hr;
}

HRESULT DeviceWatcher::stop() {
    if (notifier_ == nullptr) return S_OK;
    HRESULT hr = notifier_->close();
    if (FAILED(hr)) return hr;
    hr = enumerator_->UnregisterEndpointNotificationCallback(notifier_);
    // A method MMDevice entered between close() and Unregister returns without
    // delivering; wait for it to leave before the object can be deleted.
    notifier_->close();
    notifier_->Release();
    enumerator_->Release();
    notifier_ = nullptr;
    enumerator_ = nullptr;
    return hr;
}

}  // namespace isotone::devices
