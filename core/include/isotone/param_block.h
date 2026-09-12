// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The shared-memory layout, defined once and used by the APO, the PipeWire
// daemon and the addon (plan 4.7). Fixed size, POD, little-endian, no pointers,
// so it can be mapped by processes built with different compilers.
//
// Concurrency is a seqlock. The UI writes and the host reads; the host never
// blocks and never waits. If the host catches a write in progress it uses the
// parameters it already had and tries again on the next block, which at 48 kHz
// is under a millisecond away.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "isotone/types.h"

namespace isotone {

inline constexpr uint32_t kParamMagic   = 0x544F5349u;  // 'ISOT' little-endian
inline constexpr uint32_t kParamVersion = 1u;
inline constexpr uint32_t kParamMaxBands = 64u;

enum class HostState : uint32_t {
    NotLoaded = 0,
    Running   = 1,
    Error     = 2,
};

// A Band flattened to fixed-width fields. Kept separate from Band so that
// changing the in-memory model does not silently change the wire format.
struct ParamBand {
    uint32_t id;
    uint32_t type;          // FilterType
    uint32_t width_mode;    // WidthMode
    uint32_t channels;      // ChannelMask, 0 = all
    float    fc;
    float    gain_db;
    float    width;
    uint32_t flags;         // bit 0 enabled, bit 1 shelf_corner
};
static_assert(sizeof(ParamBand) == 32, "ParamBand layout must stay fixed");

inline constexpr uint32_t kBandFlagEnabled     = 1u << 0;
inline constexpr uint32_t kBandFlagShelfCorner = 1u << 1;

struct ParamBlockHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t size;             // sizeof(ParamBlock), so a reader can sanity check
    uint32_t seq;              // seqlock: odd means a write is in progress
    uint32_t sample_rate;      // written by the host
    uint32_t channels;         // written by the host
    uint32_t host_state;       // HostState, written by the host
    uint32_t host_heartbeat;   // incremented by the host once per process call
};
static_assert(sizeof(ParamBlockHeader) == 32, "header layout must stay fixed");

struct ParamBlock {
    ParamBlockHeader hdr;
    uint32_t  bypass;
    uint32_t  mute;
    uint32_t  mono;
    uint32_t  band_count;
    float     preamp_db;
    float     channel_gain_db[kMaxChannels];
    float     reserved[3];      // keeps bands[] 16-byte aligned and leaves room
    ParamBand bands[kParamMaxBands];
};
static_assert(sizeof(ParamBlock) % 16 == 0, "ParamBlock should stay 16-byte aligned");
static_assert(std::atomic_ref<uint32_t>::is_always_lock_free,
              "the seqlock needs lock-free 32-bit atomics on this target");

// Header of the post-EQ audio ring. `samples` follows immediately in the mapped
// region; capacity is in frames and the buffer holds capacity * channels floats.
struct AudioRingHeader {
    uint32_t write_index;   // frames, monotonically increasing, host writes last
    uint32_t capacity;      // frames
    uint32_t channels;
    uint32_t reserved;
};

// ---------------------------------------------------------------------------
// Conversion

void to_param_block(const EqState& state, ParamBlock* out);
void from_param_block(const ParamBlock& block, EqState* out);

// Initialises a freshly mapped block: magic, version, size, everything else
// zeroed. Call once by whichever side creates the mapping.
void init_param_block(ParamBlock* block);

bool param_block_valid(const ParamBlock& block);

// ---------------------------------------------------------------------------
// Seqlock

// Writer side. Bumps seq to odd, runs `write`, bumps to even.
// std::atomic_ref rather than a cast to std::atomic*: the block has to stay a
// plain copyable POD for the mapped layout, and atomic_ref is exactly the tool
// for applying atomic operations to an object that is not declared atomic.
template <typename F>
void param_block_write(ParamBlock* block, F&& write) {
    std::atomic_ref<uint32_t> seq(block->hdr.seq);
    const uint32_t start = seq.load(std::memory_order_relaxed);
    seq.store(start + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    write(block);

    std::atomic_thread_fence(std::memory_order_release);
    seq.store(start + 2, std::memory_order_relaxed);
}

// Reader side. Copies into `out` and returns true only if the copy was taken
// from a consistent snapshot. Never blocks; the caller keeps its previous
// parameters on false.
bool param_block_read(const ParamBlock* block, ParamBlock* out, int max_attempts = 4);

}  // namespace isotone
