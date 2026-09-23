#pragma once

// What a control surface asked of HANDOFF §5.8's rules, with no output thread to ask.
//
// `control::RuleControl` is an interface precisely so that this can exist: the live
// implementation is `output::OutputRunner`, which owns a Link session, three sockets and a
// thread — none of which a test about an *address table* has any business building. Shared
// by tests/control/osc_control_test.cpp and tests/control/midi_control_test.cpp, because
// §5.7's two rule addresses arrive over both surfaces and the claim is the same either way.
//
// The two calls come from whichever thread the surface runs on, so this is written to be
// safe on one: RtMidi's callback and the OSC receiver's loop both reach it directly.

#include "core/control/rule_control.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace takt4::testing {

class RecordingRules final : public control::RuleControl {
public:
    void panic(bool engaged) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        panics_.push_back(engaged);
    }

    void setRuleEnabled(std::string_view id, bool enabled) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        enables_.emplace_back(std::string(id), enabled);
    }

    void setRuleMuted(std::string_view id, bool muted) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        mutes_.emplace_back(std::string(id), muted);
    }

    void setRuleRate(std::string_view id, double factor, bool relative) override {
        const std::lock_guard<std::mutex> lock(mutex_);
        rates_.push_back(Rate{std::string(id), factor, relative});
    }

    void fireManual() override {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++manuals_;
    }

    /// How many times `/ctl/manual` was asked for.
    int manuals() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return manuals_;
    }

    /// One `rule/<id>/{double,halve,rate,reset}` as it was asked for. `relative` is what
    /// separates the button spellings from the fader one, so it has to be recorded.
    struct Rate {
        std::string id;
        double factor = 1.0;
        bool relative = false;
    };

    /// Every `panic` in order, as the state it was asked for — §5.8's latch means `false` is
    /// a release and not a repeat, so the sequence matters and a count would not do.
    std::vector<bool> panics() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return panics_;
    }

    /// Every `rule/<id>/enable`, in order, as (id, state).
    std::vector<std::pair<std::string, bool>> enables() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return enables_;
    }

    /// Every `rule/<id>/mute`, in order, as (id, state).
    std::vector<std::pair<std::string, bool>> mutes() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return mutes_;
    }

    /// Every rate change, in order.
    std::vector<Rate> rates() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return rates_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<bool> panics_;
    std::vector<std::pair<std::string, bool>> enables_;
    std::vector<std::pair<std::string, bool>> mutes_;
    std::vector<Rate> rates_;
    int manuals_ = 0;
};

} // namespace takt4::testing
