"""madmom's offline RNNDownBeatProcessor + DBNDownBeatTrackingProcessor over every track,
as a second, independent pseudo-reference. Same .beats layout as ref_beatthis.py, written
to tests/data/tracking/refeval/ref_madmom/. About forty seconds a track.

madmom reads the original file in references/audio (decode_all.py's mapping.json names it
for each WAV), not the 22.05 kHz decode the other tools read: its networks were trained on
44.1 kHz audio and its filterbank reaches 17 kHz, which a 22.05 kHz file leaves empty above
11 kHz. The committed references were made from the decodes, before this was noticed; what
that changes is measured in tests/data/tracking/refeval/README.md (the 2026-09-25 audit's P12).

madmom's own activations (100 fps, two columns) go to the work tree as
<stem>.madmom_act.npy — they are large and reproducible, so they are not committed.
"""
import json
import sys
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np

from common import AUDIO, REFS, ROOT, WORK

OUT = REFS / "ref_madmom"
ACTS = WORK / "madmom_act"
SOURCES = ROOT / "references" / "audio"


def one(job):
    wav, source = job
    from madmom.features.downbeats import (DBNDownBeatTrackingProcessor,
                                           RNNDownBeatProcessor)
    out = OUT / f"{wav.stem}.beats"
    if out.exists():
        return f"exists  {wav.stem}"
    t0 = time.time()
    act = RNNDownBeatProcessor()(str(source))
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
    mapping = json.loads((AUDIO / "mapping.json").read_text(encoding="utf-8"))
    jobs = [(w, SOURCES / mapping.get(w.name, "")) for w in wavs]
    missing = [w.name for w, s in jobs if not s.is_file()]
    if missing:
        raise SystemExit(f"no original in {SOURCES} for {len(missing)} decodes, e.g. {missing[0]}; "
                         "madmom is not run on the 22.05 kHz decodes")
    # Two workers: madmom's DBN over a four-minute track at 3 and 4 beats to the bar takes a
    # gigabyte, and six at once ran this machine out of memory (2026-09-26).
    with ProcessPoolExecutor(max_workers=2) as pool:
        for line in pool.map(one, jobs):
            print(line, flush=True)
