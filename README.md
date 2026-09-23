# takt4

takt4 listens to one channel of your audio interface, works out where the beat, the
downbeat, the tempo and the meter are, and tells the rest of the rig — over **Ableton
Link, OSC, MIDI beat clock and MIDI notes, and Art-Net DMX** — with a rule engine for
firing events on the music.

It is built for playing out: VJ software (Resolume, TouchDesigner, MadMapper), lighting
desks, media servers, lasers, DAWs. No click track, no tapping along, nothing to line up
beforehand — point it at the sound coming out of the mixer and it follows.

**[Download the latest release](https://github.com/Fohdeesha/takt4/releases/latest).**
`takt4.exe` is one file: the neural network and the state space are compiled in, so you
copy the executable anywhere and run it. It keeps a `settings.json` beside itself, which
makes two copies in two folders two rigs — a rehearsal setup and a show setup.

Your rig is saved on its own a few seconds after every change, and each save replaces the
file whole, so a crash or a power cut never leaves half of one. Each clean start also keeps
a copy as `settings.json.bak`. If the file is ever damaged, takt4 sets it aside as
`settings.json.corrupt-<date>`, starts from that copy and tells you so. If takt4 itself
falls over, it writes a crash report (`takt4-crash-<date>.dmp`) beside the executable and
offers to start again.

Windows today. The macOS and Linux presets are in the tree and the code is kept portable,
but Windows is the only platform currently built and tested.

## What it does

- **Follows the music.** A neural network reads the audio 50 times a second; an exact
  forward filter over madmom's bar-pointer state space turns that into beats, downbeats,
  a tempo and a meter — with a confidence you can see and gate on. It costs 40 ms, which
  is what a centred analysis window costs and nothing more.
- **Drives everything at once.** Ableton Link (tempo and phase, with the detected meter as
  the quantum), MIDI beat clock at 24 PPQN, a generic OSC namespace, MIDI notes, CC,
  program change and pitch bend, and Art-Net DMX to lighting nodes. Any number of named
  targets, each with its own enable and its own offset.
- **Runs the lights.** Patch your fixtures once — an RGB par, a moving head — and rules
  aim at them by name: fade, flash, pulse, strobe, a color or a hue sweep, a random
  pan/tilt or a circle, each over a duration you can spell in bars. The movement window is
  a safety limit, so a random position can never send a head into the audience.
- **Fires events on the music.** A rule is *when → only if → send*: every 4 bars, only
  above 0.6 confidence and only in a drop, send a Resolume clip drawn from a shuffle bag.
  Edited by clicking, not by typing JSON.
- **Stays hands-on while it plays.** Tap the tempo, snap the downbeat, ÷2, ×2, pin the
  lock so a breakdown cannot drop it, set the tempo range, trim the latency. None of it
  stops the tracker, reseeds anything or drops the lock. From the keyboard: **T** taps,
  **D** snaps the downbeat and **Esc** is PANIC (in a text box, Esc just leaves the box).
- **Rides out the rig failing.** If the interface stops sending — unplugged, power-cycled,
  reset by its driver, or moved to another sample rate by another program — the readout
  says **NO AUDIO** and takt4 reopens it as soon as it answers, while Link and the MIDI clock
  carry the last tempo on. It never changes an interface's sample rate: it opens at whatever
  the interface is already running at. **RESCAN** finds devices switched on after takt4
  started, and a MIDI device unplugged mid-set is picked up again when it comes back.
- **Takes orders from elsewhere.** An OSC control socket, and MIDI learn — press a pad on
  your controller and it is bound. Each rule can be enabled, muted or made to fire twice as
  often from a Stream Deck, mid-set.

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
- **send** — an OSC message; a MIDI note, note off, CC, program change or pitch bend; or a
  lighting effect aimed at your fixtures. OSC and MIDI go to whichever outputs you tick.
  Any number in it can be a generator: shuffle, random, round-robin, weighted, fixed, a
  live value (BPM, bar, confidence, meter, intensity), or a ramp that sweeps over a whole
  number of bars, locked to the downbeat. Then a sequence of follow-ups — a release, or
  something else entirely — each delayed in milliseconds, beats or bars from the moment the
  rule fired.

Two live controls sit beside each rule and are **not** saved with it, because they are
performance gestures rather than configuration: **mute**, which leaves the rule running and
stops it sending — so unmuting rejoins the music in phase instead of restarting its shuffle
bag — and **÷2 / ×2**, which makes it fire twice as often or half as often. Both are
reachable from OSC, so a Stream Deck can drop a layer out for eight bars.

Rig presets build a working setup in one pick: clips on three Resolume layers, Resolume's
tempo and resync, a breathing dashboard, Euclidean MIDI stabs. Everything a preset writes
is ordinary editable data, and a preset can be exported to another machine. **PANIC** — the
button, or **Esc** — stops every rule instantly and stays engaged, however many times it is
pressed, until you press **RELEASE** beside it.

## Taking orders

Switch **OSC in** on and takt4 listens — loopback only unless you tick the box that opens
it to the network. Every address hangs off the same prefix as the outputs:

```
/takt4/ctl/tap                      tap the tempo
/takt4/ctl/downbeat                 snap the downbeat to now
/takt4/ctl/tempo/halve              ÷2
/takt4/ctl/tempo/double             ×2
/takt4/ctl/lock            <0|1>    pin the lock, or release it
/takt4/ctl/panic           [0|1]    halt every rule; bare engages
/takt4/ctl/rule/<id>/enable <0|1>   arm a rule, or take it out of the show
/takt4/ctl/rule/<id>/mute   <0|1>   keep it running, stop it sending
/takt4/ctl/rule/<id>/double         fire half as often — press twice for a quarter
/takt4/ctl/rule/<id>/halve          fire twice as often
/takt4/ctl/rule/<id>/rate   <f>     set the multiplier outright, for a fader
/takt4/ctl/rule/<id>/reset          back to the rate the rule was written with
```

`<id>` is the rule's id, and **`all`** means every rule at once. `lock`, `enable` and
`mute` insist on their `<0|1>` rather than toggling, and `rate` insists on its number: a
toggle depends on a state the sender cannot see, so a surface that missed one message would
be inverted for the rest of the set. The rest are buttons and are sent bare.

Every one of these is also bindable to a MIDI note or CC through **LEARN** — except the
ones that name a rule, because pressing a pad says which button and never which rule.

## Lights

takt4 speaks **Art-Net** (the DMX-over-Ethernet protocol, *Art-Net™ Designed by and
Copyright Artistic Licence*). Add an output of kind **Art-Net**, type your node's IP —
port 6454 is filled in for you — and open **fixtures**.

DMX is not like the other outputs, and it is worth knowing why before you build a rule.
OSC and MIDI are *events*: a rule fires, one message leaves, nothing is owed afterwards. A
DMX universe is *state* — 512 levels that a controller re-sends continuously — so a fade is
not a message, it is takt4 sending a slightly different frame forty times a second until it
arrives. takt4 does that for you; what it means in practice is that a rule says **what the
lights should become and over how long**, not what to transmit.

### The patch

A fixture has a name, an optional **group**, a universe, the start address printed on the
back of it, and a channel map saying what each of its channels does. Pick the nearest of
the ready-made shapes — dimmer, RGB, RGBW, dimmer + RGB, LED par, and an 8-bit or 16-bit
moving head — and edit from there. Those come with sensible parked levels, which matters
more than it sounds: a moving head with its shutter channel at zero emits nothing however
hard a rule drives its dimmer. **IDENTIFY** flashes one fixture so you can find it in the
truss; **TEST** beside a channel holds that one channel at a value of your choice for three
seconds and then puts it back, which is how you check the map is right without unplugging
anything; and a bar beside every channel shows what takt4 is sending on it right now.

A moving head also gets a **movement window** — how much of its pan and tilt travel a rule
may use, in percent. Set it once from the stage. Every random position and every path is a
fraction of *that* window, so one rule means the same gesture on six differently-rigged
heads and none of them can be sent into the audience.

### The effects

A rule aims at fixtures or groups by name, picks one effect, and gives it a duration in
milliseconds, beats or bars:

| effect | what it does |
|---|---|
| **level** | one channel to a level. With a duration it is a fade, with none a snap — "fade in" and "fade out" are this, at full and at zero. A level aimed at **dimmer** on a par that has no dimmer channel scales its color instead, which is what brightness *is* on an LED par |
| **color** | the color channels to one color. On an RGBW fixture a neutral white uses the white LED |
| **flash** | straight to a peak and decay back over the duration — the beat hit |
| **pulse** | a cosine between two levels, N times over the duration |
| **strobe** | on and off between two levels, N times over the duration, with a duty cycle |
| **hue sweep** | round the color wheel from one angle to another |
| **position** | pan and tilt to one place, over a move time. Random is a `random` generator on pan |
| **path** | a circle, figure-8, sweep or square around the middle of the movement window |
| **home** | back to the middle of the window |
| **blackout** | every light-emitting channel to zero |

Every effect is **bounded by its duration**, deliberately: a strobe that ran until
something stopped it is a fixture left strobing because the rule that would have stopped it
was disabled, edited, or never fired. A strobe for two bars, re-fired every two bars, is
both the natural gesture and the one that cannot get stuck.

The level, the color and the pan/tilt are ordinary generators, so everything the clip
triggers can do they can do too, and a slow sweep across the room is a ramp on pan over four
bars.

A color effect asks where its color comes from, and there are two answers:

- **pick colors** — a row of swatches under the rule. Click one to open a hue / saturation /
  brightness picker, `+` adds another, and the chip above them says how they are drawn:
  *shuffle* for a palette that never repeats, *cycle* to walk them in order, *fixed* for one
  color. The picker sends the color to that rule's fixtures **as you drag it**, so you are
  choosing against the light coming out of the lamp rather than against a square on a screen.
- **mix red, green, blue** — one generator per component, 0 to 255 each. *Random* over
  0–255, 0–40 and 200–255 is "a random color, keep the green out of it and the blue up".
  Every other generator works here too: a ramp on red over four bars, or a green that
  follows the intensity.

Fade in and fade out is one rule: a level to full over a beat, and a follow-up two bars
later that fades to zero over a beat. The *delay* is when the follow-up starts; the
*duration* is how long it takes.

**PANIC** halts every rule and cancels every running effect, and then **keeps sending the
last frame**. The lights freeze rather than going dark — if takt4 is one source among
several, or your node is merging it with a desk, a panic button that drove everything to
zero would black out a stage that was not takt4's to black out. A deliberate blackout is an
effect a rule can fire.

### Notes on the wire

Art-Net 4 requires **unicast**, so takt4 sends to the address you typed and does not
discover nodes: a show rig has a fixed address written on the back of it, and broadcasting
ArtPoll twenty times a minute onto a venue's network is worse manners than asking once. A
broadcast address works if your rig is built that way. Frames are paced at the
specification's ceiling of 44 Hz per universe and re-sent every 900 ms when nothing is
moving, which is what tells a node takt4 is still alive. One Art-Net target carries every
universe your patch uses unless you list which ones it should carry.

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

There is a second set of presets that builds the same tree with the runtime checks turned
on. `windows-asan` is AddressSanitizer, which is all Windows has; `linux-asan` adds the
undefined-behaviour checks and `linux-tsan` looks for races between the audio, output and
UI threads. The suite is meant to pass clean under each of them, and it reports when the
tests *run*, so the build alone proves nothing:

```sh
cmake --preset windows-asan
cmake --build --preset windows-asan
ctest --preset windows-asan
```

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
