// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// The Appearance page's live preview: a session of its own with the prototype's
// sample (the first eight bands of "HD 650 · tuned", band 4 selected) and a
// fixed spectrum, for a ResponseGraph to draw in the current theme. It has no
// output, so nothing it holds is written anywhere, and the real EqSession is
// untouched.

#pragma once

#include "eqsession.h"

class PreviewSession : public EqSession {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit PreviewSession(QObject* parent = nullptr);

    bool spectrumLevels(const double* freqs, size_t n, double* out_db) const override;
    bool spectrumPeakLevels(const double* freqs, size_t n, double* out_db) const override;
};
