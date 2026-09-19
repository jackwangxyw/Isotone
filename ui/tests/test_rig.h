// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// What the model tests stand on, on either platform: a place of their own that
// no real engine reads (Windows, a Local\ region namespace and the self test's
// saved-state directory; Linux, a scratch saved-state directory), the
// DeviceLinks and Presets that write there, and an engine's region the test
// holds as a running IsoAPO or daemon would.
//
// Linux regions have no namespace: they are named for the output alone, so the
// ids the tests use (GUID strings) are what keep them clear of a real sink's.

#pragma once

#include <QString>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include "shared_mapping.h"
#else
#include <unistd.h>

#include "shared_region.h"
#endif

#include "devicelink.h"
#include "isotone/param_block.h"
#include "persisted_state.h"
#include "eqsession.h"
#include "presets.h"

namespace test_rig {

// The backend an engine's region stands for: IsoAPO, or the daemon.
#if defined(_WIN32)
inline constexpr isotone::ui::Backend kEngine = isotone::ui::Backend::native;
#else
inline constexpr isotone::ui::Backend kEngine = isotone::ui::Backend::pipewire;
#endif

// An output of the other kind: Equalizer APO on Windows (Isotone.txt in the
// sandbox); on Linux a sink the daemon is not hosting, whose edits reach its
// saved state alone.
#if defined(_WIN32)
inline constexpr isotone::ui::Backend kOtherEngine = isotone::ui::Backend::equalizer_apo;
#else
inline constexpr isotone::ui::Backend kOtherEngine = isotone::ui::Backend::pipewire;
#endif

inline unsigned long pid() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

class Place {
public:
    explicit Place(const std::string& what) {
#if defined(_WIN32)
        ns_ = L"Local\\IsotoneTest." + std::to_wstring(pid()) + L"." + std::wstring(what.begin(), what.end()) + L".";
#else
        dir_ = std::filesystem::temp_directory_path() / ("isotone-test-" + std::to_string(pid()) + "-" + what);
        std::filesystem::remove_all(dir_);
#endif
    }
    ~Place() {
#if defined(_WIN32)
        const std::vector<std::string> used = used_;   // saved_path appends to it
        for (const std::string& o : used) std::filesystem::remove(saved_path(o));
#else
        std::filesystem::remove_all(dir_);
#endif
    }
    Place(const Place&) = delete;
    Place& operator=(const Place&) = delete;

    // `compat` is where an Equalizer APO output writes (Windows only).
    std::unique_ptr<isotone::ui::DeviceLink> link(const QString& compat = QString()) const {
#if defined(_WIN32)
        return std::make_unique<isotone::ui::DeviceLink>(ns_, compat.toStdWString());
#else
        (void)compat;
        return std::make_unique<isotone::ui::DeviceLink>(dir_.string());
#endif
    }

    std::unique_ptr<Presets> presets(const QString& data, EqSession* session, Presets::OutputList outputs,
                                     const QString& compat = QString()) const {
#if defined(_WIN32)
        return std::make_unique<Presets>(data, session, std::move(outputs), ns_, compat.toStdWString());
#else
        (void)compat;
        return std::make_unique<Presets>(data, session, std::move(outputs), dir_.string());
#endif
    }

    // The output's saved state is this test's: removed now and again at the end.
    // On Windows the directory is the self test's and outlives the test, so a
    // file left by an earlier one would otherwise be read as this one's.
    void own(const std::string& output) const { std::filesystem::remove(saved_path(output)); }

    // The saved state an output's engine starts from.
    std::filesystem::path saved_path(const std::string& output) const {
        used_.push_back(output);
#if defined(_WIN32)
        return isotone::win::persisted_state_path(isotone::win::persisted_state_dir(true), isotone::ui::widen_id(output));
#else
        return isotone::posix::persisted_state_path(dir_.string(), output);
#endif
    }

    // False when there is none.
    bool read_saved(const std::string& output, isotone::ParamBlock* out) const {
#if defined(_WIN32)
        return isotone::win::read_persisted_state(saved_path(output).wstring(), out) == isotone::win::PersistedRead::Loaded;
#else
        return isotone::posix::read_persisted_state(saved_path(output).string(), out) == isotone::posix::PersistedRead::Loaded;
#endif
    }

    // Writes it; true when written.
    bool write_saved(const std::string& output, const isotone::ParamBlock& block) const {
#if defined(_WIN32)
        return isotone::win::write_persisted_state(saved_path(output).wstring(), block) == ERROR_SUCCESS;
#else
        std::filesystem::create_directories(dir_);
        return isotone::posix::write_persisted_state(saved_path(output).string(), block) == 0;
#endif
    }

#if defined(_WIN32)
    const std::wstring& ns() const { return ns_; }
#endif

private:
#if defined(_WIN32)
    std::wstring ns_;
#else
    std::filesystem::path dir_;
#endif
    mutable std::vector<std::string> used_;
};

// An output's engine region, created as IsoAPO or the daemon creates it, for as
// long as this lives.
class Engine {
public:
    Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine() {
#if !defined(_WIN32)
        if (!name_.empty()) isotone::posix::SharedRegion::unlink_region(name_);
#endif
    }

    bool open(const Place& place, const std::string& output) {
#if defined(_WIN32)
        return mapping_.create_or_open(isotone::win::mapping_name(place.ns().c_str(), isotone::ui::widen_id(output))) ==
               ERROR_SUCCESS;
#else
        (void)place;
        name_ = isotone::posix::region_name(output);
        return region_.create_or_open(name_) == 0;
#endif
    }

    isotone::ParamBlock* params() const {
#if defined(_WIN32)
        return mapping_.params();
#else
        return region_.params();
#endif
    }

    isotone::EqState state() const {
        isotone::EqState s;
        from_param_block(*params(), &s);
        return s;
    }

private:
#if defined(_WIN32)
    isotone::win::SharedMapping mapping_;
#else
    isotone::posix::SharedRegion region_;
    std::string name_;
#endif
};

}  // namespace test_rig
