// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// EQ by ear on EqSession, without QML or audio: which outputs it is for, the
// frequency and its nudges, the marks, and the band they make.

#include "doctest.h"

#include <QSignalSpy>

#include <cmath>

#include "eqbyear.h"
#include "eqsession.h"

using namespace isotone;
using namespace isotone::ui;

namespace {

OutputTarget layout(uint32_t channels, uint32_t mask) {
    return OutputTarget{L"", Backend::none, OutputLayout{channels, mask, 48000}};
}

Band peak(uint32_t id, double fc, double gain) {
    Band b;
    b.id = id;
    b.fc = fc;
    b.gain_db = gain;
    b.width = 1.41;
    return b;
}

void two_bands(EqSession& s) {
    EqState st;
    st.bands = {peak(1, 100, -1), peak(2, 8000, 2)};
    s.loadState(&st);
}

void mark_at(EqByEar& ear, double start, double top, double end) {
    ear.setFrequency(start);
    ear.mark(EqByEar::Start);
    ear.setFrequency(top);
    ear.mark(EqByEar::Top);
    ear.setFrequency(end);
    ear.mark(EqByEar::End);
}

}  // namespace

TEST_CASE("the band marks make: a peak at Top, as wide as Start to End, 3 dB against what was heard") {
    SUBCASE("a peak is cut") {
        const EqByEar::MarkedBand b = EqByEar::bandFromMarks(2600, 3400, 4400, false);
        CHECK(b.fc == 3400);
        CHECK(b.gainDb == -3.0);
        CHECK(b.q == doctest::Approx(1.89));   // 3400 / 1800, to two places
    }
    SUBCASE("a dip is boosted") {
        CHECK(EqByEar::bandFromMarks(2600, 3400, 4400, true).gainDb == 3.0);
    }
    SUBCASE("in any order") {
        CHECK(EqByEar::bandFromMarks(4400, 3400, 2600, false).q == doctest::Approx(1.89));
        const EqByEar::MarkedBand outside = EqByEar::bandFromMarks(2600, 5000, 4400, false);
        CHECK(outside.fc == 5000);
        CHECK(outside.q == doctest::Approx(2.78));
    }
    SUBCASE("Start and End at one frequency is as narrow as a band goes, and a wide span as wide") {
        CHECK(EqByEar::bandFromMarks(1000, 1000, 1000, false).q == 50.0);
        CHECK(EqByEar::bandFromMarks(20, 100, 20000, false).q == doctest::Approx(0.1));
    }
}

TEST_CASE("EQ by ear is for stereo and 2.1 outputs only") {
    EqSession session;
    EqByEar ear(&session);
    QSignalSpy changed(&ear, &EqByEar::supportedChanged);

    session.useTarget(layout(2, 0x3));
    CHECK(ear.supported());
    session.useTarget(layout(3, 0xB));
    CHECK(ear.supported());
    for (const auto& [channels, mask] : {std::pair{6u, 0x60Fu}, std::pair{8u, 0x63Fu}, std::pair{1u, 0x4u}, std::pair{3u, 0x7u}}) {
        CAPTURE(channels);
        session.useTarget(layout(channels, mask));
        CHECK_FALSE(ear.supported());
        ear.setPlaying(true);
        CHECK_FALSE(ear.playing());
    }
    CHECK(changed.count() == 1);   // once, when it stopped being supported
    session.useTarget(layout(2, 0x3));
    CHECK(ear.supported());
    CHECK(changed.count() == 2);
}

TEST_CASE("the tone's frequency: set, nudged, and kept between 20 Hz and 20 kHz") {
    EqSession session;
    EqByEar ear(&session);
    ear.setFrequency(1000);
    ear.nudge(EqByEar::kNudgeOctaves);
    CHECK(ear.frequency() == doctest::Approx(1000 * std::pow(2.0, 1.0 / 48)));
    ear.nudge(-EqByEar::kCoarseNudgeOctaves);
    CHECK(ear.frequency() == doctest::Approx(1000 * std::pow(2.0, 1.0 / 48 - 1.0 / 6)));
    ear.nudge(20);
    CHECK(ear.frequency() == 20000);
    ear.setFrequency(3);
    CHECK(ear.frequency() == 20);

    ear.setLevelDb(-12);
    CHECK(ear.levelDb() == -12);
    ear.setLevelDb(6);
    CHECK(ear.levelDb() == 0);   // over full scale is clipping, not louder
}

TEST_CASE("the tone's channel is the top bar's: L, R or both") {
    EqSession session;
    EqByEar ear(&session);
    QSignalSpy changed(&ear, &EqByEar::channelChanged);
    CHECK(ear.channel() == EqByEar::Both);   // L+R, the session's default view
    session.setViewChannel(0);
    CHECK(ear.channel() == EqByEar::Left);
    session.setViewChannel(1);
    CHECK(ear.channel() == EqByEar::Right);
    session.setViewChannel(2);
    CHECK(ear.channel() == EqByEar::Both);
    CHECK(changed.count() == 3);

    SUBCASE("on 2.1, the front speaker shown alone, or both") {
        session.useTarget(layout(3, 0xB));
        session.setShowingMask(0x1);
        CHECK(ear.channel() == EqByEar::Left);
        session.setShowingMask(0x2);
        CHECK(ear.channel() == EqByEar::Right);
        for (const int mask : {0, 0x3, 0x7, 0x4, 0x5}) {
            CAPTURE(mask);
            session.setShowingMask(mask);
            CHECK(ear.channel() == (mask == 0x5 ? EqByEar::Left : EqByEar::Both));
        }
    }
    SUBCASE("an ear made after the view was chosen starts on it") {
        session.setViewChannel(1);
        EqByEar later(&session);
        CHECK(later.channel() == EqByEar::Right);
    }
}

TEST_CASE("a mark records the frequency, and Add band needs all three") {
    EqSession session;
    two_bands(session);
    EqByEar ear(&session);
    CHECK_FALSE(ear.canAddBand());
    ear.setFrequency(2600.4);
    ear.mark(EqByEar::Start);
    CHECK(ear.start() == 2600);
    CHECK(ear.top() == 0);
    CHECK_FALSE(ear.canAddBand());
    ear.setFrequency(3400);
    ear.mark(EqByEar::Top);
    CHECK_FALSE(ear.canAddBand());
    ear.setFrequency(4400);
    ear.mark(EqByEar::End);
    CHECK(ear.canAddBand());

    ear.clearMarks();
    CHECK(ear.start() == 0);
    CHECK(ear.end() == 0);
    CHECK_FALSE(ear.canAddBand());
    ear.addBand();
    CHECK(session.rowCount() == 2);
}

TEST_CASE("Add band: the marked band at the end, selected, on the tone's channels, one step") {
    EqSession session;

    SUBCASE("stereo, left") {
        two_bands(session);
        EqByEar ear(&session);
        session.setViewChannel(0);
        mark_at(ear, 2600, 3400, 4400);
        ear.addBand();

        REQUIRE(session.rowCount() == 3);
        const Band* b = session.bandAt(2);
        CHECK(b->type == FilterType::Peaking);
        CHECK(b->fc == 3400);
        CHECK(b->gain_db == -3.0);
        CHECK(b->width == doctest::Approx(1.89));
        CHECK(b->channels == 0x1);
        CHECK(session.selectedRow() == 2);
        CHECK(ear.start() == 0);   // the marks are used up
        CHECK_FALSE(ear.canAddBand());

        session.undo();
        CHECK(session.rowCount() == 2);
        CHECK_FALSE(session.canUndo());
    }
    SUBCASE("stereo, both: every channel") {
        two_bands(session);
        EqByEar ear(&session);
        ear.setDip(true);
        mark_at(ear, 200, 150, 100);
        ear.addBand();
        REQUIRE(session.rowCount() == 3);
        CHECK(session.bandAt(2)->channels == kAllChannels);
        CHECK(session.bandAt(2)->gain_db == 3.0);
        CHECK(session.bandAt(2)->width == doctest::Approx(1.5));
    }
    SUBCASE("2.1: the front pair, not the sub") {
        session.useTarget(layout(3, 0xB));
        session.loadState(nullptr);   // a state that names no layout yet
        REQUIRE(session.state().layout_channels == 0);
        EqByEar ear(&session);
        mark_at(ear, 900, 1000, 1100);
        ear.addBand();
        REQUIRE(session.rowCount() == 1);
        CHECK(session.bandAt(0)->channels == 0x3);
        CHECK(session.state().layout_channels == 3);   // the mask addresses the output's layout
        CHECK(session.state().layout_speaker_mask == 0xB);

        session.setShowingMask(0x2);
        mark_at(ear, 900, 1000, 1100);
        ear.addBand();
        REQUIRE(session.rowCount() == 2);   // the right alone is not the pair's band
        CHECK(session.bandAt(1)->channels == 0x2);
    }
    SUBCASE("sorted by frequency, the new band takes its place there, selected") {
        two_bands(session);
        session.setByFrequency(true);
        EqByEar ear(&session);
        mark_at(ear, 900, 1000, 1100);
        ear.addBand();
        CHECK(session.bandAt(session.selectedRow())->fc == 1000);
    }
}

TEST_CASE("another output stops the tone and clears the marks") {
    EqSession session;
    EqByEar ear(&session);
    ear.setPlaying(true);
    CHECK(ear.playing());
    ear.setFrequency(500);
    ear.mark(EqByEar::Start);

    session.useTarget(layout(3, 0xB));
    CHECK_FALSE(ear.playing());
    CHECK(ear.start() == 0);

    ear.setPlaying(true);
    ear.stop();
    CHECK_FALSE(ear.playing());
}

TEST_CASE("Add band where a peak already is adds to that peak's gain instead") {
    EqSession session;
    EqByEar ear(&session);

    SUBCASE("a second cut at the same place deepens the first, as one step") {
        mark_at(ear, 2600, 3400, 4400);
        ear.addBand();
        REQUIRE(session.rowCount() == 1);
        session.addBand(8000, 2);   // another band, selected
        mark_at(ear, 3000, 3500, 4000);   // Top a 24th of an octave above the first
        QSignalSpy committed(&session, &EqSession::committed);
        ear.addBand();
        CHECK(committed.count() == 1);   // written to the output, as any finished edit
        REQUIRE(session.rowCount() == 2);
        CHECK(session.bandAt(0)->gain_db == -6.0);
        CHECK(session.bandAt(0)->fc == 3400);   // where it was, and as wide
        CHECK(session.bandAt(0)->width == doctest::Approx(1.89));
        CHECK(session.selectedRow() == 0);
        CHECK(ear.start() == 0);

        session.undo();
        CHECK(session.bandAt(0)->gain_db == -3.0);
        CHECK(session.rowCount() == 2);
    }
    SUBCASE("a dip there lifts it back") {
        mark_at(ear, 2600, 3400, 4400);
        ear.addBand();
        ear.setDip(true);
        mark_at(ear, 2600, 3400, 4400);
        ear.addBand();
        REQUIRE(session.rowCount() == 1);
        CHECK(session.bandAt(0)->gain_db == 0.0);
    }
    SUBCASE("the nearest of two within a sixth of an octave") {
        session.addBand(1000, -2, 2.0);
        session.addBand(1100, -2, 2.0);
        mark_at(ear, 900, 1080, 1200);
        ear.addBand();
        REQUIRE(session.rowCount() == 2);
        CHECK(session.bandAt(0)->gain_db == -2.0);
        CHECK(session.bandAt(1)->gain_db == -5.0);
    }
    SUBCASE("further than a sixth of an octave is a new band") {
        session.addBand(1000, -3, 2.0);
        mark_at(ear, 1000, 1000 * std::pow(2.0, 1.0 / 6.0) * 1.01, 1300);
        ear.addBand();
        CHECK(session.rowCount() == 2);
    }
    SUBCASE("on other channels, disabled, or not a peak is a new band") {
        session.addBand(1000, -3, 2.0, 0x1);   // left only
        session.setViewChannel(1);
        mark_at(ear, 900, 1000, 1100);
        ear.addBand();
        CHECK(session.rowCount() == 2);

        session.setViewChannel(2);
        session.addBand(5000, -3);
        session.setEnabled(2, false);
        mark_at(ear, 4500, 5000, 5500);
        ear.addBand();
        CHECK(session.rowCount() == 4);

        session.addBand(12000, -3);
        session.setType(4, static_cast<int>(FilterType::HighShelf));
        mark_at(ear, 11000, 12000, 13000);
        ear.addBand();
        CHECK(session.rowCount() == 6);
    }
    SUBCASE("at the band limit it still adds onto a peak that is there") {
        for (int i = 0; session.canAddBand(); ++i) session.addBand(30 * std::pow(2.0, i / 7.0), -1);
        mark_at(ear, 18000, 19000, 20000);   // the top band is 30 Hz * 2^9, 0.3 octaves under
        CHECK_FALSE(ear.canAddBand());
        const double fc = session.bandAt(20)->fc;
        mark_at(ear, fc * 0.9, fc, fc * 1.1);
        CHECK(ear.canAddBand());
        ear.addBand();
        CHECK(session.bandAt(20)->gain_db == -4.0);
    }
    SUBCASE("left on a band on the left, both on a band on every channel") {
        session.addBand(1000, -3, 2.0, 0x1);
        session.setViewChannel(0);
        mark_at(ear, 900, 1000, 1100);
        ear.addBand();
        CHECK(session.rowCount() == 1);
        CHECK(session.bandAt(0)->gain_db == -6.0);

        session.addBand(3000, -3);
        session.setViewChannel(2);
        mark_at(ear, 2900, 3000, 3100);
        ear.addBand();
        CHECK(session.rowCount() == 2);
        CHECK(session.bandAt(1)->gain_db == -6.0);
    }
}
