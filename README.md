# takt4

![takt4's main window, tracking a track at 128 BPM and sending it to Link, two MIDI clocks and OSC, beside the rule editor with a Resolume clip rule open](docs/screenshot.png)

takt4 listens to the music coming out of your mixer, works out where the beat, the downbeat
and the tempo are, and drives the rest of your rig with it: **Ableton Link, MIDI clock, OSC,
MIDI notes and Art-Net lighting**, all at once. On top of that there's a rule engine for
firing things on the music, like clips, cues, notes and light looks.

No click track, no tapping along, nothing to line up beforehand. Point it at the sound and it
follows. It's made for playing out: VJ software (Resolume, TouchDesigner, MadMapper), lights,
lasers, media servers, DAWs and drum machines.

## Get it

- Grab `takt4.exe` from the **[latest release](https://github.com/Fohdeesha/takt4/releases/latest)**.
  It's one file with the model built in, so put it anywhere and run it. It keeps its settings
  in a `settings.json` next to itself, so two copies in two folders are two separate rigs.
- You need Windows 10 or 11 and the latest
  [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vc14/vc_redist.x64.exe).
  Install it even if you think you already have it: an older one lets takt4 start and then
  fall over.
- takt4 isn't code-signed, so Windows may say it "protected your PC". Click **More info**,
  then **Run anyway**.

## Quick start

1. Pick your interface under **audio in** and the inputs under **channels**. It listens to a
   stereo pair by default; tick **mono** for a feed on a single input. Hit **start**.
2. Give it a few bars. The tempo shows up, the beat circles start moving, and it says
   **locked** once it's sure.
3. Under **outputs**, tick **Link** to join an Ableton Link session, or use **+ add output**
   for OSC, MIDI, MIDI clock or Art-Net.
4. Click **add a rule** to start firing things on the beat.

If it's a beat or two out, press **downbeat** (or **D**) on the one. If it's running at half
or double speed, use **÷2** or **×2**, and tick **keep BPM in** to hold it inside a range for
good.

## What it does

- **Tracks the music.** A neural network reads the audio 50 times a second, and a filter
  turns that into beats, downbeats, tempo and meter, with a confidence you can see and gate
  on. The status bar spells out your setup's latency, and once the tempo locks takt4 predicts
  the beats, so you can send things early to make up for it.
- **Sends everything at once.** Link (tempo and phase), MIDI beat clock (a separate clock per
  device), a ready-made OSC feed, MIDI notes, CC, program change and pitch bend, and Art-Net
  DMX. Every output has its own on/off and its own delay.
- **Fires rules on the music.** For example: every 4 bars, but only when the confidence is
  high, send Resolume a random clip that doesn't repeat until they've all played.
- **Runs lights.** Patch your fixtures, then aim fades, flashes, strobes, colour palettes and
  moving-head paths at them from rules.
- **Stays hands-on.** Tap, downbeat, ÷2, ×2, **lock** (to stop a breakdown dropping the
  tempo), and a latency slider. None of it stops the tracker or drops the lock. Keys: **T**
  taps, **D** is downbeat, **M** fires your manual rules, **Esc** is PANIC.
- **Takes remote control.** Any of that from OSC, or from a MIDI controller with learn.
- **Survives a rough night.** If the interface gets unplugged, its driver hangs or something
  changes its sample rate, takt4 tells you, reopens it when it's back, and keeps Link and the
  clock going at the last tempo in the meantime. Your setup saves itself a few seconds after
  every change, and if takt4 ever crashes it writes a crash report and offers to restart.

## Rules

Open the rule editor with the button in the main window's **triggers** row. A rule has four
parts, and each one folds away:

- **A · when**: every N beats or bars, the downbeat, a tempo change, lock or unlock, an
  intensity change (breakdown vs drop), an onset (a hit), the **M** key, or a euclidean
  pattern (3 in 8, 5 in 16...). **÷2** and **×2** change how often it fires, live. The fast
  triggers get an "at most once every ... ms" limit.
- **B · only if** (off until you tick it): confidence over a level, a probability, intensity,
  and a BPM range.
- **C · send**: an OSC address, a MIDI note, note off, CC, program change or pitch bend, or a
  lighting effect, to whichever outputs you pick. Every `{slot}` in an OSC address gets its
  own value: **shuffle** (the default, which never repeats until everything has played),
  random, cycle, weighted, fixed, a live value (BPM, bar, confidence...) or a ramp across bars.
  The presets fill in Resolume, TouchDesigner and MadMapper addresses for you.
- **D · then send**: follow-ups, like the release of a Resolume clip or a note off, each
  delayed in milliseconds, beats or bars.

Click a rule's dot in the list to mute it. It keeps running, so it comes back in time. **test**
fires it once, **select a preset** adds a ready-made set (Resolume clips on 3 layers, tempo
and resync, and more), and the event log along the bottom shows everything that went out.
**PANIC** (or **Esc**) stops every rule and stays on until you press **release**.

Everything saves by itself, and **export** / **import** move your whole setup (tempo settings,
rules, outputs and lighting) to another machine.

## Delays

Nothing downstream is ready the instant a beat is heard: a media server is a frame or two
behind, and a robot that has to move is a lot more. There's one **latency** slider for the
whole rig and one delay per output, and they add up. **Positive is later, negative is
earlier.** Once the tempo is locked takt4 knows when the next beat is due, so a −300 ms output
really does get its cue 300 ms before the beat. Click any underlined number to type a value.

## OSC

Every OSC output gets this feed, whatever your rules do:

```
/takt4/bpm         float   the tempo
/takt4/beat        int     1, on every beat
/takt4/beat/bar    int     which beat of the bar, 1..N
/takt4/downbeat    int     1, on downbeats
/takt4/confidence  float   0 to 1
/takt4/locked      int     0 or 1
/takt4/meter       int     beats per bar
/takt4/resync      int     1, when the tracker has just found itself again
```

To control takt4, tick **listen** on the OSC row under **inputs** (port 7001, this machine
only unless you tick **allow other machines**):

```
/takt4/ctl/tap                      tap the tempo
/takt4/ctl/downbeat                 the one is now
/takt4/ctl/tempo/halve              ÷2
/takt4/ctl/tempo/double             ×2
/takt4/ctl/lock            <0|1>    pin the lock, or let it go
/takt4/ctl/panic                    PANIC
/takt4/ctl/panic/release            let go of PANIC
/takt4/ctl/manual                   fire the manual rules
/takt4/ctl/rule/<id>/enable <0|1>   switch a rule on or off
/takt4/ctl/rule/<id>/mute   <0|1>   mute or unmute a rule
/takt4/ctl/rule/<id>/double         fire half as often
/takt4/ctl/rule/<id>/halve          fire twice as often
/takt4/ctl/rule/<id>/rate   <f>     set the rate outright, for a fader
/takt4/ctl/rule/<id>/reset          back to the rate it was written with
```

`<id>` can be `all`. The switches always take a `0` or `1` rather than toggling, so a control
surface that misses a message can't end up backwards. For MIDI, pick your controller's port
and an action on the MIDI row, press **learn**, then hit the pad. A pad bound to lock pins it
only while held.

## Lights

Add an output of kind **Art-Net**, type your node's IP (port 6454 is filled in), and click
**patch lights**. Give each fixture a name, an optional group, a universe and its start
address, and pick the closest shape (dimmer, RGB, RGBW, dimmer + RGB, LED par, or an 8- or
16-bit moving head). **IDENTIFY** flashes a fixture so you can find it, **TEST** holds one
channel for three seconds, and a bar next to every channel shows what's being sent. A moving
head gets a movement window, so a random position can never point it at the audience.

Rules aim at fixtures or groups with an effect and a duration: level / fade, color, flash,
pulse, strobe, hue sweep, position, path (circle, figure-8, sweep, square), home or
blackout. Every effect ends when its duration does, so nothing gets stuck strobing. Colours
come from a palette you pick (the picker lights the actual lamps while you drag) or from
red, green and blue values of their own.

**PANIC** freezes the lights where they are, in case takt4 is one of several sources on the
rig. **stop** and quitting black them out.

## How well it tracks

Beat and downbeat F-measure (`mir_eval`, 70 ms), on the weights that ship:

| tested on | beat | downbeat |
|---|---|---|
| **23 electronic tracks**, 91 minutes, against Beat This! | **0.84** (0.92 on the 15 where two reference systems agree) | 0.66 |
| **GiantSteps**, 664 Beatport previews, tempo only | tempo within 4% on **86%** | |
| **Ballroom**, 698 clips, 6.1 hours | 0.95 | 0.93 |

The tempo readout is right on 20 of the 23 electronic tracks. Ballroom is part of what the
model was trained on, so that row is a sanity check. On the 70 clips it never saw, it scores
0.92 and 0.88. The details, including everything that was tried and didn't work, are in
[tests/data/tracking/](tests/data/tracking/refeval/README.md) and
[TRACKING-PROPOSAL.md](TRACKING-PROPOSAL.md).

The network is BeatNet+'s architecture, run with RTNeural. The shipped weights are BeatNet+'s
`generic` set fine-tuned on a 1,325-track library of breakbeat, electro, IDM and house at
85–140 BPM (drum and bass counted at half time), labelled by Beat This!, and trained alongside
Raveform, osu2beat and Ballroom so it doesn't forget what it already knew.

It counts in 4/4. For a set with waltzes in it, close takt4 and add `"meters": [3, 4]` to the
`"preset"` part of `settings.json`.

## Building it

You don't need to build anything to use takt4. If you want to:

- CMake 3.28+, Visual Studio 2022 or 2026 (C++20), and `git clone --recurse-submodules`.
- For the UI: rustup (the Rust version is pinned in `rust-toolchain.toml`), `curl` on `PATH`,
  and Python 3. The `windows-core` preset builds the engine, the console and the tests
  without any of those.
- Network access on the first configure, for the pinned, hash-checked dependencies.

```sh
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

The default test presets stay off your hardware and your network, since the machine may be a
live rig. `ctest --preset windows-msvc-all` runs everything, so close Resolume, Live and
anything else using your interface first. `windows-asan` builds and runs the same suite under
AddressSanitizer, and `linux-tsan` under ThreadSanitizer.

`takt4-cli` ships next to the app and runs the engine without the UI: listing devices,
tracking a file or a live input, dumping the network's output. Run it with no arguments for
the list.

## License

GPLv3, see [LICENSE](LICENSE). takt4 is built on PortAudio, Ableton Link, Slint, Skia,
RtMidi, RTNeural, Eigen, r8brain, KissFFT, nlohmann/json and the Steinberg ASIO SDK (under
its GPLv3 option), the Rust crates Slint is made of, and the Archivo, DM Mono and Chivo Mono
fonts. Every licence is in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt), which is also built into the app: click
**about**. BeatNet+'s weights, which the shipped model is fine-tuned from, come with no
licence stated upstream.

The tests use seventeen ten-second excerpts of commercial recordings. They're not part of
takt4 and never built into it; [tests/data/features/](tests/data/features/README.md) lists
them and says how a rights holder can have one removed.

*Art-Net™ Designed by and Copyright Artistic Licence. ASIO is a trademark and software of
Steinberg Media Technologies GmbH.*
