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
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include "isotone/audio_ring.h"
#include "isotone/types.h"

namespace isotone {

inline constexpr uint32_t kParamMagic   = 0x544F5349u;  // 'ISOT' little-endian
// Covers the whole shared region below, not only ParamBlock. 2: audio ring
// header gained pending_index, epoch and writer. 3: mono removed. 4: speaker
// setup. 5: the layout the per-channel fields were written for.
//
// The saved state files (windows/transport/persisted_state.h) hold a block with
// this version, and a reader rejects any other. That is accepted only before the
// first release, when no saved files exist in the field: from the first release
// on, a reader must migrate an older version rather than reject it, or every
// device a user set up starts flat after an update.
inline constexpr uint32_t kParamVersion = 5u;
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
    uint32_t speaker_mask;     // written by the host; which speaker each channel is (speakers.h)
    uint32_t host_reserved[3];
};
static_assert(sizeof(ParamBlockHeader) == 48, "header layout must stay fixed");

// SpeakerSetup flattened. Masks are ChannelMask; upmix is Upmix.
struct ParamSpeakers {
    float    delay_ms[kMaxChannels];
    uint32_t inverted;
    uint32_t muted;
    float    lip_sync_ms;
    uint32_t flags;             // kSpeakerFlag*
    uint32_t upmix;
    float    crossover_hz;
    uint32_t small_speakers;
    float    lfe_lowpass_hz;
    float    reserved[4];
};
static_assert(sizeof(ParamSpeakers) == 80, "ParamSpeakers layout must stay fixed");

inline constexpr uint32_t kSpeakerFlagSwapLeftRight = 1u << 0;
inline constexpr uint32_t kSpeakerFlagSwapFrontRear = 1u << 1;
inline constexpr uint32_t kSpeakerFlagBassManagement = 1u << 2;

struct ParamBlock {
    ParamBlockHeader hdr;
    uint32_t      bypass;
    uint32_t      mute;
    uint32_t      band_count;
    float         preamp_db;
    // EqState::layout_channels and layout_speaker_mask: written by the UI with
    // the parameters, unlike the host's format in the header.
    uint32_t      layout_channels;
    uint32_t      layout_speaker_mask;
    uint32_t      reserved[2];
    float         channel_gain_db[kMaxChannels];
    ParamSpeakers speakers;
    ParamBand     bands[kParamMaxBands];
};
static_assert(sizeof(ParamBlock) % 16 == 0, "ParamBlock should stay 16-byte aligned");
static_assert(std::atomic_ref<uint32_t>::is_always_lock_free,
              "the seqlock needs lock-free 32-bit atomics on this target");

// ---------------------------------------------------------------------------
// The shared region: one per device, parameters down and audio up.
//
//   [ParamBlock][AudioRingHeader][kRingCapacityFrames * kMaxChannels floats]
//
// 32768 frames is 680 ms at 48 kHz and 85 ms at 384 kHz. The UI drains about 60
// times a second, so even the highest rate leaves five times the headroom.

inline constexpr uint32_t kRingCapacityFrames = 32768u;
inline constexpr size_t kSharedRegionBytes =
    sizeof(ParamBlock) + sizeof(AudioRingHeader) +
    size_t{kRingCapacityFrames} * kMaxChannels * sizeof(float);
static_assert(sizeof(ParamBlock) % alignof(AudioRingHeader) == 0,
              "the ring header must start on its own alignment");

inline ParamBlock* region_params(void* base) { return static_cast<ParamBlock*>(base); }
inline AudioRingHeader* region_ring(void* base) {
    return reinterpret_cast<AudioRingHeader*>(static_cast<char*>(base) + sizeof(ParamBlock));
}

// Initialises a freshly created region. The magic is written last, so a second
// opener that sees a valid header sees a finished one.
// Writes the region's headers. `seed`, when given, fills the parameter block
// before the magic is written, so no other process can open the region and
// write it at the same moment.
void init_shared_region(void* base, void (*seed)(ParamBlock* block, void* context) = nullptr,
                        void* context = nullptr);

// True if `bytes` of mapped memory at `base` hold a region this build
// understands.
bool shared_region_valid(const void* base, size_t bytes);

// Host side. The header fields after `seq` belong to the host; the UI only
// reads them. Several APO instances can share one region, so updates are atomic.
inline void host_publish_format(ParamBlock* block, uint32_t sample_rate, uint32_t channels,
                                uint32_t speaker_mask, HostState state) {
    std::atomic_ref<uint32_t>(block->hdr.sample_rate).store(sample_rate, std::memory_order_relaxed);
    std::atomic_ref<uint32_t>(block->hdr.channels).store(channels, std::memory_order_relaxed);
    std::atomic_ref<uint32_t>(block->hdr.speaker_mask).store(speaker_mask, std::memory_order_relaxed);
    std::atomic_ref<uint32_t>(block->hdr.host_state)
        .store(static_cast<uint32_t>(state), std::memory_order_relaxed);
}

// Once per process call. The UI treats a heartbeat that stops moving as an idle
// engine, whatever host_state says.
inline void host_heartbeat(ParamBlock* block) {
    std::atomic_ref<uint32_t>(block->hdr.host_heartbeat).fetch_add(1, std::memory_order_relaxed);
}

inline uint32_t param_block_seq(const ParamBlock* block) {
    return std::atomic_ref<const uint32_t>(block->hdr.seq).load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Conversion

// False if `state` has more than kParamMaxBands bands: the rest are not
// written, and the engine does not play them. The UI must not create them.
bool to_param_block(const EqState& state, ParamBlock* out);
void from_param_block(const ParamBlock& block, EqState* out);

// Initialises a freshly mapped block: magic, version, size, everything else
// zeroed. Call once by whichever side creates the mapping.
void init_param_block(ParamBlock* block);

bool param_block_valid(const ParamBlock& block);

// ---------------------------------------------------------------------------
// Seqlock

// How long a writer waits for a lock another writer holds before treating that
// writer as dead. Writing a block takes microseconds.
inline constexpr std::chrono::milliseconds kParamWriterTimeout{200};

// Writer side. Takes seq from even to odd, runs `write`, and takes it back to
// even. Not real-time safe: it can wait for another writer.
//
// The lock is a compare-and-swap, so two writers (the UI and isotone-shm, or two
// users' UIs) cannot both hold it; storing an odd value let both in, and the
// first to finish marked the other's half-written block consistent. A value
// that stays odd for kParamWriterTimeout was left by a writer that died
// mid-write, and the next writer steps past it to a new odd value, which the
// dead writer's own unlock no longer matches. A writer that merely stalled that
// long inside `write` and then resumes can still mix its fields into the next
// write; nothing on the writer side can prevent that without a real mutex.
//
// std::atomic_ref rather than a cast to std::atomic*: the block has to stay a
// plain copyable POD for the mapped layout, and atomic_ref is exactly the tool
// for applying atomic operations to an object that is not declared atomic.
template <typename F>
void param_block_write(ParamBlock* block, F&& write) {
    std::atomic_ref<uint32_t> seq(block->hdr.seq);
    uint32_t mine = 0;
    uint32_t stuck = 0;
    std::chrono::steady_clock::time_point stuck_since{};
    for (;;) {
        uint32_t cur = seq.load(std::memory_order_relaxed);
        if ((cur & 1u) == 0u) {
            if (seq.compare_exchange_weak(cur, cur + 1u, std::memory_order_relaxed)) {
                mine = cur + 1u;
                break;
            }
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (stuck_since == std::chrono::steady_clock::time_point{} || cur != stuck) {
            stuck = cur;
            stuck_since = now;
        } else if (now - stuck_since >= kParamWriterTimeout) {
            if (seq.compare_exchange_strong(cur, cur + 2u, std::memory_order_relaxed)) {
                mine = cur + 2u;
                break;
            }
            continue;
        }
        std::this_thread::yield();
    }
    std::atomic_thread_fence(std::memory_order_release);

    write(block);

    std::atomic_thread_fence(std::memory_order_release);
    // Fails only if another writer took the lock over after timing out on this one.
    uint32_t expected = mine;
    seq.compare_exchange_strong(expected, mine + 1u, std::memory_order_relaxed);
}

// Reader side. Copies into `out` and returns true only if the copy was taken
// from a consistent snapshot. Never blocks; the caller keeps its previous
// parameters on false.
bool param_block_read(const ParamBlock* block, ParamBlock* out, int max_attempts = 4);

}  // namespace isotone
