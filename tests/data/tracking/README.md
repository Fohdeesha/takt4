# Particle filter reference traces

The reference data for the Phase 4 gate. `tools/pf_reference.py` restates BeatNet+'s
`particle_filtering_cascade` and writes what it does to the committed activations;
`tests/tracking/particle_filter_test.cpp` requires `src/core/tracking/` to reproduce
every row of it exactly.

**The particle filter is no longer the default decoder** (since 2026-09-08 that is the exact
forward filter, `src/core/tracking/forward_filter.hpp`; `tracking::Decoder` records the
measurements, and `refeval/` beside this directory holds the electronic-material
gate the choice was made on). This gate stands unchanged: the particle filter ships as
`--decoder pf` and the `"particle"` setting, and it is what the tests that need a beat stream
that never moves run on. The forward filter has no parity gate of this kind — it is a
deterministic computation over a state space built in C++, and its gate is the two
evaluations — but it was checked against the numpy prototype it was ported from
(`tools/refeval/decoders.py`, `fwd100`) to three decimals on every track's score in the
electronic gate, and to the millisecond on its timing profile.

| File | Contents |
|---|---|
| `<name>.npy` | int32, shape (500, 4), one row per activation frame — see below |
| `reference.json` | the seed, the state space blob's checksum, a checksum per excerpt, and what the restatement scored against upstream |

The four columns are the filter's state, not just its output:

| Column | |
|---|---|
| 0 | `gathering` — the median beat particle, taken before that frame's motion. Every decision the filter makes rests on it. |
| 1 | `down_max` — the commonest downbeat particle |
| 2 | what was emitted: 0 nothing, 1 downbeat, 2 beat |
| 3 | the beat interval the median sits in, in frames — 60 / (interval × 0.02) is the tempo |

Columns 0 and 1 summarise both particle clouds, so the test fails on the first frame
that diverges rather than only when a beat moves.

**The published meter is deliberately not among them**, and a change to the downbeat stage
should check this list before assuming it has broken the gate. `ParticleFilter::meterOf`
reads the whole downbeat cloud's mass — with a memory and an incumbent's margin, neither of
them upstream's — and feeds `beatsPerBar` only. `down_max` is untouched by it, which
matters because `down_max` is what decides `isBeatState` and therefore whether a beat goes
out as a downbeat: columns 1 and 2. So the meter has been reworked twice now, on
2026-09-05, without this gate moving a bit, and `tools/pf_reference.py` has never needed to
compute a meter at all. That separation is cheap to keep and expensive to re-derive.

The input is
`tests/data/model/generic/<name>.npy` columns 3 and 4 — the softmax probabilities for
beat and downbeat that Phase 3 recorded — so a regression in the model fails the Phase 3
test rather than muddying this one.

## Why the gate is not "reproduce upstream's beat times"

Phase 3 could hold the C++ against PyTorch directly, because a network is deterministic.
A particle filter is not, and upstream's randomness comes from numpy's legacy
`RandomState`, whose stream nothing outside numpy reproduces.

That is not a technicality that a tolerance would paper over. Running upstream's own
filter over these 18 excerpts with **eight different seeds** and comparing each pair:

| | agreement |
|---|---|
| identical frames | **0.57** |
| F1 within one 20 ms frame | **0.83** |
| F1 at MIR's 70 ms tolerance | **0.91** |

Upstream does not agree with *itself* on 43% of beat frames. There is no tolerance at
which "the C++ reproduces upstream" would mean anything.

So the gate is split, the way Phase 3 split `tools/beatnet_model.py` from the real `.pt`
files.

**The hard gate — the C++ against the restatement.** `tools/pf_reference.py` states the
same algorithm with a generator that *is* specified: xoshiro256++ over a splitmix64
seeding, twenty lines, written identically in `src/core/tracking/random.hpp`. Everything
downstream of it is integer arithmetic and IEEE doubles in a fixed order, so the two
agree bit for bit. All 9000 frames × 4 columns match exactly. That is the regression
gate, and it is as tight as a gate can be.

**The soft gate — the restatement against upstream.** `pf_reference.py` refuses to write
anything unless the restatement tracks upstream as well as upstream tracks itself, on
three statistics at once, over six seeds each. As of the committed traces:

| | upstream vs itself | restatement vs upstream | floor |
|---|---|---|---|
| beat F1 @ 70 ms | 0.9015 | **0.9002** | 0.8515 |
| downbeat F1 @ 70 ms | 0.7420 | **0.7373** | 0.6920 |
| same tempo interval, per frame | 0.6594 | **0.6544** | 0.6094 |

All three land within 0.005 of upstream's agreement with its own reruns. The floor is
0.05 below that, which is the margin for the two consuming entirely different RNG
streams.

## What the soft gate catches, measured

Beat-time F1 on its own is too coarse to be worth much, which is why there are three
statistics. Breaking `pf_reference.py` by hand, one change at a time, and re-running the
comparison at its shipped settings — 18 excerpts, 6 seeds, 70 ms:

| Mutation | beat F1 | downbeat F1 | same tempo | |
|---|---|---|---|---|
| *as written* | 0.9002 | 0.7373 | 0.6544 | passes |
| information gate off (0.4 → 0.0) | 0.8821 | 0.7257 | 0.6029 | **caught** |
| beat observation density flat | 0.5380 | 0.5155 | 0.3143 | **caught** |
| beat and downbeat densities swapped | 0.8965 | 0.0069 | 0.6405 | **caught** |
| resample threshold 0.1 → 0.5 | 0.8876 | 0.7300 | 0.5843 | **caught** |
| no injection at all (0.8 → 2.0) | 0.8757 | 0.7187 | 0.5987 | **caught** |
| particles never move | 0.8442 | 0.4432 | 0.0060 | **caught** |
| injection stride 6 → 7 | 0.9007 | 0.7482 | 0.6447 | passes |
| emission threshold 0.4 → 0.2 | 0.8943 | 0.7051 | 0.6428 | passes |
| downbeat injection 0.7 → 0.95 | 0.8998 | 0.7399 | 0.6541 | passes |

The three bottom rows are what this check honestly cannot see: at this sample size they
are inside the noise of a stochastic filter compared against another stochastic filter,
and arguably they are not breakage — the tracker still behaves the same way. Note also
that the density swap is invisible to beat F1 and obvious to downbeat F1, and that the
threshold changes are invisible to both but show in the tempo statistic. Each of the
three earns its place.

Every one of those mutations fails the hard gate on the first frame or two. The
deliberate check for that is in the other direction: setting the injection stride to 7
in `src/core/tracking/particle_filter.cpp` fails
`tests/tracking/particle_filter_test.cpp` at frame 4 of the first excerpt.

## Regenerating

```sh
python tools/dump_statespace.py            # if assets/statespace/ changed
python tools/pf_reference.py
```

The second needs madmom (for nothing but reading the blob's provenance) and a BeatNet+
checkout at `references/beatnet-plus` for the upstream comparison — the same one
`tools/convert_weights.py` documents. `--check-upstream -` skips it, which is only for
iterating; nothing should be committed that has not passed it.

The comparison takes about three minutes. It is mandatory rather than optional because
the restatement is the only thing standing between the C++ and a filter that reproduces
itself perfectly while being the wrong filter.
