# Golden feature excerpts

The reference data for the Phase 2 gate: the C++ feature front end
(`src/core/features/`) must reproduce madmom's input features for BeatNet+ to within
floating-point tolerance, on real music, before any model output is trusted.

Each excerpt is three files with one base name:

| File | Contents |
|---|---|
| `<name>.wav` | 10 s, 22050 Hz, mono, 16-bit PCM — the exact samples the C++ side reads |
| `<name>.npy` | madmom's features for those exact samples: float32, shape (500, 288) — 144 log-filterbank bands, then their positive first differences, per 20 ms frame |
| `<name>.json` | where the excerpt came from (source, offset, and how the offset was chosen), signal statistics, and the package versions that produced the `.npy` |

`tests/features/feature_extractor_test.cpp` runs every `.wav` here through
`FeatureExtractor` and asserts the largest absolute difference to the `.npy` is at most
1e-5. The differences actually seen are a factor of 36 below that, and are floating-point
noise rather than disagreement: across every excerpt, **no filterbank value differs by
more than one float32 ulp** (2.384e-7 at the magnitudes involved; 44 % to 62 % of all
values, depending on the excerpt, are bit-identical), and their first differences — one
subtraction further on, so up to two ulps by construction — are within 1.2 ulp,
2.794e-7 at worst.

The test fails when this directory has no excerpts and warns while it has fewer than
ten, the number the gate calls for. There are eighteen: seventeen ten-second windows of
real music, and `synthetic`, a generated drum-machine pattern that carries no rights.

## The recordings

Seventeen of the excerpts are ten seconds of commercially released recordings, listed below.
**They are not covered by takt4's licence** (GPLv3, in [LICENSE](../../../LICENSE)): each
remains the property of its rights holders, and nothing here grants any right to them.

They are here because what this folder checks has to be checked on real music, and a check
nobody else can run is not one. Each is ten seconds of a track two and a half to eight
minutes long, reduced to 22 kHz mono, and used only as input to automated tests, which read
the samples and never play them. None is a substitute for the recording. They are included
in the belief that this is fair use.

**If you hold the rights to one of these recordings and want it removed,** open an issue at
<https://github.com/Fohdeesha/takt4/issues> saying which one, and it will be taken out —
from the repository's history as well, not only from its current files (see *Taking one
out*, below).

| Artist | Title | The ten seconds | Of a track | Excerpt |
|---|---|---|---|---|
| 808 State | In Yer Face (Bicep Remix) | 2:51–3:01 | 7:42 | `in-yer-face` |
| Autechre | Rale | 2:26–2:36 | 3:43 | `rale` |
| Brazilian Girls | Pirates | 0:14–0:24 | 3:33 | `pirates` |
| Broadcast | Winter Now | 1:27–1:37 | 3:48 | `winter-now` |
| Clark | Outside Plume | 3:47–3:57 | 4:21 | `outside-plume` |
| Clipping | True Believer | 1:01–1:11 | 3:45 | `true-believer` |
| Daedelus | A Complicated Geometry | 0:17–0:27 | 3:32 | `complicated-geometry` |
| Deerhoof | The Galaxist | 1:05–1:15 | 2:41 | `the-galaxist` |
| Jamie Lidell | Your Sweet Boom | 2:00–2:10 | 3:13 | `your-sweet-boom` |
| Jensen Interceptor | Model 2029 | 1:13–1:23 | 4:59 | `model-2029` |
| Jungle | Good Times | 0:52–1:02 | 3:01 | `good-times` |
| Mitsuto Suzuki | Jack Yourself | 1:37–1:47 | 4:23 | `jack-yourself` |
| Nourished By Time | Hell of A Ride | 0:33–0:43 | 3:44 | `hell-of-a-ride` |
| Primus | Wynona's Big Brown Beaver | 3:29–3:39 | 4:24 | `big-brown-beaver` |
| Squarepusher | Vic Acid | 2:19–2:29 | 3:07 | `vic-acid` |
| Two Fingers | Keman Rhythm | 1:38–1:48 | 3:17 | `keman-rhythm` |
| Zuli | Trigger Finger | 3:47–3:57 | 4:33 | `trigger-finger` |

Each excerpt's `.json` names the file it was cut from and the offset. The `.npy` beside it,
and the model outputs under `tests/data/model/`, are measurements taken from the excerpts;
these seventeen `.wav` files are the only commercial audio anywhere in the repository.

What reads the audio: `tests/features/feature_extractor_test.cpp` takes every `.wav` in
this folder, and `tests/engine/beat_engine_test.cpp` names several, holding `in-yer-face`
and `pirates` against the beats in `tests/data/tracking/shipped/`. The model test reads the
`.npy` features, not the audio.

### Taking one out

First, point any test that names the excerpt at another one (search the tests for `NAME`),
check the suite passes, and commit and push that. Keep at least ten `.wav` files here, or
the feature test warns. Then, with [git-filter-repo](https://github.com/newren/git-filter-repo)
2.47 or later, in a fresh clone:

```sh
git filter-repo --sensitive-data-removal --invert-paths \
  --path tests/data/features/NAME.wav
git push --force --mirror origin
```

Then ask GitHub Support to purge what they keep of the old commits: GitHub's guide
[Removing sensitive data from a repository](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository)
says what to send them. Every commit id changes, tags included, and every other clone has to
be cloned again.

## Adding an excerpt

The Python side is madmom, built from a pinned commit; see `tools/requirements.txt`
for the two-step install. Then, from the repository root:

```sh
python tools/make_golden.py path/to/track.flac --auto               # tool picks the window
python tools/make_golden.py path/to/track.flac                      # 10 s from 30 s in
python tools/make_golden.py path/to/track.flac --offset 95 --name track-drop
```

Keep the full tracks out of git (`references/` is ignored). Anything libsndfile decodes
works — WAV, FLAC, MP3, OGG, AIFF; AAC/M4A do not. The tool decodes and resamples with
librosa (soundfile + soxr, the way BeatNet+ loads audio), quantises to 16 bits, writes
the WAV, reads it back, and computes the features from what it read, so both sides
start from identical samples. Pick tracks that differ in genre, tempo, density and
production — sparse and busy, acoustic and electronic, quiet and loud. Ten seconds each,
and in the history for good once pushed, so add a commercial recording to the table under
*The recordings* in the same commit.

Which ten seconds is what `--auto` decides, since a fixed offset lands in the quiet
intro of some tracks and the outro of others. It reads the whole track and scores every
whole-second window by how strongly it pulses — the peak of its onset envelope's
autocorrelation between 40 and 200 BPM — discounted for sitting away from the loudness
the track spends most of its time at, and for containing a second much quieter than
that, which is what an intro, an outro or a breakdown looks like from here. It prints
the five best windows and takes the first. The window it settled on, and what it
measured there, are recorded in the sidecar as `offset_source` and `offset_selection`;
re-cutting the same excerpt later needs only `--offset`, not the analysis.

`takt4-cli features IN.wav OUT.npy --compare GOLDEN.npy` does the same comparison as
the test for one file, and writes the C++ features to a `.npy` numpy can load.

## Regenerating

The `.npy` files depend on the exact versions of numpy, scipy and madmom (the goldens
are madmom's float32 arithmetic, not a mathematical ideal). Bumping any of the pins in
`tools/requirements*.txt` means running `python tools/make_golden.py --regenerate`, which
recomputes every excerpt's `.npy` from its committed `.wav` (the exact input) and updates
the package versions in its sidecar, keeping where the excerpt came from; then
`tools/dump_filterbank.py`, and committing the results together with the pin. Passing a
committed `.wav` to `make_golden.py` as a source instead reproduces the `.npy` but writes a
sidecar that describes a ten-second file cut at 0 s.
