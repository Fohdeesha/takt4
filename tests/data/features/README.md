# Golden feature excerpts

The reference data for the Phase 2 gate: the C++ feature front end
(`src/core/features/`) must reproduce madmom's input features for BeatNet+ to within
floating-point tolerance, on real music, before any model output is trusted.

Each excerpt is three files with one base name:

| File | Contents |
|---|---|
| `<name>.wav` | 10 s, 22050 Hz, mono, 16-bit PCM — the exact samples the C++ side reads |
| `<name>.npy` | madmom's features for those exact samples: float32, shape (500, 288) — 144 log-filterbank bands, then their positive first differences, per 20 ms frame |
| `<name>.json` | where the excerpt came from (source, offset), signal statistics, and the package versions that produced the `.npy` |

`tests/features/feature_extractor_test.cpp` runs every `.wav` here through
`FeatureExtractor` and asserts the largest absolute difference to the `.npy` is at most
1e-5. The differences actually seen are one float32 ulp (2.4e-7). The test fails when
this directory has no excerpts and warns while it has fewer than ten: the gate needs at
least ten varied real recordings, and only `synthetic` (a generated drum-machine
pattern, no rights attached) is here until they are added.

## Adding an excerpt

The Python side is madmom, built from a pinned commit; see `tools/requirements.txt`
for the two-step install. Then, from the repository root:

```sh
python tools/make_golden.py path/to/track.flac                     # 10 s from 30 s in
python tools/make_golden.py path/to/track.flac --offset 95 --name track-drop
```

Keep the full tracks out of git (`references/` is ignored). Anything libsndfile decodes
works — WAV, FLAC, MP3, OGG, AIFF; AAC/M4A do not. The tool decodes and resamples with
librosa (soundfile + soxr, the way BeatNet+ loads audio), quantises to 16 bits, writes
the WAV, reads it back, and computes the features from what it read, so both sides
start from identical samples. Pick excerpts that differ in genre, tempo, density and
production — sparse and busy, acoustic and electronic, quiet and loud — and that are
short enough not to matter: ten seconds each, in the repository forever.

`takt4-cli features IN.wav OUT.npy --compare GOLDEN.npy` does the same comparison as
the test for one file, and writes the C++ features to a `.npy` numpy can load.

## Regenerating

The `.npy` files depend on the exact versions of numpy, scipy and madmom (the goldens
are madmom's float32 arithmetic, not a mathematical ideal). Bumping any of the pins in
`tools/requirements*.txt` means re-running `tools/make_golden.py` for every excerpt
(from the source tracks, or from the committed `.wav` files, which are the exact input)
and `tools/dump_filterbank.py`, then committing the results together with the pin.
