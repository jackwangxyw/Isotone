// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors
//
// How the spectrum is drawn: the points the graph asks for, their smoothing and
// the curve through them, and the analyzer's mean-power bands. The owner's
// report (2026-09-15): one point per pixel, each the loudest bin in its span,
// drew every harmonic as a spike.

#include "doctest.h"

#include <QPainterPath>
#include <QPointF>

#include <cmath>
#include <vector>

#include "eqsession.h"   // ResponseGraph holds a QPointer to it
#include "responsegraph.h"
#include "spectrum.h"

using namespace isotone::ui;

namespace {

constexpr double kPi = 3.14159265358979323846;

// An analyzer fed `seconds` of a signal, ready to be sampled.
void feed(SpectrumAnalyzer& a, const std::vector<float>& mono) {
    a.push(mono.data(), mono.size(), 1);
    a.update(48000.0, 10.0);   // long enough that the smoothing has settled
}

std::vector<double> log_points(double lo, double hi, size_t n) {
    std::vector<double> f(n);
    for (size_t i = 0; i < n; ++i) f[i] = lo * std::exp(std::log(hi / lo) * static_cast<double>(i) / static_cast<double>(n - 1));
    return f;
}

}  // namespace

TEST_CASE("the spectrum is drawn from far fewer points than pixels") {
    CHECK(ResponseGraph::spectrumPoints(1300) == 260);
    CHECK(ResponseGraph::spectrumPoints(400) == 80);
    // Held between 64 and 320, so a narrow window keeps a readable curve and a
    // wide one does not go back to a point per pixel.
    CHECK(ResponseGraph::spectrumPoints(100) == 64);
    CHECK(ResponseGraph::spectrumPoints(4000) == 320);
}

TEST_CASE("display smoothing rounds a spike and leaves a straight line alone") {
    std::vector<double> spike(9, -60.0);
    spike[4] = -20.0;
    std::vector<double> smoothed = spike;
    ResponseGraph::smoothForDisplay(smoothed);
    CHECK(smoothed[4] > -45.0);            // the peak is kept, lower
    CHECK(smoothed[4] < -25.0);
    CHECK(smoothed[3] > spike[3] + 5.0);   // its neighbours rise
    CHECK(smoothed[0] == doctest::Approx(-60.0));

    std::vector<double> ramp(9);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = -80.0 + 5.0 * static_cast<double>(i);
    std::vector<double> kept = ramp;
    ResponseGraph::smoothForDisplay(kept);
    for (size_t i = 2; i + 2 < ramp.size(); ++i) CHECK(kept[i] == doctest::Approx(ramp[i]).epsilon(1e-9));

    // The ends are averaged over what there is, never left at a floor.
    std::vector<double> flat(5, -30.0);
    ResponseGraph::smoothForDisplay(flat);
    for (double v : flat) CHECK(v == doctest::Approx(-30.0));
}

TEST_CASE("the curve goes through its points and bends between them") {
    const std::vector<QPointF> points = {{0, 100}, {10, 60}, {20, 80}, {30, 40}};
    const QPainterPath path = ResponseGraph::curveThrough(points);
    REQUIRE(path.elementCount() > 0);
    CHECK(path.pointAtPercent(0).x() == doctest::Approx(0.0));
    CHECK(path.pointAtPercent(0).y() == doctest::Approx(100.0));
    CHECK(path.currentPosition().x() == doctest::Approx(30.0));
    CHECK(path.currentPosition().y() == doctest::Approx(40.0));
    // Curved, not a polyline: between two points it leaves the straight line.
    const QPointF middle = path.pointAtPercent(0.5);
    CHECK(middle.x() > 0.0);
    CHECK(middle.x() < 30.0);
    bool off_the_line = false;
    for (double t = 0.05; t < 0.95; t += 0.05) {
        const QPointF q = path.pointAtPercent(t);
        if (q.x() >= 10.0 && q.x() <= 20.0) {
            const double straight = 60.0 + (q.x() - 10.0) * (80.0 - 60.0) / 10.0;
            off_the_line = off_the_line || std::abs(q.y() - straight) > 0.5;
        }
    }
    CHECK(off_the_line);
    CHECK(ResponseGraph::curveThrough({}).elementCount() == 0);
}

TEST_CASE("a band of bins reads as its loudest bin or as its mean power") {
    SpectrumAnalyzer a;
    const double rate = 48000.0, bin = rate / SpectrumAnalyzer::kFftSize;
    const double f = 170 * bin;   // 996 Hz, on a bin
    std::vector<float> sine(SpectrumAnalyzer::kFftSize);
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * f * static_cast<double>(i) / rate));
    feed(a, sine);

    // A display point whose span holds many bins: the loudest reads the sine's
    // level, the mean reads lower because the rest of the span is silent.
    const std::vector<double> freqs = log_points(20.0, 20000.0, 260);
    std::vector<double> loudest(freqs.size()), mean(freqs.size());
    a.levels_at(freqs.data(), freqs.size(), loudest.data(), SpectrumAnalyzer::Bands::Loudest);
    a.levels_at(freqs.data(), freqs.size(), mean.data(), SpectrumAnalyzer::Bands::Mean);
    size_t at = 0;
    for (size_t i = 0; i < freqs.size(); ++i)
        if (std::abs(freqs[i] - f) < std::abs(freqs[at] - f)) at = i;
    CHECK(loudest[at] == doctest::Approx(-6.0).epsilon(0.02));
    CHECK(mean[at] < loudest[at] - 3.0);

    // Noise: the mean of a span is steadier than its loudest bin, which is what
    // made the drawn curve hairy.
    SpectrumAnalyzer n;
    std::vector<float> noise(SpectrumAnalyzer::kFftSize);
    uint32_t seed = 12345;
    for (float& v : noise) {
        seed = seed * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<double>(seed >> 8) / 8388608.0 - 1.0) * 0.2f;
    }
    feed(n, noise);
    std::vector<double> n_loudest(freqs.size()), n_mean(freqs.size());
    n.levels_at(freqs.data(), freqs.size(), n_loudest.data(), SpectrumAnalyzer::Bands::Loudest);
    n.levels_at(freqs.data(), freqs.size(), n_mean.data(), SpectrumAnalyzer::Bands::Mean);
    const auto wobble = [&](const std::vector<double>& v) {
        double sum = 0.0;
        size_t count = 0;
        for (size_t i = 200; i + 1 < v.size(); ++i) {   // above 5 kHz, where a span holds many bins
            sum += std::abs(v[i + 1] - v[i]);
            ++count;
        }
        return sum / static_cast<double>(count);
    };
    MESSAGE("wobble mean " << wobble(n_mean) << " loudest " << wobble(n_loudest));
    // Steadier point to point, which is what the drawn curve shows.
    CHECK(wobble(n_mean) < wobble(n_loudest) * 0.9);
}
