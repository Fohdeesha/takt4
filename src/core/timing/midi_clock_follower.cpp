#include "core/timing/midi_clock_follower.hpp"

#include <algorithm>

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
        reading.present = true;
        reading.bpm = 60e6 / (fit_.microsPerTick() * kTicksPerBeat);
        reading.beat = (fit_.indexAt(now) + static_cast<double>(songOffset_)) / kTicksPerBeat;
        reading.anchored = placed_;
        reading.placements = placements_;
        reading.placedAt = placedAt_;
    }
    publish(now);
    return reading;
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
        resumed_ = startedSinceTick_ && knownMicrosPerTick_ > 0.0;
        if (resumed_) {
            fit_.startAgainAt(knownMicrosPerTick_);
        } else {
            fit_.startAgain();
        }
        unbroken_ = 0;
    }
    startedSinceTick_ = false;
    fit_.add(ticks_, at);
    heardTick_ = true;
    lastTickAt_ = at;
    ++unbroken_;
    if (playing_) {
        lastSongTick_ = ticks_ + songOffset_;
    }
    ++ticks_;
}

void MidiClockFollower::placeNextTick(std::int64_t songTick) noexcept {
    // The next tick is number `ticks_`.
    songOffset_ = songTick - ticks_;
    lastSongTick_ = songTick - 1;
    placed_ = true;
    ++placements_;
    placedAt_ = static_cast<double>(songTick) / kTicksPerBeat;
}

void MidiClockFollower::rememberTempo() noexcept {
    if (followed()) {
        knownMicrosPerTick_ = fit_.microsPerTick();
    }
}

bool MidiClockFollower::followed() const noexcept {
    return unbroken_ >= (resumed_ ? 1U : kSettledTicks) && fit_.fitted();
}

double MidiClockFollower::absentAfter() const noexcept {
    const auto least = static_cast<double>(kAbsentMicros);
    return fit_.fitted() ? std::max(kAbsentTicks * fit_.microsPerTick(), least) : least;
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
    status.dropped = dropped_.load(std::memory_order_relaxed);
    status_.publish(status);
}

} // namespace takt4::timing
