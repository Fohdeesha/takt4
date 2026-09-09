"""Check every annotation against its audio: does the beat grid sit where the annotation says?

    python tools/train/check.py                 # every track with features
    python tools/train/check.py --sets harmonix --report 20
    python tools/train/check.py --weights runs/electronic/best.pt --tag electronic   # the fine-tune's hearing

A fetched audio file is not necessarily the one that was annotated: Harmonix's YouTube
versions are different edits of most tracks (durations differ by a median of ten
seconds), any set can hold a re-upload, and an encoder can move the whole file by a
fraction of a beat. Two witnesses are heard, both TRACKING-PROPOSAL.md §2.1's on/off
ratio — the mean of a signal on the annotated beats (best of ±2 frames) over its mean
halfway between them — for every time shift within half a beat, in 20 ms steps, over the
whole track and per 60 s window:

  * **the kick**: the positive spectral difference summed over the filterbank's nine bands
    below 140 Hz, read straight from the stored features. No model involved. On the
    four-on-the-floor material Raveform is mostly made of it is the beat itself, and where
    it is strong (ratio ≥ KICK_STRONG at its best shift) it decides: `ok` if that shift is
    within OFFSET of zero, `misaligned` if not.
  * **the shipped model**: `generic_weights.pt`'s P(beat) + P(downbeat). Where the kick is
    not decisive (breakbeats, no kick, a kick on every eighth) the model's curve is read
    the same way, and additionally per window: strong windows whose best shifts spread
    (on the circle of one beat) by more than DRIFT_SPAN are `drifting` — a different
    edit. No peak anywhere is `unverified`: the model does not hear this grid at all,
    which on a set of sound provenance (Raveform's audio is the annotated YouTube id,
    osu's is MD5-checked) is exactly what the fine-tune exists for, and on Harmonix
    cannot be told from the wrong edit.

The model's activations are cached under WORK/act/<tag>/ so a re-run, or the same check
with a fine-tuned model (`--weights ... --tag ...`), does not recompute them; the kick
curve is always recomputed (it is cheap). Writes WORK/alignment.json (or
alignment.<tag>.json): per track both curves' verdicts and the flag; finetune.py excludes
what it names. Ballroom, the model's own training set, is the calibration: 672 of its 698
clips read `ok` by the model alone, and the flagged rest are Rumba and Samba clips where
the model peaks off the count.
"""
import argparse
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (ALIGNMENT, FPS, GENERIC_WEIGHTS, NUM_BANDS, WORK, add_tools_path,  # noqa: E402
                    feature_paths, load_json, load_manifest, lower_priority, now, read_beats,
                    save_json)

SHIFTS = np.round(np.arange(-0.5, 0.5001, 0.02), 3)
STRONG = 2.5          # model: a ratio at which the grid is unmistakably in the activation
KICK_STRONG = 2.0     # kick: the onset function is decisive above this
KICK_OFFSET = 0.10    # kick: a best shift beyond this (s) is a misalignment — more than the
                      # 70 ms tolerance, well past Raveform's +0.04..+0.06 convention —
                      # unless the kick grid is plainly present at the annotation (KICK_AT0)
KICK_AT0 = 2.0
OFFSET = 0.06         # model: a best shift beyond this with the grid weak at 0 (DROP, AT0)
                      # is a misalignment: a 60 ms offset scores near zero at 70 ms tolerance
DROP = 0.5            # ...the ratio at 0 below this fraction of the best...
AT0 = 5.0             # ...and below this in absolute terms (Ballroom's p10 is 6)
WINDOW = 60.0         # seconds per drift window
DRIFT_SPAN = 0.15     # strong windows whose best shifts differ (circularly) by more than this drift
SKIP = 2.0            # seconds at the start left out: the LSTM warms up from a zero state,
                      # and its first second of output would dominate the off-beat mean
REACH = 2             # frames either side a beat may fall on (±40 ms)
KICK_BANDS = range(0, 9)   # filterbank bands centred 37.5 .. 137.5 Hz (12.5 Hz steps)


def local_max(act):
    pad = np.concatenate([np.full(REACH, act[0]), act, np.full(REACH, act[-1])])
    out = pad[REACH:REACH + len(act)].copy()
    for k in range(1, REACH + 1):
        out = np.maximum(out, pad[REACH - k:REACH - k + len(act)])
        out = np.maximum(out, pad[REACH + k:REACH + k + len(act)])
    return out


def ratio_curve(lmax, times, half_beat):
    """The on/off ratio for every shift in SHIFTS within half a beat (others stay 0): a
    shift of a whole beat lands on the grid again, and the question is whether the
    nearest grid is where the annotation says."""
    T = len(lmax)
    mids = (times[:-1] + times[1:]) / 2
    out = np.zeros(len(SHIFTS))
    for i, s in enumerate(SHIFTS):
        if abs(s) > half_beat + 1e-9:
            continue
        fb = np.floor((times + s) * FPS).astype(int)
        fb = fb[(fb >= SKIP * FPS) & (fb < T)]
        fm = np.floor((mids + s) * FPS).astype(int)
        fm = fm[(fm >= SKIP * FPS) & (fm < T)]
        if len(fb) < 4 or len(fm) < 4:
            continue
        out[i] = lmax[fb].mean() / max(lmax[fm].mean(), 1e-4)
    return out


def circular_span(shifts, period):
    """The spread of shifts on the circle of one beat: 2π minus the largest gap."""
    if len(shifts) < 2:
        return 0.0
    ang = np.sort(np.mod(np.asarray(shifts) / period, 1.0))
    gaps = np.diff(np.concatenate([ang, [ang[0] + 1.0]]))
    return float((1.0 - gaps.max()) * period)


def curve_verdict(signal, times):
    """(ratio at 0, best shift, best ratio, windows) for one signal."""
    lmax = local_max(signal)
    ibi = float(np.median(np.diff(times)))
    half = 0.5 * ibi
    curve = ratio_curve(lmax, times, half)
    i0 = int(np.argmin(np.abs(SHIFTS)))
    best = int(np.argmax(curve))
    windows = []
    start = times[0]
    while start < times[-1]:
        sel = times[(times >= start) & (times < start + WINDOW)]
        if len(sel) >= 20:
            c = ratio_curve(lmax, sel, half)
            b = int(np.argmax(c))
            windows.append({"start": round(float(start), 1), "shift": float(SHIFTS[b]),
                            "ratio": round(float(c[b]), 2), "ratio0": round(float(c[i0]), 2)})
        start += WINDOW
    return float(curve[i0]), float(SHIFTS[best]), float(curve[best]), windows, ibi


def analyse(act, kick, times):
    m0, ms, mb, mwin, ibi = curve_verdict(act, times)
    k0, ks, kb, _, _ = curve_verdict(kick, times)
    strong = [w for w in mwin if w["ratio"] >= STRONG]
    span = circular_span([w["shift"] for w in strong], ibi) if len(strong) >= 2 else 0.0
    if kb >= KICK_STRONG:
        by = "kick"
        flag = "ok" if abs(ks) < KICK_OFFSET or k0 >= KICK_AT0 else "misaligned"
    else:
        by = "model"
        if mb < STRONG:
            flag = "unverified"
        elif len(strong) >= 2 and span > DRIFT_SPAN:
            flag = "drifting"
        elif abs(ms) >= OFFSET and m0 < DROP * mb and m0 < AT0:
            flag = "misaligned"
        else:
            flag = "ok"
    return {"ratio0": round(m0, 3), "best_shift": ms, "best_ratio": round(mb, 3),
            "kick_ratio0": round(k0, 3), "kick_shift": ks, "kick_ratio": round(kb, 3),
            "windows": mwin, "strong_windows": len(strong), "drift_span": round(span, 3),
            "ibi": round(ibi, 4), "by": by, "flag": flag}


def kick_onset(feats):
    """The kick-band onset function from stored (frames, 288) features: the positive
    spectral difference (columns 144..) summed over KICK_BANDS."""
    cols = [NUM_BANDS + j for j in KICK_BANDS]
    return feats[:, cols].astype(np.float32).sum(axis=1)


CHUNK = 30000     # frames per pass when one pass over the whole track is refused (10 min)
OVERLAP = 500     # frames of the previous chunk re-run so the LSTM is warm at the seam


def activation_of(model, feats, device):
    """P(beat) + P(downbeat) per frame from `model`. One pass over the whole track; where
    cuDNN refuses the sequence (an hour-long DJ mix: CUDNN_STATUS_NOT_SUPPORTED, 2026-09-09,
    on the operator's library), the same in chunks of CHUNK frames, each begun OVERLAP
    frames early so the state is warm where the pieces are joined."""
    import torch

    def run(block):
        x = torch.from_numpy(block.astype(np.float32)).unsqueeze(0).to(device)
        probs = torch.softmax(model.inference_forward(x)[0], dim=0).cpu().numpy()
        return (probs[0] + probs[1]).astype(np.float32)

    with torch.no_grad():
        try:
            return run(feats)
        except RuntimeError as e:
            if "CUDNN" not in str(e).upper() and "out of memory" not in str(e).lower():
                raise
            torch.cuda.empty_cache()
        out = np.zeros(feats.shape[0], dtype=np.float32)
        start = 0
        while start < feats.shape[0]:
            begin = max(0, start - OVERLAP)
            piece = run(feats[begin:start + CHUNK])
            out[start:start + CHUNK] = piece[start - begin:]
            start += CHUNK
        return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sets", nargs="*", default=None)
    ap.add_argument("--weights", default=str(GENERIC_WEIGHTS))
    ap.add_argument("--tag", default=None, help="name for this model's activations and result "
                                                "file; default: the weights file's stem")
    ap.add_argument("--report", type=int, default=12, help="worst tracks to list per set")
    ap.add_argument("--force", action="store_true", help="redo tracks already in the result")
    a = ap.parse_args(argv)
    lower_priority()
    add_tools_path()
    import torch
    from BeatNetPlus.model import BeatNetPlusBranch

    tag = a.tag or Path(a.weights).stem
    out_path = ALIGNMENT if a.tag is None else ALIGNMENT.with_suffix(f".{a.tag}.json")
    act_dir = WORK / "act" / tag
    device = "cuda" if torch.cuda.is_available() else "cpu"
    model = BeatNetPlusBranch(288, 150, 4, device)
    model.load_state_dict(torch.load(a.weights, map_location=device, weights_only=True), strict=True)
    model.eval()
    torch.set_num_threads(4)

    manifest = load_manifest()
    result = load_json(out_path, {"created": now(), "tracks": {}})
    result["weights"] = a.weights
    result["rules"] = {"STRONG": STRONG, "KICK_STRONG": KICK_STRONG, "KICK_OFFSET": KICK_OFFSET,
                       "KICK_AT0": KICK_AT0, "OFFSET": OFFSET, "DROP": DROP, "AT0": AT0,
                       "DRIFT_SPAN": DRIFT_SPAN, "REACH": REACH, "SKIP": SKIP,
                       "KICK_BANDS": list(KICK_BANDS)}
    t0 = time.time()
    for set_name, info in manifest["sets"].items():
        if a.sets and set_name not in a.sets:
            continue
        todo = [t for t in info["tracks"]
                if a.force or f"{set_name}/{t['id']}" not in result["tracks"]]
        print(f"{set_name}: {len(todo)} tracks to check on {device}", flush=True)
        (act_dir / set_name).mkdir(parents=True, exist_ok=True)
        for n, t in enumerate(todo, 1):
            feat_path, _ = feature_paths(set_name, t["id"])
            if not feat_path.exists():
                continue
            feats = np.load(feat_path)
            act_path = act_dir / set_name / f"{t['id']}.npy"
            if act_path.exists():
                act = np.load(act_path)
            else:
                act = activation_of(model, feats, device)
                np.save(act_path, act)
            # The labels as annotated (or as shift_labels.py moved them), not as octave.py
            # halved them: the question is whether the annotation's grid is on the audio.
            times, _ = read_beats(t.get("beats_original", t["beats"]))
            r = analyse(act, kick_onset(feats), times) if len(times) >= 8 else {"flag": "few_beats", "windows": []}
            result["tracks"][f"{set_name}/{t['id']}"] = r
            if n % 200 == 0:
                print(f"  {n}/{len(todo)} ({(time.time() - t0) / 60:.1f} min)", flush=True)
        save_json(out_path, result)

    # The distributions, per set.
    for set_name, info in manifest["sets"].items():
        rows = [(t, result["tracks"].get(f"{set_name}/{t['id']}")) for t in info["tracks"]]
        rows = [(t, r) for t, r in rows if r and "kick_ratio" in r]
        if not rows:
            continue
        r0 = np.array([r["ratio0"] for _, r in rows])
        rb = np.array([r["best_ratio"] for _, r in rows])
        kb = np.array([r["kick_ratio"] for _, r in rows])
        ks = np.array([r["kick_shift"] for _, r in rows])
        flags, by = {}, {}
        for _, r in rows:
            flags[r["flag"]] = flags.get(r["flag"], 0) + 1
            by[r["by"]] = by.get(r["by"], 0) + 1
        print(f"\n=== {set_name}: {len(rows)} tracks; flags {flags}; decided by {by}")
        print(f"  model ratio at 0: p10 {np.percentile(r0, 10):.2f} median {np.median(r0):.2f} "
              f"p90 {np.percentile(r0, 90):.2f}; best below {STRONG}: {(rb < STRONG).sum()}")
        kstrong = kb >= KICK_STRONG
        print(f"  kick decisive on {kstrong.sum()}; of those, best shift within {KICK_OFFSET} s: "
              f"{(np.abs(ks[kstrong]) < KICK_OFFSET).sum()}, beyond: {(np.abs(ks[kstrong]) >= KICK_OFFSET).sum()}")
        hist = {}
        for s in ks[kstrong]:
            hist[s] = hist.get(s, 0) + 1
        top = sorted(hist.items(), key=lambda kv: -kv[1])[:6]
        print("  kick best shift where decisive: " + ", ".join(f"{s:+.2f}s:{n}" for s, n in top))
        bad = [(t, r) for t, r in rows if r["flag"] != "ok"]
        for t, r in sorted(bad, key=lambda tr: tr[1]["ratio0"])[:a.report]:
            print(f"    {t['id']:<34} model {r['ratio0']:5.2f}/{r['best_ratio']:6.2f}@{r['best_shift']:+.2f} "
                  f"kick {r['kick_ratio0']:5.2f}/{r['kick_ratio']:5.2f}@{r['kick_shift']:+.2f} "
                  f"windows {r['strong_windows']}/{len(r['windows'])} span {r['drift_span']:.2f}  "
                  f"{r['flag']} ({r['by']})" + (f"  {t.get('genre', '')}" if t.get("genre") else ""))
    print(f"\nwrote {out_path}")


if __name__ == "__main__":
    main(sys.argv[1:])
