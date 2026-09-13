// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The audio path. Everything a host calls on its real-time thread lives here.
//
// Contract for process(): no allocation, no locking, no logging, no system
// calls. All storage is reserved in initialize(). set_target() is also safe to
// call from the audio thread, because that is where a host applies a param block
// it has just copied out of shared memory.

#pragma once

#include <cstdint>
#include <vector>

#include "isotone/biquad.h"
#include "isotone/types.h"

namespace isotone {

// Time constant of the one-pole parameter smoothers, and the length of the
// crossfade used for changes that cannot be smoothed continuously.
inline constexpr double kSmoothingTauSeconds  = 0.020;
inline constexpr double kCrossfadeSeconds     = 0.010;

// Coefficients are recomputed once per control block. 32 frames at 48 kHz is
// 0.67 ms; the count scales with the rate to hold that interval.
inline constexpr uint32_t kControlBlockAt48k = 32;

uint32_t control_block_frames(double sample_rate);

// Turns on flush-to-zero and denormals-are-zero for the calling thread. A host
// calls this once when its audio thread starts. Denormal numbers arise whenever
// a filter's ringing decays toward silence, and on x86 they are handled in
// microcode at roughly a hundred times the cost of a normal multiply, which is
// enough to blow a real-time deadline.
void enable_denormal_flushing();

class Processor {
public:
    Processor() = default;

    // Reserves everything process() will touch. Safe to call again to change
    // rate or channel count; it is not real-time safe.
    void initialize(double sample_rate, uint32_t channels, uint32_t max_frames,
                    uint32_t max_bands = 64);

    // Sets the destination for every smoothed parameter. Bands beyond
    // max_bands() are ignored. Real-time safe: no allocation.
    void set_target(const EqState& state);

    // Jumps every smoothed parameter to its target with no ramp, and clears
    // filter state. For cold start, so the first block is already correct.
    void reset();

    // Planar float32, `channels()` pointers of at least `frames` samples each.
    // Processes in place.
    void process(float* const* planar, uint32_t frames);

    // Interleaved float32, frames * channels() samples. Processes in place.
    void process_interleaved(float* interleaved, uint32_t frames);

    double   sample_rate() const { return sample_rate_; }
    uint32_t channels()    const { return channels_; }
    uint32_t max_frames()  const { return max_frames_; }
    uint32_t max_bands()   const { return max_bands_; }
    uint32_t band_count()  const { return band_count_; }

    // True while any parameter is still moving toward its target or any
    // crossfade is in flight. Tests use it to wait for settling.
    bool is_settling() const;

private:
    struct State {
        double s1 = 0.0, s2 = 0.0;
        void clear() { s1 = 0.0; s2 = 0.0; }
    };

    struct BandSlot {
        // Target shape, copied from the EqState.
        FilterType  type         = FilterType::Peaking;
        WidthMode   width_mode   = WidthMode::Q;
        bool        shelf_corner = false;
        ChannelMask channels     = kAllChannels;
        bool        enabled      = false;
        uint32_t    id           = 0;

        // Smoothed parameters. fc and width move in the log domain so that a
        // drag feels the same at 50 Hz as at 5 kHz; gain moves in dB.
        double log_fc_target = 0.0, log_fc_cur = 0.0;
        double gain_target   = 0.0, gain_cur   = 0.0;
        double log_w_target  = 0.0, log_w_cur  = 0.0;

        BiquadCoeffs coeffs;
        BiquadCoeffs old_coeffs;

        // Crossfade progress, 1.0 when not fading.
        double fade = 1.0;
        double fade_step = 0.0;

        bool occupied = false;
    };

    void recompute_band(BandSlot& b);
    void advance_smoothers(uint32_t frames);
    void begin_crossfade(uint32_t index);
    void process_block(float* const* planar, uint32_t offset, uint32_t frames);

    double   sample_rate_ = 48000.0;
    uint32_t channels_    = 0;
    uint32_t max_frames_  = 0;
    uint32_t max_bands_   = 0;
    uint32_t band_count_  = 0;
    uint32_t control_frames_ = kControlBlockAt48k;

    double smoothing_coef_ = 1.0;   // per control block
    double fade_step_      = 1.0;   // per control block

    std::vector<BandSlot> bands_;
    std::vector<State>    state_new_;   // [band * channels + channel]
    std::vector<State>    state_old_;

    // Global smoothed values.
    double preamp_target_ = 0.0, preamp_cur_ = 0.0;
    double trim_target_[kMaxChannels] = {};
    double trim_cur_[kMaxChannels]    = {};
    double mute_target_ = 1.0, mute_cur_ = 1.0;   // linear, 1 = audible
    double bypass_target_ = 0.0, bypass_cur_ = 0.0;  // 1 = fully dry

    std::vector<float>  dry_;        // one control block of pre-EQ audio, per channel
    std::vector<float>  scratch_;    // interleaving scratch
    std::vector<float*> pointers_;   // planar pointers into scratch_
};

}  // namespace isotone
