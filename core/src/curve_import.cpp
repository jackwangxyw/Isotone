// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 The Isotone authors

#include "isotone/curve_import.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>

#include "isotone/response.h"

namespace isotone {

namespace {

constexpr double kFitLowHz = 20.0;
constexpr double kFitHighHz = 20000.0;
constexpr size_t kGridPoints = 256;
// The candidates a band is chosen from, and how many bands the fit will use.
constexpr double kCandidateStep = 1.122462048;   // a sixth of an octave
constexpr double kCandidateQs[] = {0.7, 1.0, 1.41, 2.0, 3.0, 4.5, 7.0};
constexpr size_t kMaxFitBands = 12;
// Close enough to the curve to stop adding bands, rms dB.
constexpr double kCloseEnoughDb = 0.22;
// Where the end shapes are judged: a high-pass under the first, a high shelf over
// the second.
constexpr double kShapeLowHz = 120.0;
constexpr double kShapeHighHz = 6000.0;
constexpr int kRefineRounds = 3;
// A gain under this is not worth a band.
constexpr double kKeepGainDb = 0.1;

bool starts_with_word(const std::string& text, const char* word, size_t* after) {
    size_t i = text.find_first_not_of(" \t\r\n");
    if (i == std::string::npos) return false;
    const size_t n = std::char_traits<char>::length(word);
    for (size_t k = 0; k < n; ++k) {
        if (i + k >= text.size() || std::tolower(static_cast<unsigned char>(text[i + k])) != std::tolower(static_cast<unsigned char>(word[k])))
            return false;
    }
    *after = i + n;
    return true;
}

// "GraphicEQ: 20 -1.2; 21 -1.3; ..." Upstream splits on ';' and reads two numbers.
std::vector<CurvePoint> parse_graphic_eq(const std::string& text, size_t from) {
    std::vector<CurvePoint> points;
    size_t i = text.find(':', from);
    if (i == std::string::npos) return points;
    ++i;
    while (i < text.size()) {
        size_t end = text.find(';', i);
        if (end == std::string::npos) end = text.size();
        const std::string pair = text.substr(i, end - i);
        char* rest = nullptr;
        const double hz = std::strtod(pair.c_str(), &rest);
        if (rest != pair.c_str()) {
            char* after = nullptr;
            const double db = std::strtod(rest, &after);
            if (after != rest && hz > 0 && std::isfinite(db)) points.push_back({hz, db});
        }
        i = end + 1;
    }
    return points;
}

// FilterCurve:f0="10" f1="11.7" ... v0="-39.956" v1="-37.212" ...
std::vector<CurvePoint> parse_filter_curve(const std::string& text) {
    std::vector<double> f, v;
    const auto read = [&](char key, std::vector<double>* out) {
        for (size_t index = 0;; ++index) {
            const std::string name = std::string(1, key) + std::to_string(index) + "=\"";
            const size_t at = text.find(name);
            if (at == std::string::npos) break;
            const size_t start = at + name.size();
            const size_t end = text.find('"', start);
            if (end == std::string::npos) break;
            out->push_back(std::strtod(text.substr(start, end - start).c_str(), nullptr));
        }
    };
    read('f', &f);
    read('v', &v);
    std::vector<CurvePoint> points;
    for (size_t i = 0; i < f.size() && i < v.size(); ++i)
        if (f[i] > 0 && std::isfinite(v[i])) points.push_back({f[i], v[i]});
    return points;
}

// The curve at `hz`, straight between its points in log frequency, and held flat
// outside its ends.
double curve_at(const std::vector<CurvePoint>& curve, double hz) {
    if (hz <= curve.front().hz) return curve.front().db;
    if (hz >= curve.back().hz) return curve.back().db;
    const auto after = std::lower_bound(curve.begin(), curve.end(), hz,
                                        [](const CurvePoint& p, double f) { return p.hz < f; });
    const CurvePoint& hi = *after;
    const CurvePoint& lo = *(after - 1);
    const double t = std::log(hz / lo.hz) / std::log(hi.hz / lo.hz);
    return lo.db + (hi.db - lo.db) * t;
}

Band shaped_band(uint32_t id, FilterType type, double fc, double gain_db, double q) {
    Band b;
    b.id = id;
    b.type = type;
    b.fc = fc;
    b.gain_db = gain_db;
    b.width = q;
    b.width_mode = WidthMode::Q;
    return b;
}

// Solves (A + ridge I) x = b for a small symmetric positive definite A, in place.
bool solve_symmetric(std::vector<double>& a, std::vector<double>& b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        size_t pivot = i;
        for (size_t r = i + 1; r < n; ++r)
            if (std::abs(a[r * n + i]) > std::abs(a[pivot * n + i])) pivot = r;
        if (std::abs(a[pivot * n + i]) < 1e-12) return false;
        if (pivot != i) {
            for (size_t c = 0; c < n; ++c) std::swap(a[i * n + c], a[pivot * n + c]);
            std::swap(b[i], b[pivot]);
        }
        const double d = a[i * n + i];
        for (size_t r = i + 1; r < n; ++r) {
            const double factor = a[r * n + i] / d;
            if (factor == 0.0) continue;
            for (size_t c = i; c < n; ++c) a[r * n + c] -= factor * a[i * n + c];
            b[r] -= factor * b[i];
        }
    }
    for (size_t i = n; i-- > 0;) {
        double sum = b[i];
        for (size_t c = i + 1; c < n; ++c) sum -= a[i * n + c] * b[c];
        b[i] = sum / a[i * n + i];
    }
    return true;
}

// The dB of one band over `freqs`.
std::vector<double> shape_of(const Band& band, const std::vector<double>& freqs, double sample_rate) {
    std::vector<double> db(freqs.size());
    band_magnitude_db(band, freqs.data(), freqs.size(), sample_rate, db.data());
    return db;
}

// A high-pass for a curve that rolls off at the bottom faster than peaking bands
// can: the owner's export was made with one, plus a high shelf (2026-09-15). The
// candidate that fits the target best under kShapeLowHz, or nothing when none of
// them is a real improvement on leaving it to the bands.
std::optional<Band> fit_high_pass(const std::vector<double>& freqs, const std::vector<double>& target,
                                  double sample_rate) {
    const auto region_error = [&](const std::vector<double>* shape) {
        double sum = 0.0;
        size_t count = 0;
        for (size_t i = 0; i < freqs.size(); ++i) {
            if (freqs[i] > kShapeLowHz) break;
            const double value = shape ? (*shape)[i] : 0.0;
            sum += (value - target[i]) * (value - target[i]);
            ++count;
        }
        return count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
    };
    const double flat = region_error(nullptr);
    if (flat < 3.0) return std::nullopt;   // nothing steep enough to need one

    std::optional<Band> best;
    double best_error = flat * 0.7;   // it has to be a clear improvement
    for (double q : {0.5, 0.707, 1.0, 1.4}) {
        for (double fc = 12.0; fc <= 300.0; fc *= 1.06) {
            const Band candidate = shaped_band(1, FilterType::HighPass, fc, 0.0, q);
            const std::vector<double> shape = shape_of(candidate, freqs, sample_rate);
            const double error = region_error(&shape);
            if (error < best_error) {
                best_error = error;
                best = candidate;
            }
        }
    }
    return best;
}

// A high shelf for a curve that lifts or drops the whole top end. Its gain comes
// from a closed-form least-squares fit of the unit shelf over kShapeHighHz up.
std::optional<Band> fit_high_shelf(const std::vector<double>& freqs, const std::vector<double>& target,
                                   double sample_rate, double max_gain_db) {
    const auto region_error = [&](const std::vector<double>* shape, double gain) {
        double sum = 0.0;
        size_t count = 0;
        for (size_t i = 0; i < freqs.size(); ++i) {
            if (freqs[i] < kShapeHighHz) continue;
            const double value = shape ? (*shape)[i] * gain : 0.0;
            sum += (value - target[i]) * (value - target[i]);
            ++count;
        }
        return count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
    };
    const double flat = region_error(nullptr, 0.0);
    if (flat < 2.0) return std::nullopt;

    std::optional<Band> best;
    double best_error = flat * 0.7;
    for (double fc = 1000.0; fc <= 12000.0; fc *= 1.08) {
        const Band unit = shaped_band(1, FilterType::HighShelf, fc, 1.0, 0.707);
        const std::vector<double> shape = shape_of(unit, freqs, sample_rate);
        double top = 0.0, bottom = 0.0;
        for (size_t i = 0; i < freqs.size(); ++i) {
            if (freqs[i] < kShapeHighHz) continue;
            top += shape[i] * target[i];
            bottom += shape[i] * shape[i];
        }
        if (bottom <= 0.0) continue;
        const double gain = std::clamp(top / bottom, -max_gain_db, max_gain_db);
        const double error = region_error(&shape, gain);
        if (error < best_error) {
            best_error = error;
            best = shaped_band(1, FilterType::HighShelf, fc, gain, 0.707);
        }
    }
    return best;
}

}  // namespace

std::vector<CurvePoint> parse_curve(const std::string& text) {
    std::vector<CurvePoint> points;
    size_t after = 0;
    if (starts_with_word(text, "GraphicEQ", &after)) {
        points = parse_graphic_eq(text, after);
    } else if (starts_with_word(text, "FilterCurve", &after)) {
        points = parse_filter_curve(text);
    } else {
        // A curve on a line of its own inside a larger file.
        size_t line_start = 0;
        while (line_start < text.size() && points.empty()) {
            size_t line_end = text.find('\n', line_start);
            if (line_end == std::string::npos) line_end = text.size();
            const std::string line = text.substr(line_start, line_end - line_start);
            size_t ignored = 0;
            if (starts_with_word(line, "GraphicEQ", &ignored) || starts_with_word(line, "FilterCurve", &ignored))
                points = parse_curve(line);
            line_start = line_end + 1;
        }
    }
    std::stable_sort(points.begin(), points.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.hz < b.hz; });
    return points;
}

CurveFit fit_curve(const std::vector<CurvePoint>& curve, double sample_rate, double max_gain_db) {
    CurveFit fit;
    if (curve.size() < 2) return fit;

    // The grid the fit is measured on: the curve's own range, down to 10 Hz, since
    // a high-pass can follow a rolloff below the lowest band.
    const double lo = std::max(curve.front().hz, 10.0);
    const double hi = std::min(curve.back().hz, sample_rate / 2.0 * 0.95);
    if (!(lo < hi)) return fit;
    const std::vector<double> freqs = log_grid(lo, hi, kGridPoints);

    std::vector<double> target(freqs.size());
    for (size_t i = 0; i < freqs.size(); ++i) target[i] = curve_at(curve, freqs[i]);

    // The shapes the curve was made with, where it has them: a high-pass holds a
    // rolloff steeper than a peaking band can, and a high shelf the whole top end
    // (the owner's export has both, 2026-09-15). What is left goes to the bands.
    std::vector<Band> shapes;
    if (const std::optional<Band> hp = fit_high_pass(freqs, target, sample_rate)) shapes.push_back(*hp);
    if (const std::optional<Band> shelf = fit_high_shelf(freqs, target, sample_rate, max_gain_db))
        shapes.push_back(*shelf);
    for (const Band& b : shapes) {
        const std::vector<double> db = shape_of(b, freqs, sample_rate);
        for (size_t i = 0; i < freqs.size(); ++i) target[i] -= db[i];
    }

    // Candidates: peaking bands a sixth of an octave apart, a few widths each.
    struct Candidate {
        double fc = 0;
        double q = 0;
        std::vector<double> shape;   // dB at 1 dB of gain
        double energy = 0;           // shape . shape
    };
    std::vector<Candidate> candidates;
    for (double fc = kFitLowHz; fc <= kFitHighHz * kCandidateStep; fc *= kCandidateStep) {
        for (double q : kCandidateQs) {
            Candidate c;
            c.fc = fc;
            c.q = q;
            c.shape = shape_of(shaped_band(1, FilterType::Peaking, fc, 1.0, q), freqs, sample_rate);
            for (double v : c.shape) c.energy += v * v;
            if (c.energy > 1e-9) candidates.push_back(std::move(c));
        }
    }

    // One band at a time, each the one that takes most of the error out, until the
    // curve is followed closely enough. The file came from a handful of filters and
    // has to come back as a handful, not as one band per third of an octave: the
    // owner's export of thirteen sliders, six of them at 0 dB, fitted as thirty
    // bands (2026-09-15).
    std::vector<Band> chosen;
    std::vector<double> composite(freqs.size(), 0.0);
    EqState state;
    const auto residual_of = [&](std::vector<double>* out) {
        double square_sum = 0.0;
        for (size_t i = 0; i < freqs.size(); ++i) {
            (*out)[i] = target[i] - composite[i];
            square_sum += (*out)[i] * (*out)[i];
        }
        return std::sqrt(square_sum / static_cast<double>(freqs.size()));
    };
    std::vector<double> residual(freqs.size());
    while (chosen.size() < kMaxFitBands) {
        if (residual_of(&residual) < kCloseEnoughDb) break;
        const Candidate* best = nullptr;
        double best_gain = 0, best_score = 0;
        for (const Candidate& c : candidates) {
            double dot = 0.0;
            for (size_t i = 0; i < freqs.size(); ++i) dot += c.shape[i] * residual[i];
            const double gain = std::clamp(dot / c.energy, -max_gain_db, max_gain_db);
            // How much of the square error this band takes out.
            const double score = gain * (2.0 * dot - gain * c.energy);
            if (score > best_score) {
                best_score = score;
                best_gain = gain;
                best = &c;
            }
        }
        if (best == nullptr || std::abs(best_gain) < kKeepGainDb) break;
        chosen.push_back(shaped_band(1, FilterType::Peaking, best->fc, best_gain, best->q));
        state.bands = chosen;
        magnitude_db(state, 2, 0, 0, freqs.data(), freqs.size(), sample_rate, composite.data());
    }

    // The bands overlap, so their gains are solved together once they are chosen,
    // and refined against the real composite (a peaking filter's shape widens a
    // little with gain, so one linear solve is not enough).
    const size_t m = chosen.size();
    if (m > 0) {
        std::vector<std::vector<double>> shape(m);
        for (size_t j = 0; j < m; ++j)
            shape[j] = shape_of(shaped_band(1, FilterType::Peaking, chosen[j].fc, 1.0, chosen[j].width), freqs, sample_rate);

        std::vector<double> ata(m * m, 0.0);
        for (size_t j = 0; j < m; ++j) {
            for (size_t k = j; k < m; ++k) {
                double sum = 0.0;
                for (size_t i = 0; i < freqs.size(); ++i) sum += shape[j][i] * shape[k][i];
                ata[j * m + k] = sum;
                ata[k * m + j] = sum;
            }
            ata[j * m + j] += 1e-3 * ata[j * m + j] + 1e-6;
        }

        std::vector<double> gains(m);
        for (size_t j = 0; j < m; ++j) gains[j] = chosen[j].gain_db;
        for (int round = 0; round < kRefineRounds; ++round) {
            residual_of(&residual);
            std::vector<double> atb(m, 0.0);
            for (size_t j = 0; j < m; ++j) {
                double sum = 0.0;
                for (size_t i = 0; i < freqs.size(); ++i) sum += shape[j][i] * residual[i];
                atb[j] = sum;
            }
            std::vector<double> a = ata;
            if (!solve_symmetric(a, atb, m)) break;
            for (size_t j = 0; j < m; ++j) {
                gains[j] = std::clamp(gains[j] + atb[j], -max_gain_db, max_gain_db);
                chosen[j].gain_db = gains[j];
            }
            state.bands = chosen;
            magnitude_db(state, 2, 0, 0, freqs.data(), freqs.size(), sample_rate, composite.data());
        }
    }

    double worst = 0.0, square_sum = 0.0;
    for (size_t i = 0; i < freqs.size(); ++i) {
        const double error = composite[i] - target[i];
        worst = std::max(worst, std::abs(error));
        square_sum += error * error;
    }
    fit.worst_db = worst;
    fit.rms_db = std::sqrt(square_sum / static_cast<double>(freqs.size()));

    uint32_t id = 1;
    for (Band& b : shapes) {
        b.id = id++;
        fit.bands.push_back(b);
    }
    for (Band& b : chosen) {
        if (std::abs(b.gain_db) < kKeepGainDb) continue;
        b.id = id++;
        fit.bands.push_back(b);
    }
    std::stable_sort(fit.bands.begin(), fit.bands.end(), [](const Band& a, const Band& b) { return a.fc < b.fc; });
    id = 1;
    for (Band& b : fit.bands) b.id = id++;
    return fit;
}

}  // namespace isotone
