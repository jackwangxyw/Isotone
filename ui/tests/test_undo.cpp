// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// EqSession's undo and redo: every committed edit is one step.

#include "doctest.h"

#include <QSignalSpy>

#include "eqsession.h"

using namespace isotone;

namespace {

Band peak(uint32_t id, double fc, double gain) {
    Band b;
    b.id = id;
    b.fc = fc;
    b.gain_db = gain;
    b.width = 1.41;
    return b;
}

// A session with three cuts (so in Auto), nothing to undo.
void three(EqSession& s) {
    EqState st;
    st.bands = {peak(1, 100, -1), peak(2, 1000, -2), peak(3, 5000, -3)};
    s.loadState(&st);
}

}  // namespace

TEST_CASE("undo: a drag is one step, and undo and redo move through the steps") {
    EqSession s;
    three(s);
    CHECK_FALSE(s.canUndo());
    CHECK_FALSE(s.canRedo());

    s.select(1);
    for (int i = 1; i <= 20; ++i) s.setGain(1, 2 + i * 0.25);   // a drag
    CHECK_FALSE(s.canUndo());   // nothing is a step before it is done
    s.finishEdit();
    CHECK(s.canUndo());
    CHECK(s.bandAt(1)->gain_db == 7.0);

    s.setFrequency(2, 6000);   // a typed value
    s.finishEdit();

    s.undo();
    CHECK(s.bandAt(2)->fc == 5000);
    CHECK(s.bandAt(1)->gain_db == 7.0);
    CHECK(s.canRedo());
    s.undo();
    CHECK(s.bandAt(1)->gain_db == -2.0);   // the whole drag at once
    CHECK_FALSE(s.canUndo());
    s.undo();   // nothing more
    CHECK(s.bandAt(1)->gain_db == -2.0);

    s.redo();
    CHECK(s.bandAt(1)->gain_db == 7.0);
    s.redo();
    CHECK(s.bandAt(2)->fc == 6000);
    CHECK_FALSE(s.canRedo());
}

TEST_CASE("undo: toggles, type, channels, add, delete and duplicate are steps; a new edit ends redo") {
    EqSession s;
    three(s);
    s.setEnabled(0, false);
    s.setType(0, static_cast<int>(FilterType::LowShelf));
    s.setChannels(0, 1);
    s.addBand(2000, -3);
    s.deleteBand(2);
    s.duplicateBand(0);
    s.setAutoPreamp(false);
    s.setMuted(true);
    s.setEqOn(false);
    REQUIRE(s.rowCount() == 4);

    int steps = 0;
    while (s.canUndo()) {
        s.undo();
        ++steps;
    }
    CHECK(steps == 9);
    REQUIRE(s.rowCount() == 3);
    CHECK(s.bandAt(0)->enabled);
    CHECK(s.bandAt(0)->type == FilterType::Peaking);
    CHECK(s.bandAt(0)->channels == kAllChannels);
    CHECK(s.bandAt(2)->fc == 5000);
    CHECK(s.autoPreamp());
    CHECK_FALSE(s.muted());
    CHECK(s.eqOn());

    s.redo();
    CHECK_FALSE(s.bandAt(0)->enabled);
    s.setGain(1, -1);
    s.finishEdit();
    CHECK_FALSE(s.canRedo());
}

TEST_CASE("undo: an edit that changes nothing is no step") {
    EqSession s;
    three(s);
    s.setGain(0, -1);   // what it is
    s.finishEdit();
    s.setEnabled(0, true);
    CHECK_FALSE(s.canUndo());
    // A drag that ends where it started.
    s.setGain(0, 4);
    s.setGain(0, -1);
    s.finishEdit();
    CHECK_FALSE(s.canUndo());
}

TEST_CASE("undo: balance and the preamp are steps") {
    EqSession s;
    three(s);
    REQUIRE(s.autoPreamp());
    s.setBalance(-0.5);
    s.finishEdit();
    s.setPreampDb(-4);
    s.finishEdit();
    s.undo();
    CHECK(s.autoPreamp());
    CHECK(s.balance() == -0.5);
    s.undo();
    CHECK(s.balance() == 0.0);
}

TEST_CASE("undo: a deleted band comes back with its id, place and selection") {
    EqSession s;
    three(s);
    QSignalSpy deleted(&s, &EqSession::bandDeleted);
    s.select(1);
    s.deleteBand(1);
    REQUIRE(deleted.size() == 1);
    CHECK(deleted[0][0].toInt() == 2);   // "Band 2 deleted"
    const int step = deleted[0][1].toInt();
    CHECK(s.selectedRow() == 1);         // the next band
    CHECK(s.bandAt(1)->id == 3);

    s.undo();
    REQUIRE(s.rowCount() == 3);
    CHECK(s.bandAt(1)->id == 2);
    CHECK(s.selectedRow() == 1);
    s.redo();
    CHECK(s.rowCount() == 2);
    CHECK(s.bandAt(s.selectedRow())->id == 3);

    // The toast's Undo undoes that deletion only while it is the last step.
    CHECK(s.undoStep(step));
    CHECK(s.rowCount() == 3);
    s.select(0);
    s.deleteBand(0);
    const int later = deleted.back()[1].toInt();
    s.setGain(0, -5);
    s.finishEdit();
    CHECK_FALSE(s.undoStep(later));
    CHECK(s.rowCount() == 2);
    s.undo();
    CHECK(s.undoStep(later));
    CHECK(s.rowCount() == 3);
    CHECK(s.bandAt(0)->id == 1);
    CHECK(s.selectedRow() == 0);
}

TEST_CASE("undo: an added band goes away and the band selected before it is selected again") {
    EqSession s;
    three(s);
    s.select(2);
    s.addBand(300, 2);
    CHECK(s.selectedRow() == 3);
    s.undo();
    CHECK(s.rowCount() == 3);
    CHECK(s.selectedRow() == 2);
    s.redo();
    CHECK(s.rowCount() == 4);
    CHECK(s.selectedRow() == 3);
}

TEST_CASE("undo: loading what an output plays starts the history again") {
    EqSession s;
    three(s);
    s.setGain(0, 5);
    s.finishEdit();
    REQUIRE(s.canUndo());
    QSignalSpy history(&s, &EqSession::historyChanged);
    three(s);
    CHECK_FALSE(s.canUndo());
    CHECK_FALSE(s.canRedo());
    CHECK(history.size() >= 1);
}

TEST_CASE("undo: a preset's EQ is one step and keeps what belongs to the output") {
    EqSession s;
    EqState st;
    st.bands = {peak(1, 100, -1)};
    st.mute = true;
    st.bypass = true;
    s.loadState(&st);
    s.setBalance(0.3);
    s.finishEdit();

    EqState eq;
    eq.bands = {peak(4, 250, -3), peak(9, 4000, 2)};
    eq.preamp_db = -2;
    eq.auto_preamp = false;
    s.setEqPart(eq);
    REQUIRE(s.rowCount() == 2);
    CHECK(s.bandAt(0)->id == 4);
    CHECK(s.preampDb() == -2);
    CHECK_FALSE(s.autoPreamp());
    CHECK(s.muted());
    CHECK_FALSE(s.eqOn());
    CHECK(s.balance() == doctest::Approx(0.3));

    s.undo();
    REQUIRE(s.rowCount() == 1);
    CHECK(s.bandAt(0)->fc == 100);
    CHECK(s.autoPreamp());
}
