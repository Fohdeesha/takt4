"""Decode every track in references/audio to mono 22050 Hz float32 WAV, the way the
reference tracks were decoded for takt4's tools (librosa.load + soundfile).

    python tools/refeval/decode_all.py

Writes references/refeval-work/audio/<stem>.wav and a mapping.json from stem to source
file. Idempotent: a WAV that exists is left alone.
"""
import json
import re
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from common import AUDIO, ROOT

SRC = ROOT / "references" / "audio"


def slug(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_")


def decode(path: Path):
    import librosa
    import soundfile as sf

    out = AUDIO / (slug(path.stem) + ".wav")
    if out.exists():
        return path.name, out.name, "exists"
    y, sr = librosa.load(str(path), sr=22050, mono=True)
    sf.write(str(out), y, sr, subtype="FLOAT")
    return path.name, out.name, f"{len(y) / sr:.1f}s"


if __name__ == "__main__":
    AUDIO.mkdir(parents=True, exist_ok=True)
    files = sorted(p for p in SRC.iterdir() if p.suffix.lower() in (".flac", ".mp3", ".wav"))
    mapping = {}
    with ProcessPoolExecutor(max_workers=8) as pool:
        for src, dst, note in pool.map(decode, files):
            mapping[dst] = src
            print(f"{note:>8}  {dst}  <-  {src}", flush=True)
    (AUDIO / "mapping.json").write_text(json.dumps(mapping, indent=2, ensure_ascii=False),
                                        encoding="utf-8")
    print("done", len(mapping))
