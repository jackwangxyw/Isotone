// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/processor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "isotone/apo_config.h"
#include "isotone/speakers.h"

// Flush-to-zero lives in SSE1; denormals-are-zero needs SSE3. MSVC on x64 always
// has both. GCC and Clang only define the SSE3 intrinsic when the target allows
// it, so DAZ is guarded separately rather than assumed.
#if defined(_MSC_VER) && defined(_M_X64)
#include <pmmintrin.h>
#include <xmmintrin.h>
#define ISOTONE_HAVE_FTZ 1
#define ISOTONE_HAVE_DAZ 1
#elif defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#define ISOTONE_HAVE_FTZ 1
#if defined(__SSE3__)
#include <pmmintrin.h>
#define ISOTONE_HAVE_DAZ 1
#endif
#endif

namespace isotone {
namespace {

constexpr double kMinWidth = 1e-4;

double db_to_linear(double db) { return std::pow(10.0, db / 20.0); }

// One step of a one-pole smoother: current moves a fixed fraction of the
// remaining distance each control block, which gives an exponential approach
// with no overshoot and no discontinuity.
inline void approach(double& cur, double target, double coef) {
    cur += (target - cur) * coef;
}

inline bool near_enough(double a, double b, double eps) { return std::abs(a - b) <= eps; }

// Transposed direct form II. Two state words per section, and the input sample
// is read once, which is why this form is preferred for float state: the
// rounding error does not accumulate through a delay line of inputs.
inline double step(const BiquadCoeffs& c, double& s1, double& s2, double x) {
    const double y = c.b0 * x + s1;
    s1 = c.b1 * x - c.a1 * y + s2;
    s2 = c.b2 * x - c.a2 * y;
    return y;
}

void set_identity(double m[kMaxChannels][kMaxChannels]) {
    for (uint32_t o = 0; o < kMaxChannels; ++o) {
        for (uint32_t i = 0; i < kMaxChannels; ++i) {
            m[o][i] = o == i ? 1.0 : 0.0;
        }
    }
}

double finite_or(double v, double fallback) { return std::isfinite(v) ? v : fallback; }

bool covers(ChannelMask mask, uint32_t channel) {
    return mask == kAllChannels || (channel < kMaskChannels && (mask & (ChannelMask{1} << channel)) != 0);
}

// A band with a zero or negative width designs to identity (biquad.cpp), so the
// processor treats it as disabled too: what is drawn is what is heard.
bool effective_enabled(const Band& in) {
    return in.enabled && !(std::isfinite(in.width) && in.width <= 0.0);
}

}  // namespace

uint32_t control_block_frames(double sample_rate) {
    if (!(sample_rate > 0.0)) {
        return kControlBlockAt48k;
    }
    const double scaled = sample_rate * kControlBlockAt48k / 48000.0;
    const long   n      = std::lround(scaled);
    return static_cast<uint32_t>(std::clamp<long>(n, 1, 512));
}

void enable_denormal_flushing() {
#if defined(ISOTONE_HAVE_FTZ)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif
#if defined(ISOTONE_HAVE_DAZ)
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

void Processor::initialize(double sample_rate, uint32_t channels, uint32_t max_frames,
                           uint32_t max_bands, uint32_t speaker_mask) {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
    channels_    = channels;
    max_frames_  = max_frames;
    max_bands_   = max_bands;
    band_count_  = 0;

    control_frames_ = control_block_frames(sample_rate_);

    // Fraction of the remaining distance covered per control block, for a time
    // constant of kSmoothingTauSeconds.
    const double block_seconds = control_frames_ / sample_rate_;
    smoothing_coef_ = 1.0 - std::exp(-block_seconds / kSmoothingTauSeconds);
    fade_step_      = block_seconds / kCrossfadeSeconds;

    bands_.assign(max_bands_, BandSlot{});
    slot_claimed_.assign(max_bands_, 0);
    band_slot_.assign(max_bands_, 0);
    state_new_.assign(static_cast<size_t>(max_bands_) * channels_, State{});
    state_old_.assign(static_cast<size_t>(max_bands_) * channels_, State{});

    scratch_.assign(static_cast<size_t>(max_frames_) * channels_, 0.0f);
    pointers_.assign(channels_, nullptr);

    preamp_target_ = preamp_cur_ = 0.0;
    preamp_begin_lin_ = preamp_end_lin_ = 1.0;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        trim_target_[c] = trim_cur_[c] = 0.0;
    }
    mute_target_   = mute_cur_   = 1.0;
    bypass_target_ = bypass_cur_ = 0.0;

    // Speaker setup.
    speaker_mask_ = speaker_mask != 0 ? speaker_mask : default_speaker_mask(channels_);
    lfe_ = speaker_channel(speaker_mask_, channels_, kSpeakerLowFrequency);
    routed_ = std::min(channels_, kMaxChannels);

    set_identity(mat_target_);
    set_identity(mat_cur_);
    set_identity(mat_begin_);
    mat_settled_ = mat_identity_ = true;
    mat_active_ = mat_interp_ = false;

    log_xover_target_ = log_xover_cur_ = std::log(80.0);
    log_lfe_target_   = log_lfe_cur_   = std::log(120.0);
    recompute_bass_filters();
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        bass_target_[c] = bass_cur_[c] = bass_begin_[c] = 0.0;
        xover_lp_state_[c].clear();
        xover_hp_state_[c].clear();
    }
    lfe_state_.clear();
    bass_active_ = false;

    chan_target_.assign(channels_, 1.0);
    chan_cur_.assign(channels_, 1.0);
    post_prev_.assign(channels_, 1.0);

    const auto longest = static_cast<uint32_t>(std::ceil(kMaxDelaySeconds * sample_rate_)) + 1;
    delay_size_ = 1;
    while (delay_size_ < longest) {
        delay_size_ <<= 1;
    }
    delay_buf_.assign(static_cast<size_t>(channels_) * delay_size_, 0.0f);
    delay_pos_ = 0;
    delay_target_.assign(channels_, 0);
    delay_cur_.assign(channels_, 0);
    delay_old_.assign(channels_, 0);
    delay_fade_.assign(channels_, 1.0);
    delay_fade_step_ = 1.0 / (kCrossfadeSeconds * sample_rate_);
}

void Processor::recompute_bass_filters() {
    const double xover = std::exp(log_xover_cur_);
    xover_lp_ = butterworth2(FilterType::LowPass, xover, sample_rate_);
    xover_hp_ = butterworth2(FilterType::HighPass, xover, sample_rate_);
    lfe_lp_   = butterworth2(FilterType::LowPass, std::exp(log_lfe_cur_), sample_rate_);
}

void Processor::set_speaker_targets(const SpeakerSetup& sp) {
    // Routing.
    double routing[kMaxChannels][kMaxChannels];
    routing_matrix(sp, speaker_mask_, channels_, routing);
    bool identity = true;
    bool changed = false;
    for (uint32_t o = 0; o < kMaxChannels; ++o) {
        for (uint32_t i = 0; i < kMaxChannels; ++i) {
            const double v = routing[o][i];
            identity &= v == (o == i ? 1.0 : 0.0);
            changed |= v != mat_target_[o][i];
            mat_target_[o][i] = v;
        }
    }
    mat_identity_ = identity;
    if (changed) {
        mat_settled_ = false;
    }

    // Bass management. The crossover and LFE low-pass keep their previous value
    // when given a non-finite one, for the same reason as the band parameters.
    log_xover_target_ = std::log(std::clamp(finite_or(sp.crossover_hz, std::exp(log_xover_target_)),
                                            kMinBassHz, kMaxBassHz));
    log_lfe_target_   = std::log(std::clamp(finite_or(sp.lfe_lowpass_hz, std::exp(log_lfe_target_)),
                                            kMinBassHz, kMaxBassHz));
    const bool managed = sp.bass_management && lfe_ >= 0;
    for (uint32_t c = 0; c < routed_; ++c) {
        const bool redirected = (sp.small_speakers & (ChannelMask{1} << c)) != 0;
        bass_target_[c] = static_cast<int>(c) == lfe_ ? (managed ? 1.0 : 0.0)
                                                      : (managed && redirected ? 1.0 : 0.0);
    }

    // Polarity and speaker mute.
    for (uint32_t c = 0; c < channels_; ++c) {
        const ChannelMask bit = c < kMaskChannels ? ChannelMask{1} << c : 0;
        const double sign = (sp.inverted & bit) != 0 ? -1.0 : 1.0;
        chan_target_[c] = (sp.muted & bit) != 0 ? 0.0 : sign;
    }

    // Delay, rounded to whole samples the way Equalizer APO's DelayFilter does.
    for (uint32_t c = 0; c < channels_; ++c) {
        const double ms = channel_delay_ms(sp, c, kMaxDelaySeconds * 1000.0);
        const double samples = std::floor(ms * sample_rate_ / 1000.0 + 0.5);
        delay_target_[c] = static_cast<uint32_t>(std::min(samples, static_cast<double>(delay_size_ - 1)));
    }
}

void Processor::recompute_band(BandSlot& b) {
    Band band;
    band.type         = b.type;
    band.fc           = std::exp(b.log_fc_cur);
    band.gain_db      = b.gain_cur;
    band.width        = std::exp(b.log_w_cur);
    band.width_mode   = b.width_mode;
    band.shelf_corner = b.shelf_corner;
    band.channels     = b.channels;
    band.enabled      = b.enabled;
    b.coeffs = design(band, sample_rate_);
}

void Processor::begin_crossfade(uint32_t index) {
    BandSlot& b = bands_[index];
    b.old_coeffs = b.coeffs;
    b.old_channels = b.channels;
    b.fade = 0.0;
    b.fade_prev = 0.0;

    // The old filter has to carry on from exactly where it was, so hand it the
    // live state. The new filter starts from rest: its own startup transient is
    // then multiplied by a crossfade weight that begins at zero, which is what
    // keeps the swap silent. Leaving stale state in the new filter instead lets
    // the previous filter's accumulated energy appear at the output in one
    // sample, which is a loud click.
    for (uint32_t c = 0; c < channels_; ++c) {
        const size_t k = static_cast<size_t>(index) * channels_ + c;
        state_old_[k] = state_new_[k];
        state_new_[k].clear();
    }
}

void Processor::apply_band(uint32_t index, const Band& in) {
    BandSlot& b = bands_[index];

    // A non-finite value keeps the previous target. Fed to a smoother it would
    // make the current value NaN, and a NaN smoother never recovers, so one bad
    // write to shared memory would silence the device until the stream
    // restarted. Finite values are clamped for the same reason.
    const double fc      = std::max(clamp_fc(in.fc, sample_rate_), kMinFc);
    const bool   enabled = effective_enabled(in);
    const double log_fc  = std::isfinite(fc) ? std::log(fc) : b.log_fc_target;
    const double log_w   = std::isfinite(in.width) && in.width > 0.0 ? std::log(std::max(in.width, kMinWidth))
                                                                     : b.log_w_target;
    const double gain    = std::isfinite(in.gain_db) ? std::clamp(in.gain_db, -kMaxBandGainDb, kMaxBandGainDb)
                                                     : b.gain_target;

    // A change of shape that cannot be interpolated: the filter is a different
    // filter afterwards, so its output is crossfaded instead.
    const bool discontinuous = b.occupied && (b.type != in.type ||
                                              b.width_mode != in.width_mode ||
                                              b.shelf_corner != in.shelf_corner ||
                                              b.enabled != enabled ||
                                              b.id != in.id ||
                                              b.channels != in.channels);
    // Widths in different modes are different units; sweeping between them
    // designs filters that were never asked for.
    const bool mode_changed = b.occupied && b.width_mode != in.width_mode;

    if (!b.occupied) {
        // A band appearing where there was none: start it at its target so it
        // does not sweep in from a default, and fade it up from silence.
        b.log_fc_cur = log_fc;
        b.gain_cur   = gain;
        b.log_w_cur  = log_w;
    }

    if (discontinuous || !b.occupied) {
        const bool was_occupied = b.occupied;
        begin_crossfade(index);
        if (!was_occupied) {
            // Nothing was here before, so the outgoing filter is a wire.
            b.old_coeffs = BiquadCoeffs::identity();
        }
    }

    b.type         = in.type;
    b.width_mode   = in.width_mode;
    b.shelf_corner = in.shelf_corner;
    b.channels     = in.channels;
    b.enabled      = enabled;
    b.id           = in.id;
    b.occupied     = true;

    b.log_fc_target = log_fc;
    b.gain_target   = gain;
    b.log_w_target  = log_w;
    if (mode_changed) {
        b.log_w_cur = log_w;
    }

    if (discontinuous || b.fade < 1.0) {
        // Recompute immediately so the crossfade targets the new shape from its
        // first sample rather than one control block later.
        recompute_band(b);
    }
}

// A band that went away fades out to unity rather than vanishing.
void Processor::remove_band(uint32_t index) {
    BandSlot& b = bands_[index];
    begin_crossfade(index);
    b.occupied = false;
    b.enabled  = false;
    b.coeffs   = BiquadCoeffs::identity();
}

// Chooses a slot for each of the first `count` bands, in band_slot_. A band
// keeps the slot that holds (or is queued to hold) its id. A new band takes a
// free slot, else one still fading out, where it waits for the fade to end,
// else the slot of a band that is being removed, which then crossfades to it.
void Processor::match_bands(const EqState& state, uint32_t count) {
    constexpr uint32_t kNone = ~uint32_t{0};
    std::fill(slot_claimed_.begin(), slot_claimed_.end(), uint8_t{0});
    for (uint32_t k = 0; k < count; ++k) {
        band_slot_[k] = kNone;
        const uint32_t id = state.bands[k].id;
        for (uint32_t i = 0; i < max_bands_; ++i) {
            const BandSlot& b = bands_[i];
            const bool queued_add = b.pending && !b.pending_remove;
            if (slot_claimed_[i] || !(b.occupied || queued_add)) continue;
            if ((queued_add ? b.pending_band.id : b.id) == id) {
                band_slot_[k] = i;
                slot_claimed_[i] = 1;
                break;
            }
        }
    }
    const auto place = [&](uint32_t k, bool (*usable)(const BandSlot&)) {
        for (uint32_t i = 0; i < max_bands_ && band_slot_[k] == kNone; ++i) {
            if (!slot_claimed_[i] && usable(bands_[i])) {
                band_slot_[k] = i;
                slot_claimed_[i] = 1;
            }
        }
    };
    for (uint32_t k = 0; k < count; ++k) place(k, [](const BandSlot& b) { return !b.occupied && !b.pending && b.fade >= 1.0; });
    for (uint32_t k = 0; k < count; ++k) place(k, [](const BandSlot& b) { return !b.occupied; });
    for (uint32_t k = 0; k < count; ++k) place(k, [](const BandSlot&) { return true; });
}

void Processor::set_target(const EqState& state) {
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(state.bands.size(), max_bands_));
    match_bands(state, count);

    for (uint32_t k = 0; k < count; ++k) {
        const Band& in = state.bands[k];
        const uint32_t i = band_slot_[k];
        BandSlot&   b  = bands_[i];
        const bool shape_differs = !b.occupied || b.type != in.type || b.width_mode != in.width_mode ||
                                   b.shelf_corner != in.shelf_corner || b.enabled != effective_enabled(in) ||
                                   b.id != in.id || b.channels != in.channels;
        if (b.fade < 1.0 && shape_differs) {
            b.pending        = true;
            b.pending_remove = false;
            b.pending_band   = in;
            continue;
        }
        b.pending = false;
        b.pending_remove = false;
        apply_band(i, in);
    }

    for (uint32_t i = 0; i < max_bands_; ++i) {
        if (slot_claimed_[i]) continue;
        BandSlot& b = bands_[i];
        if (b.occupied && b.fade < 1.0) {
            b.pending        = true;
            b.pending_remove = true;
            continue;
        }
        b.pending = false;
        b.pending_remove = false;
        if (b.occupied) {
            remove_band(i);
        }
    }

    band_count_ = count;

    if (std::isfinite(state.preamp_db)) {
        preamp_target_ = std::clamp(state.preamp_db, kMinLevelDb, kMaxLevelDb);
    }
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        if (std::isfinite(state.channel_gain_db[c])) {
            trim_target_[c] = std::clamp(state.channel_gain_db[c], kMinLevelDb, kMaxLevelDb);
        }
    }
    mute_target_   = state.mute ? 0.0 : 1.0;
    bypass_target_ = state.bypass ? 1.0 : 0.0;

    set_speaker_targets(state.speakers);
}

void Processor::reset() {
    preamp_cur_ = preamp_target_;
    preamp_begin_lin_ = preamp_end_lin_ = db_to_linear(preamp_cur_);
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        trim_cur_[c] = trim_target_[c];
    }
    mute_cur_   = mute_target_;
    bypass_cur_ = bypass_target_;

    for (uint32_t i = 0; i < max_bands_; ++i) {
        BandSlot& b = bands_[i];
        if (b.pending) {
            b.pending = false;
            if (b.pending_remove) {
                b.pending_remove = false;
                if (b.occupied) remove_band(i);
            } else {
                apply_band(i, b.pending_band);
            }
        }
        b.log_fc_cur = b.log_fc_target;
        b.gain_cur   = b.gain_target;
        b.log_w_cur  = b.log_w_target;
        b.fade       = 1.0;
        b.fade_prev  = 1.0;
        recompute_band(b);
    }
    for (State& s : state_new_) s.clear();
    for (State& s : state_old_) s.clear();

    std::memcpy(mat_cur_, mat_target_, sizeof(mat_cur_));
    std::memcpy(mat_begin_, mat_target_, sizeof(mat_begin_));
    mat_settled_ = true;
    mat_interp_  = false;
    mat_active_  = !mat_identity_;

    log_xover_cur_ = log_xover_target_;
    log_lfe_cur_   = log_lfe_target_;
    recompute_bass_filters();
    bass_active_ = false;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        bass_cur_[c] = bass_begin_[c] = bass_target_[c];
        bass_active_ |= bass_target_[c] > 0.0;
        xover_lp_state_[c].clear();
        xover_hp_state_[c].clear();
    }
    lfe_state_.clear();

    for (uint32_t c = 0; c < channels_; ++c) {
        chan_cur_[c] = chan_target_[c];
        const double trim = c < kMaxChannels ? db_to_linear(trim_cur_[c]) : 1.0;
        post_prev_[c] = trim * mute_cur_ * chan_cur_[c];
        delay_cur_[c] = delay_old_[c] = delay_target_[c];
        delay_fade_[c] = 1.0;
    }
    std::fill(delay_buf_.begin(), delay_buf_.end(), 0.0f);
    delay_pos_ = 0;
}

void Processor::advance_smoothers(uint32_t frames) {
    // Every smoother and fade moves by the time `frames` covers, so a host's
    // buffer size does not change how fast parameters move. A full control
    // block is the common case and uses the precomputed coefficient.
    const double part  = static_cast<double>(frames) / static_cast<double>(control_frames_);
    const double coef  = frames == control_frames_ ? smoothing_coef_
                                                   : 1.0 - std::pow(1.0 - smoothing_coef_, part);
    const double fstep = fade_step_ * part;

    preamp_begin_lin_ = db_to_linear(preamp_cur_);
    approach(preamp_cur_, preamp_target_, coef);
    preamp_end_lin_ = db_to_linear(preamp_cur_);
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        approach(trim_cur_[c], trim_target_[c], coef);
    }
    // Mute ends exactly on its target, as the routing and bass gains do, so a
    // muted output is digital silence rather than a decaying fraction.
    approach(mute_cur_, mute_target_, coef);
    if (near_enough(mute_cur_, mute_target_, 1e-6)) mute_cur_ = mute_target_;

    bypass_cur_ += std::clamp(bypass_target_ - bypass_cur_, -fstep, fstep);

    for (uint32_t i = 0; i < max_bands_; ++i) {
        BandSlot& b = bands_[i];
        if (b.pending && b.fade >= 1.0) {
            // The fade this change waited for ended in an earlier block, whose
            // weight ramp ran to its last sample: start the change's own fade.
            // Starting it in the block the fade ends in would restart that
            // block's ramp from zero and step the output.
            b.pending = false;
            if (b.pending_remove) {
                b.pending_remove = false;
                if (b.occupied) remove_band(i);
            } else {
                apply_band(i, b.pending_band);
            }
        }
        if (!b.occupied && b.fade >= 1.0) {
            b.fade_prev = 1.0;
            continue;
        }
        approach(b.log_fc_cur, b.log_fc_target, coef);
        approach(b.gain_cur,   b.gain_target,   coef);
        approach(b.log_w_cur,  b.log_w_target,  coef);
        b.fade_prev = b.fade;
        if (b.fade < 1.0) {
            b.fade = std::min(1.0, b.fade + fstep);
        }
        recompute_band(b);
    }

    // Routing matrix.
    if (!mat_settled_) {
        std::memcpy(mat_begin_, mat_cur_, sizeof(mat_begin_));
        bool settled = true;
        for (uint32_t o = 0; o < routed_; ++o) {
            for (uint32_t i = 0; i < routed_; ++i) {
                approach(mat_cur_[o][i], mat_target_[o][i], coef);
                if (near_enough(mat_cur_[o][i], mat_target_[o][i], 1e-6)) {
                    mat_cur_[o][i] = mat_target_[o][i];
                } else {
                    settled = false;
                }
            }
        }
        mat_settled_ = settled;
        mat_interp_  = true;
        mat_active_  = true;
    } else {
        mat_interp_ = false;
        mat_active_ = !mat_identity_;
    }

    // Bass management.
    if (!near_enough(log_xover_cur_, log_xover_target_, 1e-9) ||
        !near_enough(log_lfe_cur_, log_lfe_target_, 1e-9)) {
        approach(log_xover_cur_, log_xover_target_, coef);
        approach(log_lfe_cur_, log_lfe_target_, coef);
        recompute_bass_filters();
    }
    bass_active_ = false;
    for (uint32_t c = 0; c < routed_; ++c) {
        bass_begin_[c] = bass_cur_[c];
        approach(bass_cur_[c], bass_target_[c], coef);
        if (near_enough(bass_cur_[c], bass_target_[c], 1e-6)) {
            bass_cur_[c] = bass_target_[c];
        }
        if (bass_begin_[c] == 0.0 && bass_cur_[c] == 0.0) {
            // Out of use: start from rest next time, so no stale ringing leaks in.
            xover_lp_state_[c].clear();
            xover_hp_state_[c].clear();
            if (static_cast<int>(c) == lfe_) lfe_state_.clear();
        } else {
            bass_active_ = true;
        }
    }

    // Polarity and speaker mute.
    for (uint32_t c = 0; c < channels_; ++c) {
        approach(chan_cur_[c], chan_target_[c], coef);
        if (near_enough(chan_cur_[c], chan_target_[c], 1e-6)) chan_cur_[c] = chan_target_[c];
    }

    // A delay change starts when the previous one has finished fading.
    for (uint32_t c = 0; c < channels_; ++c) {
        if (delay_fade_[c] >= 1.0 && delay_target_[c] != delay_cur_[c]) {
            delay_old_[c]  = delay_cur_[c];
            delay_cur_[c]  = delay_target_[c];
            delay_fade_[c] = 0.0;
        }
    }
}

bool Processor::is_settling() const {
    if (!near_enough(preamp_cur_, preamp_target_, 1e-4)) return true;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        if (!near_enough(trim_cur_[c], trim_target_[c], 1e-4)) return true;
    }
    if (!near_enough(mute_cur_, mute_target_, 1e-5)) return true;
    if (!near_enough(bypass_cur_, bypass_target_, 1e-5)) return true;

    for (const BandSlot& b : bands_) {
        if (b.fade < 1.0 || b.pending) return true;
        if (!b.occupied) continue;
        if (!near_enough(b.log_fc_cur, b.log_fc_target, 1e-6)) return true;
        if (!near_enough(b.gain_cur,   b.gain_target,   1e-4)) return true;
        if (!near_enough(b.log_w_cur,  b.log_w_target,  1e-6)) return true;
    }

    if (!mat_settled_) return true;
    if (!near_enough(log_xover_cur_, log_xover_target_, 1e-6)) return true;
    if (!near_enough(log_lfe_cur_, log_lfe_target_, 1e-6)) return true;
    for (uint32_t c = 0; c < routed_; ++c) {
        if (bass_cur_[c] != bass_target_[c]) return true;
    }
    for (uint32_t c = 0; c < channels_; ++c) {
        if (!near_enough(chan_cur_[c], chan_target_[c], 1e-5)) return true;
        if (delay_cur_[c] != delay_target_[c] || delay_fade_[c] < 1.0) return true;
    }
    return false;
}

void Processor::stage_matrix(float* const* planar, uint32_t offset, uint32_t frames) {
    if (!mat_active_) {
        return;
    }
    double in[kMaxChannels];
    for (uint32_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n + 1) / static_cast<double>(frames);
        for (uint32_t i = 0; i < routed_; ++i) {
            in[i] = planar[i][offset + n];
        }
        for (uint32_t o = 0; o < routed_; ++o) {
            double acc = 0.0;
            for (uint32_t i = 0; i < routed_; ++i) {
                const double m = mat_interp_ ? mat_begin_[o][i] + (mat_cur_[o][i] - mat_begin_[o][i]) * t
                                             : mat_cur_[o][i];
                // 0 * NaN is NaN: a bad sample must reach only the outputs it feeds.
                if (m != 0.0) acc += m * in[i];
            }
            planar[o][offset + n] = static_cast<float>(acc);
        }
    }
}

void Processor::stage_bass(float* const* planar, uint32_t offset, uint32_t frames) {
    if (!bass_active_) {
        return;
    }
    auto lr4 = [](const BiquadCoeffs& k, Lr4State& s, double x) {
        return step(k, s.b.s1, s.b.s2, step(k, s.a.s1, s.a.s2, x));
    };
    for (uint32_t n = 0; n < frames; ++n) {
        const double t = static_cast<double>(n + 1) / static_cast<double>(frames);
        double sub = 0.0;
        for (uint32_t c = 0; c < routed_; ++c) {
            if (static_cast<int>(c) == lfe_ || (bass_begin_[c] == 0.0 && bass_cur_[c] == 0.0)) {
                continue;
            }
            // The main speaker keeps what is above the crossover and the sub
            // takes what is below. The two halves are in phase at every
            // frequency, so they add back to the input's level.
            const double m  = bass_begin_[c] + (bass_cur_[c] - bass_begin_[c]) * t;
            const double x  = planar[c][offset + n];
            double lo = lr4(xover_lp_, xover_lp_state_[c], x);
            double hi = lr4(xover_hp_, xover_hp_state_[c], x);
            if (!std::isfinite(lo) || !std::isfinite(hi)) {
                // Non-finite input poisons filter state for good; start over.
                xover_lp_state_[c].clear();
                xover_hp_state_[c].clear();
                lo = hi = 0.0;
            }
            planar[c][offset + n] = static_cast<float>(std::isfinite(x) ? x + (hi - x) * m : 0.0);
            sub += lo * m;
        }
        if (lfe_ >= 0) {
            const uint32_t c = static_cast<uint32_t>(lfe_);
            double x = planar[c][offset + n];
            if (bass_begin_[c] != 0.0 || bass_cur_[c] != 0.0) {
                const double m = bass_begin_[c] + (bass_cur_[c] - bass_begin_[c]) * t;
                x += (lr4(lfe_lp_, lfe_state_, x) - x) * m;
                if (!std::isfinite(x)) {
                    lfe_state_.clear();
                    x = 0.0;
                }
            }
            planar[c][offset + n] = static_cast<float>(x + sub);
        }
    }
}

void Processor::stage_delay(float* const* planar, uint32_t offset, uint32_t frames) {
    const uint32_t mask = delay_size_ - 1;
    for (uint32_t c = 0; c < channels_; ++c) {
        float* chan = planar[c] + offset;
        float* ring = delay_buf_.data() + static_cast<size_t>(c) * delay_size_;
        uint32_t pos = delay_pos_;
        if (delay_cur_[c] == 0 && delay_fade_[c] >= 1.0) {
            for (uint32_t n = 0; n < frames; ++n) {
                ring[pos] = chan[n];
                pos = (pos + 1) & mask;
            }
            continue;
        }
        const uint32_t cur = delay_cur_[c];
        const uint32_t old = delay_old_[c];
        double fade = delay_fade_[c];
        for (uint32_t n = 0; n < frames; ++n) {
            ring[pos] = chan[n];
            const double now = ring[(pos - cur) & mask];
            if (fade < 1.0) {
                fade = std::min(1.0, fade + delay_fade_step_);
                const double before = ring[(pos - old) & mask];
                chan[n] = static_cast<float>(before + (now - before) * fade);
            } else {
                chan[n] = static_cast<float>(now);
            }
            pos = (pos + 1) & mask;
        }
        delay_fade_[c] = fade;
    }
    delay_pos_ = (delay_pos_ + frames) & mask;
}

void Processor::process_block(float* const* planar, uint32_t offset, uint32_t frames, double bypass_begin,
                              double bypass_end) {
    // Order: routing, bass management, EQ with the per-speaker gains, delay.
    // Speaker EQ therefore acts on what each speaker actually plays, including
    // the bass that bass management sends to the sub.
    stage_matrix(planar, offset, frames);
    stage_bass(planar, offset, frames);
    // Bypass is a wet/dry crossfade around the EQ stage alone: preamp and bands.
    // The mix is interpolated across the block, because a mix that steps once
    // per control block is itself a staircase and audible.
    const bool bypassing = bypass_begin > 1e-9 || bypass_end > 1e-9;

    for (uint32_t c = 0; c < channels_; ++c) {
        // Only the first kMaxChannels channels have a trim; the rest sit at 0 dB.
        const double trim = c < kMaxChannels ? db_to_linear(trim_cur_[c]) : 1.0;
        const double post_begin = post_prev_[c];
        const double post_end = trim * mute_cur_ * chan_cur_[c];
        post_prev_[c] = post_end;
        float* chan = planar[c] + offset;

        for (uint32_t n = 0; n < frames; ++n) {
            // Preamp, crossfade weights and post gain are all ramped across the
            // block, so none of them steps once per control block.
            const double t = static_cast<double>(n + 1) / static_cast<double>(frames);
            const double dry = static_cast<double>(chan[n]);
            double x = dry * (preamp_begin_lin_ + (preamp_end_lin_ - preamp_begin_lin_) * t);

            for (uint32_t i = 0; i < max_bands_; ++i) {
                BandSlot& b = bands_[i];
                if (!b.occupied && b.fade >= 1.0 && b.fade_prev >= 1.0) {
                    continue;
                }
                const bool in_new = covers(b.channels, c);
                const size_t k = static_cast<size_t>(i) * channels_ + c;
                if (b.fade < 1.0 || b.fade_prev < 1.0) {
                    // A change of channels fades the band out where it was and
                    // in where it is going, like any other change of shape.
                    const bool in_old = covers(b.old_channels, c);
                    if (!in_new && !in_old) {
                        continue;
                    }
                    const double w = b.fade_prev + (b.fade - b.fade_prev) * t;
                    const double y_old = in_old ? step(b.old_coeffs, state_old_[k].s1, state_old_[k].s2, x) : x;
                    const double y_new = in_new ? step(b.coeffs, state_new_[k].s1, state_new_[k].s2, x) : x;
                    x = y_old * (1.0 - w) + y_new * w;
                } else {
                    if (!in_new) {
                        continue;
                    }
                    x = step(b.coeffs, state_new_[k].s1, state_new_[k].s2, x);
                }
            }

            if (bypassing) {
                const double mix = bypass_begin + (bypass_end - bypass_begin) * t;
                x = x * (1.0 - mix) + dry * mix;
            }

            double y = x * (post_begin + (post_end - post_begin) * t);
            if (!std::isfinite(y) || std::abs(y) > std::numeric_limits<float>::max()) {
                // Non-finite input, a filter that overflowed, or gains that
                // multiply past what a float holds would keep filter state NaN
                // until the stream restarted, or send inf downstream. Start this
                // channel's filters from rest and output silence for the sample.
                for (uint32_t i = 0; i < max_bands_; ++i) {
                    const size_t k = static_cast<size_t>(i) * channels_ + c;
                    state_new_[k].clear();
                    state_old_[k].clear();
                }
                y = 0.0;
            }
            chan[n] = static_cast<float>(y);
        }
    }

    stage_delay(planar, offset, frames);
}

void Processor::process(float* const* planar, uint32_t frames) {
    if (channels_ == 0 || frames == 0 || planar == nullptr) {
        return;
    }

    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(control_frames_, frames - done);
        const double mix_begin = bypass_cur_;
        advance_smoothers(n);
        process_block(planar, done, n, mix_begin, bypass_cur_);
        done += n;
    }
}

void Processor::process_interleaved(float* interleaved, uint32_t frames) {
    if (channels_ == 0 || frames == 0 || interleaved == nullptr || frames > max_frames_) {
        return;
    }
    for (uint32_t c = 0; c < channels_; ++c) {
        pointers_[c] = scratch_.data() + static_cast<size_t>(c) * max_frames_;
        for (uint32_t n = 0; n < frames; ++n) {
            pointers_[c][n] = interleaved[static_cast<size_t>(n) * channels_ + c];
        }
    }
    process(pointers_.data(), frames);
    for (uint32_t c = 0; c < channels_; ++c) {
        for (uint32_t n = 0; n < frames; ++n) {
            interleaved[static_cast<size_t>(n) * channels_ + c] = pointers_[c][n];
        }
    }
}

}  // namespace isotone
