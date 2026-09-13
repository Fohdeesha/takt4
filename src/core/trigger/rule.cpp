#include "core/trigger/rule.hpp"

#include "core/output/osc_message.hpp"

#include <algorithm>
#include <utility>

namespace takt4::trigger {

namespace {

/// MIDI note numbers, controller numbers, velocities and controller values are all seven
/// bits. A generator can produce anything, so what it produces is clamped rather than
/// wrapped: a note wrapped from 130 to 2 is a note nobody asked for, and the top of the
/// range is at least the note the operator was reaching towards.
constexpr int kMidiMax = 127;
constexpr int kMidiChannels = 16;

/// One splitmix64 round, to turn a rule's seed and the role of one of its generators into a
/// stream unrelated to its neighbours'. Adding the role to the seed would do nearly as well
/// — `Xoshiro256pp::reseed` runs splitmix64 over whatever it is given — but two rules whose
/// seeds are a few apart would then share streams between their different roles.
std::uint64_t mixSeed(std::uint64_t seed, std::uint64_t role) noexcept {
    std::uint64_t z = seed + role * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// Which generator of a rule is which, for `mixSeed`. Segments take the numbers from
/// `kSegmentRole` up, so a rule may have as many as an address can hold.
constexpr std::uint64_t kValueRole = 1;
constexpr std::uint64_t kNumberRole = 2;
constexpr std::uint64_t kProbabilityRole = 3;
constexpr std::uint64_t kSegmentRole = 16;

Generator::Config seeded(Generator::Config config, std::uint64_t seed, std::uint64_t role) {
    config.seed = mixSeed(seed, role);
    return config;
}

} // namespace

std::string_view labelOf(Trigger trigger) noexcept {
    switch (trigger) {
    case Trigger::Beat:
        return "beats";
    case Trigger::Bar:
        return "bars";
    case Trigger::Downbeat:
        return "downbeat";
    case Trigger::TempoChange:
        return "tempo change";
    case Trigger::LockChange:
        return "lock or unlock";
    case Trigger::IntensityChange:
        return "intensity change";
    case Trigger::Onset:
        return "onset";
    case Trigger::Manual:
        return "manual hotkey";
    case Trigger::Euclid:
        return "euclidean pattern";
    }
    return "";
}

std::string_view nameOf(Trigger trigger) noexcept {
    switch (trigger) {
    case Trigger::Beat:
        return "beat";
    case Trigger::Bar:
        return "bar";
    case Trigger::Downbeat:
        return "downbeat";
    case Trigger::TempoChange:
        return "tempo-change";
    case Trigger::LockChange:
        return "lock-change";
    case Trigger::IntensityChange:
        return "intensity-change";
    case Trigger::Onset:
        return "onset";
    case Trigger::Manual:
        return "manual";
    case Trigger::Euclid:
        return "euclid";
    }
    return "";
}

std::optional<Trigger> triggerOf(std::string_view name) noexcept {
    for (const Trigger trigger : kTriggers) {
        if (nameOf(trigger) == name) {
            return trigger;
        }
    }
    return std::nullopt;
}

bool takesEvery(Trigger trigger) noexcept {
    return trigger == Trigger::Beat || trigger == Trigger::Bar || trigger == Trigger::Euclid;
}

bool takesPulses(Trigger trigger) noexcept {
    return trigger == Trigger::Euclid;
}

std::string_view labelOf(DelayUnit unit) noexcept {
    switch (unit) {
    case DelayUnit::Milliseconds:
        return "ms";
    case DelayUnit::Beats:
        return "beats";
    case DelayUnit::Bars:
        return "bars";
    }
    return "";
}

std::string_view nameOf(DelayUnit unit) noexcept {
    switch (unit) {
    case DelayUnit::Milliseconds:
        return "ms";
    case DelayUnit::Beats:
        return "beats";
    case DelayUnit::Bars:
        return "bars";
    }
    return "";
}

std::optional<DelayUnit> delayUnitOf(std::string_view name) noexcept {
    for (const DelayUnit unit : kDelayUnits) {
        if (nameOf(unit) == name) {
            return unit;
        }
    }
    return std::nullopt;
}

bool euclidHit(std::uint32_t step, std::uint32_t pulses, std::uint32_t steps) noexcept {
    if (steps == 0 || pulses == 0) {
        return false; // no pattern, or a pattern of rests
    }
    if (pulses >= steps) {
        return true; // more hits than places to put them: every step
    }
    // The Bresenham construction: step *k* is a hit when `(k * pulses) mod steps` is below
    // `pulses`. That gives exactly `pulses` hits, spaced as evenly as whole steps allow — the
    // definition of a Euclidean rhythm — and, unlike the "count the hits due so far" form,
    // **it always puts a hit on step 0**. That is not a detail: a pattern is counted from the
    // downbeat, and one that started with a rest would be the same rhythm heard in the wrong
    // place.
    //
    // The rotation this produces is not always the one Toussaint's paper prints — E(3,8) is
    // the tresillo either way, E(5,8) comes out as a rotation of the cinquillo — and every
    // rotation of a Euclidean rhythm is a Euclidean rhythm. Starting on the beat is the
    // property worth fixing; which rotation of the remainder is a convention.
    //
    // 64-bit throughout: `step * pulses` overflows 32 bits at a pattern nobody would write,
    // and a wrong answer there would be a rhythm that quietly stopped making sense.
    return (static_cast<std::uint64_t>(step) * pulses) % steps < pulses;
}

std::string_view labelOf(Message::Kind kind) noexcept {
    switch (kind) {
    case Message::Kind::Osc:
        return "OSC address";
    case Message::Kind::MidiNote:
        return "MIDI note";
    case Message::Kind::MidiNoteOff:
        return "MIDI note off";
    case Message::Kind::MidiCc:
        return "MIDI CC";
    case Message::Kind::MidiProgramChange:
        return "MIDI program";
    case Message::Kind::MidiPitchBend:
        return "MIDI pitch bend";
    }
    return "";
}

std::string_view nameOf(Message::Kind kind) noexcept {
    switch (kind) {
    case Message::Kind::Osc:
        return "osc";
    case Message::Kind::MidiNote:
        return "midi-note";
    case Message::Kind::MidiNoteOff:
        return "midi-note-off";
    case Message::Kind::MidiCc:
        return "midi-cc";
    case Message::Kind::MidiProgramChange:
        return "midi-program";
    case Message::Kind::MidiPitchBend:
        return "midi-pitch-bend";
    }
    return "";
}

std::optional<Message::Kind> messageKindOf(std::string_view name) noexcept {
    for (const Message::Kind kind : kMessageKinds) {
        if (nameOf(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::size_t countPlaceholders(std::string_view address) noexcept {
    std::size_t count = 0;
    for (std::size_t at = 0; at < address.size();) {
        const std::size_t open = address.find('{', at);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t close = address.find('}', open + 1);
        if (close == std::string_view::npos) {
            break; // an unclosed brace is not a placeholder; see fillAddress
        }
        ++count;
        at = close + 1;
    }
    return count;
}

bool fillAddress(std::string_view address, const Value* values, std::size_t count,
                 std::string& out) {
    if (countPlaceholders(address) != count) {
        return false;
    }
    std::string built;
    built.reserve(address.size() + count * 4);
    std::size_t at = 0;
    std::size_t taken = 0;
    while (at < address.size()) {
        const std::size_t open = address.find('{', at);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t close = address.find('}', open + 1);
        if (close == std::string_view::npos) {
            break;
        }
        built.append(address, at, open - at);
        const std::size_t filled = built.size();
        values[taken++].appendTo(built);
        // A placeholder is one *segment*. A value holding a '/' would pass every OSC rule —
        // a slash is what separates segments, so `/deck/a/b` is a perfectly legal address —
        // and quietly send the message somewhere other than where the template says. That
        // is a change to the address's shape rather than its content, and it is exactly the
        // failure that arrives at a lighting desk looking like a takt4 bug.
        if (built.find('/', filled) != std::string::npos) {
            return false;
        }
        at = close + 1;
    }
    built.append(address, at, address.size() - at);
    // A template is not itself a legal address — '{' and '}' are reserved in OSC 1.0 — so
    // this catches an unbalanced brace for free: whatever was left of it survives into the
    // result and is rejected here. It also catches a generator that produced a '/' or a
    // space, which is the one failure no amount of validating the template can rule out.
    if (!output::addressIsLegal(built)) {
        return false;
    }
    out = std::move(built);
    return true;
}

Rule::Rule(Config config)
    : config_(std::move(config)), enabled_(config_.enabled),
      value_(seeded(config_.value, config_.seed, kValueRole)),
      number_(seeded(config_.number, config_.seed, kNumberRole)),
      probability_(mixSeed(config_.seed, kProbabilityRole)) {
    segments_.reserve(config_.segments.size());
    for (std::size_t i = 0; i < config_.segments.size(); ++i) {
        segments_.emplace_back(seeded(config_.segments[i], config_.seed, kSegmentRole + i));
    }
    segmentValues_.reserve(config_.segments.size());
    if (config_.every == 0) {
        config_.every = 1; // "every nothing" is "every one", not a rule that never fires
    }
    validate();
}

void Rule::validate() {
    problem_.clear();
    if (config_.id.empty()) {
        problem_ = "a rule needs an id";
        return;
    }
    // §5.7 addresses a rule as `/<prefix>/ctl/rule/<id>/enable`, so the id has to survive
    // being a segment of that: legal OSC characters, and no '/' of its own to split it.
    if (config_.id.find('/') != std::string::npos || !output::addressIsLegal("/" + config_.id)) {
        problem_ = "a rule id must be usable in an OSC address: " + config_.id;
        return;
    }
    if (config_.sendKind != Message::Kind::Osc) {
        if (config_.channel < 1 || config_.channel > kMidiChannels) {
            problem_ = "a MIDI channel is 1 to 16";
        }
        return;
    }
    if (config_.address.empty()) {
        problem_ = "an OSC rule needs an address";
        return;
    }
    const std::size_t placeholders = countPlaceholders(config_.address);
    if (placeholders != config_.segments.size()) {
        problem_ = "the address has " + std::to_string(placeholders) +
                   " templated segments and the rule has " +
                   std::to_string(config_.segments.size()) + " generators for them";
        return;
    }
    // Fill it once with zeroes. That proves the fixed part of the template is a legal
    // address and that its braces are balanced — everything about the address that does not
    // depend on what the generators will produce.
    std::vector<Value> zeroes(placeholders, Value::ofInt(0));
    std::string trial;
    if (!fillAddress(config_.address, zeroes.data(), zeroes.size(), trial)) {
        problem_ = "not a legal OSC address once filled in: " + config_.address;
    }
}

void Rule::reset() noexcept {
    for (Generator& generator : segments_) {
        generator.reset();
    }
    value_.reset();
    number_.reset();
    probability_.reseed(mixSeed(config_.seed, kProbabilityRole));
    lastBpmSeen_ = -1.0;
    lastLockedSeen_ = -1;
    lastIntensitySeen_ = -1;
    lastFired_ = -1.0;
    fires_ = 0;
}

bool Rule::seesChange(const Context& context) noexcept {
    switch (config_.trigger) {
    case Trigger::TempoChange: {
        if (!(context.bpm > 0.0)) {
            return false; // nothing tracked is not a tempo, and moving off it is not a change
        }
        if (!(lastBpmSeen_ > 0.0)) {
            lastBpmSeen_ = context.bpm;
            return false;
        }
        const double moved = std::abs(context.bpm - lastBpmSeen_);
        if (moved <= std::abs(config_.tempoChangeTolerance) * lastBpmSeen_) {
            return false;
        }
        lastBpmSeen_ = context.bpm;
        return true;
    }
    case Trigger::LockChange: {
        const int locked = context.locked ? 1 : 0;
        const bool changed = lastLockedSeen_ >= 0 && locked != lastLockedSeen_;
        lastLockedSeen_ = locked;
        return changed;
    }
    case Trigger::IntensityChange: {
        const auto intensity = static_cast<int>(context.intensity);
        const bool changed = lastIntensitySeen_ >= 0 && intensity != lastIntensitySeen_;
        lastIntensitySeen_ = intensity;
        return changed;
    }
    default:
        return false;
    }
}

bool Rule::conditionsHold(const Context& context) noexcept {
    const Conditions& only = config_.conditions;
    if (context.confidence < only.minConfidence) {
        return false;
    }
    if (!only.allows(context.intensity)) {
        return false;
    }
    if (context.bpm < only.minBpm || context.bpm > only.maxBpm) {
        return false;
    }
    if (lastFired_ >= 0.0 && context.now - lastFired_ < only.cooldownSeconds) {
        return false;
    }
    // Last, and only when it can exclude anything. See the header.
    if (only.probability < 1.0 && probability_.nextDouble() >= only.probability) {
        return false;
    }
    return true;
}

std::optional<Message> Rule::fire(const Context& context) {
    Message message;
    message.kind = config_.sendKind;
    // §5.6's rule subset, carried on the message rather than looked up later: a follow-up
    // is sent after the rule set may have been replaced, and it has to go where the press
    // went. `outputMask_` is set by whoever knows what the outputs are — see `outputMask`.
    message.outputs = outputMask_;
    // Rebuilt from scratch each fire, in the order `lastSlots` documents. Cleared first so a
    // fire that cannot build its address leaves nothing behind: the editor would otherwise
    // go on showing the values of the last fire that worked, beside a rule that is failing.
    lastSlots_.clear();
    if (config_.sendKind == Message::Kind::Osc) {
        segmentValues_.clear();
        for (Generator& generator : segments_) {
            segmentValues_.push_back(generator.next(context));
        }
        if (!fillAddress(config_.address, segmentValues_.data(), segmentValues_.size(), address_)) {
            return std::nullopt;
        }
        message.address = address_;
        message.hasArgument = config_.sendValue;
        lastSlots_.assign(segmentValues_.begin(), segmentValues_.end());
        if (config_.sendValue) {
            message.argument = value_.next(context);
            lastSlots_.push_back(message.argument);
        }
    } else {
        message.channel = std::clamp(config_.channel, 1, kMidiChannels);
        // **Only the fields the kind actually puts on the wire**, and in the order
        // `lastSlots` documents — which is the order §5.9's editor draws the chips and the
        // order `RulesController::slotConfig` indexes them by. A pitch bend has no number
        // and a program change no value, so neither has a chip; recording one anyway paired
        // every chip with the wrong generator. Measured: a bend of 12000 read as 9 on
        // screen, because slot 0 held the number generator nothing had sent.
        //
        // Not drawn either, for the same reason: a generator whose value never leaves should
        // not be spending a shuffle's bag on it.
        if (sendsNumber(config_.sendKind)) {
            // The clamped number, not what the generator offered: what went on the wire is
            // what the chip beside the generator should say, or a note pushed back into
            // range would read on screen as the note that was sent.
            message.number = std::clamp(number_.next(context).asInt(), 0, kMidiMax);
            lastSlots_.push_back(Value::ofInt(message.number));
        }
        if (sendsValue(config_.sendKind)) {
            // Pitch bend is 14-bit and everything else is a data byte, so the ceiling is the
            // kind's rather than a constant — clamping a bend to 127 would pin it hard left.
            message.value =
                std::clamp(value_.next(context).asInt(), 0, valueCeiling(config_.sendKind));
            lastSlots_.push_back(Value::ofInt(message.value));
        }
    }
    ++fires_;
    lastFired_ = context.now;
    return message;
}

void Rule::followUpsFor(const Message& fired,
                        std::vector<std::pair<std::size_t, Message>>& out) const {
    const std::size_t many = std::min(config_.followUps.size(), kMaxFollowUps);
    for (std::size_t i = 0; i < many; ++i) {
        const FollowUp& owed = config_.followUps[i];
        // A kind on the wrong side of the OSC/MIDI divide has nothing to inherit — no address
        // one way, no channel or note the other — so it is skipped rather than sent as
        // whatever the defaults happen to be. The editor does not offer one; a hand-edited
        // preset can still hold one, and silence beats a note on channel 1 nobody asked for.
        if (owed.kind && !followUpFits(*owed.kind, fired.kind)) {
            continue;
        }
        // The same address and the same MIDI target: §5.6's release is the press again with a
        // different value, which is what makes it a release rather than a second event. An
        // entry that names a kind changes that one field and keeps the rest, so "note on then
        // CC" still goes to the channel and the outputs the note went to.
        Message follow = fired;
        if (owed.kind) {
            follow.kind = *owed.kind;
        } else if (fired.kind == Message::Kind::MidiNote) {
            // **A note's release is a real Note Off**, not the same Note On with velocity
            // zero. The velocity-zero convention is widely understood and not universal: the
            // operator's laser controller holds its clip until `0x80` arrives, so the rig
            // never released. See the note on `Message::Kind`. The configured value rides
            // along as the *release* velocity, which is a real field of Note Off.
            follow.kind = Message::Kind::MidiNoteOff;
        }
        if (follow.kind == Message::Kind::Osc) {
            follow.argument = owed.value;
            follow.hasArgument = true;
        } else {
            // Only an explicit kind brings its own number. A release inherits the fired
            // message's, which is the whole point of one: the note to let go of is the note
            // that was drawn, and a shuffled rule draws a different one every time.
            if (owed.kind && sendsNumber(*owed.kind)) {
                follow.number = std::clamp(owed.number, 0, kMidiMax);
            }
            follow.value = std::clamp(owed.value.asInt(), 0, valueCeiling(follow.kind));
        }
        out.emplace_back(i, std::move(follow));
    }
}

double Rule::followUpDelay(const Context& context, std::size_t index) const noexcept {
    if (index >= config_.followUps.size()) {
        return 0.0;
    }
    const FollowUp& owed = config_.followUps[index];
    const double milliseconds = std::max(0.0, owed.delaySeconds);
    if (owed.unit == DelayUnit::Milliseconds) {
        return milliseconds;
    }
    if (!(context.bpm > 0.0)) {
        return milliseconds; // nothing tracked to count beats of; see the header
    }
    double beats = std::max(0.0, owed.delayBeats);
    if (owed.unit == DelayUnit::Bars) {
        // The meter the tracker is reporting, never four (§5.5). Before it has an opinion a
        // bar is one beat, which is short rather than wrong — the alternative is assuming a
        // meter and holding a clip for four beats of a waltz.
        beats *= static_cast<double>(std::max<std::uint32_t>(1, context.meter));
    }
    return beats * 60.0 / context.bpm;
}

} // namespace takt4::trigger
