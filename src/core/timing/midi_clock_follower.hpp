#pragma once

#include "core/midi/midi_input_hub.hpp"
#include "core/rt/published.hpp"
#include "core/rt/spsc_ring.hpp"
#include "core/timing/grid_source.hpp"
#include "core/timing/tick_fit.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace takt4::timing {

/// **Following a MIDI clock** — a DJ mixer's, a DAW's — as a `GridSource`.
///
/// A MIDI clock gives its tempo by spacing alone, 24 Timing Clock messages to the beat, and says
/// where the beats are only with Start (the next tick is the first of the song), Continue (the
/// song goes on from where it stopped) and Song Position (where it will go on from, in sixteenths).
/// So:
///
///   * **The tempo, and when each tick was**, come from a line through the moments the ticks
///     arrived (`TickFit`), each stamped by the hub as it came in — never from when this thread
///     got round to it.
///   * **The beat** is the ticks counted since the last Start, Continue or Song Position, over 24.
///     Until one of those, they are counted from the first tick heard, and their whole numbers are
///     nobody's beats (`GridReading::anchored` false): a DJ mixer's clock runs from power-on with
///     no Start, and the operator's DOWNBEAT gives the beats their phase.
///   * **Each Start, Continue and Song Position places bar 1** (`GridReading::placements`), over
///     any DOWNBEAT pressed before it: that is what they mean.
///   * **Stop** halts the beats at once when the operator has asked for that (`setObeyStartStop`),
///     until a Start or Continue. Otherwise it is let pass — which the MIDI specification does not
///     do and a DJ mixer needs: a DJM keeps its clock running after its Stop button, and a stray
///     press must not black out the rig.
///   * **Present** while ticks keep coming — one within the last `kAbsentTicks` tick gaps, or
///     `kAbsentMicros`, whichever is longer — once `kSettledTicks` have come without a break: two
///     beats to measure the tempo by. A clock that stops is coasted by `BeatGrid`.
///   * **Only at a tempo a clock could have**: `kMinBpm` to
///     `kMaxBpm`, and in a run's first beats within `kWidestStart` of the tempo it last ran at. A
///     delivery stall hands its ticks over in a bunch, microseconds apart: fitted, they were a
///     tempo of hundreds of thousands of BPM, and a beat went out every millisecond. Ticks
///     bunched as a run starts begin its count again (`TickFit::Options::leastGap`), and a
///     tempo is remembered only from a line through a beat of ticks.
///   * **Ticks lost in a gap are counted** when the clock comes back with no Start, Continue or
///     Song Position: the new line says when the first tick back would have been, and the ticks
///     between are added to the song position, so the beats stay on the music. A gap too long, or
///     across a tempo the two lines disagree on, to count with confidence leaves the clock
///     unplaced instead, for DOWNBEAT to put right.
///   * **A clock that starts again on a Start or Continue** — a DAW's, which ticks only while its
///     song plays — is present from its first tick, taken to be at the tempo it last ran at until
///     it has ticked enough to measure its own (`TickFit::startAgainAt`). So the song's first
///     downbeat goes out on the tick that begins it, not two beats on. A clock that comes back
///     with no Start, or with no tempo known from before, has two beats to show first, so a stray
///     tick never sets the rig going. The tempo is kept from one run to the next: a known tempo
///     that is wrong is only the line's slope for the first `TickFit::Options::firstTaken` ticks,
///     a quarter of a beat, and the beat the first tick begins is on that tick whatever it is.
///
/// The messages arrive on the driver's thread (`midi::MidiInputListener`) and are passed through a
/// lock-free ring to `ExternalBeats`' thread, which does everything else (`GridSource`). The port
/// is opened and closed by whoever owns this (`listen`, `stopListening`), on their own thread, so
/// a port that cannot be opened is said there.
class MidiClockFollower final : public GridSource, public midi::MidiInputListener {
public:
    /// Ticks without a break before the tempo is believed: two beats — or the first, when the
    /// clock starts again on a Start or Continue at a tempo known from before.
    static constexpr std::uint64_t kSettledTicks = 48;
    /// A clock is gone when no tick has come for this many of its tick gaps...
    static constexpr double kAbsentTicks = 4.0;
    /// ...or this long, whichever is longer.
    static constexpr std::int64_t kAbsentMicros = 100'000;
    /// The tempi a clock is followed at. The tempo window's own limits (`settings`), the MIDI
    /// clock's sanity range.
    static constexpr double kMinBpm = 20.0;
    static constexpr double kMaxBpm = 400.0;
    /// How far from the tempo the clock last ran at its first beats may be, either way.
    static constexpr double kWidestStart = 2.5;
    /// Before a run has a line there is no tick gap to measure by, and the slowest clock followed
    /// sets how long a gap starts a new run: half as long again as a tick at `kMinBpm`. At
    /// `kAbsentMicros`, every tick of a clock under 25 BPM began a run of its own, and it was never
    /// followed.
    static constexpr double kAbsentUnfittedMicros = 1.5 * 60e6 / (kMinBpm * 24.0);
    /// A gap longer than this many ticks — sixteen beats — is not counted across: over it a tempo a
    /// quarter of a percent out is a tick out, and nothing says the clock held its tempo while it
    /// was not heard.
    static constexpr double kLongestCountedGapTicks = 16.0 * 24.0;
    /// Messages the ring holds: a minute of ticks at 174 BPM, where the worker takes them every
    /// millisecond.
    static constexpr std::size_t kRingCapacity = 4096;

    /// What the clock is doing, for the window and the console. Published by the worker; any
    /// thread.
    struct Status {
        /// Ticks arriving.
        bool ticking = false;
        /// The tempo they say, once they are followed (`kSettledTicks`); zero before.
        double bpm = 0.0;
        /// Whether a Start, Continue or Song Position has said where the beats are. Until one has,
        /// the beats are wherever the ticks were first counted from, and DOWNBEAT is what puts
        /// them on the beat.
        bool placed = false;
        /// A Stop has come since the last Start or Continue.
        bool stopped = false;
        /// Where in its beat the last tick was, 0 to 23 — the song's own count once a Start,
        /// Continue or Song Position has placed it, the ticks' count from the first before.
        std::uint32_t tickInBeat = 0;
        /// Ticks this run, ticks held back as deliveries held up, and how many times the tempo
        /// was taken to have moved (`TickFit`).
        std::uint64_t ticks = 0;
        std::uint64_t heldBack = 0;
        std::uint64_t tempoChanges = 0;
        /// Ticks that came bunched as a run started (`TickFit::bunched`), and the readings refused
        /// for a tempo no clock has (`kMinBpm`, `kMaxBpm`, `kWidestStart`).
        std::uint64_t bunched = 0;
        std::uint64_t refused = 0;
        /// Ticks a gap was found to have lost, added to the song position; and gaps that could not
        /// be counted across, which left the clock unplaced.
        std::uint64_t ticksLost = 0;
        std::uint64_t gapsUncounted = 0;
        /// Messages lost because the ring was full.
        std::uint64_t dropped = 0;
    };

    /// Listens through `inputs`, and reads the present on its clock.
    explicit MidiClockFollower(midi::MidiInputHub& inputs);
    ~MidiClockFollower() override;

    /// Starts listening to the input `port` names (`midi::MidiInputHub::listen`), and returns its
    /// full name. Throws as that does, saying "MIDI clock: " first. The owner's thread.
    std::string listen(std::string_view port);
    /// Stops listening. The owner's thread; safe when not listening.
    void stopListening() noexcept;
    /// The full name of the input listened to, or empty.
    std::string port() const;

    /// Whether a Stop halts the beats. Any thread, any time: switched on while the clock is
    /// stopped, the beats halt at once.
    void setObeyStartStop(bool obey) noexcept { obey_.store(obey, std::memory_order_relaxed); }
    bool obeyStartStop() const noexcept { return obey_.load(std::memory_order_relaxed); }

    Status status() const noexcept { return status_.load(); }

    // --- GridSource: `ExternalBeats`' thread -------------------------------------------------
    void begin(std::uint32_t beatsPerBar) noexcept override;
    GridReading read() noexcept override;
    void end() noexcept override;

    // --- midi::MidiInputListener: the driver's thread ------------------------------------------
    void onMidi(std::span<const unsigned char> message, std::int64_t atMicros) noexcept override;
    bool wantsClock() const noexcept override { return true; }

private:
    /// A clock's message as it came: its status byte, Song Position's two data bytes, and when.
    struct Message {
        std::int64_t at = 0;
        unsigned char status = 0;
        unsigned char data1 = 0;
        unsigned char data2 = 0;
    };

    void handle(const Message& message) noexcept;
    void tick(std::int64_t at) noexcept;
    /// The next tick is song tick `songTick`: the bar is placed afresh.
    void placeNextTick(std::int64_t songTick) noexcept;
    /// Keeps the tempo of the run of ticks so far, if it was followed, to start the next at.
    void rememberTempo() noexcept;
    /// Whether this run of ticks is followed: enough of them without a break to go on.
    bool followed() const noexcept;
    /// Whether a reading at `bpm` may be followed — see the class.
    bool believable(double bpm) const noexcept;
    /// The ticks a gap lost, counted once the run after it is followed — see the class.
    void countGap() noexcept;
    /// How long without a tick before the clock is gone.
    double absentAfter() const noexcept;
    bool ticking(std::int64_t now) const noexcept;
    void publish(std::int64_t now) noexcept;

    midi::MidiInputHub& inputs_;
    std::atomic<bool> obey_{false};
    rt::SpscRing<Message, kRingCapacity> messages_;
    std::atomic<std::uint64_t> dropped_{0};
    rt::Published<Status> status_;

    // Everything below is the worker's alone, from `begin` to `end`.
    TickFit fit_;
    /// Ticks counted this run; the next tick's number. The fit's numbering.
    std::int64_t ticks_ = 0;
    bool heardTick_ = false;
    std::int64_t lastTickAt_ = 0;
    /// Ticks since the clock last came (back): what `kSettledTicks` counts.
    std::uint64_t unbroken_ = 0;
    /// Song tick = tick number + this.
    std::int64_t songOffset_ = 0;
    bool placed_ = false;
    std::uint32_t placements_ = 0;
    /// Where the last placement put the song, in beats (`GridReading::placedAt`).
    double placedAt_ = 0.0;
    /// The tempo the clock last ran at while followed, in microseconds a tick: what it is taken to
    /// start again at. Kept from run to run; zero until one has been followed.
    double knownMicrosPerTick_ = 0.0;
    /// A Start or Continue since the last tick: the next run of ticks is a song starting.
    bool startedSinceTick_ = false;
    /// This run of ticks began on one, at a tempo known from before: followed from its first tick.
    bool resumed_ = false;
    /// Between a Start or Continue and a Stop: the song position moves on with each tick.
    bool playing_ = false;
    /// A Stop since the last Start or Continue.
    bool stopSeen_ = false;
    /// The song tick of the last tick counted while playing: where a Continue goes on from.
    std::int64_t lastSongTick_ = -1;
    /// A Song Position that came while stopped, for the next Continue.
    bool songPositionPending_ = false;
    std::int64_t pendingSongTick_ = 0;
    /// A gap with no Start, Continue or Song Position after it, waiting to be counted across: when
    /// the last tick before it came, the first tick's number after it, and the tempo before it.
    bool gapPending_ = false;
    std::int64_t gapFrom_ = 0;
    std::int64_t gapIndex_ = 0;
    double gapMicrosPerTick_ = 0.0;
    std::uint64_t refused_ = 0;
    std::uint64_t ticksLost_ = 0;
    std::uint64_t gapsUncounted_ = 0;
};

} // namespace takt4::timing
