"""Beat This! (CPJKU, ISMIR 2024) over every decoded WAV as an offline pseudo-reference.

Writes tests/data/tracking/refeval/ref_beatthis/<stem>.beats as '<seconds>\\t<beat in bar>'
with downbeat = 1, the Ballroom layout. Needs `beat_this` in the venv and the `final0`
checkpoint at references/beat-this/final0.ckpt (81 MB; the package's own downloader fails
through torch hub, so fetch the CPJKU share URL named in beat_this/inference.py directly).

About five seconds a track on the CPU. Existing files are left alone: the references are
committed, and regenerating one is a deliberate act.
"""
import sys
import time

import numpy as np
import soundfile as sf
import torch

from beat_this.inference import Audio2Beats

from common import AUDIO, REFS, ROOT

OUT = REFS / "ref_beatthis"

torch.set_num_threads(8)


def positions(beats, downbeats):
    """Beat-in-bar labels from beats and downbeats, counting up from each downbeat."""
    labels = np.zeros(len(beats), dtype=int)
    down = set(np.round(downbeats, 4))
    count = 0
    seen_down = False
    for i, b in enumerate(beats):
        if round(float(b), 4) in down:
            count = 1
            seen_down = True
        elif seen_down:
            count += 1
        labels[i] = count if seen_down else 0
    return labels


def main(names):
    OUT.mkdir(parents=True, exist_ok=True)
    a2b = Audio2Beats(checkpoint_path=str(ROOT / "references" / "beat-this" / "final0.ckpt"),
                      device="cpu", dbn=False)
    wavs = sorted(AUDIO.glob("*.wav"))
    if names:
        wavs = [w for w in wavs if any(n in w.stem for n in names)]
    for wav in wavs:
        out = OUT / f"{wav.stem}.beats"
        if out.exists():
            print(f"exists  {wav.stem}", flush=True)
            continue
        y, sr = sf.read(str(wav), dtype="float32")
        t0 = time.time()
        beats, downbeats = a2b(y, sr)
        labels = positions(beats, downbeats)
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            for b, l in zip(beats, labels):
                f.write(f"{b:.4f}\t{l}\n")
        tempo = 60.0 / np.median(np.diff(beats)) if len(beats) > 2 else 0.0
        print(f"{time.time() - t0:6.1f}s  {wav.stem:<45} {len(beats):5d} beats "
              f"{len(downbeats):4d} down  ~{tempo:6.1f} BPM", flush=True)


if __name__ == "__main__":
    main(sys.argv[1:])
