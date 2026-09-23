#include "core/audio/input_watchdog.hpp"

#include <cmath>

namespace takt4::audio {

InputWatchdog::InputWatchdog() : InputWatchdog(Options{}) {}

InputWatchdog::InputWatchdog(Options options) : options_(options) {}

void InputWatchdog::reset(double openedRate, double now) {
    openedRate_ = openedRate;
    openedAt_ = now;
    lastCallbacks_ = 0;
    lastCallbackAt_ = now;
    anyCallback_ = false;
    samples_.clear();
}

InputWatchdog::Reading InputWatchdog::observe(const InputStreamCounters& counters, double now) {
    Reading reading;
    reading.inputOverflows = counters.inputOverflows;

    bool advanced = false;
    if (counters.callbacks != lastCallbacks_) {
        lastCallbacks_ = counters.callbacks;
        lastCallbackAt_ = now;
        anyCallback_ = true;
        advanced = true;
    }

    if (!anyCallback_) {
        // Nothing yet. Given its grace period, and then it is as dead as one that stopped.
        if (now - openedAt_ < options_.startupGraceSeconds) {
            reading.verdict = Verdict::Starting;
            return reading;
        }
        reading.verdict = Verdict::Silent;
        reading.silentForSeconds = now - openedAt_;
        samples_.clear();
        return reading;
    }

    const double silentFor = now - lastCallbackAt_;
    if (silentFor >= options_.silentAfterSeconds) {
        reading.verdict = Verdict::Silent;
        reading.silentForSeconds = silentFor;
        // A rate measured across a gap is not a rate; start the window again when the audio
        // comes back.
        samples_.clear();
        return reading;
    }

    reading.verdict = Verdict::Healthy;
    if (silentFor > options_.stallSeconds) {
        // A stall that has not yet become silence. The frames it lost would read as a slower
        // clock, so the measurement starts again once the callbacks come back.
        samples_.clear();
        return reading;
    }
    // Only at a look that saw the callbacks move, so every sample sits on delivered audio.
    if (advanced) {
        samples_.push_back(Sample{now, counters.framesIn});
    }
    if (samples_.empty()) {
        return reading;
    }
    // Keep a little over a window's worth of looks: stalls leave gaps that are not counted
    // below, so the samples have to reach further back than the window itself.
    while (samples_.size() > 2 &&
           now - samples_[1].at >= options_.rateWindowSeconds + 2.0 * options_.stallSeconds) {
        samples_.pop_front();
    }
    // The rate over consecutive pairs of looks that were a normal distance apart, and only
    // those. A pair that straddles a dropout — frames the driver never delivered — is left
    // out, so a hiccup cannot read as a slower clock; leaving out a normal pair biases nothing,
    // because each pair's own frames-per-second is right. A whole-window average was the first
    // version, and a single 80 ms dropout pulled it 4 % low (measured, by the test that found
    // it). Over the pairs kept, the counters' buffer-sized steps still average out to about a
    // tenth of a percent across three seconds.
    const double longestPair = options_.stallSeconds * 0.6;
    double frames = 0.0;
    double seconds = 0.0;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        const double gap = samples_[i].at - samples_[i - 1].at;
        if (gap <= 0.0 || gap > longestPair || samples_[i].frames < samples_[i - 1].frames) {
            continue;
        }
        frames += static_cast<double>(samples_[i].frames - samples_[i - 1].frames);
        seconds += gap;
    }
    if (seconds >= options_.rateWindowSeconds) {
        reading.measuredRate = frames / seconds;
        if (openedRate_ > 0.0 &&
            std::abs(reading.measuredRate / openedRate_ - 1.0) > options_.rateTolerance) {
            reading.verdict = Verdict::RateChanged;
        }
    }
    return reading;
}

} // namespace takt4::audio
