# takt4

takt4 listens to one channel of your audio interface, works out where the beat, the
downbeat, the tempo and the meter are, and tells the rest of the rig — over **Ableton
Link, OSC, MIDI beat clock and MIDI notes** — with a rule engine for firing events on the
music.

It is built for playing out: VJ software (Resolume, TouchDesigner, MadMapper), lighting
desks, media servers, lasers, DAWs. No click track, no tapping along, nothing to line up
beforehand — point it at the sound coming out of the mixer and it follows.

**[Download the latest release](https://github.com/Fohdeesha/takt4/releases/latest).**
`takt4.exe` is one file: the neural network and the state space are compiled in, so you
copy the executable anywhere and run it. It keeps a `settings.json` beside itself, which
makes two copies in two folders two rigs — a rehearsal setup and a show setup.

Windows today. The macOS and Linux presets are in the tree and the code is kept portable,
but Windows is the only platform currently built and tested.

## What it does

- **Follows the music.** A neural network reads the audio 50 times a second; an exact
  forward filter over madmom's bar-pointer state space turns that into beats, downbeats,
  a tempo and a meter — with a confidence you can see and gate on. It costs 40 ms, which
  is what a centred analysis window costs and nothing more.
- **Drives everything at once.** Ableton Link (tempo and phase, with the detected meter as
  the quantum), MIDI beat clock at 24 PPQN, a generic OSC namespace, and MIDI notes, CC,
  program change and pitch bend. Any number of named targets, each with its own enable and
  its own offset.
- **Fires events on the music.** A rule is *when → only if → send*: every 4 bars, only
  above 0.6 confidence and only in a drop, send a Resolume clip drawn from a shuffle bag.
  Edited by clicking, not by typing JSON.
- **Stays hands-on while it plays.** Tap the tempo, snap the downbeat, ÷2, ×2, pin the
  lock so a breakdown cannot drop it, set the tempo range, trim the latency. None of it
  stops the tracker, reseeds anything or drops the lock.
- **Takes orders from elsewhere.** An OSC control socket, and MIDI learn — press a pad on
  your controller and it is bound.

## How well it tracks

Beat and downbeat F-measure, scored with `mir_eval` at its 70 ms tolerance, on the weights
that ship:

| measured on | beat F | downbeat F |
|---|---|---|
| **23 electronic tracks**, 91 minutes — the material the app is for, against Beat This! | **0.84**, and **0.92** over the fifteen of them where two independent reference systems agree with each other | 0.66 |
| **Ballroom** — 698 clips, 6.1 hours, with bars of three and four | **0.95** | 0.93 |
| **GiantSteps** — 664 Beatport previews, tempo annotations only | tempo right within 4 % on **86 %** of them | — |

Read honestly: Ballroom is a set the underlying model was **trained** on, so that row says
the engine reproduces what the model can do over six hours of real audio and nothing about
how it generalises. The other two were held out, and they are the rows that mean
something — though there is no human ground truth for the 23, so on the eight where the
reference systems disagree with each other a score is agreement with a convention. Every
number, including what was tried and rejected, is in
[tests/data/tracking/refeval/](tests/data/tracking/refeval/README.md) and
[tests/data/tracking/evaluation/](tests/data/tracking/evaluation/README.md).

Underneath, the engine is held to the reference implementations it was ported from: the
feature front end matches madmom to within one float32 ulp and the network matches PyTorch
to 3.6e-6, on eighteen excerpts that ship with the test suite.

## The model, and what it was trained on

The network is BeatNet+'s architecture — a convolution block, a dense layer, four stacked
LSTMs and a softmax over beat / downbeat / non-beat — run through RTNeural on a worker
thread, well inside the 20 ms of audio each step stands for. The weights that ship,
`electronic`, are BeatNet+'s published `generic` set **fine-tuned on
the operator's own library** — 1,325 tracks of breakbeat, electro, IDM and house at 85–140,
with drum and bass labelled at half time because that is how it is counted. The labels come
from an ensemble of Beat This!'s three seeds, with the tracks its own seeds disagree about
left out, trained at triple weight beside Raveform, osu2beat2025 and Ballroom so the model
does not forget what it already knew. Against the stock weights on the same decoder: the
eight hardest tracks in the harness went **0.62 → 0.69** beat F, GiantSteps tempo **0.77 →
0.86**, and the tempo readout is right on 19 of the 23 tracks where it was right on 16.

BeatNet+'s three published sets are still there — `--weights generic`, `generic-main` for
percussion-heavy material, `af-non-percussive` for ambient and classical. The pipeline
that built the fine-tune is `tools/train/`, and
[TRACKING-PROPOSAL.md](TRACKING-PROPOSAL.md) is the full record of what was measured,
including everything that was tried and rejected.

## Outputs

Each output has a name, a kind, a destination, an enable and an offset, and rules can be
routed to any subset of them. Link and MIDI clock are their own rows.

The generic OSC namespace goes to every OSC target, whatever else is configured:

```
/takt4/bpm         float   the published tempo
/takt4/beat        int     1, on every beat
/takt4/beat/bar    int     which beat of the bar it was, 1..N
/takt4/downbeat    int     1, on downbeats only
/takt4/confidence  float   0 to 1
/takt4/locked      int     0 or 1
/takt4/meter       int     the detected beats per bar
/takt4/resync      int     1, when the tracker has just re-found itself
```

The state addresses repeat on every beat, so anything that starts late is right again
within a beat. `/takt4` is a default you can change, so two instances on one network can
be told apart.

## Rules

Open **triggers**. A rule is three columns:

- **when** — every beat, every N beats, every bar, every N bars, on the downbeat, on a
  tempo change, on lock or unlock, when the intensity changes (takt4 tells a breakdown
  from a drop out of the audio it is already analysing), on a manual hotkey, or on a
  Euclidean pattern: 3-in-8 is the tresillo, 5-in-16 the bossa, locked to the tracker's
  own beat rather than a clock of its own.
- **only if** — confidence above a threshold, intensity in a set, BPM in a range, a
  probability, a cooldown.
- **send** — an OSC message or a MIDI note, note off, CC, program change or pitch bend,
  to whichever outputs you tick. Any number in it can be a generator: shuffle, random,
  round-robin, weighted, fixed, a live value (BPM, bar, confidence, meter, intensity), or
  a ramp that sweeps over a whole number of bars, locked to the downbeat. Then a
  sequence of follow-ups — a release, or something else entirely — each delayed in
  milliseconds, beats or bars from the moment the rule fired.

Rig presets build a working setup in one pick: clips on three Resolume layers, Resolume's
tempo and resync, a breathing dashboard, Euclidean MIDI stabs. Everything a preset writes
is ordinary editable data, and a preset can be exported to another machine. **PANIC** stops
every rule instantly.

## Offsets

Nothing downstream is ready at the instant a beat is detected: a media server is a frame
or two behind, a robot that has to physically move is far more. Two sliders move things
around the beat and they add up — one **latency** for the whole rig, and one **per
output**, because the lag belongs to the thing on the end of each cable and one number
cannot describe a rig with a media server and a laser on it.

Both are signed, and **positive is later, negative is earlier**. Link and MIDI clock carry
a running grid, so a negative offset really does shift them earlier; an OSC message is one
datagram about a beat that has already happened and cannot be sent into the past, so
"earlier" there is measured from the **next** beat — the message is held and arrives that
far ahead of the beat it lands on, which downstream cannot tell apart.

**What you type is milliseconds and has nothing to do with the tempo**: measure your
device's lag once and it stays right as the music changes. What varies is the wait — a
device told −300 ms is held 352 ms at 92 BPM and 169 ms at 128, landing on a beat both
times — and each row also shows the offset as a fraction of the beat now playing, for when
you would rather think in beats.

## Building

You do not need to build anything to use takt4 — the release is one file. To build it
anyway:

- CMake 3.28+, a C++20 compiler (Visual Studio 2022 or 2026), and `git clone
  --recurse-submodules` (PortAudio and Ableton Link are submodules).
- A Rust toolchain 1.92+ and `curl` on `PATH`, for the Slint UI. `TAKT4_BUILD_UI=OFF` —
  the `windows-core` preset — builds the engine, the console and the tests without either.
- Network access on the first configure: the remaining dependencies are fetched from
  pinned, hash-checked archives.

```sh
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

The executable lands in `build/windows-msvc/bin/Release/`. `takt4 --version` prints what
it was built with.

## Development console

`takt4-cli` ships beside the app and exercises the engine without the UI — listing
devices, metering an input, dumping the network's activations, running the whole chain
over a file or a live input, and tapping a track's beats out by hand. `takt4-cli` with no
arguments lists everything it takes.

## License

GPLv3 — see [LICENSE](LICENSE).

| Component | Used for | License |
|---|---|---|
| [PortAudio](https://github.com/PortAudio/portaudio) | Audio input: ASIO, WASAPI, CoreAudio, ALSA, JACK | MIT |
| [r8brain-free-src](https://github.com/avaneev/r8brain-free-src) | Resampling the input | MIT |
| [KissFFT](https://github.com/mborgerding/kissfft) | The STFT behind the feature front end | BSD-3-Clause |
| [Ableton Link](https://github.com/Ableton/link) | Tempo sync | GPLv2 or later |
| [asio](https://github.com/chriskohlhoff/asio) (Kohlhoff, bundled by Link) | Networking for Link | Boost Software License |
| [Steinberg ASIO SDK](https://www.steinberg.net/asiosdk) | ASIO host API on Windows | GPLv3 (dual-licensed; see [third_party/README.md](third_party/README.md)) |
| [RTNeural](https://github.com/jatinchowdhury18/RTNeural) | Neural inference | BSD-3-Clause |
| [BeatNet+](https://github.com/mjhydri/BeatNet-Plus) weights | The model the shipped set is fine-tuned from | **None stated upstream** |
| [Eigen](https://eigen.tuxfamily.org) (bundled by RTNeural) | RTNeural's math backend | MPL-2.0 |
| [RtMidi](https://github.com/thestk/rtmidi) | MIDI clock and notes | MIT-style |
| [madmom](https://github.com/CPJKU/madmom) | Build-time only, never linked or shipped: `tools/` runs it to compute the filterbank table and the state space, and the golden features the C++ is checked against | 2-clause BSD for its source, which is all that is used. Its pretrained data and models are CC BY-NC-SA 4.0 and none of them is loaded here — every table is computed from configuration |
| [nlohmann/json](https://github.com/nlohmann/json) | Settings and presets | MIT |
| [Slint](https://slint.dev) | User interface | GPLv3 (triple-licensed) |
| [Skia](https://skia.org) (prebuilt by [rust-skia](https://github.com/rust-skia/rust-skia)) | UI rendering | BSD-3-Clause, plus the permissive licenses of what its archive bundles |
| [Catch2](https://github.com/catchorg/Catch2) | Tests only, not shipped | Boost Software License |
