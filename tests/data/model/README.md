# PyTorch reference activations

The reference data for the Phase 3 gate: the C++ model (`src/core/model/`) must
reproduce what the real BeatNet+ network computes, on the same test set Phase 2 used,
before anything downstream is trusted.

One directory per weight set — `generic`, `generic-main`, `af-non-percussive`, the
three published sets converted into `assets/weights/` by `tools/convert_weights.py`, and
`electronic`, the fine-tune the app ships by default — holding one `.npy` per golden
excerpt, named after it:

| File | Contents |
|---|---|
| `<set>/<name>.npy` | float32, shape (500, 6). Columns 0–2 are the logits the network emits for beat, downbeat and non-beat; columns 3–5 are the softmax of them, which is what the tracker consumes. |
| `<set>.json` | which `.pt` produced them and its SHA-256, the upstream commit, the torch and numpy versions, and per excerpt a checksum, the logit range and the strongest beat and downbeat probability |

The input is **not** anything C++ produced: it is `tests/data/features/<name>.npy`,
madmom's own features for that excerpt. So a regression in the feature front end fails
the Phase 2 test rather than muddying this one, and this is a test of the model alone.

`tests/model/beat_model_test.cpp` runs all 72 excerpt-and-weight-set pairs through
`BeatModel`, one frame at a time as the live path does, and asserts the largest
absolute difference is at most 1e-4 on the logits and 2e-5 on the probabilities. What
is actually seen on MSVC 2022 x64 Release is **1.29e-5 and 3.58e-6** (the shipped
`electronic`: 5.48e-6 and 1.49e-6). For scale,
PyTorch's own frame-by-frame and whole-sequence runs of the same weights on the same
input differ by up to 7.3e-6 (`worst_streaming_vs_sequence` in each sidecar), so the
C++ sits within a factor of two of what float32 reordering costs PyTorch itself. The
margin above that is for GCC and Apple Clang.

## Regenerating

`tools/model_reference.py` writes everything here. It needs torch — pinned separately
in `tools/requirements-torch.txt` because nothing else in `tools/` wants a 250 MB
install — and the three published `.pt` files, which are not vendored:

```sh
git clone https://github.com/mjhydri/BeatNet-Plus references/beatnet-plus
git -C references/beatnet-plus checkout bb90eb0a9065b101a4b4c4cb2b2061950266cb4b
pip install -r tools/requirements-torch.txt
python tools/convert_weights.py references/beatnet-plus/src/BeatNetPlus/models
python tools/model_reference.py
```

`references/` is git-ignored, which is where the weight files and the checkout belong;
only the converted blobs under `assets/weights/` and the traces here are committed.

`electronic` is not published: it is traced from the checkpoint it was converted from,
which lives with the training runs, and the script refuses a checkpoint whose sha256 is
not the `source_sha256` in `assets/weights/electronic.json`:

```sh
python tools/model_reference.py --checkpoint <training>/runs/electronic-library-v2/soup-48-80.pt --name electronic
```

The traces are produced by BeatNet+'s `online` path — the whole sequence through the
LSTM from a zero state — because that is a single unambiguous reference. takt4 runs the
network a frame at a time instead, so `model_reference.py` runs *both* on every excerpt
and refuses to write anything if they disagree by more than 1e-5.

Bumping the torch pin, or the excerpts under `tests/data/features/`, means re-running
both scripts and committing the results with the pin.
