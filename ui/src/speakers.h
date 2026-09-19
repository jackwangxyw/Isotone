// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The speaker setup of the current output, for the Speakers view and panel, the
// band popover's Target row and the Showing picker (docs/ui-spec.md, "Speakers
// panel", "Speakers view"). A list model with a row per speaker of the output's
// layout, over EqSession's edited state: every change goes through
// EqSession::editSpeakers, so it reaches the output and the saved state.
//
// Solo and test tones are not in the state: they are EqSession's live
// overrides, written to the output and never saved. Leaving the Speakers view
// ends both (endSession). The test tone plays through TestTone on the output.

#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <string>
#include <vector>

#include "isotone/speaker_layouts.h"
#include "speaker_setup.h"
#include "speakerstore.h"
#include "test_tone.h"

class EqSession;
class QQmlEngine;
class QJSEngine;

// "Stereo", "2.1", "5.1", "7.1", or "<n> ch" for another layout; a mask of 0 is
// the usual one for the channel count.
QString speaker_layout_name(uint32_t channels, uint32_t speaker_mask);

class Speakers : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int count READ rowCount NOTIFY layoutChanged)
    Q_PROPERTY(int channels READ channels NOTIFY layoutChanged)
    // "Stereo", "2.1", "5.1", "7.1", or "<n> ch" for another layout.
    Q_PROPERTY(QString layoutName READ layoutName NOTIFY layoutChanged)
    // The picker's layouts the output supports (refreshSupportedLayouts).
    Q_PROPERTY(QStringList supportedLayouts READ supportedLayouts NOTIFY supportedLayoutsChanged)
    // [{code, name, channel, lfe}] in channel order.
    Q_PROPERTY(QVariantList speakers READ speakerList NOTIFY layoutChanged)
    // [{name, mask, codes, builtin}]: All, Front, Surround, Sub, then the user's.
    Q_PROPERTY(QVariantList groups READ groups NOTIFY groupsChanged)

    Q_PROPERTY(double crossoverHz READ crossoverHz WRITE setCrossoverHz NOTIFY setupChanged)
    Q_PROPERTY(double lfeLowpassHz READ lfeLowpassHz WRITE setLfeLowpassHz NOTIFY setupChanged)
    Q_PROPERTY(bool bassManagement READ bassManagement NOTIFY setupChanged)
    Q_PROPERTY(int smallSpeakers READ smallSpeakers NOTIFY setupChanged)
    // isotone::Upmix: 0 off, 1 all, 2 no centre.
    Q_PROPERTY(int upmix READ upmix WRITE setUpmix NOTIFY setupChanged)
    Q_PROPERTY(bool swapFrontRear READ swapFrontRear WRITE setSwapFrontRear NOTIFY setupChanged)
    Q_PROPERTY(bool swapLeftRight READ swapLeftRight WRITE setSwapLeftRight NOTIFY setupChanged)
    Q_PROPERTY(double lipSyncMs READ lipSyncMs WRITE setLipSyncMs NOTIFY setupChanged)

    // The table's time column: distance (true) or delay.
    Q_PROPERTY(bool distanceMode READ distanceMode WRITE setDistanceMode NOTIFY distanceModeChanged)
    Q_PROPERTY(bool testTones READ testTones WRITE setTestTones NOTIFY tonesChanged)
    // The row whose tone plays, -1 for none.
    Q_PROPERTY(int playingRow READ playingRow NOTIFY tonesChanged)
    Q_PROPERTY(int soloRow READ soloRow NOTIFY soloChanged)

    // The Showing picker: "all", "group:<name>" or "speaker:<channel>".
    Q_PROPERTY(QString showing READ showing WRITE setShowing NOTIFY showingChanged)
    Q_PROPERTY(QString showingLabel READ showingLabel NOTIFY showingChanged)
    // [{key, label, detail, separator}]
    Q_PROPERTY(QVariantList showingItems READ showingItems NOTIFY groupsChanged)

public:
    enum Role {
        CodeRole = Qt::UserRole + 1,
        NameRole,
        ChannelRole,
        LevelRole,       // dB
        DistanceRole,    // m
        DelayRole,       // ms
        InvertedRole,
        MutedRole,       // the speaker's own mute
        SoloMutedRole,   // muted by another speaker's solo
        SoloedRole,
        SmallRole,
        LfeRole,
        PlayingRole,
    };
    Q_ENUM(Role)

    // An HRESULT on Windows, an errno on Linux; 0 when it changed.
    using LayoutSetter = std::function<int(const std::string& output, isotone::SpeakerLayout layout)>;

    explicit Speakers(EqSession* session, QObject* parent = nullptr);
    ~Speakers() override;
    static Speakers* create(QQmlEngine* qml, QJSEngine* js);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int channels() const;
    QString layoutName() const;
    QStringList supportedLayouts() const { return supported_; }
    QVariantList speakerList() const;
    QVariantList groups() const;

    double crossoverHz() const;
    void setCrossoverHz(double hz);
    double lfeLowpassHz() const;
    void setLfeLowpassHz(double hz);
    bool bassManagement() const;
    int smallSpeakers() const;
    int upmix() const;
    void setUpmix(int upmix);
    bool swapFrontRear() const;
    void setSwapFrontRear(bool on);
    bool swapLeftRight() const;
    void setSwapLeftRight(bool on);
    double lipSyncMs() const;
    void setLipSyncMs(double ms);

    bool distanceMode() const { return distance_mode_; }
    void setDistanceMode(bool on);
    bool testTones() const { return tones_; }
    void setTestTones(bool on);
    int playingRow() const { return playing_; }
    int soloRow() const { return solo_; }

    QString showing() const { return showing_; }
    void setShowing(const QString& key);
    QString showingLabel() const;
    QVariantList showingItems() const;

    Q_INVOKABLE void setLevel(int row, double db);
    Q_INVOKABLE void setDistance(int row, double metres);
    Q_INVOKABLE void setDelay(int row, double ms);
    Q_INVOKABLE void setInverted(int row, bool on);
    Q_INVOKABLE void setMuted(int row, bool on);
    Q_INVOKABLE void toggleSolo(int row);
    // A speaker whose bass goes to the LFE; bass management is on while any is.
    Q_INVOKABLE void setSmall(int row, bool on);
    // Plays row's tone, or stops it when it is the one playing. Test tones only.
    Q_INVOKABLE void toggleTone(int row);
    // False when the name is empty or taken, or no speaker of the layout is given.
    Q_INVOKABLE bool addGroup(const QString& name, const QStringList& codes);
    Q_INVOKABLE void removeGroup(const QString& name);
    // Channels of a picker layout ("5.1" is 6), 0 for another name.
    Q_INVOKABLE int layoutChannels(const QString& name) const;
    Q_INVOKABLE void refreshSupportedLayouts();
    // Changes the output's speaker setup: an HRESULT on Windows, an errno on
    // Linux, 0 when it changed. The new format reaches the session through the
    // device notification.
    Q_INVOKABLE int setLayout(const QString& name);
    // Leaving the Speakers view: test tones stop and solo ends.
    Q_INVOKABLE void endSession();

    // Tests replace the call that changes a real layout.
    void setLayoutSetter(LayoutSetter setter) { layout_setter_ = std::move(setter); }
    // Tests keep the store elsewhere.
    void setStore(const SpeakerStore& store);

signals:
    void layoutChanged();
    void supportedLayoutsChanged();
    void groupsChanged();
    void setupChanged();
    void distanceModeChanged();
    void tonesChanged();
    void soloChanged();
    void showingChanged();
    // A test tone could not play; an HRESULT on Windows, an errno on Linux.
    void toneFailed(int hr);
    // Speaker settings or groups could not be saved.
    void saveFailed();

private:
    const isotone::EqState& state() const;
    uint32_t speakerMask() const;
    bool validRow(int row) const { return row >= 0 && row < rowCount(); }
    void sessionChanged();
    void reloadOutput();
    void updateOverrides();
    void startTone();
    void stopTone();
    void edit(const std::function<void(isotone::EqState*)>& change);
    void rowsChanged();
    isotone::ChannelMask showingMask(const QString& key, bool* valid) const;

    QPointer<EqSession> session_;
    SpeakerStore store_;
    std::vector<isotone::ui::Speaker> speakers_;
    std::vector<isotone::ui::SpeakerGroup> user_groups_;
    std::string guid_;
    uint32_t channels_ = 0, mask_ = 0;
    double farthest_ = isotone::ui::kDefaultFarthestM;   // EqSession's undo extra, so undo restores it
    QStringList supported_;
    bool distance_mode_ = true;
    bool tones_ = false;
    int playing_ = -1;
    int solo_ = -1;
    QString showing_ = QStringLiteral("all");
    isotone::ui::TestTone tone_;
    quint64 tone_generation_ = 0;
    LayoutSetter layout_setter_;
};
