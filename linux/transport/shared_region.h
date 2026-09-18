// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The per-sink shared region on Linux: a POSIX shared-memory object holding the
// ParamBlock and the audio ring, the counterpart of windows/transport's named
// file mapping. The layout, the seqlock and the ring are core's and identical on
// both platforms; only the naming and the object's lifetime differ.
//
// Two differences from Windows, both simplifications. A sink is identified by
// its PipeWire node name rather than an endpoint GUID. And both ends run as the
// same user, so the object is created 0600 and needs no security descriptor: on
// Windows the host is audiodg as LocalService, which is why that side must be
// the creator and why kMappingSddl exists.
//
// One difference that is not a simplification: a POSIX shm object outlives the
// process that made it. A daemon that is killed leaves its region behind, so
// unlike the Windows mapping it can be found stale, and create_or_open has to be
// able to take a broken one back over. See the note there.

#pragma once

#include <string>

#include "isotone/param_block.h"

namespace isotone::posix {

// A PipeWire node name (node.name, say "alsa_output.pci-0000_00_1f.3.analog-stereo")
// reduced to characters an shm object name and a file name can both carry:
// anything outside [A-Za-z0-9._-] becomes '_'. A name too long for the object
// name limit is truncated and given an 8-hex FNV-1a suffix of the whole name, so
// two long names cannot quietly share one region. Empty in, empty out.
std::string sanitize_key(const std::string& node_name);

// "/isotone.<key>". Empty when `node_name` is empty.
std::string region_name(const std::string& node_name);

class SharedRegion {
public:
    SharedRegion() = default;
    SharedRegion(const SharedRegion&) = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;
    ~SharedRegion() { close(); }

    // Host side. Creates the region and initialises it, or adopts one that is
    // already there. `seed` fills the parameter block of a region this call
    // creates, before any other process can see the region as valid.
    //
    // An existing region that is not valid is re-initialised rather than
    // refused. On Windows that case cannot arise, because the mapping dies with
    // its last handle; here a daemon killed between shm_open and the header
    // write leaves a region that no later run could ever use, and refusing it
    // would wedge that sink until someone deleted the file by hand.
    //
    // Returns an errno value, 0 on success.
    int create_or_open(const std::string& name, void (*seed)(ParamBlock* block, void* context) = nullptr,
                       void* context = nullptr);

    // Client side. Opens an existing region; ENOENT means no daemon has created
    // it yet. An empty name is EINVAL.
    int open(const std::string& name);

    void close();

    // Removes the name. The region stays alive for whoever still has it mapped,
    // as POSIX shm names work like file names. The daemon calls this when it
    // gives up a sink, so the next run starts clean.
    static int unlink_region(const std::string& name);

    bool is_open() const { return base_ != nullptr; }
    bool created() const { return created_; }
    ParamBlock* params() const { return base_ != nullptr ? region_params(base_) : nullptr; }
    AudioRingHeader* ring() const { return base_ != nullptr ? region_ring(base_) : nullptr; }

private:
    int map_fd(int fd);

    void* base_    = nullptr;
    bool  created_ = false;
};

}  // namespace isotone::posix
