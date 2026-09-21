// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// EqSession, the band model, and ResponseGraph's placement, without QML or an output.

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <QDir>
#include <QApplication>
#include <QImage>
#include <QPainter>

#include <cmath>

#include "eqsession.h"
#include "isotone/response.h"
#include "responsegraph.h"
#include "speakers.h"
#include "isotone/param_block.h"

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

TEST_CASE("a type without gain shows none and takes none") {
    // Low pass, high pass, band pass, notch and all pass ignore gain in the
    // processor; their columns showed a slider and "+0.0 dB" that did nothing.
    EqSession session;
    EqState s;
    s.bands = {band(1, FilterType::Peaking, 1000, 3, 1, WidthMode::Q),
               band(2, FilterType::LowShelf, 100, 3, 1, WidthMode::Q),
               band(3, FilterType::HighShelf, 8000, 3, 1, WidthMode::Q),
               band(4, FilterType::LowPass, 5000, 0, 0.7, WidthMode::Q),
               band(5, FilterType::HighPass, 30, 0, 0.7, WidthMode::Q),
               band(6, FilterType::BandPass, 1000, 0, 1, WidthMode::Q),
               band(7, FilterType::Notch, 60, 0, 30, WidthMode::Q),
               band(8, FilterType::AllPass, 500, 0, 1, WidthMode::Q)};
    session.loadState(&s);
    REQUIRE(session.rowCount() == 8);
    for (int row = 0; row < 8; ++row) {
        CAPTURE(row);
        const bool has_gain = row < 3;
        CHECK(session.data(session.index(row), EqSession::HasGainRole).toBool() == has_gain);
        session.setGain(row, -5);
        CHECK(session.bandAt(row)->gain_db == (has_gain ? -5.0 : s.bands[static_cast<size_t>(row)].gain_db));
    }
}

TEST_CASE("Auto preamp is on unless the loaded preamp is not what Auto sets") {
    // The region and Isotone.txt do not carry the mode, so every output loaded
    // with Auto off. A preamp Auto would have set is Auto's; any other was set by hand.
    EqSession session;
    CHECK(session.autoPreamp());

    session.loadState(nullptr);
    CHECK(session.autoPreamp());

    EqState flat;
    session.loadState(&flat);
    CHECK(session.autoPreamp());
    CHECK(session.preampDb() == 0.0);

    EqState boosted;
    boosted.bands = {band(1, FilterType::Peaking, 1000, 6, 1, WidthMode::Q)};
    boosted.preamp_db = -6.0;
    session.loadState(&boosted);
    CHECK(session.autoPreamp());
    CHECK(session.preampDb() == -6.0);

    // Written as text, the preamp is rounded; still Auto's.
    boosted.preamp_db = -6.004;
    session.loadState(&boosted);
    CHECK(session.autoPreamp());

    boosted.preamp_db = -3.0;
    session.loadState(&boosted);
    CHECK_FALSE(session.autoPreamp());
    CHECK(session.preampDb() == -3.0);   // loading changes nothing the output plays

    EqState cut;
    cut.bands = {band(1, FilterType::Peaking, 1000, -6, 1, WidthMode::Q)};
    cut.preamp_db = 0.0;
    session.loadState(&cut);
    CHECK(session.autoPreamp());   // Auto only cuts
}

TEST_CASE("changing a band's type keeps the band, and a slope becomes a Q off a shelf") {
    EqSession session;
    EqState s;
    s.bands = {band(7, FilterType::Peaking, 1200, 4, 2, WidthMode::Q),
               band(8, FilterType::LowShelf, 150, 6, 12, WidthMode::SlopeDb),
               band(9, FilterType::HighShelf, 6000, 0, 6, WidthMode::SlopeDb),
               band(10, FilterType::Peaking, 800, 3, 1.5, WidthMode::BandwidthOct)};
    session.loadState(&s);
    const auto type = [&](int row) { return session.data(session.index(row), EqSession::TypeRole).toInt(); };
    CHECK(type(0) == static_cast<int>(FilterType::Peaking));

    session.setType(0, static_cast<int>(FilterType::LowPass));
    const Band* b = session.bandAt(0);
    CHECK(b->type == FilterType::LowPass);
    CHECK(type(0) == static_cast<int>(FilterType::LowPass));
    CHECK(session.data(session.index(0), EqSession::TypeNameRole).toString() == QStringLiteral("Low pass"));
    CHECK(b->id == 7);
    CHECK(b->fc == 1200);
    CHECK(b->width == 2);
    CHECK(b->width_mode == WidthMode::Q);
    session.setType(0, static_cast<int>(FilterType::Peaking));
    CHECK(session.bandAt(0)->gain_db == 4);   // the gain comes back with a type that has one

    // A slope shelf to another shelf stays a slope.
    session.setType(1, static_cast<int>(FilterType::HighShelf));
    CHECK(session.bandAt(1)->width_mode == WidthMode::SlopeDb);
    CHECK(session.bandAt(1)->width == 12);

    // Off a shelf, the slope is read as Q by the processor: it becomes the Q with
    // the same shape, 1 / sqrt((A + 1/A)(1/S - 1) + 2) with S = slope / 12.
    session.setType(1, static_cast<int>(FilterType::Peaking));
    CHECK(session.bandAt(1)->width_mode == WidthMode::Q);
    CHECK(session.bandAt(1)->width == doctest::Approx(1.0 / std::sqrt(2.0)));
    // With a slope under 12 dB the gain matters: 6 dB at 6 dB/oct.
    s.bands[1].width = 6;
    session.loadState(&s);
    session.setType(1, static_cast<int>(FilterType::Peaking));
    const double a = std::pow(10.0, 6.0 / 40.0);
    CHECK(session.bandAt(1)->width == doctest::Approx(1.0 / std::sqrt((a + 1.0 / a) * (2.0 - 1.0) + 2.0)));
    session.setType(2, static_cast<int>(FilterType::Notch));
    CHECK(session.bandAt(2)->width_mode == WidthMode::Q);
    CHECK(session.bandAt(2)->width == doctest::Approx(std::sqrt(0.5 / 2.0)));   // A = 1: sqrt(S / 2)
    CHECK(session.data(session.index(2), EqSession::WidthLabelRole).toString() == QStringLiteral("Q 0.50"));

    // A bandwidth stays one, held where the processor holds it for the new type.
    session.setType(3, static_cast<int>(FilterType::LowShelf));
    CHECK(session.bandAt(3)->width_mode == WidthMode::BandwidthOct);
    CHECK(session.bandAt(3)->width == 1.5);

    session.setType(3, 99);   // not a type
    CHECK(session.bandAt(3)->type == FilterType::LowShelf);
}

TEST_CASE("a band's channels on a stereo output") {
    EqSession session;
    EqState s;
    s.bands = {band(1, FilterType::Peaking, 1000, 3, 1, WidthMode::Q)};
    session.loadState(&s);
    const auto target = [&] { return session.data(session.index(0), EqSession::TargetRole).toString(); };
    const auto which = [&] { return session.data(session.index(0), EqSession::ChannelsRole).toInt(); };
    CHECK(target() == QStringLiteral("L+R"));
    CHECK(which() == 2);
    session.setChannels(0, 0);
    CHECK(session.bandAt(0)->channels == 0x1);
    CHECK(target() == QStringLiteral("L"));
    CHECK(which() == 0);
    session.setChannels(0, 1);
    CHECK(session.bandAt(0)->channels == 0x2);
    CHECK(target() == QStringLiteral("R"));
    CHECK(which() == 1);
    session.setChannels(0, 2);
    CHECK(session.bandAt(0)->channels == kAllChannels);
    CHECK(target() == QStringLiteral("L+R"));
    session.setChannels(0, 3);
    CHECK(session.bandAt(0)->channels == kAllChannels);

    // Both bits of a stereo output are both channels.
    s.bands[0].channels = 0x3;
    session.loadState(&s);
    CHECK(target() == QStringLiteral("L+R"));
    CHECK(which() == 2);
}

TEST_CASE("duplicate and reset gain") {
    EqSession session;
    EqState s;
    s.bands = {band(4, FilterType::HighShelf, 5000, -3, 0.7, WidthMode::Q),
               band(2, FilterType::Peaking, 200, 5, 3, WidthMode::Q)};
    s.bands[0].channels = 0x2;
    session.loadState(&s);

    session.duplicateBand(0);
    REQUIRE(session.rowCount() == 3);
    const Band* copy = session.bandAt(1);   // next to the original
    CHECK(copy->id == 5);                   // a fresh id
    CHECK(copy->type == FilterType::HighShelf);
    CHECK(copy->fc == 5000);
    CHECK(copy->gain_db == -3);
    CHECK(copy->width == 0.7);
    CHECK(copy->channels == 0x2);
    CHECK(session.selectedRow() == 1);
    CHECK(session.bandAt(2)->id == 2);
    CHECK(session.data(session.index(1), EqSession::ColorIndexRole).toInt() !=
          session.data(session.index(0), EqSession::ColorIndexRole).toInt());

    session.resetGain(2);
    CHECK(session.bandAt(2)->gain_db == 0.0);
    CHECK(session.bandAt(2)->fc == 200);

    while (session.canAddBand()) session.duplicateBand(0);
    CHECK(session.rowCount() == static_cast<int>(kParamMaxBands));
    session.duplicateBand(0);
    CHECK(session.rowCount() == static_cast<int>(kParamMaxBands));
}

TEST_CASE("the graph draws the channel in view, and a handle sits on its band's channel") {
    // A band on the right channel alone drew a flat curve with its handle at 0 dB:
    // the graph drew channel 0 only (found 2026-09-14).
    EqSession session;
    EqState s;
    s.bands = {band(1, FilterType::Peaking, 1000, 6, 1, WidthMode::Q), band(2, FilterType::Peaking, 100, -4, 1, WidthMode::Q)};
    s.bands[0].channels = 0x2;
    s.bands[1].channels = 0x1;
    session.loadState(&s);
    ResponseGraph graph;
    graph.setSession(&session);

    CHECK(session.viewChannel() == 2);   // L+R
    CHECK(graph.handleDb(0) == doctest::Approx(6.0).epsilon(1e-3));
    CHECK(graph.handleDb(1) == doctest::Approx(-4.0).epsilon(1e-3));
    CHECK(graph.onView(0));
    CHECK(graph.onView(1));
    CHECK(std::abs(graph.compositeAt(1000)) < 0.1);   // the left channel, in L+R

    session.setViewChannel(1);   // R
    CHECK(graph.compositeAt(1000) == doctest::Approx(6.0).epsilon(1e-3));
    CHECK(graph.handleDb(0) == doctest::Approx(6.0).epsilon(1e-3));
    CHECK(graph.onView(0));
    CHECK_FALSE(graph.onView(1));
    // A band off the view sits where the view's curve is at its frequency.
    CHECK(std::abs(graph.handleDb(1)) < 0.1);

    session.setViewChannel(0);   // L
    CHECK(graph.compositeAt(100) == doctest::Approx(-4.0).epsilon(1e-3));
    CHECK(std::abs(graph.handleDb(0)) < 0.1);
    CHECK_FALSE(graph.onView(0));
    CHECK(graph.onView(1));

    session.setViewChannel(5);
    CHECK(session.viewChannel() == 0);
}

TEST_CASE("a curve that leaves the plot is not drawn along its edge") {
    // The owner, 2026-09-19: a high-pass below the bottom of the plot showed as a
    // flat line along the bottom edge. It leaves through the bottom instead.
    EqSession session;
    EqState s;
    s.bands = {band(1, FilterType::HighPass, 200, 0, 0.707, WidthMode::Q)};
    session.loadState(&s);
    ResponseGraph graph;
    graph.setSession(&session);
    graph.setSize(QSizeF(1060, 404));
    // Only the composite's line is drawn, in red.
    graph.setProperty("accent", QColor(255, 0, 0));
    graph.setProperty("fillEdgeAlpha", 0.0);
    graph.setProperty("fillMidAlpha", 0.0);
    for (const char* name : {"gridMajor", "gridMinor", "zeroLine", "labelColour", "bell", "spectrumEdge", "spectrumFill"})
        graph.setProperty(name, QColor(0, 0, 0, 0));
    REQUIRE(graph.compositeAt(30) < -20.0);   // well under the bottom of the plot there

    QImage image(1060, 404, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    graph.paint(&painter);
    painter.end();

    const double bottom = graph.plotTop() + graph.plotHeight();
    const int x_from = static_cast<int>(graph.xOf(20)), x_to = static_cast<int>(graph.xOf(40));
    int on_edge = 0, below = 0, inside = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = image.pixelColor(x, y);
            if (c.alpha() == 0 || c.red() == 0) continue;
            if (y > bottom + 1) ++below;
            else if (y >= bottom - 3 && x >= x_from && x <= x_to) ++on_edge;
            else ++inside;
        }
    }
    CAPTURE(on_edge);
    CAPTURE(below);
    REQUIRE(inside > 0);   // the curve was drawn
    CHECK(on_edge == 0);
    CHECK(below == 0);
}

TEST_CASE("an output's layout is named by its channels and speakers") {
    // The outputs list called every layout above two channels that was not 7.1 "5.1".
    CHECK(speaker_layout_name(2, 0x3) == QStringLiteral("Stereo"));
    CHECK(speaker_layout_name(3, 0xB) == QStringLiteral("2.1"));
    CHECK(speaker_layout_name(6, 0x60F) == QStringLiteral("5.1"));
    CHECK(speaker_layout_name(6, 0x3F) == QStringLiteral("5.1"));   // back speakers
    CHECK(speaker_layout_name(8, 0xFF) == QStringLiteral("7.1"));   // wide
    CHECK(speaker_layout_name(8, 0x63F) == QStringLiteral("7.1"));
    CHECK(speaker_layout_name(8, 0) == QStringLiteral("7.1"));
    CHECK(speaker_layout_name(4, 0x33) == QStringLiteral("4 ch"));
    CHECK(speaker_layout_name(6, 0x637) == QStringLiteral("6 ch"));   // no LFE
}

TEST_CASE("the curve is drawn at the output's rate, not always at 48 kHz") {
    // The graph designed every band at a fixed 48 kHz while the engine designs
    // them at the output's rate, so the curve drawn was not the curve heard
    // (plan 4.5). A Q 4 bell at 15 kHz was 2.6 dB out on a 96 kHz output and a
    // low pass at 18 kHz 3.6 dB out on a 44.1 kHz one (measured 2026-09-21).
    EqSession session;
    EqState s;
    s.bands = {band(1, FilterType::Peaking, 15000, 9, 4, WidthMode::Q)};
    session.loadState(&s);
    ResponseGraph graph;
    graph.setSession(&session);

    const std::string guid = "{8f4d2a10-0000-4000-8000-0000000000fa}";
    const auto at_rate = [&](double rate) {
        session.useTarget(isotone::ui::OutputTarget{guid, isotone::ui::Backend::none,
                                                    isotone::ui::OutputLayout{2, 0x3, rate}});
        session.loadState(&s);
        return graph.compositeAt(16248.0);
    };
    // What the engine plays at each rate, from the core itself.
    const auto engine = [&](double rate) {
        EqState probe = s;
        probe.layout_channels = 2;
        probe.layout_speaker_mask = 0x3;
        double db = 0.0;
        const double hz = 16248.0;
        isotone::magnitude_db(probe, 2, 0x3, 0, &hz, 1, rate, &db);
        return db;
    };

    for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
        CAPTURE(rate);
        CHECK(at_rate(rate) == doctest::Approx(engine(rate)).epsilon(1e-6));
    }
    // The rates really do differ, so the check above is not vacuous.
    CHECK(std::abs(engine(96000.0) - engine(48000.0)) > 1.0);
}

int main(int argc, char** argv) {
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    // Settings and presets never go to the owner's %APPDATA%\Isotone.
    if (!qEnvironmentVariableIsSet("ISOTONE_DATA_DIR"))
        qputenv("ISOTONE_DATA_DIR", QDir::temp().filePath(QStringLiteral("isotone-model-tests")).toUtf8());
    QApplication app(argc, argv);   // the tray menu's tests need widgets
    return doctest::Context(argc, argv).run();
}
