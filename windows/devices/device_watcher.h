// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// MMDevice notifications for the UI: endpoints added, removed, changing state
// or default role, and changes to the mix format or names.
//
// Threading. Callbacks arrive on a thread the MMDevice API owns, not the thread
// that called start(). Microsoft's rules for IMMNotificationClient
// (learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient):
// "The methods of the interface must be nonblocking. The client should never
// wait on a synchronization object during an event callback"; never call
// Register/UnregisterEndpointNotificationCallback from a callback; "never
// release the final reference on an MMDevice API object during an event
// callback". So the callback must only copy the event and hand it to its own
// thread (in Qt, a queued invocation), and re-read devices there: calling
// enumerate_render_endpoints or read_engine inside the callback breaks those
// rules. Endpoint ID strings are opaque (same page); compare them with
// Endpoint::id.
//
// What is not reported. Engine changes: devicetool writes the registry only,
// and no MMDevice notification for them is relied on (not measured), so call
// read_engine after devicetool exits. Events for capture endpoints are not
// filtered out of added, removed and state changed, since telling the flow
// apart would mean calling into the enumerator from the callback; the UI's
// re-read finds nothing new for them. Format changes arrive as
// OnPropertyValueChanged for PKEY_AudioEngine_DeviceFormat; that Windows raises
// it when the format is changed in the Sound control panel is documented
// behaviour of the property, not measured here.
//
// COM: start() and stop() on the same thread, with COM initialised in either
// apartment.

#pragma once

#include <windows.h>

#include <mmdeviceapi.h>

#include <atomic>
#include <functional>
#include <string>

namespace isotone::devices {

enum class DeviceEventKind {
    added,
    removed,
    state_changed,     // `state` is the new DEVICE_STATE_* value
    default_changed,   // render only; `role` is the role; `device_id` empty when no device has it
    format_changed,    // PKEY_AudioEngine_DeviceFormat
    // PKEY_Device_FriendlyName, PKEY_Device_DeviceDesc, PKEY_DeviceInterface_FriendlyName
    // or {b3f8fa53-0004-438e-9003-51a46e139bfc},6 (Endpoint::device_name)
    name_changed,
};

struct DeviceEvent {
    DeviceEventKind kind = DeviceEventKind::added;
    std::wstring device_id;
    DWORD state = 0;
    ERole role = eConsole;
};

using DeviceEventCallback = std::function<void(const DeviceEvent&)>;

// The IMMNotificationClient itself. Public so its filtering can be tested by
// calling the methods directly; the UI uses DeviceWatcher.
class DeviceNotifier final : public IMMNotificationClient {
public:
    explicit DeviceNotifier(DeviceEventCallback callback) : callback_(std::move(callback)) {}

    // After close() returns the callback is never called again, and it has
    // been destroyed. A callback already running when close() is called is
    // waited for, so close() takes as long as that callback. Called from inside
    // the callback it cannot wait for itself: it returns
    // E_ILLEGAL_METHOD_CALL and changes nothing.
    HRESULT close();

    // IUnknown. Created with a reference count of 1.
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;

    // IMMNotificationClient. Each returns S_OK.
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR device_id, DWORD new_state) override;
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR device_id) override;
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR device_id) override;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR default_device_id) override;
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR device_id, const PROPERTYKEY key) override;

private:
    ~DeviceNotifier() = default;
    void deliver(DeviceEventKind kind, LPCWSTR device_id, DWORD state = 0, ERole role = eConsole);

    std::atomic<ULONG> refs_{1};
    std::atomic<bool> closed_{false};
    std::atomic<int> in_flight_{0};
    DeviceEventCallback callback_;
};

class DeviceWatcher {
public:
    DeviceWatcher() = default;
    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;
    ~DeviceWatcher() { stop(); }

    // Registers for notifications. E_ILLEGAL_STATE_CHANGE if already started.
    HRESULT start(DeviceEventCallback callback);

    // Closes the notifier (see DeviceNotifier::close: no callback after this
    // returns), then unregisters it. Anything the callback captured can be
    // destroyed once stop() has returned S_OK. From inside the callback it
    // returns E_ILLEGAL_METHOD_CALL and stays registered. S_OK when not started.
    // The notifier is deleted once Unregister has returned and no method of it
    // is running. Whether Unregister waits for a notification MMDevice has
    // already picked the notifier for, but not yet called, is not documented;
    // if it does not, that call could reach a deleted object. Not observed.
    HRESULT stop();

    bool started() const { return notifier_ != nullptr; }

private:
    IMMDeviceEnumerator* enumerator_ = nullptr;
    DeviceNotifier* notifier_ = nullptr;
};

}  // namespace isotone::devices
