// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/processor.h"

#include <algorithm>
#include <cmath>

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
                           uint32_t max_bands) {
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
    state_new_.assign(static_cast<size_t>(max_bands_) * channels_, State{});
    state_old_.assign(static_cast<size_t>(max_bands_) * channels_, State{});

    dry_.assign(static_cast<size_t>(control_frames_) * channels_, 0.0f);
    scratch_.assign(static_cast<size_t>(max_frames_) * channels_, 0.0f);
    pointers_.assign(channels_, nullptr);

    preamp_target_ = preamp_cur_ = 0.0;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        trim_target_[c] = trim_cur_[c] = 0.0;
    }
    mute_target_   = mute_cur_   = 1.0;
    bypass_target_ = bypass_cur_ = 0.0;
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
    b.fade = 0.0;

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

void Processor::set_target(const EqState& state) {
    const uint32_t count = static_cast<uint32_t>(std::min<size_t>(state.bands.size(), max_bands_));

    for (uint32_t i = 0; i < count; ++i) {
        const Band& in = state.bands[i];
        BandSlot&   b  = bands_[i];

        // A non-finite value keeps the previous target. Fed to a smoother it
        // would make the current value NaN, and a NaN smoother never recovers,
        // so one bad write to shared memory would silence the device until the
        // stream restarted.
        const double fc     = std::max(clamp_fc(in.fc, sample_rate_), kMinFc);
        const double width  = std::max(in.width, kMinWidth);
        const double log_fc = std::isfinite(fc) ? std::log(fc) : b.log_fc_target;
        const double log_w  = std::isfinite(width) ? std::log(width) : b.log_w_target;
        const double gain   = std::isfinite(in.gain_db) ? in.gain_db : b.gain_target;

        // A change of shape that cannot be interpolated: the filter is a
        // different filter afterwards, so its output is crossfaded instead.
        const bool discontinuous = b.occupied && (b.type != in.type ||
                                                  b.width_mode != in.width_mode ||
                                                  b.shelf_corner != in.shelf_corner ||
                                                  b.enabled != in.enabled ||
                                                  b.id != in.id ||
                                                  b.channels != in.channels);

        if (!b.occupied) {
            // A band appearing where there was none: start it at its target so
            // it does not sweep in from a default, and fade it up from silence.
            b.log_fc_cur = log_fc;
            b.gain_cur   = gain;
            b.log_w_cur  = log_w;
        }

        if (discontinuous || !b.occupied) {
            const bool was_occupied = b.occupied;
            begin_crossfade(i);
            if (!was_occupied) {
                // Nothing was here before, so the outgoing filter is a wire.
                b.old_coeffs = BiquadCoeffs::identity();
            }
        }

        b.type         = in.type;
        b.width_mode   = in.width_mode;
        b.shelf_corner = in.shelf_corner;
        b.channels     = in.channels;
        b.enabled      = in.enabled;
        b.id           = in.id;
        b.occupied     = true;

        b.log_fc_target = log_fc;
        b.gain_target   = gain;
        b.log_w_target  = log_w;

        if (discontinuous || b.fade < 1.0) {
            // Recompute immediately so the crossfade targets the new shape from
            // its first sample rather than one control block later.
            recompute_band(b);
        }
    }

    // Bands that went away fade out to unity rather than vanishing.
    for (uint32_t i = count; i < max_bands_; ++i) {
        BandSlot& b = bands_[i];
        if (b.occupied) {
            begin_crossfade(i);
            b.occupied = false;
            b.enabled  = false;
            b.coeffs   = BiquadCoeffs::identity();
        }
    }

    band_count_ = count;

    if (std::isfinite(state.preamp_db)) {
        preamp_target_ = state.preamp_db;
    }
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        if (std::isfinite(state.channel_gain_db[c])) {
            trim_target_[c] = state.channel_gain_db[c];
        }
    }
    mute_target_   = state.mute ? 0.0 : 1.0;
    bypass_target_ = state.bypass ? 1.0 : 0.0;
}

void Processor::reset() {
    preamp_cur_ = preamp_target_;
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        trim_cur_[c] = trim_target_[c];
    }
    mute_cur_   = mute_target_;
    bypass_cur_ = bypass_target_;

    for (BandSlot& b : bands_) {
        b.log_fc_cur = b.log_fc_target;
        b.gain_cur   = b.gain_target;
        b.log_w_cur  = b.log_w_target;
        b.fade       = 1.0;
        recompute_band(b);
    }
    for (State& s : state_new_) s.clear();
    for (State& s : state_old_) s.clear();
}

void Processor::advance_smoothers(uint32_t) {
    approach(preamp_cur_, preamp_target_, smoothing_coef_);
    for (uint32_t c = 0; c < kMaxChannels; ++c) {
        approach(trim_cur_[c], trim_target_[c], smoothing_coef_);
    }
    approach(mute_cur_, mute_target_, smoothing_coef_);

    bypass_cur_ += std::clamp(bypass_target_ - bypass_cur_, -fade_step_, fade_step_);

    for (uint32_t i = 0; i < max_bands_; ++i) {
        BandSlot& b = bands_[i];
        if (!b.occupied && b.fade >= 1.0) {
            continue;
        }
        approach(b.log_fc_cur, b.log_fc_target, smoothing_coef_);
        approach(b.gain_cur,   b.gain_target,   smoothing_coef_);
        approach(b.log_w_cur,  b.log_w_target,  smoothing_coef_);
        if (b.fade < 1.0) {
            b.fade = std::min(1.0, b.fade + fade_step_);
        }
        recompute_band(b);
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
        if (b.fade < 1.0) return true;
        if (!b.occupied) continue;
        if (!near_enough(b.log_fc_cur, b.log_fc_target, 1e-6)) return true;
        if (!near_enough(b.gain_cur,   b.gain_target,   1e-4)) return true;
        if (!near_enough(b.log_w_cur,  b.log_w_target,  1e-6)) return true;
    }
    return false;
}

void Processor::process_block(float* const* planar, uint32_t offset, uint32_t frames) {
    const double preamp = db_to_linear(preamp_cur_);

    for (uint32_t c = 0; c < channels_; ++c) {
        // Only the first kMaxChannels channels have a trim; the rest sit at 0 dB.
        const double trim = c < kMaxChannels ? db_to_linear(trim_cur_[c]) : 1.0;
        const double post = trim * mute_cur_;
        float* chan = planar[c] + offset;

        for (uint32_t n = 0; n < frames; ++n) {
            double x = static_cast<double>(chan[n]) * preamp;

            for (uint32_t i = 0; i < max_bands_; ++i) {
                BandSlot& b = bands_[i];
                if (!b.occupied && b.fade >= 1.0) {
                    continue;
                }
                if (b.channels != kAllChannels &&
                    (c >= kMaskChannels || (b.channels & (ChannelMask{1} << c)) == 0)) {
                    continue;
                }
                const size_t k = static_cast<size_t>(i) * channels_ + c;
                if (b.fade < 1.0) {
                    const double y_old = step(b.old_coeffs, state_old_[k].s1, state_old_[k].s2, x);
                    const double y_new = step(b.coeffs, state_new_[k].s1, state_new_[k].s2, x);
                    x = y_old * (1.0 - b.fade) + y_new * b.fade;
                } else {
                    x = step(b.coeffs, state_new_[k].s1, state_new_[k].s2, x);
                }
            }

            chan[n] = static_cast<float>(x * post);
        }
    }
}

void Processor::process(float* const* planar, uint32_t frames) {
    if (channels_ == 0 || frames == 0 || planar == nullptr) {
        return;
    }

    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = std::min(control_frames_, frames - done);
        const bool need_dry = bypass_cur_ > 1e-9 || bypass_target_ > 1e-9;

        if (need_dry) {
            for (uint32_t c = 0; c < channels_; ++c) {
                const float* src = planar[c] + done;
                std::copy(src, src + n, dry_.data() + static_cast<size_t>(c) * control_frames_);
            }
        }

        const double mix_begin = bypass_cur_;
        advance_smoothers(n);
        const double mix_end = bypass_cur_;

        process_block(planar, done, n);

        // Bypass is a wet/dry crossfade. The mix is interpolated across the
        // sub-block rather than held at one value for it, because a mix that
        // steps once per control block is itself a staircase and audible.
        if (need_dry && (mix_begin > 1e-9 || mix_end > 1e-9)) {
            for (uint32_t c = 0; c < channels_; ++c) {
                const float* src = dry_.data() + static_cast<size_t>(c) * control_frames_;
                float* dst = planar[c] + done;
                for (uint32_t i = 0; i < n; ++i) {
                    const double t = static_cast<double>(i + 1) / static_cast<double>(n);
                    const double mix = mix_begin + (mix_end - mix_begin) * t;
                    dst[i] = static_cast<float>(dst[i] * (1.0 - mix) + src[i] * mix);
                }
            }
        }
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
