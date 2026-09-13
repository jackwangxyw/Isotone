// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// Band and EqState: the model every host, the addon and the UI share.

#pragma once

#include <cstdint>
#include <vector>

namespace isotone {

// Kernel filter types. These are the eight distinct biquad designs; the richer
// set of Equalizer APO config tokens (PK/PEQ/Modal, LS/LSC, LP/LPQ, ...) maps
// onto these eight plus the WidthMode and shelf_corner fields below.
// Matches upstream Equalizer APO's BiQuad::Type set (filters/BiQuad.h).
enum class FilterType : uint8_t {
    Peaking = 0,
    LowPass,
    HighPass,
    BandPass,
    Notch,
    AllPass,
    LowShelf,
    HighShelf,
};

// How the `width` field of a Band is interpreted. Equalizer APO accepts all
// three spellings in a config file and they produce different alpha terms
// (verified in upstream filters/BiQuad.cpp).
enum class WidthMode : uint8_t {
    Q = 0,          // alpha = sin(w0) / (2Q)
    BandwidthOct,   // alpha = sin(w0) * sinh(ln2/2 * BW * w0/sin(w0))
    SlopeDb,        // shelves only; S = slope/12, alpha = sin(w0)/2 * sqrt((A+1/A)(1/S-1)+2)
};

// Bitmask over output channels. 0 means "all channels". A mask can name
// channels 0 to kMaskChannels - 1; channels past that are reached only by
// all-channel bands.
using ChannelMask = uint32_t;
inline constexpr ChannelMask kAllChannels = 0;
inline constexpr uint32_t kMaskChannels = 32;

// Channels that can carry a trim in EqState and the param block, and that the
// audio ring stores. Not a limit on how many channels are processed: the
// processor filters every channel of the stream.
inline constexpr uint32_t kMaxChannels = 8;

struct Band {
    uint32_t    id       = 0;                       // stable across edits, for UI handles and undo
    FilterType  type     = FilterType::Peaking;
    double      fc       = 1000.0;                  // Hz, clamped at design time
    double      gain_db  = 0.0;                     // PK, LS, HS only
    double      width    = 1.0;                     // Q, bandwidth in octaves, or slope in dB
    WidthMode   width_mode = WidthMode::Q;
    // Shelves only. True reproduces Equalizer APO's `LS`/`HS` tokens, which
    // shift the design frequency away from fc (the DCX2496 correction in
    // upstream BiQuadFilter::initialize). False is the `LSC`/`HSC` behaviour,
    // which uses fc directly. AutoEq emits LSC/HSC, so false.
    bool        shelf_corner = false;
    ChannelMask channels = kAllChannels;
    bool        enabled  = true;
};

enum class Upmix : uint8_t {
    Off = 0,
    All,        // front left/right also feed centre, sides and backs
    NoCentre,   // sides and backs only
};

// Speaker setup for outputs with more than two channels. Per-speaker values
// index channels in stream order, like channel_gain_db, and roles (front left,
// LFE, ...) come from the stream's speaker mask. Channel groups need nothing
// here: a group is a name for a ChannelMask, and bands already take a mask.
struct SpeakerSetup {
    double      delay_ms[kMaxChannels] = {0, 0, 0, 0, 0, 0, 0, 0};  // time alignment
    ChannelMask inverted = 0;           // polarity
    ChannelMask muted    = 0;           // mute and solo
    double      lip_sync_ms = 0.0;      // added to every channel
    bool        swap_left_right = false;
    bool        swap_front_rear = false;
    Upmix       upmix = Upmix::Off;
    bool        bass_management = false;
    double      crossover_hz = 80.0;    // 24 dB/oct Linkwitz-Riley
    // Speakers whose bass below the crossover goes to LFE. Not `small`:
    // rpcndr.h defines that as a macro.
    ChannelMask small_speakers = 0;
    double      lfe_lowpass_hz = 120.0; // 24 dB/oct, on the LFE channel's own content
};

struct EqState {
    bool              bypass      = false;
    double            preamp_db   = 0.0;
    bool              auto_preamp = false;          // computed UI-side, see plan 4.6
    std::vector<Band> bands;                        // unbounded in the core
    double            channel_gain_db[kMaxChannels] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool              mute        = false;
    SpeakerSetup      speakers;
};

// True if `band` contributes to output channel `channel`.
inline bool band_affects_channel(const Band& band, uint32_t channel) {
    // The bounds check matters: shifting a 32-bit value by 32 or more is
    // undefined, and on x86 it wraps, so channel 33 would match bit 1.
    return band.channels == kAllChannels ||
           (channel < kMaskChannels && (band.channels & (ChannelMask{1} << channel)) != 0);
}

}  // namespace isotone
