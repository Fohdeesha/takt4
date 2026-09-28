"""What listening to one side of a stereo feed costs the tracker, against the average of both
sides the model was trained and every gate was measured on — TRACKING-PROPOSAL.md §7.18.

    python tools/refeval/stereo.py harness      # the 23 tracks, window off and on
    python tools/refeval/stereo.py giantsteps   # GiantSteps' 664 previews, tempo only

Each source is decoded once in stereo at 22050 Hz and tracked as the average (L+R)/2, the left
side, the right side, and one leg flipped (L-R)/2 (the harness only), with per-track stereo
statistics. Beat files go under references/stereo-work/<set>/<variant>/takt4/, results to
references/stereo-work/<set>.results.json. Score the harness per variant with score.py:

    TAKT4_REFEVAL_WORK=references/stereo-work/harness/left \
        python tools/refeval/score.py --json out.json takt4:nofold

Freeze the CLI for a run (STEREO_CLI=a copy of takt4-cli.exe): the build tree's is rebuilt
while this runs. CPU-heavy: ask before running it on a rig somebody is using.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
# A frozen copy: the build tree's CLI is rebuilt while this runs.
CLI = Path(os.environ.get("STEREO_CLI", ROOT / "build" / "windows-msvc" / "bin" / "Release" / "takt4-cli.exe"))
OUT = ROOT / "references" / "stereo-work"
SR = 22050
TMP = Path(os.environ.get("STEREO_TMP", tempfile.gettempdir())) / "takt4-stereo"
CONFIGS = {"nofold": ["--bpm", "off"], "fold70": ["--bpm", "70-140"]}
lock = threading.Lock()


def stats(L, R):
    def e(x):
        return float(np.dot(x.astype(np.float64), x.astype(np.float64)))
    M, S = (L + R) / 2, (L - R) / 2
    corr = e_lr = float(np.dot(L.astype(np.float64), R.astype(np.float64)))
    denom = np.sqrt(e(L) * e(R))
    corr = e_lr / denom if denom > 0 else 0.0
    side_db = 10 * np.log10(max(e(S), 1e-20) / max(e(M), 1e-20))
    # Below 150 Hz: the kick and the bass.
    Mf, Sf = np.fft.rfft(M.astype(np.float64)), np.fft.rfft(S.astype(np.float64))
    f = np.fft.rfftfreq(len(M), 1.0 / SR)
    lo = f < 150
    low_db = 10 * np.log10(max(np.sum(np.abs(Sf[lo]) ** 2), 1e-20) / max(np.sum(np.abs(Mf[lo]) ** 2), 1e-20))
    lr_db = 10 * np.log10(max(e(L), 1e-20) / max(e(R), 1e-20))
    return {"corr": corr, "side_db": side_db, "side_low_db": low_db, "l_over_r_db": lr_db}


def decode(src):
    import librosa
    y, _ = librosa.load(str(src), sr=SR, mono=False)
    if y.ndim == 1:
        y = np.stack([y, y])
    return y[0].astype(np.float32), y[1].astype(np.float32)


def variants(L, R, names):
    all_ = {"mid": (L + R) / 2, "left": L, "right": R, "flip": (L - R) / 2}
    return {k: all_[k].astype(np.float32) for k in names}


def track(wav, beats, config):
    beats.parent.mkdir(parents=True, exist_ok=True)
    beats.unlink(missing_ok=True)
    r = subprocess.run([str(CLI), "track", str(wav), "--out", str(beats)] + CONFIGS[config],
                       capture_output=True, text=True)
    if r.returncode != 0 or not beats.exists():
        raise RuntimeError(f"{wav.name} {config}: exit {r.returncode} {r.stderr[-200:]}")


def published_tempo(beats):
    rows = np.loadtxt(beats, ndmin=2)
    if rows.size == 0 or rows.shape[1] < 3:
        return 0.0
    keep = rows[:, 0] >= 5.0
    return float(np.median(rows[keep, 2])) if keep.any() else 0.0


def one(set_name, stem, src, names, configs):
    import soundfile as sf
    L, R = decode(src)
    st = stats(L, R)
    out = {"stats": st, "tempo": {}}
    TMP.mkdir(parents=True, exist_ok=True)
    for v, x in variants(L, R, names).items():
        wav = TMP / f"{set_name}.{stem}.{v}.wav"
        sf.write(str(wav), x, SR, subtype="FLOAT")
        try:
            for c in configs:
                beats = OUT / set_name / v / "takt4" / f"{stem}.{c}.beats"
                track(wav, beats, c)
                out["tempo"][f"{v}.{c}"] = published_tempo(beats)
        finally:
            wav.unlink(missing_ok=True)
    return stem, out


def harness_sources():
    mapping = json.loads((ROOT / "references" / "refeval-work" / "audio" / "mapping.json").read_text(encoding="utf-8"))
    refs = {p.stem for p in (ROOT / "tests" / "data" / "tracking" / "refeval" / "ref_beatthis").glob("*.beats")}
    out = {}
    for wav_name, src in mapping.items():
        stem = Path(wav_name).stem
        if stem in refs:
            out[stem] = ROOT / "references" / "audio" / src
    return out


def giantsteps_sources():
    gs = ROOT / "references" / "datasets" / "giantsteps-tempo"
    refs = {}
    for d in (gs / "annotations_v2" / "tempo", gs / "annotations" / "tempo"):
        for p in sorted(d.glob("*.bpm")):
            if p.stem not in refs:
                try:
                    bpm = float(p.read_text(encoding="utf-8").strip().split()[0])
                except (ValueError, IndexError):
                    continue
                if bpm > 0:
                    refs[p.stem] = bpm
    out = {}
    for stem in refs:
        mp3 = gs / "audio" / f"{stem}.mp3"
        if mp3.exists():
            out[stem] = mp3
    return out, refs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("set", choices=["harness", "giantsteps"])
    ap.add_argument("--jobs", type=int, default=10)
    ap.add_argument("--limit", type=int, default=None)
    a = ap.parse_args()
    if a.set == "harness":
        sources, refs = harness_sources(), None
        names, configs = ["mid", "left", "right", "flip"], ["nofold", "fold70"]
    else:
        sources, refs = giantsteps_sources()
        names, configs = ["mid", "left", "right"], ["nofold"]
    items = sorted(sources.items())
    if a.limit:
        items = items[:a.limit]
    print(f"{CLI}\n{a.set}: {len(items)} sources x {names} x {configs}", flush=True)
    results, failures = {}, {}
    done = 0

    def job(item):
        stem, src = item
        try:
            return one(a.set, stem, src, names, configs)
        except Exception as e:  # noqa: BLE001
            return stem, {"error": str(e)}

    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for stem, res in pool.map(job, items):
            done += 1
            if "error" in res:
                failures[stem] = res["error"]
                print(f"[{done}/{len(items)}] {stem} FAILED {res['error']}", flush=True)
                continue
            if refs is not None:
                res["ref"] = refs[stem]
            results[stem] = res
            if done % 25 == 0 or a.set == "harness":
                print(f"[{done}/{len(items)}] {stem} corr {res['stats']['corr']:.3f} "
                      f"side {res['stats']['side_db']:.1f} dB low {res['stats']['side_low_db']:.1f} dB "
                      + " ".join(f"{k}={v:.1f}" for k, v in res["tempo"].items()), flush=True)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f"{a.set}.results.json").write_text(json.dumps({"cli": str(CLI), "results": results,
                                                          "failures": failures}, indent=1),
                                               encoding="utf-8")
    print(f"done: {len(results)} ok, {len(failures)} failed", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
