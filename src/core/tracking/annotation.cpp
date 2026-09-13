#include "core/tracking/annotation.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace takt4::tracking {

namespace {

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    return n % 2 == 1 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double position = fraction * static_cast<double>(values.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = std::min(lower + 1, values.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return values[lower] * (1.0 - weight) + values[upper] * weight;
}

/// Sorted, and every press within `within` of the one kept before it folded into that
/// one — carrying its downbeat with it, so "beat key, then downbeat key" is a downbeat.
std::vector<Tap> debounce(std::vector<Tap> taps, double within) {
    std::stable_sort(taps.begin(), taps.end(),
                     [](const Tap& a, const Tap& b) { return a.seconds < b.seconds; });
    std::vector<Tap> out;
    out.reserve(taps.size());
    for (const Tap& tap : taps) {
        if (!out.empty() && tap.seconds - out.back().seconds < within) {
            out.back().downbeat = out.back().downbeat || tap.downbeat;
            continue;
        }
        out.push_back(tap);
    }
    return out;
}

/// The strongest local maximum of `activation` within `window` of `seconds` that reaches
/// `least`, placed to a fraction of a frame by the parabola through it and its two
/// neighbours; nothing when there is none.
std::optional<double> nearestPeak(const std::vector<float>& activation, double fps, double seconds,
                                  double window, float least) {
    if (activation.size() < 3) {
        return std::nullopt;
    }
    const long last = static_cast<long>(activation.size()) - 1;
    const long from = std::max(1L, static_cast<long>(std::floor((seconds - window) * fps)));
    const long to = std::min(last - 1, static_cast<long>(std::ceil((seconds + window) * fps)));
    std::optional<long> best;
    for (long f = from; f <= to; ++f) {
        const std::size_t i = static_cast<std::size_t>(f);
        const float here = activation[i];
        if (here < least || here < activation[i - 1] || here < activation[i + 1]) {
            continue;
        }
        const bool stronger = !best || here > activation[static_cast<std::size_t>(*best)];
        const bool asStrongAndNearer =
            best && here == activation[static_cast<std::size_t>(*best)] &&
            std::abs(static_cast<double>(f) / fps - seconds) <
                std::abs(static_cast<double>(*best) / fps - seconds);
        if (stronger || asStrongAndNearer) {
            best = f;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    const std::size_t b = static_cast<std::size_t>(*best);
    // Widened explicitly: the parabola is fitted in double, and GCC and Clang refuse the
    // implicit float-to-double promotion under -Wdouble-promotion -Werror.
    const double left = static_cast<double>(activation[b - 1]);
    const double centre = static_cast<double>(activation[b]);
    const double right = static_cast<double>(activation[b + 1]);
    const double curvature = left - 2.0 * centre + right;
    const double offset = curvature < 0.0 ? std::clamp(0.5 * (left - right) / curvature, -0.5, 0.5) : 0.0;
    const double at = (static_cast<double>(*best) + offset) / fps;
    if (std::abs(at - seconds) > window + 0.5 / fps) {
        return std::nullopt;
    }
    return at;
}

} // namespace

Annotation annotate(std::vector<Tap> taps, const std::vector<float>& activation,
                    const AnnotationOptions& options) {
    Annotation out;
    taps = debounce(std::move(taps), options.debounceSeconds);
    out.stats.taps = taps.size();
    if (taps.empty()) {
        return out;
    }

    // Snap: every tap with a peak within reach goes to it; the rest move back by the
    // median of how far the snapped ones moved, which is the thumb's own lateness.
    std::vector<double> offsets;
    std::vector<std::optional<double>> snapped(taps.size());
    if (options.snapWindowSeconds > 0.0 && !activation.empty()) {
        for (std::size_t i = 0; i < taps.size(); ++i) {
            snapped[i] = nearestPeak(activation, options.activationFps, taps[i].seconds,
                                     options.snapWindowSeconds,
                                     static_cast<float>(options.snapMinActivation));
            if (snapped[i]) {
                offsets.push_back(taps[i].seconds - *snapped[i]);
            }
        }
    }
    out.stats.snapped = offsets.size();
    out.stats.medianOffsetSeconds = median(offsets);
    out.stats.spreadSeconds =
        offsets.empty() ? 0.0 : percentile(offsets, 0.9) - percentile(offsets, 0.1);
    for (std::size_t i = 0; i < taps.size(); ++i) {
        taps[i].seconds = snapped[i] ? *snapped[i] : taps[i].seconds - out.stats.medianOffsetSeconds;
    }
    // Two taps can snap onto one peak, or swap places on the way; the order and the
    // debounce hold after the snap as before it.
    taps = debounce(std::move(taps), options.debounceSeconds);

    // The octave the file is written at.
    if (options.octave < 0 && taps.size() >= 2) {
        // Every other tap, on the parity of the first downbeat so that one survives.
        std::size_t parity = 0;
        for (std::size_t i = 0; i < taps.size(); ++i) {
            if (taps[i].downbeat) {
                parity = i % 2;
                break;
            }
        }
        std::vector<Tap> kept;
        for (std::size_t i = 0; i < taps.size(); ++i) {
            if (i % 2 == parity) {
                kept.push_back(taps[i]);
            }
        }
        taps = std::move(kept);
    } else if (options.octave > 0 && taps.size() >= 2) {
        std::vector<Tap> doubled;
        doubled.reserve(taps.size() * 2);
        for (std::size_t i = 0; i < taps.size(); ++i) {
            doubled.push_back(taps[i]);
            if (i + 1 < taps.size()) {
                doubled.push_back(Tap{0.5 * (taps[i].seconds + taps[i + 1].seconds), false});
            }
        }
        taps = std::move(doubled);
    }

    // The bar: the downbeats as tapped, the count between them as the meter, and every
    // beat numbered from the downbeat before it — or, before the first, back from it.
    std::vector<std::size_t> downs;
    for (std::size_t i = 0; i < taps.size(); ++i) {
        if (taps[i].downbeat) {
            downs.push_back(i);
        }
    }
    int meter = std::max(1, options.defaultMeter);
    if (downs.size() >= 2) {
        std::vector<double> gaps;
        for (std::size_t k = 1; k < downs.size(); ++k) {
            gaps.push_back(static_cast<double>(downs[k] - downs[k - 1]));
        }
        meter = std::clamp(static_cast<int>(std::lround(median(gaps))), 1, 16);
    }
    out.times.reserve(taps.size());
    for (const Tap& tap : taps) {
        out.times.push_back(tap.seconds);
    }
    out.beatInBar.assign(taps.size(), 0);
    if (!downs.empty()) {
        for (std::size_t i = 0; i < taps.size(); ++i) {
            const auto after = std::upper_bound(downs.begin(), downs.end(), i);
            const long anchor = static_cast<long>(after == downs.begin() ? downs.front() : *(after - 1));
            const long distance = static_cast<long>(i) - anchor;
            out.beatInBar[i] = static_cast<int>(((distance % meter) + meter) % meter) + 1;
        }
    }
    out.stats.meter = downs.empty() ? 0 : meter;
    out.stats.downbeats = downs.size();
    if (out.times.size() >= 2) {
        std::vector<double> gaps;
        for (std::size_t i = 1; i < out.times.size(); ++i) {
            gaps.push_back(out.times[i] - out.times[i - 1]);
        }
        const double gap = median(gaps);
        out.stats.bpm = gap > 0.0 ? 60.0 / gap : 0.0;
    }
    return out;
}

void writeBeats(const std::filesystem::path& path, const Annotation& annotation) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error(path.string() + ": cannot create");
    }
    out << std::fixed << std::setprecision(4);
    for (std::size_t i = 0; i < annotation.times.size(); ++i) {
        out << annotation.times[i] << '\t' << annotation.beatInBar[i] << '\n';
    }
}

void writeTaps(const std::filesystem::path& path, const std::vector<Tap>& taps) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error(path.string() + ": cannot create");
    }
    out << std::fixed << std::setprecision(4);
    for (const Tap& tap : taps) {
        out << tap.seconds;
        if (tap.downbeat) {
            out << "\td";
        }
        out << '\n';
    }
}

std::vector<Tap> readTaps(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error(path.string() + ": cannot open");
    }
    std::vector<Tap> taps;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        Tap tap;
        if (!(fields >> tap.seconds)) {
            continue; // blank, or a comment
        }
        std::string flag;
        if (fields >> flag) {
            tap.downbeat = flag == "d" || flag == "D" || flag == "1";
        }
        taps.push_back(tap);
    }
    return taps;
}

} // namespace takt4::tracking
