// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// EqSession, the band model, without QML or an output.

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <QCoreApplication>

#include "eqsession.h"

using namespace isotone;

namespace {

Band band(uint32_t id, FilterType type, double fc, double gain, double width, WidthMode mode) {
    Band b;
    b.id = id;
    b.type = type;
    b.fc = fc;
    b.gain_db = gain;
    b.width = width;
    b.width_mode = mode;
    return b;
}

}  // namespace

TEST_CASE("a band's width is shown and changed in its own unit") {
    // A bandwidth or dB slope was shown as "Q 1.50", and a scroll on the band set
    // Q mode with that number: a 1.5 octave bell became Q 1.62 at once, and a
    // 12 dB slope shelf became Q 12.96 (found in the all-filters screenshots,
    // 2026-09-14).
    EqSession session;
    EqState s;
    s.bands = {band(1, FilterType::Peaking, 3000, -9, 1.5, WidthMode::BandwidthOct),
               band(2, FilterType::LowShelf, 200, 6, 12, WidthMode::SlopeDb),
               band(3, FilterType::Peaking, 1000, 6, 1.41, WidthMode::Q)};
    session.loadState(&s);
    REQUIRE(session.rowCount() == 3);

    const auto label = [&](int row) { return session.data(session.index(row), EqSession::WidthLabelRole).toString(); };
    CHECK(label(0) == QStringLiteral("1.50 oct"));
    CHECK(label(1) == QStringLiteral("12.0 dB/oct"));
    CHECK(label(2) == QStringLiteral("Q 1.41"));

    session.setWidth(0, 1.5 * 1.08);
    session.setWidth(1, 12 * 1.08);
    session.setWidth(2, 1.41 * 1.08);
    CHECK(session.bandAt(0)->width_mode == WidthMode::BandwidthOct);
    CHECK(session.bandAt(0)->width == doctest::Approx(1.62));
    CHECK(session.bandAt(1)->width_mode == WidthMode::SlopeDb);
    CHECK(session.bandAt(1)->width == doctest::Approx(12.96));
    CHECK(session.bandAt(2)->width_mode == WidthMode::Q);
    CHECK(session.bandAt(2)->width == doctest::Approx(1.5228));
    CHECK(label(0) == QStringLiteral("1.62 oct"));

    // Q keeps the view's range; the other units stop where the processor does.
    session.setWidth(2, 500);
    CHECK(session.bandAt(2)->width == 50.0);
    session.setWidth(0, 1e-6);
    CHECK(session.bandAt(0)->width == doctest::Approx(0.00145));
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return doctest::Context(argc, argv).run();
}
