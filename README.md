# takt4

![takt4's main window, tracking a track at 128 BPM and sending it to Link, two MIDI clocks and OSC, beside the rule editor with a Resolume clip rule open](docs/screenshot.png)

takt4 listens to incoming audio (WASAPI, ASIO, etc), finds the beat, downbeat and tempo, and sends them to the rest of
your rig: **Ableton Link, OSC, MIDI clock, MIDI notes and Art-Net (DMX)**. Plus optional built in triggers to fire clips,
cues, notes, lights, lasers etc exactly on time with whatever audio you feed it. No manual tapping, locks on to new songs automatically

## Get it

- Download `takt4.exe` from the **[latest release](https://github.com/Fohdeesha/takt4/releases/latest)**
- Windows 10 or 11, plus the latest [Visual C++ Redistributable (x64)](https://aka.ms/vc14/vc_redist.x64.exe)
- Not code-signed: if Windows "protected your PC", click **More info**, then **Run anyway**

## Quick start

1. Pick your device and input channels at the top (a stereo pair; tick **mono** if you have one input). Click **start**
2. Give it a few bars. It says **locked** when it's sure, and nothing goes out to the rig until then
3. Under **outputs**, tick **Link**, or click **add output** and pick its **protocol**: OSC, MIDI, MIDI clock or Art-Net (DMX)
4. In the **triggers** row, click **add a rule**. Pick a ready-made set from **select a preset**, or build your own.

## If the beat is wrong (rare)

- BPM correct, but start of the count/downbeat isn't: press **downbeat** (or **D**) on the one
- Half or double speed: **÷2** or **×2**. The button stays lit and the tempo says **÷2 applied** while it's in effect
- If the number and the beat dots disagree, the line under the tempo says what the beats are going out at and which of **÷2** / **×2** matches them
- Keep it in a range: tick **keep BPM in** and set a rough range
- **tap** (or **T**) to tap it in
- **latency** moves everything earlier or later to compensate for input, output, and overall rig latency

## Triggering stuff from the tracked audio (Rules)

If you just need a solid BPM feed for an Ableton Link session, DJ equipment, or similar, you're done. However takt4 has a powerful built in triggering system if you want to drive gear from your music feed (lighting, lasers, video, samplers, etc).
Enter the rule editor (the **triggers** window). **+** adds a rule. Each rule has four sections:

- **A when**: every N beats or bars, the downbeat, an onset, a tempo change,an intensity change, etc
  - Bars fire **on beat** 1 to 16 of the bar
  - Tick **then wait** to send it so many ms, beats or bars after the trigger
- **B only if**: confidence, probability, intensity, BPM range, other filters 
- **C send**: an OSC address, a MIDI note, note off, CC, program change or pitch bend, or DMX values. **send to** picks the outputs; DMX picks fixtures under **lights** instead
- **D then send**: follow-ups (a clip release, a note off, etc) after so many ms, beats or bars

Each `{slot}` in an OSC address gets a value: shuffle (no repeats until all have played), random, cycle, weighted, fixed, a live value (BPM, bar, confidence...) or a ramp. The **preset** box fills in Resolume, TouchDesigner and MadMapper addresses

- Click a rule's dot to mute it
- **test** fires it once
- The **event log** along the bottom shows everything that goes out over the wire
- **panic** (or **Esc**, in the main window or the rule editor) stops every rule until **release** is pressed

## Delays

- One **latency** slider for the rig, plus a delay per output. They add together
- Positive is later, negative is earlier. Once locked, a −300 ms output really gets its cue 300 ms early
- Your settings here will almost always be negative: audio input, detection, and your destination apps all add latency that needs to be removed
- Click any underlined number to type a value instead of using the slider
- Right-clicking any slider puts it back to default

## OSC

takt4 sends the below global data to all OSC outputs you tick under **/takt4 global messages to**, at the right of the **outputs** heading. None get it until you do. These are separate from triggers/rules.

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

You can *control* takt4 over OSC as well, tick **listen** on the OSC row under **inputs**:

```
address                       send           does
/takt4/ctl/tap                nothing or 1   tap the tempo
/takt4/ctl/downbeat           nothing or 1   the one is now
/takt4/ctl/tempo/halve        nothing or 1   ÷2
/takt4/ctl/tempo/double       nothing or 1   ×2
/takt4/ctl/lock               1 or 0         pin the lock (1), or let it go (0)
/takt4/ctl/panic              anything       panic
/takt4/ctl/panic/release      nothing or 1   release
/takt4/ctl/manual             nothing or 1   fire the manual rules
/takt4/ctl/rule/<id>/enable   1 or 0         switch a rule on (1) or off (0)
/takt4/ctl/rule/<id>/mute     1 or 0         mute (1) or unmute (0) a rule
/takt4/ctl/rule/<id>/double   nothing or 1   fire half as often
/takt4/ctl/rule/<id>/halve    nothing or 1   fire twice as often
/takt4/ctl/rule/<id>/rate     a number       multiply its count, 0.0625 to 64 (2 = half as often), for a fader
/takt4/ctl/rule/<id>/reset    nothing or 1   back to the count it was written with
```

- Send the value as the first argument: an OSC **int** (`i`), **float** (`f`) or **boolean** (`T` is 1, `F` is 0). A string counts as nothing; any other type and the message is ignored
- **nothing or 1**: a button. Any number but 0 presses it too; a 0 is its release and does nothing, so a push button that sends 1 then 0 presses once. **panic** engages on anything, 0 included
- **1 or 0**: a switch, never a toggle, so a missed message can't leave one backwards. Anything but 0 is on; with no value it's ignored
- `<id>` is the rule's id, shown beside its name in the rule editor, or `all`

**MIDI control**: on the MIDI row under **inputs**, pick your controller and an action, click **learn**, hit the midi button you want to use. Available actions: tap, downbeat, halve, double, lock, panic, release, fire the manual rules. A pad bound to lock pins it only while held

## Lights / DMX

1. **add output**, set its **protocol** to Art-Net and type the node's IP
2. Click **patch lights** in the triggers row, then **+** to add a fixture: name, group (optional), universe, **start address** (the one on the fixture), and the nearest **mode** (dimmer, RGB, RGBW, dimmer + RGB, LED par, 8- or 16-bit moving head, Liberation zone). Or click **import** and pick a GDTF (`.gdtf`) or Open Fixture Library (`.json`) file: choose its mode, how many and where, and they arrive with every channel named and set up
3. **identify** flashes the fixture; a channel's **test** holds it at the **test sends** level for 3 s
4. In a rule, set **send as** to DMX / Art-Net and pick an effect: level / fade, color, flash, pulse, strobe, hue sweep, position, path, home, blackout, Liberation clip

- Every effect ends with its duration, so nothing is left strobing etc
- Colors come from a palette (the picker lights the real lamps as you drag)
- A moving head's **how far it moves** keeps random positions and paths inside the range you set
- A fixture with several heads moves them all. In a rule, tick which **heads** move, and slide **spread** to stagger them; a second rule can move the others
- **panic** freezes the lights where they are; **stop** and quitting black them out

## Lasers ([Liberation](https://liberationlaser.com/))

1. Open the rules (**add a rule** or **edit rules** in the triggers row), then **select a preset** > **Liberation: lasers…**
2. Fill in the prompt: how many **lasers** (1 to 4), **Liberation's IP** (127.0.0.1 on this computer) and **port** (6454 unless you changed it in Liberation), where the **first zone** is in Liberation's universe numbering, and each laser's **clips from** and **to** as Liberation names them (`21-1`). Every clip between the two, in Liberation's deck order
3. **add** patches a Liberation zone per laser, adds an Art-Net output to Liberation, switches Link on and adds a trigger per laser: every beat, 2 beats, bar and 2 bars to start. Tick **also move each laser** for a circle, figure 8, sweep or random spots, as far as you set from the center
4. In Liberation, do what the prompt lists: DMX Input on with Art-Net, one Extended 32ch profile per laser at the addresses it shows, tempo source set to Ableton Link

- A clip arms its zone and sets its intensity. **stop**, quitting, **panic** and **no signal** (the input silent for 4 s) disarm every laser. So does muting or switching off its rule. Unticking or deleting an Art-Net output sends it zeros for 3 s, so its lights go dark and its lasers disarm
- Nothing fires until takt4 has a lock

## Saving

- Everything saves by itself to `settings.json` next to `takt4.exe`
- **export** / **import** carry everything: rules, outputs, lights, tempo settings, the audio input, MIDI and OSC control with what was learned, and which sections are folded. Import replaces it all. An input that isn't on this machine stays the one saved; **rescan** or the next launch finds it once it's plugged in
- A rule's mute and its ÷2 / ×2 status aren't saved

## How well it tracks

Multiple training sets, bolstered on off-kilter electronic music that trackers usually die on. Beat and downbeat F-measure (`mir_eval`, 70 ms) with the weights that ship:

| tested on | beat | downbeat |
|---|---|---|
| 23 handpicked difficult electronic tracks vs Beat This! | 0.84 (0.92 where two references agree) | 0.66 |
| GiantSteps, 664 Beatport previews | tempo within 4% on 86% | |
| Ballroom, 698 clips, counted in 3 and 4 | 0.95 | 0.93 |

## Building it from source

You don't need to, but if you want to:

- CMake 3.28+, Visual Studio 2022 (C++20), `git clone --recurse-submodules`
- For the UI: rustup (version pinned in `rust-toolchain.toml`), `curl` on `PATH`, Python 3. The `windows-core` preset builds the engine, `takt4-cli` and the tests without them
- Network access on the first build, for the pinned, hash-checked dependencies

```sh
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

- The default tests stay off your audio, MIDI and network. `windows-msvc-all` runs everything: close anything using your interface first
- `windows-asan` runs the suite under AddressSanitizer; `linux-tsan` (Linux only) under ThreadSanitizer

## License

- GPLv3: [LICENSE](LICENSE). Third-party licences: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt), also inside the app under **about**
- BeatNet+'s weights, which the shipped model is fine-tuned from, come with no licence stated upstream
- The tests use seventeen ten-second excerpts of commercial recordings, never built into takt4: [tests/data/features/](tests/data/features/README.md) lists them and how to have one removed

*Art-Net™ Designed by and Copyright Artistic Licence. ASIO is a trademark and software of Steinberg Media Technologies GmbH.*
