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

// Longest delay a channel can have, speaker delay and lip sync together.
inline constexpr double kMaxDelaySeconds = 1.0;

// Range of the bass-management crossover and the LFE low-pass.
inline constexpr double kMinBassHz = 20.0;
inline constexpr double kMaxBassHz = 500.0;

// The largest boost or cut a band may apply, and the range of the preamp and
// the channel trims. Parameters come from shared memory any local user can
// write, so a finite but absurd value is clamped rather than allowed to
// overflow the filter state.
inline constexpr double kMaxBandGainDb = 60.0;
inline constexpr double kMinLevelDb    = -120.0;
inline constexpr double kMaxLevelDb    = 60.0;

// The range of a band's width, for the same reason. A resonant filter peaks at
// about its Q: a low-pass at Q 1000 reaches +60 dB, the gain limit, and Q 1e17
// designs a pole on the unit circle. A shelf peaks higher, by up to its gain at
// Q 10 (+80 dB at the gain limit), which is where a dB slope is already held
// (biquad.cpp). A bandwidth has the lower limits that give the same Q: Q is at
// most 1 / (2 sinh(ln 2 / 2 * BW)), so 0.00145 and 0.145 octaves reach 995 and
// 9.94. The smallest Q and slope, 1e-4, stop a wide band near Nyquist designing
// a2 = -1.
inline constexpr double kMinWidth             = 1e-4;
inline constexpr double kMaxQ                 = 1000.0;
inline constexpr double kMaxShelfQ            = 10.0;
inline constexpr double kMinBandwidthOct      = 0.00145;
inline constexpr double kMinShelfBandwidthOct = 0.145;

// A band as the processor designs it: gain and width clamped, and off when its
// width is zero or negative. Non-finite values are left for design() to
// ignore. The curve draws this, and a processor plays it for a band with no
// earlier value of its own.
Band effective_band(const Band& band);

// A preamp or trim as the processor plays it: clamped, and 0 dB when not finite.
double effective_level_db(double db);

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
    // rate or channel count; it is not real-time safe. `speaker_mask` names the
    // speaker on each channel (speakers.h); 0 means the usual layout for the
    // channel count.
    void initialize(double sample_rate, uint32_t channels, uint32_t max_frames,
                    uint32_t max_bands = 64, uint32_t speaker_mask = 0);

    // Sets the destination for every smoothed parameter. Bands beyond
    // max_bands() are ignored. Real-time safe: no allocation.
    //
    // Bands are matched to their filters by Band::id, not by position, so
    // deleting, inserting or reordering bands changes only the bands concerned.
    // Ids must be unique within a state; bands that share an id are matched in
    // list order.
    //
    // Bypass (EqState::bypass) crossfades the bands and the preamp out. Mute,
    // trims, polarity, speaker mute, routing, bass management and delay stay.
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

        // Crossfade progress, 1.0 when not fading, and its value at the start
        // of the current block, so the weight ramps per sample.
        double fade = 1.0;
        double fade_prev = 1.0;
        double fade_step = 0.0;
        // The channels the outgoing filter covered, for a crossfade that also
        // changes the band's channels.
        ChannelMask old_channels = kAllChannels;
        // The fade changes nothing but the channels. Where the band stays it is
        // the same filter, which plays alone, from its own state.
        bool only_channels = false;

        bool occupied = false;

        // A change of shape that arrives while a crossfade is still running
        // waits until it finishes, so two changes never share one fade.
        bool pending        = false;
        bool pending_remove = false;
        Band pending_band;
    };

    // Two identical second-order Butterworth sections in series: a 24 dB/oct
    // Linkwitz-Riley filter.
    struct Lr4State {
        State a, b;
        void clear() { a.clear(); b.clear(); }
    };

    // What a band in `b` is set to by `in`: its smoothed parameters' targets,
    // and whether it plays.
    struct Targets {
        double log_fc = 0.0, gain = 0.0, log_w = 0.0;
        bool   enabled = false;
    };
    Targets targets_for(const BandSlot& b, const Band& in) const;

    void recompute_band(BandSlot& b);
    void match_bands(const EqState& state, uint32_t count);
    void apply_band(uint32_t index, const Band& in);
    void remove_band(uint32_t index);
    void advance_smoothers(uint32_t frames);
    void begin_crossfade(uint32_t index);
    void process_block(float* const* planar, uint32_t offset, uint32_t frames, double bypass_begin,
                       double bypass_end);

    void set_speaker_targets(const SpeakerSetup& sp);
    void recompute_bass_filters();
    void stage_matrix(float* const* planar, uint32_t offset, uint32_t frames);
    void stage_bass(float* const* planar, uint32_t offset, uint32_t frames);
    void stage_delay(float* const* planar, uint32_t offset, uint32_t frames);

    double   sample_rate_ = 48000.0;
    uint32_t channels_    = 0;
    uint32_t max_frames_  = 0;
    uint32_t max_bands_   = 0;
    uint32_t band_count_  = 0;
    uint32_t control_frames_ = kControlBlockAt48k;

    double smoothing_coef_ = 1.0;   // per control block
    double fade_step_      = 1.0;   // per control block

    std::vector<BandSlot> bands_;
    // set_target's working storage: which slots are taken, and the slot each
    // incoming band goes to.
    std::vector<uint8_t>  slot_claimed_;
    std::vector<uint32_t> band_slot_;
    std::vector<State>    state_new_;   // [band * channels + channel]
    std::vector<State>    state_old_;

    // Global smoothed values.
    double preamp_target_ = 0.0, preamp_cur_ = 0.0;
    double preamp_begin_lin_ = 1.0, preamp_end_lin_ = 1.0;   // linear, across the current block
    double trim_target_[kMaxChannels] = {};
    double trim_cur_[kMaxChannels]    = {};
    double mute_target_ = 1.0, mute_cur_ = 1.0;   // linear, 1 = audible
    double bypass_target_ = 0.0, bypass_cur_ = 0.0;  // 1 = EQ fully bypassed

    std::vector<float>  scratch_;    // interleaving scratch
    std::vector<float*> pointers_;   // planar pointers into scratch_

    // ---- Speaker setup.
    uint32_t speaker_mask_ = 0;
    int      lfe_ = -1;     // LFE channel, -1 when the stream has none
    uint32_t routed_ = 0;   // channels the routing and bass stages cover: min(channels, kMaxChannels)

    // Swaps and upmix as one matrix, out[o] = sum mat[o][i] * in[i]. While it
    // moves it is interpolated across each control block from mat_begin_.
    double mat_target_[kMaxChannels][kMaxChannels] = {};
    double mat_cur_[kMaxChannels][kMaxChannels]    = {};
    double mat_begin_[kMaxChannels][kMaxChannels]  = {};
    bool   mat_settled_  = true;
    bool   mat_identity_ = true;   // target is identity
    bool   mat_active_   = false;  // this block needs the stage
    bool   mat_interp_   = false;  // this block interpolates from mat_begin_

    // Bass management. bass_*[c] is a small speaker's share of redirection, or
    // for the LFE channel, how much of its low-pass is applied.
    double log_xover_target_ = 0.0, log_xover_cur_ = 0.0;
    double log_lfe_target_   = 0.0, log_lfe_cur_   = 0.0;
    BiquadCoeffs xover_lp_, xover_hp_, lfe_lp_;
    double bass_target_[kMaxChannels] = {};
    double bass_cur_[kMaxChannels]    = {};
    double bass_begin_[kMaxChannels]  = {};
    // Speaker mute on the bass a small speaker sends the LFE, 1 = audible, so a
    // muted speaker plays from the sub no more than from itself. Polarity does
    // not touch the redirected bass.
    double sent_target_[kMaxChannels] = {};
    double sent_cur_[kMaxChannels]    = {};
    double sent_begin_[kMaxChannels]  = {};
    bool   bass_active_ = false;
    Lr4State xover_lp_state_[kMaxChannels];
    Lr4State xover_hp_state_[kMaxChannels];
    Lr4State lfe_state_;

    // Polarity and speaker mute as a signed linear gain per channel, and the
    // post gain each channel's previous block ended on, so a block ramps from it.
    std::vector<double> chan_target_, chan_cur_, post_prev_;

    // Delay. Each channel has a ring that is always written, so turning a delay
    // on reads real history. A change crossfades from the old tap to the new.
    std::vector<float>    delay_buf_;   // [channel * delay_size_ + index]
    uint32_t              delay_size_ = 1;   // power of two
    uint32_t              delay_pos_  = 0;
    std::vector<uint32_t> delay_target_, delay_cur_, delay_old_;
    std::vector<double>   delay_fade_;  // 1 when not fading
    double                delay_fade_step_ = 1.0;   // per sample
};

}  // namespace isotone
