// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The state the Equalizer view edits, as a list model of bands for QML, and the
// output it is for. Every edit goes to the output through DeviceLink: apply
// while a drag is in progress, commit when it is done (finishEdit, or at once for
// a toggle). Band ids are unique and kept through edits and re-sorting
// (ui-spec.md, "Engine contracts").
//
// Also reads the output's audio for the spectrum, 60 times a second.

#pragma once

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

#include "devicelink.h"
#include "isotone/types.h"
#include "outputs.h"
#include "spectrum.h"

class EqSession : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int selectedRow READ selectedRow NOTIFY selectionChanged)
    Q_PROPERTY(bool byFrequency READ byFrequency WRITE setByFrequency NOTIFY orderChanged)
    Q_PROPERTY(double preampDb READ preampDb WRITE setPreampDb NOTIFY stateChanged)
    Q_PROPERTY(bool autoPreamp READ autoPreamp WRITE setAutoPreamp NOTIFY stateChanged)
    Q_PROPERTY(double balance READ balance WRITE setBalance NOTIFY stateChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY stateChanged)
    Q_PROPERTY(bool eqOn READ eqOn WRITE setEqOn NOTIFY stateChanged)
    Q_PROPERTY(QString presetName READ presetName NOTIFY stateChanged)
    Q_PROPERTY(bool canAddBand READ canAddBand NOTIFY countChanged)
    Q_PROPERTY(bool spectrumActive READ spectrumActive NOTIFY spectrumChanged)

public:
    enum Role {
        BandIdRole = Qt::UserRole + 1,
        PositionRole,     // 1-based, in display order
        TypeNameRole,
        FrequencyRole,
        GainRole,
        QRole,
        EnabledRole,
        TargetRole,
        ColorIndexRole,   // follows the band, not its slot
        SelectedRole,
        WidthLabelRole,   // "Q 1.41", "1.50 oct", "12.0 dB/oct"
    };

    explicit EqSession(QObject* parent = nullptr);
    ~EqSession() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // What the graph draws.
    const isotone::EqState& state() const { return state_; }
    const isotone::ui::OutputLayout& layout() const { return target_.layout; }
    // The band shown at `row`, or null.
    const isotone::Band* bandAt(int row) const;
    int selectedRow() const;

    bool byFrequency() const { return by_frequency_; }
    void setByFrequency(bool on);
    double preampDb() const { return state_.preamp_db; }
    void setPreampDb(double db);
    bool autoPreamp() const { return state_.auto_preamp; }
    void setAutoPreamp(bool on);
    double balance() const { return balance_; }
    void setBalance(double b);
    bool muted() const { return state_.mute; }
    void setMuted(bool on);
    bool eqOn() const { return !state_.bypass; }
    void setEqOn(bool on);
    QString presetName() const { return preset_name_; }
    bool canAddBand() const;
    bool spectrumActive() const { return spectrum_active_; }

    // The spectrum at `freqs` in dBFS; false when no audio has arrived lately.
    bool spectrumLevels(const double* freqs, size_t n, double* out_db) const;

    // Edits the current output of `outputs` from now on, starting from what it plays.
    Q_INVOKABLE void useOutput(Outputs* outputs);
    // Replaces what is edited with `state` (flat when null), as useOutput does
    // with what the output plays. Writes nothing to the output.
    void loadState(const isotone::EqState* state);

    Q_INVOKABLE void select(int row);
    Q_INVOKABLE void setGain(int row, double db);
    Q_INVOKABLE void setFrequency(int row, double hz);
    // In the band's own width unit: Q, octaves or dB per octave.
    Q_INVOKABLE void setWidth(int row, double width);
    Q_INVOKABLE void setEnabled(int row, bool on);
    // A peaking band, selected. Does nothing at kParamMaxBands bands.
    Q_INVOKABLE void addBand(double hz, double db);
    Q_INVOKABLE void deleteBand(int row);
    // A drag or a typed value is done: commits it to the output, and re-sorts
    // by frequency if that is the order.
    Q_INVOKABLE void finishEdit();

signals:
    void countChanged();
    void selectionChanged();
    void orderChanged();
    void stateChanged();
    // Anything the curve depends on.
    void curveChanged();
    void spectrumChanged();

private:
    std::vector<size_t> displayOrder() const;
    void rebuildOrder();
    void bandChanged(int row, const QList<int>& roles);
    void updateAutoPreamp();
    void push();     // a live edit
    void commit();   // an edit that is done
    void readSpectrum();

    isotone::EqState state_;
    std::vector<size_t> order_;   // display row -> index into state_.bands
    uint32_t selected_id_ = 0;
    bool by_frequency_ = false;
    double balance_ = 0.0;
    QString preset_name_;

    std::unique_ptr<isotone::ui::DeviceLink> link_;
    isotone::ui::OutputTarget target_;
    isotone::ui::SpectrumAnalyzer analyzer_;
    std::vector<float> audio_;
    QTimer spectrum_timer_;
    QElapsedTimer clock_;
    qint64 last_frame_ms_ = -1000000;
    qint64 last_update_ms_ = 0;
    bool spectrum_active_ = false;
};
