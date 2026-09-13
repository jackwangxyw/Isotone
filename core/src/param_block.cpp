// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/param_block.h"

#include <algorithm>

namespace isotone {

void init_param_block(ParamBlock* block) {
    if (block == nullptr) {
        return;
    }
    std::memset(block, 0, sizeof(ParamBlock));
    block->hdr.magic   = kParamMagic;
    block->hdr.version = kParamVersion;
    block->hdr.size    = static_cast<uint32_t>(sizeof(ParamBlock));
    block->hdr.host_state = static_cast<uint32_t>(HostState::NotLoaded);
}

void init_shared_region(void* base, void (*seed)(ParamBlock* block, void* context), void* context) {
    if (base == nullptr) {
        return;
    }
    ParamBlock* block = region_params(base);
    init_param_block(block);
    block->hdr.magic = 0;
    audio_ring_init(region_ring(base), kRingCapacityFrames);
    if (seed != nullptr) {
        seed(block, context);
    }
    block->hdr.magic = kParamMagic;
}

bool shared_region_valid(const void* base, size_t bytes) {
    if (base == nullptr || bytes < kSharedRegionBytes) {
        return false;
    }
    const ParamBlock* block = static_cast<const ParamBlock*>(base);
    const AudioRingHeader* ring =
        reinterpret_cast<const AudioRingHeader*>(static_cast<const char*>(base) + sizeof(ParamBlock));
    return block->hdr.magic == kParamMagic && block->hdr.version == kParamVersion &&
           block->hdr.size == sizeof(ParamBlock) && ring->capacity == kRingCapacityFrames;
}

bool param_block_valid(const ParamBlock& block) {
    return block.hdr.magic == kParamMagic && block.hdr.version == kParamVersion &&
           block.hdr.size == sizeof(ParamBlock) && block.band_count <= kParamMaxBands;
}

bool to_param_block(const EqState& state, ParamBlock* out) {
    if (out == nullptr) {
        return false;
    }
    out->bypass = state.bypass ? 1u : 0u;
    out->mute   = state.mute ? 1u : 0u;
    out->preamp_db = static_cast<float>(state.preamp_db);
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        out->channel_gain_db[c] = static_cast<float>(state.channel_gain_db[c]);
    }

    const SpeakerSetup& sp = state.speakers;
    ParamSpeakers& ps = out->speakers;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        ps.delay_ms[c] = static_cast<float>(sp.delay_ms[c]);
    }
    ps.inverted       = sp.inverted;
    ps.muted          = sp.muted;
    ps.lip_sync_ms    = static_cast<float>(sp.lip_sync_ms);
    ps.flags          = (sp.swap_left_right ? kSpeakerFlagSwapLeftRight : 0u) |
                        (sp.swap_front_rear ? kSpeakerFlagSwapFrontRear : 0u) |
                        (sp.bass_management ? kSpeakerFlagBassManagement : 0u);
    ps.upmix          = static_cast<uint32_t>(sp.upmix);
    ps.crossover_hz   = static_cast<float>(sp.crossover_hz);
    ps.small_speakers = sp.small_speakers;
    ps.lfe_lowpass_hz = static_cast<float>(sp.lfe_lowpass_hz);
    std::memset(ps.reserved, 0, sizeof(ps.reserved));

    const uint32_t count =
        static_cast<uint32_t>(std::min<size_t>(state.bands.size(), kParamMaxBands));
    out->band_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        const Band& b = state.bands[i];
        ParamBand& p = out->bands[i];
        p.id         = b.id;
        p.type       = static_cast<uint32_t>(b.type);
        p.width_mode = static_cast<uint32_t>(b.width_mode);
        p.channels   = b.channels;
        p.fc         = static_cast<float>(b.fc);
        p.gain_db    = static_cast<float>(b.gain_db);
        p.width      = static_cast<float>(b.width);
        p.flags      = (b.enabled ? kBandFlagEnabled : 0u) |
                       (b.shelf_corner ? kBandFlagShelfCorner : 0u);
    }
    for (uint32_t i = count; i < kParamMaxBands; ++i) {
        std::memset(&out->bands[i], 0, sizeof(ParamBand));
    }
    return state.bands.size() <= kParamMaxBands;
}

void from_param_block(const ParamBlock& block, EqState* out) {
    if (out == nullptr) {
        return;
    }
    out->bypass = block.bypass != 0;
    out->mute   = block.mute != 0;
    out->preamp_db = block.preamp_db;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        out->channel_gain_db[c] = block.channel_gain_db[c];
    }

    // Values are range-checked where they are used (Processor::set_target); only
    // the enum needs clamping here so it is never out of range as a type.
    const ParamSpeakers& ps = block.speakers;
    SpeakerSetup& sp = out->speakers;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        sp.delay_ms[c] = ps.delay_ms[c];
    }
    sp.inverted        = ps.inverted;
    sp.muted           = ps.muted;
    sp.lip_sync_ms     = ps.lip_sync_ms;
    sp.swap_left_right = (ps.flags & kSpeakerFlagSwapLeftRight) != 0;
    sp.swap_front_rear = (ps.flags & kSpeakerFlagSwapFrontRear) != 0;
    sp.upmix           = static_cast<Upmix>(std::min<uint32_t>(ps.upmix, 2));
    sp.bass_management = (ps.flags & kSpeakerFlagBassManagement) != 0;
    sp.crossover_hz    = ps.crossover_hz;
    sp.small_speakers  = ps.small_speakers;
    sp.lfe_lowpass_hz  = ps.lfe_lowpass_hz;

    const uint32_t count = std::min(block.band_count, kParamMaxBands);
    out->bands.clear();
    out->bands.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const ParamBand& p = block.bands[i];
        Band b;
        b.id           = p.id;
        b.type         = static_cast<FilterType>(std::min<uint32_t>(p.type, 7));
        b.width_mode   = static_cast<WidthMode>(std::min<uint32_t>(p.width_mode, 2));
        b.channels     = p.channels;
        b.fc           = p.fc;
        b.gain_db      = p.gain_db;
        b.width        = p.width;
        b.enabled      = (p.flags & kBandFlagEnabled) != 0;
        b.shelf_corner = (p.flags & kBandFlagShelfCorner) != 0;
        out->bands.push_back(b);
    }
}

bool param_block_read(const ParamBlock* block, ParamBlock* out, int max_attempts) {
    if (block == nullptr || out == nullptr) {
        return false;
    }
    std::atomic_ref<const uint32_t> seq(block->hdr.seq);

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const uint32_t before = seq.load(std::memory_order_relaxed);
        if ((before & 1u) != 0) {
            continue;   // a write is in progress
        }
        std::atomic_thread_fence(std::memory_order_acquire);

        std::memcpy(out, block, sizeof(ParamBlock));

        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == before) {
            return param_block_valid(*out);
        }
    }
    return false;
}

}  // namespace isotone
