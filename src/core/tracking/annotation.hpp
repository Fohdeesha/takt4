#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace takt4::tracking {

/// One press of the operator's, on the track's own clock — TRACKING-PROPOSAL.md §3.4's
/// *annotate by tapping*, the ground truth the project otherwise lacks (§6).
struct Tap {
    double seconds = 0.0;
    /// The press that marks a downbeat: a beat, and the first of its bar.
    bool downbeat = false;
};

struct AnnotationOptions {
    /// How far a tap may be moved onto the nearest peak of the network's activation. The
    /// network's timing is sharper than a thumb's, and the peak is where every other set's
    /// labels sit (§7.8: Raveform's and Ballroom's at the model's peak to the frame), so
    /// a tap within reach of one is taken to have meant it. Zero leaves every tap where it
    /// landed.
    double snapWindowSeconds = 0.06;
    /// The least an activation peak has to reach to be snapped onto; below it the network
    /// is not hearing a beat there, and the tap stands on its own.
    double snapMinActivation = 0.10;
    /// Two presses closer together than this are one press: a bounce, or the downbeat key
    /// landing a hair after the beat key.
    double debounceSeconds = 0.08;
    /// The octave the file is written at: -1 keeps every other tap — the half-time grid
    /// the operator hears on drum and bass, recorded as the label (§7.10) — +1 puts a
    /// beat between every two taps, 0 neither.
    int octave = 0;
    /// The bar when fewer than two downbeats were tapped.
    int defaultMeter = 4;
    /// The activation's frame rate.
    double activationFps = 50.0;
};

struct AnnotationStats {
    std::size_t taps = 0;    ///< after debouncing, before the octave
    std::size_t snapped = 0; ///< taps that found a peak within the window
    /// Tap minus peak over the snapped taps: how late the operator's thumb runs. The
    /// taps that found no peak are moved back by this much, so a passage the network does
    /// not hear still gets the operator's grid at the network's timing.
    double medianOffsetSeconds = 0.0;
    /// The 10-90 % spread of the same: how steady the thumb was.
    double spreadSeconds = 0.0;
    /// Beats to the bar, as tapped between downbeats or as defaulted; 0 with no downbeat.
    int meter = 0;
    std::size_t downbeats = 0;
    double bpm = 0.0; ///< 60 over the median gap of the beats written
};

struct Annotation {
    std::vector<double> times;
    /// 1 on a downbeat and counting up to the meter between them; 0 throughout when no
    /// downbeat was tapped, which the Ballroom layout reads as "a beat, bar unknown".
    std::vector<int> beatInBar;
    AnnotationStats stats;
};

/// The taps into a beat annotation. `activation` is P(beat) + P(downbeat) per network
/// frame — what `takt4-cli beats` prints — or empty to leave the taps where they landed.
Annotation annotate(std::vector<Tap> taps, const std::vector<float>& activation,
                    const AnnotationOptions& options);

/// "<seconds> TAB <beat in bar>", the Ballroom layout that tools/evaluate.py, tools/train
/// and the reference harness all read.
void writeBeats(const std::filesystem::path& path, const Annotation& annotation);

/// The raw taps — "<seconds>" or "<seconds> TAB d" per line — so an annotation can be
/// redone with other settings. A Ballroom-layout file reads as taps too: its downbeats
/// are the rows marked 1.
void writeTaps(const std::filesystem::path& path, const std::vector<Tap>& taps);
std::vector<Tap> readTaps(const std::filesystem::path& path);

} // namespace takt4::tracking
