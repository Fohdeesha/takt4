#include "core/timing/tick_fit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

using takt4::timing::TickFit;

namespace {

const std::filesystem::path kTestData{TAKT4_TEST_DATA_DIR};

/// A MIDI clock as a receiver meets it: when each tick really was, and when it was stamped.
struct Played {
    std::vector<double> truth;
    std::vector<std::int64_t> stamps;
};

struct Sender {
    double bpm = 128.0;
    double seconds = 14.0;
    /// A tempo move by `factor`, starting `moveAt` seconds in and taking `over` seconds.
    double moveAt = -1.0;
    double factor = 1.0;
    double over = 0.0;
    /// Each tick sent on the first millisecond of a 1 ms grid at or after its moment — what
    /// takt4's own clock out does — or, when this is set, that far either way at random.
    double jitterMicros = 0.0;
};

/// Where the stamps start: a large offset, as Link's clock has after days of uptime.
constexpr std::int64_t kOrigin = 3'000'000'000'000LL;

/// The ticks `sender` sends, each delivered `delays[i % size]` microseconds late — and never
/// before the tick ahead of it, which a port's queue does not allow.
Played play(const Sender& sender, const std::vector<std::int64_t>& delays) {
    std::mt19937 random(7);
    std::uniform_real_distribution<double> jitter(-sender.jitterMicros, sender.jitterMicros);
    const double gridPhase = 317.0;
    Played played;
    double t = 0.0;
    while (t < sender.seconds * 1e6) {
        played.truth.push_back(t);
        double tempo = sender.bpm;
        if (sender.moveAt >= 0.0 && t >= sender.moveAt * 1e6) {
            const double done =
                sender.over <= 0.0 ? 1.0
                                   : std::min(1.0, (t - sender.moveAt * 1e6) / (sender.over * 1e6));
            tempo = sender.bpm * (1.0 + (sender.factor - 1.0) * done);
        }
        t += 60e6 / (tempo * 24.0);
    }
    for (std::size_t i = 0; i < played.truth.size(); ++i) {
        const double sent =
            sender.jitterMicros > 0.0
                ? played.truth[i] + jitter(random)
                : std::ceil((played.truth[i] - gridPhase) / 1000.0) * 1000.0 + gridPhase;
        std::int64_t stamp =
            static_cast<std::int64_t>(std::llround(sent)) + kOrigin + delays[i % delays.size()];
        if (!played.stamps.empty()) {
            stamp = std::max(stamp, played.stamps.back());
        }
        played.stamps.push_back(stamp);
    }
    return played;
}

/// For every tick from `first` on, where the line put it, less where it was: each predicted from
/// the ticks that had arrived a millisecond before its moment — what a follower has in hand when
/// it has to say "now".
std::vector<double> errors(TickFit& fit, const Played& played, std::size_t first = 60) {
    std::vector<double> out(played.truth.size(), 0.0);
    std::size_t fed = 0;
    for (std::size_t i = 0; i < played.truth.size(); ++i) {
        const double cutoff = played.truth[i] + static_cast<double>(kOrigin) - 1000.0;
        while (fed < played.stamps.size() && static_cast<double>(played.stamps[fed]) <= cutoff) {
            fit.add(static_cast<std::int64_t>(fed), played.stamps[fed]);
            ++fed;
        }
        if (fit.fitted()) {
            out[i] = fit.momentOf(static_cast<double>(i)) -
                     (played.truth[i] + static_cast<double>(kOrigin));
        }
    }
    out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(std::min(first, out.size())));
    return out;
}

double worst(const std::vector<double>& values) {
    double most = 0.0;
    for (const double value : values) {
        most = std::max(most, std::abs(value));
    }
    return most;
}

double rms(const std::vector<double>& values) {
    double sum = 0.0;
    for (const double value : values) {
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(values.size()));
}

double mean(const std::vector<double>& values) {
    double sum = 0.0;
    for (const double value : values) {
        sum += value;
    }
    return sum / static_cast<double>(values.size());
}

const std::vector<std::int64_t> kOnTime{40};

std::vector<std::int64_t> measuredDeliveries() {
    std::ifstream in(kTestData / "midi" / "held-up-deliveries.txt");
    std::vector<std::int64_t> delays;
    for (std::int64_t delay = 0; in >> delay;) {
        delays.push_back(delay);
    }
    return delays;
}

/// How long after tick `from` the error stays inside `within` for good, in seconds — or a
/// negative number if it never does.
double settledAfter(const std::vector<double>& error, const Played& played, std::size_t first,
                    std::size_t from, double within) {
    for (std::size_t i = from - first; i < error.size(); ++i) {
        bool stays = true;
        for (std::size_t j = i; j < error.size() && stays; ++j) {
            stays = std::abs(error[j]) <= within;
        }
        if (stays) {
            return (played.truth[i + first] - played.truth[from]) / 1e6;
        }
    }
    return -1.0;
}

std::size_t tickAt(const Played& played, double seconds) {
    return static_cast<std::size_t>(
        std::lower_bound(played.truth.begin(), played.truth.end(), seconds * 1e6) -
        played.truth.begin());
}

} // namespace

TEST_CASE("a steady MIDI clock is read to its own millisecond grid, and its tempo exactly",
          "[timing][midi]") {
    for (const double bpm : {90.0, 128.0, 174.0}) {
        INFO(bpm << " BPM");
        const Played played = play(Sender{bpm, 10.0}, kOnTime);
        TickFit fit;
        const std::vector<double> error = errors(fit, played);
        // The sender's grid alone puts each tick up to a millisecond late of its moment.
        CHECK(worst(error) <= 1000.0);
        CHECK(std::abs(60e6 / (fit.microsPerTick() * 24.0) - bpm) <= 0.01);
        CHECK(fit.heldBack() == 0);
        CHECK(fit.tempoChanges() == 0);
    }
}

TEST_CASE("MIDI clock ticks held up on the way in do not drag the line late", "[timing][midi]") {
    // The deliveries measured on a busy machine: 183 of 2081 ticks more than a millisecond late,
    // up to 28.7 ms, sixteen in a row.
    const std::vector<std::int64_t> delays = measuredDeliveries();
    REQUIRE(delays.size() == 2081);
    for (const double bpm : {90.0, 128.0, 174.0}) {
        INFO(bpm << " BPM");
        const Played played = play(Sender{bpm, 2081.0 * 60.0 / (bpm * 24.0)}, delays);
        TickFit fit;
        const std::vector<double> error = errors(fit, played);
        CHECK(worst(error) <= 2500.0);
        CHECK(rms(error) <= 700.0);
        CHECK(fit.heldBack() > 50);
        CHECK(fit.tempoChanges() == 0);

        // What a plain line through the same ticks makes of them — the measurement is a real test.
        TickFit::Options plain;
        plain.leastTolerance = 1e12;
        TickFit plainFit(plain);
        CHECK(worst(errors(plainFit, played)) > 5000.0);
    }
}

TEST_CASE("a MIDI clock moved two percent is followed within a fraction of a second",
          "[timing][midi]") {
    // Slower is followed more slowly than faster: ticks late of the line are taken as a new tempo
    // only after eight of them, since lateness that grows tick by tick is what a busy machine's
    // deliveries look like too. At 90 BPM that is 5.3 ms off at worst.
    for (const double bpm : {90.0, 128.0, 174.0}) {
        for (const double factor : {1.02, 0.98}) {
            for (const double over : {0.0, 1.0}) {
                INFO(bpm << " BPM, x" << factor << " over " << over << " s");
                const Played played = play(Sender{bpm, 14.0, 6.0, factor, over}, kOnTime);
                TickFit fit;
                const std::vector<double> error = errors(fit, played);
                const std::size_t move = tickAt(played, 6.0);
                const std::vector<double> after(
                    error.begin() + static_cast<std::ptrdiff_t>(move - 60), error.end());
                CHECK(worst(after) <= 6000.0);
                const double settled = settledAfter(error, played, 60, move, 1200.0);
                CHECK(settled >= 0.0);
                CHECK(settled <= over + 0.8);
            }
        }
    }
}

TEST_CASE("a MIDI clock that jumps to a new tempo is followed there", "[timing][midi]") {
    struct Jump {
        double from;
        double factor;
        double worstMicros;
        double settleSeconds;
    };
    // A DAW given a new tempo, a mixer's BPM counter jumping an octave. Up to an octave and a
    // little either way the new tempo is taken within a few ticks; beyond that (x3), once the
    // ticks have stayed off the old line for a beat.
    for (const Jump jump : {Jump{128.0, 0.9, 20000.0, 0.3}, Jump{128.0, 1.1, 15000.0, 0.3},
                            Jump{90.0, 128.0 / 90.0, 50000.0, 0.3}, Jump{128.0, 2.0, 50000.0, 0.3},
                            Jump{128.0, 0.5, 200000.0, 0.5}, Jump{60.0, 3.0, 1000000.0, 1.2}}) {
        INFO(jump.from << " BPM x" << jump.factor);
        const Played played = play(Sender{jump.from, 12.0, 6.0, jump.factor, 0.0}, kOnTime);
        TickFit fit;
        const std::vector<double> error = errors(fit, played);
        const std::size_t move = tickAt(played, 6.0);
        const std::vector<double> after(error.begin() + static_cast<std::ptrdiff_t>(move - 60),
                                        error.end());
        CHECK(worst(after) <= jump.worstMicros);
        const double settled = settledAfter(error, played, 60, move, 1200.0);
        CHECK(settled >= 0.0);
        CHECK(settled <= jump.settleSeconds);
        CHECK(fit.tempoChanges() >= 1);
        CHECK(std::abs(60e6 / (fit.microsPerTick() * 24.0) - jump.from * jump.factor) <= 0.05);
    }
}

TEST_CASE("a jittery MIDI clock is read through its own scatter, neither early nor late",
          "[timing][midi]") {
    // A sender that puts each tick anywhere within two milliseconds of its moment. Its scatter is
    // its own, not deliveries held up; holding back the late half of it would pull the line early.
    for (const double bpm : {90.0, 128.0, 174.0}) {
        INFO(bpm << " BPM");
        Sender sender{bpm, 20.0};
        sender.jitterMicros = 2000.0;
        const Played played = play(sender, kOnTime);
        TickFit fit;
        const std::vector<double> error = errors(fit, played);
        CHECK(std::abs(mean(error)) <= 300.0);
        CHECK(worst(error) <= 1500.0);
        CHECK(fit.tempoChanges() == 0);
    }
}

TEST_CASE("MIDI clock ticks queued up and delivered in a bunch are not a new tempo",
          "[timing][midi]") {
    // A stall: nothing for 100 ms, then the ticks it held arrive a fifth of a millisecond apart.
    // Five in a row off the line, all on one side — and at a tempo no clock has.
    std::vector<std::int64_t> delays(2000, 40);
    const double gap = 60e6 / (128.0 * 24.0);
    for (std::size_t i = 0; i < 5; ++i) {
        delays[600 + i] =
            static_cast<std::int64_t>(100000.0 - static_cast<double>(i) * (gap - 200.0));
    }
    const Played played = play(Sender{128.0, 2000.0 * gap / 1e6}, delays);
    TickFit fit;
    const std::vector<double> error = errors(fit, played);
    CHECK(fit.tempoChanges() == 0);
    CHECK(fit.heldBack() >= 3);
    CHECK(worst(error) <= 1000.0);
}

TEST_CASE("a MIDI clock line starts again from nothing, keeping its counts", "[timing][midi]") {
    TickFit fit;
    CHECK_FALSE(fit.fitted());
    fit.add(0, 1'000'000);
    CHECK_FALSE(fit.fitted());
    fit.add(1, 1'020'833);
    REQUIRE(fit.fitted());
    CHECK(std::abs(fit.microsPerTick() - 20'833.0) < 1e-6);
    CHECK(std::abs(fit.indexAt(1'041'666) - 2.0) < 1e-3);
    CHECK(std::abs(fit.momentOf(4.0) - 1'083'332.0) < 1e-3);
    fit.startAgain();
    CHECK_FALSE(fit.fitted());
}

namespace {

/// A run that starts on a delivery stall at 128 BPM: the first `burst` ticks handed over
/// `spacing` microseconds apart once it ends, at `releasedAt`, and every tick after on its own
/// moment, a tick's gap apart from where the clock had got to. Returns the ticks' moments, by
/// number, as the clock sent them.
std::vector<double> stallThenOnTime(TickFit& fit, std::size_t burst, double spacing,
                                    double releasedAt, std::size_t after) {
    const double gap = 60e6 / (128.0 * 24.0);
    // The clock's own grid: tick k at `zero + k * gap`, the burst's held back to `releasedAt`.
    const double zero = releasedAt - static_cast<double>(burst) * gap + 0.4 * gap;
    std::vector<double> sent;
    double last = 0.0;
    for (std::size_t k = 0; k < burst + after; ++k) {
        const double moment = zero + static_cast<double>(k) * gap;
        sent.push_back(moment);
        // In order, as a port's queue hands them over: a tick never before the one ahead of it.
        const double at =
            std::max(k < burst ? releasedAt + static_cast<double>(k) * spacing : moment, last);
        last = at;
        fit.add(static_cast<std::int64_t>(k), static_cast<std::int64_t>(std::llround(at)));
    }
    return sent;
}

} // namespace

TEST_CASE("MIDI clock ticks handed over in a bunch as a run starts are no tempo",
          "[timing][midi][follow]") {
    // A stall hands its ticks over a few microseconds apart, and the
    // first six of a run were taken unchecked: the line through them was 5 µs a tick — 500 000
    // BPM — and a follower trusted it. Bunched as the run starts, they start its line again, and
    // the line is made from the ticks that come at their own pace.
    const double gap = 60e6 / (128.0 * 24.0);
    for (const double spacing : {5.0, 100.0, 300.0}) {
        INFO("burst ticks " << spacing << " us apart");
        TickFit fit;
        const std::vector<double> sent = stallThenOnTime(fit, 26, spacing, 3.0e9, 30);
        REQUIRE(fit.fitted());
        CHECK(fit.bunched() >= 25);
        CHECK(std::abs(fit.microsPerTick() - gap) < 2.0);
        // The ticks the burst carried are where the clock sent them, on the line from those after.
        CHECK(std::abs(fit.momentOf(0.0) - sent[0]) < 50.0);
        CHECK(fit.taken() <= 30);
    }
}

TEST_CASE("a run started at a known tempo with its first ticks bunched starts again at that tempo",
          "[timing][midi][follow]") {
    const double gap = 60e6 / (128.0 * 24.0);
    TickFit fit;
    fit.startAgainAt(gap);
    for (std::int64_t k = 0; k < 6; ++k) {
        fit.add(k, 2'000'000'000LL + k * 5); // a Start's first ticks, handed over at once
    }
    CHECK(fit.bunched() == 5);
    // The tick after them, on its own moment: the line is the known tempo through it.
    fit.add(6, 2'000'000'000LL + static_cast<std::int64_t>(gap));
    REQUIRE(fit.fitted());
    CHECK(fit.microsPerTick() == gap);
}

TEST_CASE("a bunched delivery after a tempo change does not undo the change", "[timing][midi]") {
    // Only a run's start is judged for bunching: a tempo found mid-run is already the ticks' own,
    // and a delivery held up just after it must not throw it away.
    const Played played = play(Sender{120.0, 6.0, 3.0, 1.25, 0.0}, std::vector<std::int64_t>{0});
    TickFit fit;
    std::size_t i = 0;
    for (; i < played.stamps.size() && fit.tempoChanges() == 0; ++i) {
        fit.add(static_cast<std::int64_t>(i), played.stamps[i]);
    }
    REQUIRE(fit.tempoChanges() == 1);
    const std::size_t taken = fit.taken();
    REQUIRE(taken < TickFit::Options{}.firstTaken); // the new line is short, and still unchecked
    // A tick held up nearly a whole gap, then the next on time: the two arrive together.
    fit.add(static_cast<std::int64_t>(i), played.stamps[i + 1] - 400);
    fit.add(static_cast<std::int64_t>(i + 1), played.stamps[i + 1]);
    CHECK(fit.bunched() == 0);
    CHECK(fit.fitted());
    CHECK(fit.taken() == taken + 2);
}
