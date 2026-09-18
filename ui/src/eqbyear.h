// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// EQ by ear (docs/ui-spec.md): a sine on the current output through its EQ, the
// sweep, the Start, Top and End marks, and the band they make. For stereo and 2.1
// outputs only (owner, 2026-09-16); L and R are the front pair.
//
// The tone is SineTone through TestTone. Pausing fades it out and leaves the
// stream running a moment, so a quick play again does not reopen it; the auto
// sweep moves only while it plays, and the frequency shown follows it.

#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <windows.h>

#include <string>

#include "devicelink.h"
#include "sine_tone.h"
#include "test_tone.h"

class EqSession;
class QQmlEngine;
class QJSEngine;

class EqByEar : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool supported READ supported NOTIFY supportedChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(double frequency READ frequency WRITE setFrequency NOTIFY frequencyChanged)
    Q_PROPERTY(double levelDb READ levelDb WRITE setLevelDb NOTIFY levelChanged)
    // Channel: Left, Right, Both. The top bar's view (owner, 2026-09-16): L, R or L+R
    // on stereo; on 2.1 the front speaker the Showing picker shows alone, else both.
    Q_PROPERTY(int channel READ channel NOTIFY channelChanged)
    Q_PROPERTY(bool autoSweep READ autoSweep WRITE setAutoSweep NOTIFY sweepChanged)
    Q_PROPERTY(double sweepRate READ sweepRate WRITE setSweepRate NOTIFY sweepChanged)
    // The marks' frequencies, 0 when not set.
    Q_PROPERTY(double start READ start NOTIFY marksChanged)
    Q_PROPERTY(double top READ top NOTIFY marksChanged)
    Q_PROPERTY(double end READ end NOTIFY marksChanged)
    Q_PROPERTY(bool dip READ dip WRITE setDip NOTIFY marksChanged)
    Q_PROPERTY(bool canAddBand READ canAddBand NOTIFY marksChanged)

public:
    enum Mark { Start, Top, End };
    Q_ENUM(Mark)
    enum Channel { Left, Right, Both };
    Q_ENUM(Channel)

    static constexpr double kNudgeOctaves = 1.0 / 48.0;
    static constexpr double kCoarseNudgeOctaves = 1.0 / 6.0;
    static constexpr double kSweepRateMin = 0.05, kSweepRateMax = 10.0;
    static constexpr double kLevelMinDb = -90.0;
    // Add band on a peak this close to Top, on the same channels, adds to its gain.
    static constexpr double kSamePeakOctaves = 1.0 / 6.0;
    // How long a paused tone keeps its stream.
    static constexpr int kIdleStreamMs = 2000;

    // The band marks make: a peak at Top, Q = Top / |End - Start| (the marks are
    // edges heard, not -3 dB points, so a starting width), cut 3 dB for a peak and
    // boosted for a dip. The marks may be in any order.
    struct MarkedBand {
        double fc;
        double gainDb;
        double q;
    };
    static MarkedBand bandFromMarks(double start, double top, double end, bool dip);

    static EqByEar* create(QQmlEngine* qml, QJSEngine* js);
    explicit EqByEar(EqSession* session, QObject* parent = nullptr);
    ~EqByEar() override;

    bool supported() const;
    bool playing() const { return playing_; }
    void setPlaying(bool on);
    double frequency() const { return frequency_; }
    void setFrequency(double hz);
    double levelDb() const { return level_db_; }
    void setLevelDb(double db);
    int channel() const { return channel_; }
    bool autoSweep() const { return auto_sweep_; }
    void setAutoSweep(bool on);
    double sweepRate() const { return sweep_rate_; }
    void setSweepRate(double octaves_per_second);
    double start() const { return marks_[Start]; }
    double top() const { return marks_[Top]; }
    double end() const { return marks_[End]; }
    bool dip() const { return dip_; }
    void setDip(bool on);
    bool canAddBand() const;

    // Octaves up (negative: down).
    Q_INVOKABLE void nudge(double octaves);
    // Records the current frequency, to the hertz.
    Q_INVOKABLE void mark(int which);
    Q_INVOKABLE void clearMarks();
    // The marked band, added to the session on the tone's channels, or its gain added
    // to the nearest enabled peak within kSamePeakOctaves on those channels; the marks
    // are cleared.
    Q_INVOKABLE void addBand();
    // Leaving the view: the tone fades out.
    Q_INVOKABLE void stop();

signals:
    void supportedChanged();
    void playingChanged();
    void frequencyChanged();
    void levelChanged();
    void channelChanged();
    void sweepChanged();
    void marksChanged();
    // The stream could not open or failed; an HRESULT.
    void toneFailed(int hr);

private:
    void sessionChanged();
    void followView();
    void setChannel(int channel);
    void applySweep();
    void startStream();
    void stopStream();
    uint32_t toneMask() const;
    // A new band's channel mask for the tone's channel (EqSession::addBand's).
    int bandMask() const;
    // The row of that peak, or -1.
    int peakAt(double hz, int channelMask) const;

    QPointer<EqSession> session_;
    isotone::ui::SineTone sine_;
    isotone::ui::TestTone tone_;
    QTimer follow_;   // the sweep's frequency, while it plays
    QTimer idle_;     // closes a paused tone's stream
    quint64 generation_ = 0;
    bool stream_ = false;
    std::string stream_guid_;
    uint32_t stream_mask_ = 0;

    // What the session was for, to tell another output or layout.
    isotone::ui::OutputTarget seen_;
    bool supported_ = false;

    bool playing_ = false;
    double frequency_ = 1000.0;
    double level_db_ = isotone::ui::kEarToneDbfs;
    int channel_ = Both;
    bool auto_sweep_ = false;
    double sweep_rate_ = 1.0;
    double marks_[3] = {0.0, 0.0, 0.0};
    bool dip_ = false;
};
