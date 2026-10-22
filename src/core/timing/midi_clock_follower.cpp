#include "core/timing/midi_clock_follower.hpp"

#include <algorithm>
#include <cmath>

namespace takt4::timing {

namespace {

constexpr unsigned char kTimingClock = 0xF8;
constexpr unsigned char kStart = 0xFA;
constexpr unsigned char kContinue = 0xFB;
constexpr unsigned char kStop = 0xFC;
constexpr unsigned char kSongPosition = 0xF2;

/// Clock ticks to the beat, and to a Song Position's sixteenth.
constexpr double kTicksPerBeat = 24.0;
constexpr std::int64_t kTicksPerSixteenth = 6;

} // namespace

MidiClockFollower::MidiClockFollower(midi::MidiInputHub& inputs) : inputs_(inputs) {}

MidiClockFollower::~MidiClockFollower() {
    // Off the hub before anything it hands messages to goes.
    stopListening();
}

std::string MidiClockFollower::listen(std::string_view port) {
    return inputs_.listen(port, *this, "MIDI clock: ");
}

void MidiClockFollower::stopListening() noexcept {
    inputs_.stopListening(*this);
}

std::string MidiClockFollower::port() const {
    return inputs_.portOf(*this);
}

void MidiClockFollower::onMidi(std::span<const unsigned char> message,
                               std::int64_t atMicros) noexcept {
    if (message.empty()) {
        return;
    }
    switch (message[0]) {
    case kTimingClock:
    case kStart:
    case kContinue:
    case kStop:
    case kSongPosition:
        break;
    default:
        return; // notes, controllers, timecode: somebody else's
    }
    Message taken;
    taken.at = atMicros;
    taken.status = message[0];
    taken.data1 = message.size() > 1 ? message[1] : static_cast<unsigned char>(0);
    taken.data2 = message.size() > 2 ? message[2] : static_cast<unsigned char>(0);
    if (!messages_.tryPush(taken)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

void MidiClockFollower::begin(std::uint32_t) noexcept {
    // What came while nothing was following is not this run's.
    Message stale;
    while (messages_.tryPop(stale)) {
    }
    dropped_.store(0, std::memory_order_relaxed);
    fit_.reset();
    ticks_ = 0;
    heardTick_ = false;
    lastTickAt_ = 0;
    unbroken_ = 0;
    songOffset_ = 0;
    placed_ = false;
    placements_ = 0;
    placedAt_ = 0.0;
    playing_ = false;
    stopSeen_ = false;
    lastSongTick_ = -1;
    songPositionPending_ = false;
    pendingSongTick_ = 0;
    gapPending_ = false;
    refused_ = 0;
    ticksLost_ = 0;
    gapsUncounted_ = 0;
    // The tempo the clock last ran at is kept: the song this run starts may well be at it, and
    // until the ticks have measured their own it is only the line's slope.
    startedSinceTick_ = false;
    resumed_ = false;
    publish(inputs_.now());
}

GridReading MidiClockFollower::read() noexcept {
    Message message;
    while (messages_.tryPop(message)) {
        handle(message);
    }
    const std::int64_t now = inputs_.now();
    GridReading reading;
    reading.at = now;
    reading.stopped = stopSeen_ && obey_.load(std::memory_order_relaxed);
    if (!reading.stopped && ticking(now) && followed()) {
        const double bpm = 60e6 / (fit_.microsPerTick() * kTicksPerBeat);
        const double beat = (fit_.indexAt(now) + static_cast<double>(songOffset_)) / kTicksPerBeat;
        if (believable(bpm) && std::isfinite(beat)) {
            reading.present = true;
            reading.bpm = bpm;
            reading.beat = beat;
            reading.anchored = placed_;
            reading.placements = placements_;
            reading.placedAt = placedAt_;
        } else {
            ++refused_;
        }
    }
    publish(now);
    return reading;
}

bool MidiClockFollower::believable(double bpm) const noexcept {
    if (!std::isfinite(bpm) || bpm < kMinBpm || bpm > kMaxBpm) {
        return false;
    }
    // In a run's first beats, near what the clock last ran at: one far from it so soon is a
    // stall's ticks handed over in a bunch far likelier than a DJ's new tempo, which a few beats
    // more of ticks will show.
    if (unbroken_ < 2 * kSettledTicks && knownMicrosPerTick_ > 0.0) {
        const double known = 60e6 / (knownMicrosPerTick_ * kTicksPerBeat);
        return bpm >= known / kWidestStart && bpm <= known * kWidestStart;
    }
    return true;
}

void MidiClockFollower::end() noexcept {
    rememberTempo();
    publish(inputs_.now());
}

void MidiClockFollower::handle(const Message& message) noexcept {
    switch (message.status) {
    case kTimingClock:
        tick(message.at);
        break;
    case kStart:
        // "Sequencing begins at song-position 0 when the next Timing Clock message is received."
        playing_ = true;
        stopSeen_ = false;
        startedSinceTick_ = true;
        songPositionPending_ = false;
        placeNextTick(0);
        break;
    case kContinue:
        // On from a Song Position given while stopped, or from where the song stopped. A clock
        // never placed has no song position to go on from, and stays unplaced.
        playing_ = true;
        stopSeen_ = false;
        startedSinceTick_ = true;
        if (songPositionPending_) {
            songPositionPending_ = false;
            placeNextTick(pendingSongTick_);
        } else if (placed_) {
            placeNextTick(lastSongTick_ + 1);
        }
        break;
    case kStop:
        playing_ = false;
        stopSeen_ = true;
        break;
    case kSongPosition: {
        const std::int64_t sixteenths = static_cast<std::int64_t>(message.data1 & 0x7F) |
                                        (static_cast<std::int64_t>(message.data2 & 0x7F) << 7);
        // Sent while stopped, where it says the next Continue goes on from; placed at once if it
        // comes while the song plays.
        if (playing_) {
            placeNextTick(sixteenths * kTicksPerSixteenth);
        } else {
            songPositionPending_ = true;
            pendingSongTick_ = sixteenths * kTicksPerSixteenth;
        }
        break;
    }
    default:
        break;
    }
}

void MidiClockFollower::tick(std::int64_t at) noexcept {
    if (!heardTick_ || static_cast<double>(at - lastTickAt_) > absentAfter()) {
        // A new run of ticks — the run's first, or the clock stopped and has started again — at
        // whatever tempo and phase it starts with, measured afresh. Unless a Start or Continue
        // has just said it is a song starting: then it is taken to be at the tempo it last ran
        // at, and followed from this tick, until it has ticked enough to say.
        rememberTempo();
        // Back with nothing to say where: the ticks the gap lost are counted once this run is
        // followed (`countGap`). A Start, a Continue or a Song Position says where itself.
        gapPending_ = heardTick_ && !startedSinceTick_;
        gapFrom_ = lastTickAt_;
        gapIndex_ = ticks_;
        gapMicrosPerTick_ = knownMicrosPerTick_;
        resumed_ = startedSinceTick_ && knownMicrosPerTick_ > 0.0;
        if (resumed_) {
            fit_.startAgainAt(knownMicrosPerTick_);
        } else {
            fit_.startAgain();
        }
        unbroken_ = 0;
    }
    startedSinceTick_ = false;
    const std::uint64_t bunched = fit_.bunched();
    fit_.add(ticks_, at);
    heardTick_ = true;
    lastTickAt_ = at;
    // A tick bunched with the one before it as the run started (`TickFit::Options::leastGap`):
    // the run is counted again from the next that comes at its own pace, so it is never followed
    // on a line through a burst.
    unbroken_ = fit_.bunched() != bunched ? 0 : unbroken_ + 1;
    if (playing_) {
        lastSongTick_ = ticks_ + songOffset_;
    }
    ++ticks_;
    if (gapPending_ && followed()) {
        countGap();
    }
}

void MidiClockFollower::countGap() noexcept {
    gapPending_ = false;
    // When the first tick back would have been on the line the ticks since make — which a stall's
    // ticks handed over late, in a bunch, put just after the last tick before it, and ticks lost
    // put the length of the gap after it — in ticks of the tempo now.
    const double now = fit_.microsPerTick();
    const double ticks =
        (fit_.momentOf(static_cast<double>(gapIndex_)) - static_cast<double>(gapFrom_)) / now;
    const double whole = std::round(ticks);
    // Sure of the count only when it comes out close to whole, the gap is short enough, and the
    // tempo before it and after it agree closely enough that it cannot be a tick out over it.
    const bool steady =
        gapMicrosPerTick_ <= 0.0 || whole * std::abs(now / gapMicrosPerTick_ - 1.0) <= 0.25;
    const bool sure = std::isfinite(ticks) && whole >= 1.0 && std::abs(ticks - whole) <= 0.25 &&
                      whole <= kLongestCountedGapTicks && steady;
    if (!sure) {
        // The beats are wherever the ticks now say, which is nobody's beat: DOWNBEAT puts them
        // right, and the window says to press it.
        ++gapsUncounted_;
        placed_ = false;
        return;
    }
    const auto lost = static_cast<std::int64_t>(whole) - 1;
    songOffset_ += lost;
    ticksLost_ += static_cast<std::uint64_t>(lost);
}

void MidiClockFollower::placeNextTick(std::int64_t songTick) noexcept {
    // The next tick is number `ticks_`.
    songOffset_ = songTick - ticks_;
    lastSongTick_ = songTick - 1;
    placed_ = true;
    ++placements_;
    placedAt_ = static_cast<double>(songTick) / kTicksPerBeat;
    // Placed now, so there is nothing for a gap to have moved.
    gapPending_ = false;
}

void MidiClockFollower::rememberTempo() noexcept {
    // Only a line that is the ticks' own: followed, through a beat of them at the least, at a tempo
    // a clock has — never one fitted through a burst, which STOP during it used to keep for the
    // next Start.
    if (followed() && fit_.taken() >= static_cast<std::size_t>(kTicksPerBeat)) {
        const double micros = fit_.microsPerTick();
        const double bpm = 60e6 / (micros * kTicksPerBeat);
        if (std::isfinite(bpm) && bpm >= kMinBpm && bpm <= kMaxBpm) {
            knownMicrosPerTick_ = micros;
        }
    }
}

bool MidiClockFollower::followed() const noexcept {
    return unbroken_ >= (resumed_ ? 1U : kSettledTicks) && fit_.fitted();
}

double MidiClockFollower::absentAfter() const noexcept {
    if (!fit_.fitted()) {
        return kAbsentUnfittedMicros;
    }
    return std::max(kAbsentTicks * fit_.microsPerTick(), static_cast<double>(kAbsentMicros));
}

bool MidiClockFollower::ticking(std::int64_t now) const noexcept {
    return heardTick_ && static_cast<double>(now - lastTickAt_) <= absentAfter();
}

void MidiClockFollower::publish(std::int64_t now) noexcept {
    Status status;
    status.ticking = ticking(now);
    if (status.ticking && followed()) {
        status.bpm = 60e6 / (fit_.microsPerTick() * kTicksPerBeat);
    }
    status.placed = placed_;
    status.stopped = stopSeen_;
    if (heardTick_) {
        const auto ticksPerBeat = static_cast<std::int64_t>(kTicksPerBeat);
        const std::int64_t songTick = ticks_ - 1 + songOffset_;
        status.tickInBeat =
            static_cast<std::uint32_t>(((songTick % ticksPerBeat) + ticksPerBeat) % ticksPerBeat);
    }
    status.ticks = static_cast<std::uint64_t>(ticks_);
    status.heldBack = fit_.heldBack();
    status.tempoChanges = fit_.tempoChanges();
    status.bunched = fit_.bunched();
    status.refused = refused_;
    status.ticksLost = ticksLost_;
    status.gapsUncounted = gapsUncounted_;
    status.dropped = dropped_.load(std::memory_order_relaxed);
    status_.publish(status);
}

} // namespace takt4::timing
