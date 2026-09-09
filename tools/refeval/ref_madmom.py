"""madmom's offline RNNDownBeatProcessor + DBNDownBeatTrackingProcessor over every WAV,
as a second, independent pseudo-reference. Same .beats layout as ref_beatthis.py, written
to tests/data/tracking/refeval/ref_madmom/. About forty seconds a track.

madmom's own activations (100 fps, two columns) go to the work tree as
<stem>.madmom_act.npy — they are large and reproducible, so they are not committed.
"""
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

from common import AUDIO, REFS, WORK

OUT = REFS / "ref_madmom"
ACTS = WORK / "madmom_act"


def one(wav: Path):
    from madmom.features.downbeats import (DBNDownBeatTrackingProcessor,
                                           RNNDownBeatProcessor)
    out = OUT / f"{wav.stem}.beats"
    if out.exists():
        return f"exists  {wav.stem}"
    t0 = time.time()
    act = RNNDownBeatProcessor()(str(wav))
    np.save(ACTS / f"{wav.stem}.madmom_act.npy", act.astype(np.float32))
    proc = DBNDownBeatTrackingProcessor(beats_per_bar=[3, 4], fps=100)
    res = proc(act)  # columns: time, beat position (1 = downbeat)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        for t, pos in res:
            f.write(f"{t:.4f}\t{int(pos)}\n")
    tempo = 60.0 / np.median(np.diff(res[:, 0])) if len(res) > 2 else 0.0
    return (f"{time.time() - t0:6.1f}s  {wav.stem:<45} {len(res):5d} beats "
            f"{int(np.sum(res[:, 1] == 1)):4d} down  ~{tempo:6.1f} BPM")


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    ACTS.mkdir(parents=True, exist_ok=True)
    names = sys.argv[1:]
    wavs = sorted(AUDIO.glob("*.wav"))
    if names:
        wavs = [w for w in wavs if any(n in w.stem for n in names)]
    with ProcessPoolExecutor(max_workers=6) as pool:
        for line in pool.map(one, wavs):
            print(line, flush=True)
