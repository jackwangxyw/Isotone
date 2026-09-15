// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "devicelink.h"

#include "compat_writer.h"
#include "eapo_install.h"
#include "isotone/apo_config.h"
#include "isotone/param_block.h"
#include "loopback_capture.h"
#include "persisted_state.h"

#include <cwctype>
#include <fstream>
#include <sstream>

namespace isotone::ui {

namespace {

// An idle engine has no region; trying again on every edit of a drag would cost
// an open per frame for nothing.
constexpr ULONGLONG kReopenIntervalMs = 1000;

// A region outside Global\ is a test's (IsoAPO-selftest's): its saved state is
// the self test's too, as isotone-shm pairs them, never a real device's file.
std::wstring saved_state_path(const std::wstring& region_namespace, const std::wstring& guid) {
    return isotone::win::persisted_state_path(isotone::win::persisted_state_dir(region_namespace != L"Global\\"), guid);
}

// A GUID is ASCII.
std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

}  // namespace

DeviceLink::DeviceLink(std::wstring region_namespace, std::wstring compat_config_dir)
    : namespace_(std::move(region_namespace)), compat_dir_(std::move(compat_config_dir)) {}

DeviceLink::~DeviceLink() {
    stop_compat();
}

void DeviceLink::set_target(const OutputTarget& target) {
    const bool same_endpoint = target.guid == target_.guid && target.backend == target_.backend;
    target_ = target;
    if (same_endpoint) return;

    mapping_.close();
    last_open_attempt_ = 0;
    region_opened_ = false;
    ring_cursor_ = AudioRingCursor{};
    capture_.reset();
    capture_cursor_ = AudioRingCursor{};
    stop_compat();

    if (target_.backend == Backend::native) {
        ensure_region();
    } else if (target_.backend == Backend::equalizer_apo) {
        compat_stop_ = false;
        compat_worker_ = std::thread([this] { compat_thread(); });
        start_capture();
    }
}

bool DeviceLink::ensure_region() {
    if (mapping_.is_open()) return true;
    const ULONGLONG now = GetTickCount64();
    if (last_open_attempt_ != 0 && now - last_open_attempt_ < kReopenIntervalMs) return false;
    last_open_attempt_ = now;
    const std::wstring name = isotone::win::mapping_name(namespace_.c_str(), target_.guid);
    if (mapping_.open(name) != ERROR_SUCCESS) return false;
    region_opened_ = true;
    return true;
}

DWORD DeviceLink::write_region(const EqState& engine_state) {
    if (!ensure_region()) return ERROR_FILE_NOT_FOUND;
    param_block_write(mapping_.params(), [&](ParamBlock* b) { to_param_block(engine_state, b); });
    return ERROR_SUCCESS;
}

DWORD DeviceLink::apply(const EqState& state) {
    switch (target_.backend) {
        case Backend::native: return write_region(state);
        case Backend::equalizer_apo: {
            {
                std::lock_guard<std::mutex> lock(compat_mutex_);
                // A pending persist stays a persist: the newest state is written either way.
                const bool persist = compat_pending_ && compat_pending_->persist;
                compat_pending_ = CompatRequest{state, target_, persist};
            }
            compat_wake_.notify_one();
            return ERROR_SUCCESS;
        }
        case Backend::none: break;
    }
    return ERROR_NOT_READY;
}

DWORD DeviceLink::commit(const EqState& state) {
    if (target_.backend != Backend::equalizer_apo) return apply(state);
    {
        std::lock_guard<std::mutex> lock(compat_mutex_);
        compat_pending_ = CompatRequest{state, target_, true};
    }
    compat_wake_.notify_one();
    return ERROR_SUCCESS;
}

DWORD DeviceLink::save(const EqState& state, const EqState& engine_state) {
    if (target_.backend != Backend::native) return ERROR_NOT_SUPPORTED;
    ParamBlock block{};
    to_param_block(state, &block);
    const std::wstring path = saved_state_path();
    if (path.empty()) return ERROR_INVALID_NAME;
    const DWORD error = isotone::win::write_persisted_state(path, block);
    if (error != ERROR_SUCCESS) return error;
    // IsoAPO reads the file before it creates the region: a stream that started
    // in between would otherwise keep the old state. So the region too, now.
    last_open_attempt_ = 0;
    write_region(engine_state);
    return ERROR_SUCCESS;
}

std::wstring DeviceLink::saved_state_path() const { return isotone::ui::saved_state_path(namespace_, target_.guid); }

bool DeviceLink::load_current(EqState* out) {
    if (target_.backend == Backend::native) {
        ParamBlock block{};
        if (ensure_region() && param_block_read(mapping_.params(), &block)) {
            from_param_block(block, out);
            return true;
        }
        const std::wstring path = saved_state_path();
        if (!path.empty() && isotone::win::read_persisted_state(path, &block) == isotone::win::PersistedRead::Loaded) {
            from_param_block(block, out);
            return true;
        }
        return false;
    }
    if (target_.backend == Backend::equalizer_apo) {
        std::filesystem::path dir = compat_dir_;
        if (dir.empty()) dir = isotone::compat::locate_equalizer_apo().config_path;
        std::ifstream in(dir / "Isotone.txt", std::ios::binary);
        if (!in) return false;
        std::stringstream text;
        text << in.rdbuf();
        const ChannelLayout layout{target_.layout.channels, target_.layout.speaker_mask};
        const std::wstring bare = target_.guid.size() == 38 ? target_.guid.substr(1, 36) : target_.guid;
        for (const isotone::compat::ParsedDevice& d :
             isotone::compat::parse_isotone_file(text.str(), [&](const std::string&) { return layout; })) {
            std::wstring guid(d.endpoint_guid.begin(), d.endpoint_guid.end());
            for (wchar_t& c : guid) c = static_cast<wchar_t>(towlower(c));
            if (guid.find(bare) == std::wstring::npos) continue;
            *out = d.state;
            return true;
        }
    }
    return false;
}

void DeviceLink::compat_thread() {
    std::filesystem::path dir = compat_dir_;
    if (dir.empty()) dir = isotone::compat::locate_equalizer_apo().config_path;
    isotone::compat::CompatWriter writer(dir);
    DWORD loaded = writer.load();
    for (;;) {
        std::optional<CompatRequest> request;
        {
            std::unique_lock<std::mutex> lock(compat_mutex_);
            compat_wake_.wait(lock, [this] { return compat_stop_ || compat_pending_.has_value(); });
            if (!compat_pending_ && compat_stop_) break;
            request.swap(compat_pending_);
        }
        isotone::compat::DeviceConfig device;
        device.endpoint_guid = narrow(request->target.guid);
        device.layout = ChannelLayout{request->target.layout.channels, request->target.layout.speaker_mask};
        device.sample_rate = request->target.layout.sample_rate;
        device.state = request->state;
        DWORD error = loaded;
        if (error == ERROR_SUCCESS) error = request->persist ? writer.persist(device) : writer.apply(device);
        std::lock_guard<std::mutex> lock(compat_mutex_);
        compat_error_ = error;
    }
    const DWORD flushed = writer.flush();
    std::lock_guard<std::mutex> lock(compat_mutex_);
    if (flushed != ERROR_SUCCESS) compat_error_ = flushed;
}

void DeviceLink::stop_compat() {
    if (!compat_worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(compat_mutex_);
        compat_stop_ = true;
    }
    compat_wake_.notify_one();
    compat_worker_.join();
    compat_pending_.reset();
}

DWORD DeviceLink::last_compat_error() const {
    std::lock_guard<std::mutex> lock(compat_mutex_);
    return compat_error_;
}

void DeviceLink::start_capture() {
    capture_ = std::make_unique<isotone::compat::LoopbackCapture>();
    if (FAILED(capture_->start(narrow(target_.guid), 2000))) capture_.reset();
}

uint32_t DeviceLink::read_audio(float* out, uint32_t max_frames, uint32_t* channels, double* sample_rate) {
    *channels = 0;
    if (target_.backend == Backend::native) {
        if (!ensure_region()) return 0;
        *sample_rate = mapping_.params()->hdr.sample_rate;
        return audio_ring_read(mapping_.ring(), kRingCapacityFrames, &ring_cursor_, out, max_frames, channels);
    }
    if (target_.backend == Backend::equalizer_apo && capture_ && capture_->running()) {
        *sample_rate = capture_->sample_rate();
        return capture_->read(&capture_cursor_, out, max_frames, channels);
    }
    return 0;
}

}  // namespace isotone::ui
