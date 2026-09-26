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
production — sparse and busy, acoustic and electronic, quiet and loud — and that are
short enough not to matter: ten seconds each, in the repository forever.

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
