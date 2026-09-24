"""Fine-tune BeatNet+ from `generic_weights.pt` on the laid-out sets (TRACKING-PROPOSAL.md §7.5, step 3).

    python tools/train/finetune.py --config tools/train/configs/electronic.yaml
    python tools/train/finetune.py --config ... learning_rate=0.0001 name=electronic-lr1e-4
    python tools/train/finetune.py --config ... --resume          # from the run's last.pt
    python tools/train/finetune.py --config ... --validate-only   # the pretrained weights' numbers

What is BeatNet+'s, imported from references/beatnet-plus and not restated: the network
(`BeatNetPlusBranch`), the weighted cross-entropy with class weights [60, 200, 1], the
features and the frame the ground truth puts a beat on (features.py), the 15 s crops,
batch 40, Adam at 5e-4, early stopping on validation beat F. That is the paper's
single-branch fine-tune (`train.py`'s guided_finetuning mode with no accompaniment).

What is not, and why:

  * **The loader.** train.py's dataset draws each crop from one RandomState that every
    DataLoader worker gets a pickled copy of, so with workers (its default is four) every
    epoch trains on the same 15 s of every track — measured on this machine: three epochs,
    identical crops. Here the crop starts are drawn in the main process by a sampler
    seeded by (seed, epoch) and handed to the workers with the index; every epoch is a
    different, reproducible draw, whatever the worker count.
  * **Validation per set, on fixed windows.** The proposal's gate is per set: Ballroom
    may lose at most 0.01 beat F (it is what the model already knows), the electronic
    sets have to gain. So every validation decodes each set's validation split with
    madmom's DBN (beats 3 or 4 to the bar, 55–215 BPM, as the C++ forward filter has it)
    and scores beat and downbeat F with mir_eval at 70 ms with the first five seconds
    trimmed — tools/evaluate.py's convention — on a fixed window of each track (the middle
    `val_seconds`; Ballroom's clips are 30 s and whole). Selection is the mean beat F over
    `select_sets` among the validations where the gate set held; epoch 0, before any
    training, is validated too, and is the gate's baseline.
  * **No tensorboard**; log.csv, val.json and status.json are written as it goes.

Output, under WORK/runs/<name>/: best.pt and last.pt (the branch's state_dict, exactly
what tools/convert_weights.py reads), epoch_NNN.pt every `checkpoint_every`, resume.pt
(model + optimizer + history), config.json, log.csv, val.json, status.json.

Runs below normal priority with four CPU threads; the GPU work is a 767 k-parameter
LSTM and leaves the desktop alone.
"""
import argparse
import csv
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (ALIGNMENT, FEATURE_DIM, FPS, GENERIC_WEIGHTS, HOP, NUM_BANDS, ROOT, RUNS,  # noqa: E402
                    SAMPLE_RATE, add_tools_path, feature_paths, load_json, load_manifest,
                    lower_priority, now, read_beats, save_json)

DEFAULTS = {
    "name": "electronic",
    "pretrained": str(GENERIC_WEIGHTS),
    "sets": {"raveform": 1, "osu2beat": 1, "harmonix": 1, "ballroom": 1},
    "harmonix_genres": ["Dance/Electronic"],
    "max_bpm": {},           # {set: bpm}: leave out a set's tracks annotated faster than this
    "exclude_flags": ["held_out", "duration_mismatch", "few_beats", "misaligned", "drifting"],
    "exclude_flags_per_set": {"harmonix": ["unverified"]},
    # {flag: repeats}: a track carrying the flag is trained on this many times instead of its
    # set's weight, rather than being dropped. For a label a second teacher will not confirm,
    # where dropping it also removes what it had to teach — see §7.16, where excluding the
    # octave disagreements outright cost the harness 0.03 beat F and broke three octaves.
    # Carrying several such flags takes the lowest. It never *raises* a weight, and a flag
    # named in exclude_flags is excluded, not downweighted.
    "downweight_flags": {},
    "alignment_exempt_sets": ["ballroom"],
    "seq_len": 750,
    "batch_size": 40,
    "learning_rate": 5e-4,
    "class_weights": [60, 200, 1],
    "grad_clip": 0.0,
    "l2sp": 0.0,             # L2-SP: loss += l2sp * ||theta - theta_pretrained||^2, anchors the fine-tune
    "genre_weights": {},     # {set: {genre: repeats}} on top of the set's own weight (Ballroom's genres)
    "stretch": [1.0, 1.0],   # time-stretch augmentation in the feature domain: a crop is read from
    "stretch_prob": 0.0,     # seq_len * s source frames, s log-uniform in [lo, hi], with this probability
    "stretch_min_bpm": 0.0,  # ...only for tracks at least this fast (a slowing stretch of slow material
                             # would leave the 55 BPM floor of the decoder's state space)
    "max_epochs": 300,
    "val_every": 2,
    "patience": 15,
    "checkpoint_every": 10,
    "val_tracks_per_set": 80,
    "val_seconds": 120,
    "val_decoder": "dbn",    # "dbn": madmom's offline DBN over the activations (fast, non-causal);
                             # "cli": takt4-cli, the shipped causal forward filter, over WAV excerpts
    "val_bpm": "off",        # cli: the window — "off" as the published trackers are measured, or
                             # "70-140", the operator's, so selection reads what the rig would
    "val_meters": "",        # cli: --meters for the decoder ("4", "3,4"); empty for its default
    "cli_jobs": 6,
    "gate_set": "ballroom",
    "gate_margin": 0.01,
    "select_sets": ["raveform", "osu2beat", "harmonix"],
    "seed": 42,
    "num_workers": 4,
    "dbn_workers": 4,
    "device": "cuda",
    "limit_tracks": 0,
}


def load_config(path, overrides):
    cfg = dict(DEFAULTS)
    if path:
        with open(path, encoding="utf-8") as f:
            cfg.update(yaml.safe_load(f) or {})
    for ov in overrides:
        key, _, val = ov.partition("=")
        if key not in cfg:
            raise SystemExit(f"unknown config key {key!r}")
        cfg[key] = yaml.safe_load(val)
    return cfg


# ---------------------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------------------

class Entry:
    __slots__ = ("set", "id", "feat", "gt", "frames", "beats", "seconds", "genre", "bpm",
                 "flags")

    def __init__(self, set_name, track, frames):
        self.set, self.id = set_name, track["id"]
        self.feat, self.gt = feature_paths(set_name, track["id"])
        self.frames = frames
        self.flags = tuple(track["flags"])      # for cfg["downweight_flags"]
        self.beats = track["beats"]
        self.seconds = track["seconds"]
        self.genre = track.get("genre", "")
        # The tempo of the labels the track trains on: halved by octave.py where it was.
        self.bpm = float(track.get("bpm_halved", track.get("bpm", 0.0))
                         if track.get("octave") == "half" else track.get("bpm", 0.0))


def select_tracks(cfg, manifest, alignment):
    """(train entries, {set: val entries}, report) honouring every flag and filter."""
    excluded = {}
    downweighted, kept_train = {}, {}
    train, val = [], {}
    for set_name in cfg["sets"]:
        info = manifest["sets"].get(set_name)
        if info is None:
            raise SystemExit(f"set {set_name!r} is not in the manifest; run layout.py")
        flags = set(cfg["exclude_flags"]) | set(cfg.get("exclude_flags_per_set", {}).get(set_name, []))
        kept = {"train": [], "val": []}
        for t in info["tracks"]:
            reasons = [f for f in t["flags"] if f in flags]
            a = alignment.get(f"{set_name}/{t['id']}") if alignment else None
            if a and a.get("flag") in flags and set_name not in cfg.get("alignment_exempt_sets", []):
                reasons.append(a["flag"])
            # A listed-duration mismatch is a suspicion; the activation check settles it.
            if "duration_mismatch" in reasons and a and a.get("flag") == "ok" \
                    and abs(a.get("best_shift", 1.0)) <= 0.05:
                reasons.remove("duration_mismatch")
            if set_name == "harmonix" and cfg.get("harmonix_genres") and t.get("genre") not in cfg["harmonix_genres"]:
                reasons.append("genre")
            ceiling = cfg.get("max_bpm", {}).get(set_name)
            if ceiling and float(t.get("bpm", 0.0)) > ceiling:
                reasons.append(f"faster_than_{int(ceiling)}")
            feat_path, gt_path = feature_paths(set_name, t["id"])
            if not feat_path.exists() or not gt_path.exists():
                reasons.append("no_features")
            if reasons:
                for r in reasons:
                    excluded.setdefault(set_name, {}).setdefault(r, 0)
                    excluded[set_name][r] += 1
                continue
            frames = int(np.load(feat_path, mmap_mode="r").shape[0])
            kept[t["split"]].append(Entry(set_name, t, frames))
        if cfg["limit_tracks"]:
            kept["train"] = kept["train"][:cfg["limit_tracks"]]
            kept["val"] = kept["val"][:max(4, cfg["limit_tracks"] // 8)]
        rng = np.random.RandomState(cfg["seed"])
        rng.shuffle(kept["val"])
        kept["val"] = sorted(kept["val"][:cfg["val_tracks_per_set"]], key=lambda e: e.id)
        genre_w = cfg.get("genre_weights", {}).get(set_name, {})
        down = cfg.get("downweight_flags", {})
        for e in kept["train"]:
            repeats = int(cfg["sets"][set_name]) * int(genre_w.get(e.genre, 1))
            marked = [int(down[f]) for f in e.flags if f in down]
            if marked:
                repeats = min(repeats, min(marked))   # downweight, never promote
                downweighted[set_name] = downweighted.get(set_name, 0) + 1
            train.extend([e] * repeats)
        val[set_name] = kept["val"]
        kept_train[set_name] = len(kept["train"])
    report = {s: {"train_tracks": kept_train.get(s, 0),
                  "train_crops_per_epoch": sum(1 for e in train if e.set == s),
                  "val_tracks": len(val[s]), "excluded": excluded.get(s, {}),
                  **({"downweighted": downweighted[s]} if s in downweighted else {})}
              for s in cfg["sets"]}
    return train, val, report


def stretch_crop(src, src_cls, L, s):
    """A crop of L frames read from round(L * s) source frames: the features linearly
    interpolated along time, the spectral-difference half scaled by 1/s (a change that
    took one source hop now spans s output hops), and every beat re-placed on the frame
    floor(source frame / s) — the convention build_ground_truth uses, floor(time * fps).
    Tempo becomes tempo / s: s = 1.4 turns 128 BPM house into 91, s = 0.7 into 183."""
    pos = np.arange(L, dtype=np.float64) * s
    i0 = np.minimum(pos.astype(np.int64), src.shape[0] - 2)
    w = (pos - i0)[:, None].astype(np.float32)
    feats = src[i0] * (1.0 - w) + src[i0 + 1] * w
    feats[:, NUM_BANDS:] /= np.float32(s)
    cls = np.full(L, 2, np.int64)
    for label in (0, 1):                       # downbeats last, so they win a collision
        frames = np.where(src_cls == label)[0]
        out = np.floor(frames / s).astype(np.int64)
        out = out[out < L]
        cls[out] = label
    return feats, cls


class CropDataset:
    """Indexed by (entry index, start frame); returns a (seq_len, 288) float32 crop and
    its (seq_len,) class targets, padded with class 2 (neither) past the end. With
    `stretch`, a crop is time-stretched by a factor drawn from (seed, index, start), so a
    given draw is reproducible across workers and runs."""

    def __init__(self, entries, seq_len, stretch=(1.0, 1.0), stretch_prob=0.0, seed=0, stretch_min_bpm=0.0):
        self.entries, self.seq_len = entries, seq_len
        self.stretch, self.stretch_prob, self.seed = tuple(stretch), stretch_prob, seed
        self.stretch_min_bpm = stretch_min_bpm

    def __len__(self):
        return len(self.entries)

    def __getitem__(self, item):
        import torch
        index, start = item
        e = self.entries[index]
        x = np.load(e.feat, mmap_mode="r")
        y = np.load(e.gt, mmap_mode="r")
        L = self.seq_len
        s = 1.0
        if self.stretch_prob > 0 and e.frames > L + 2 and e.bpm >= self.stretch_min_bpm:
            rng = np.random.default_rng([self.seed, index, start])
            if rng.random() < self.stretch_prob:
                lo, hi = np.log(self.stretch[0]), np.log(self.stretch[1])
                s = float(np.exp(rng.uniform(lo, hi)))
                s = min(s, (e.frames - start - 1) / L)   # no source past the end
        if s != 1.0 and s > 0.05:
            Ls = int(np.ceil(L * s)) + 1
            src = np.asarray(x[start:start + Ls], dtype=np.float32)
            src_cls = np.asarray(y[start:start + Ls], dtype=np.int64)
            feats, cls = stretch_crop(src, src_cls, L, s)
        elif e.frames <= L:
            feats = np.zeros((L, FEATURE_DIM), np.float32)
            feats[:e.frames] = x[:e.frames]
            cls = np.full(L, 2, np.int64)
            cls[:e.frames] = y[:e.frames]
        else:
            feats = np.asarray(x[start:start + L], dtype=np.float32)
            cls = np.asarray(y[start:start + L], dtype=np.int64)
        return torch.from_numpy(feats), torch.from_numpy(cls)


class CropSampler:
    """One crop per entry per epoch, shuffled, starts drawn here from (seed, epoch)."""

    def __init__(self, entries, seq_len, seed):
        self.entries, self.seq_len, self.seed, self.epoch = entries, seq_len, seed, 0

    def __len__(self):
        return len(self.entries)

    def __iter__(self):
        rng = np.random.default_rng([self.seed, self.epoch])
        order = rng.permutation(len(self.entries))
        for i in order:
            frames = self.entries[i].frames
            start = int(rng.integers(0, frames - self.seq_len)) if frames > self.seq_len else 0
            yield (int(i), start)


# ---------------------------------------------------------------------------------------
# Validation: DBN decode + mir_eval in worker processes
# ---------------------------------------------------------------------------------------

_DBN = None


def _decode_and_score(args):
    """(set, id, preds (T, 2), reference times, reference positions) -> scores."""
    global _DBN
    import mir_eval
    from madmom.features import DBNDownBeatTrackingProcessor
    set_name, tid, preds, ref_t, ref_p = args
    if _DBN is None:
        _DBN = DBNDownBeatTrackingProcessor(beats_per_bar=[3, 4], fps=FPS, observation_lambda=16)
    try:
        decoded = _DBN(preds)
    except Exception:  # noqa: BLE001 - madmom raises on degenerate input; score as zero
        decoded = np.zeros((0, 2))
    est_t = decoded[:, 0] if len(decoded) else np.zeros(0)
    est_d = decoded[decoded[:, 1] == 1, 0] if len(decoded) else np.zeros(0)
    ref_b = mir_eval.beat.trim_beats(ref_t)
    ref_d = mir_eval.beat.trim_beats(ref_t[ref_p == 1])
    est_b = mir_eval.beat.trim_beats(est_t)
    est_d = mir_eval.beat.trim_beats(est_d)
    beat_f = mir_eval.beat.f_measure(ref_b, est_b) if len(ref_b) and len(est_b) else 0.0
    down_f = mir_eval.beat.f_measure(ref_d, est_d) if len(ref_d) and len(est_d) else 0.0
    return set_name, tid, float(beat_f), float(down_f), int(len(est_t))


def val_window(entry, val_seconds):
    """(first frame, frames) of the fixed window: the middle `val_seconds` of the track."""
    L = int(val_seconds * FPS)
    if entry.frames <= L:
        return 0, entry.frames
    return (entry.frames - L) // 2, L


def validate(model, val_sets, cfg, pool, device):
    import torch
    import torch.nn.functional as F
    model.eval()
    cw = torch.tensor(cfg["class_weights"], dtype=torch.float32, device=device)
    jobs, losses = [], {}
    with torch.no_grad():
        for set_name, entries in val_sets.items():
            losses[set_name] = []
            for e in entries:
                f0, L = val_window(e, cfg["val_seconds"])
                feats = np.asarray(np.load(e.feat, mmap_mode="r")[f0:f0 + L], dtype=np.float32)
                cls = np.asarray(np.load(e.gt, mmap_mode="r")[f0:f0 + L], dtype=np.int64)
                x = torch.from_numpy(feats).unsqueeze(0).to(device)
                logits = model.inference_forward(x)                      # (1, 3, T)
                losses[set_name].append(float(F.cross_entropy(
                    logits, torch.from_numpy(cls).unsqueeze(0).to(device), weight=cw)))
                probs = torch.softmax(logits[0], dim=0).cpu().numpy()     # (3, T)
                preds = np.ascontiguousarray(probs[:2].T, dtype=np.float64)  # (T, 2)
                times, positions = read_beats(e.beats)
                t0 = f0 / FPS
                keep = (times >= t0) & (times < t0 + L / FPS)
                jobs.append((set_name, e.id, preds, times[keep] - t0, positions[keep]))
    per_set = {s: {"beat_f": [], "down_f": [], "tracks": {}} for s in val_sets}
    for set_name, tid, bf, df, n in pool.map(_decode_and_score, jobs, chunksize=4):
        per_set[set_name]["beat_f"].append(bf)
        per_set[set_name]["down_f"].append(df)
        per_set[set_name]["tracks"][tid] = [round(bf, 4), round(df, 4), n]
    out = {}
    for s, r in per_set.items():
        out[s] = {"n": len(r["beat_f"]),
                  "beat_f": float(np.mean(r["beat_f"])) if r["beat_f"] else 0.0,
                  "down_f": float(np.mean(r["down_f"])) if r["down_f"] else 0.0,
                  "loss": float(np.mean(losses[s])) if losses[s] else 0.0,
                  "tracks": r["tracks"]}
    model.train()
    return out


# ---------------------------------------------------------------------------------------
# Validation through the shipped decoder: takt4-cli over WAV excerpts of the val tracks
# ---------------------------------------------------------------------------------------

def prepare_val_wavs(val_sets, cfg):
    """One float32 mono WAV per validation track, the same window validate() scores, under
    WORK/val_wav/<set>/<id>.wav — made once, reused by every run. Returns {(set, id): (path, t0)}."""
    import librosa
    import soundfile as sf
    out = {}
    made = 0
    for set_name, entries in val_sets.items():
        d = RUNS.parent / "val_wav" / set_name
        d.mkdir(parents=True, exist_ok=True)
        for e in entries:
            f0, L = val_window(e, cfg["val_seconds"])
            path = d / f"{e.id}.wav"
            if not path.exists():
                audio, _ = librosa.load(manifest_wav(set_name, e.id), sr=SAMPLE_RATE, mono=True)
                s0, s1 = f0 * HOP, (f0 + L) * HOP
                sf.write(str(path), audio[s0:s1].astype(np.float32), SAMPLE_RATE, subtype="FLOAT")
                made += 1
            out[(set_name, e.id)] = (path, f0 / FPS)
    if made:
        print(f"  wrote {made} validation excerpts under {RUNS.parent / 'val_wav'}", flush=True)
    return out


_MANIFEST_WAVS = None


def manifest_wav(set_name, track_id):
    global _MANIFEST_WAVS
    if _MANIFEST_WAVS is None:
        _MANIFEST_WAVS = {(s, t["id"]): t["wav"] for s, info in load_manifest()["sets"].items()
                          for t in info["tracks"]}
    return _MANIFEST_WAVS[(set_name, track_id)]


def write_cli_weights(model, path):
    """The model's weights as the blob takt4-cli loads (tools/convert_weights.py's format)."""
    from convert_weights import blob
    state = {k: v.detach().cpu() for k, v in model.state_dict().items()}
    path.write_bytes(blob(state))


def _cli_track(args):
    """Run takt4-cli over one excerpt; (set, id, beat F, downbeat F, tempo acc1, beats, error).

    `error` is None, or why the CLI did not run: that is not a score of zero, and it used to
    be returned as one (the audit's Python-tools list), so a broken CLI or a missing weights
    file read as a model that had forgotten every beat. An excerpt the CLI ran over and found
    no beats in *is* a zero, and still scores as one."""
    import subprocess
    import tempfile
    import mir_eval
    cli, weights, set_name, tid, wav, t0, ref_t, ref_p, bpm, meters = args
    with tempfile.TemporaryDirectory(prefix="takt4-val-") as scratch:
        out = Path(scratch) / "beats.txt"
        cmd = [str(cli), "track", str(wav), "--out", str(out), "--bpm", bpm or "off",
               "--weights", str(weights), "--seed", "1", "--confidence", "0.15"]
        if meters:
            cmd += ["--meters", str(meters)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not out.exists():
            why = (r.stderr or r.stdout).strip()[-300:] or "no beat file written"
            return set_name, tid, 0.0, 0.0, 0, 0, f"exit {r.returncode}: {why}"
        rows = np.loadtxt(out, ndmin=2)
    if rows.size == 0:
        return set_name, tid, 0.0, 0.0, 0, 0, None
    est_t, est_p = rows[:, 0], rows[:, 1].astype(int)
    bpm_col = rows[:, 2] if rows.shape[1] > 2 else None
    ref_b = mir_eval.beat.trim_beats(ref_t)
    ref_d = mir_eval.beat.trim_beats(ref_t[ref_p == 1])
    est_b = mir_eval.beat.trim_beats(est_t)
    est_d = mir_eval.beat.trim_beats(est_t[est_p == 1])
    beat_f = mir_eval.beat.f_measure(ref_b, est_b) if len(ref_b) and len(est_b) else 0.0
    down_f = mir_eval.beat.f_measure(ref_d, est_d) if len(ref_d) and len(est_d) else 0.0
    ref_bpm = 60.0 / np.median(np.diff(ref_t)) if len(ref_t) > 2 else 0.0
    if bpm_col is not None and (est_t >= 5.0).any():
        published = float(np.median(bpm_col[est_t >= 5.0]))
    else:
        published = 60.0 / np.median(np.diff(est_t)) if len(est_t) > 2 else 0.0
    acc1 = int(ref_bpm > 0 and published > 0 and abs(published - ref_bpm) / ref_bpm <= 0.04)
    return set_name, tid, float(beat_f), float(down_f), acc1, int(len(est_t)), None


def validate_cli(model, val_sets, cfg, cli, wavs, run_dir, pool):
    """validate()'s numbers, measured by the shipped decoder instead of the offline DBN."""
    weights = run_dir / "val_weights.bin"
    write_cli_weights(model, weights)
    jobs = []
    for set_name, entries in val_sets.items():
        for e in entries:
            wav, t0 = wavs[(set_name, e.id)]
            times, positions = read_beats(e.beats)
            f0, L = val_window(e, cfg["val_seconds"])
            keep = (times >= t0) & (times < t0 + L / FPS)
            jobs.append((cli, weights, set_name, e.id, wav, t0, times[keep] - t0, positions[keep],
                         str(cfg.get("val_bpm", "off")), str(cfg.get("val_meters", "") or "")))
    per_set = {s: {"beat_f": [], "down_f": [], "acc1": [], "tracks": {}} for s in val_sets}
    failed = []
    for set_name, tid, bf, df, a1, n, error in pool.map(_cli_track, jobs):
        if error is not None:
            failed.append(f"{set_name}/{tid}: {error}")
            continue
        per_set[set_name]["beat_f"].append(bf)
        per_set[set_name]["down_f"].append(df)
        per_set[set_name]["acc1"].append(a1)
        per_set[set_name]["tracks"][tid] = [round(bf, 4), round(df, 4), n, a1]
    if failed:
        # Stop rather than select on it: a validation missing some of its excerpts is a
        # different validation, and one scored on failures is not a measurement at all.
        raise RuntimeError(f"takt4-cli failed on {len(failed)} of {len(jobs)} validation "
                           "excerpts:\n  " + "\n  ".join(failed[:10]))
    return {s: {"n": len(r["beat_f"]),
                "beat_f": float(np.mean(r["beat_f"])) if r["beat_f"] else 0.0,
                "down_f": float(np.mean(r["down_f"])) if r["down_f"] else 0.0,
                "acc1": float(np.mean(r["acc1"])) if r["acc1"] else 0.0,
                "tracks": r["tracks"]} for s, r in per_set.items()}


def selection_score(val, cfg):
    """Mean beat F over every validation track of the `select_sets` (each set weighted by
    its track count, so six Harmonix tracks do not count as much as eighty of Raveform)."""
    sets = [s for s in cfg["select_sets"] if s in val and val[s]["n"] > 0]
    total = sum(val[s]["n"] for s in sets)
    return float(sum(val[s]["beat_f"] * val[s]["n"] for s in sets) / total) if total else 0.0


def fmt_val(val):
    return "  ".join(f"{s}: F {v['beat_f']:.4f} dF {v['down_f']:.4f}"
                     + (f" acc1 {v['acc1']:.3f}" if "acc1" in v else "")
                     + (f" loss {v['loss']:.3f}" if "loss" in v else "") + f" (n={v['n']})"
                     for s, v in val.items())


# ---------------------------------------------------------------------------------------
# Training
# ---------------------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--config", default=None)
    ap.add_argument("--resume", action="store_true")
    ap.add_argument("--validate-only", action="store_true")
    ap.add_argument("overrides", nargs="*", help="key=value")
    a = ap.parse_args(argv)
    cfg = load_config(a.config, a.overrides)
    lower_priority()
    add_tools_path()

    import torch
    import torch.nn.functional as F
    from torch.utils.data import DataLoader
    from BeatNetPlus.model import BeatNetPlusBranch

    torch.manual_seed(cfg["seed"])
    np.random.seed(cfg["seed"])
    torch.backends.cudnn.deterministic = True
    torch.backends.cudnn.benchmark = False
    torch.set_num_threads(4)
    device = torch.device(cfg["device"] if torch.cuda.is_available() or cfg["device"] == "cpu" else "cpu")

    run_dir = RUNS / cfg["name"]
    run_dir.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest()
    alignment = (load_json(ALIGNMENT) or {}).get("tracks", {})
    train_entries, val_sets, report = select_tracks(cfg, manifest, alignment)
    print(f"run {run_dir} on {device}")
    for s, r in report.items():
        print(f"  {s}: {r['train_tracks']} train tracks ({r['train_crops_per_epoch']} crops/epoch), "
              f"{r['val_tracks']} val tracks; excluded {r['excluded']}")
    if not train_entries:
        raise SystemExit("nothing to train on")

    model = BeatNetPlusBranch(FEATURE_DIM, 150, 4, device)
    state = torch.load(cfg["pretrained"], map_location="cpu", weights_only=True)
    model.load_state_dict(state, strict=True)
    model.to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=cfg["learning_rate"])
    cw = torch.tensor(cfg["class_weights"], dtype=torch.float32, device=device)
    # L2-SP (Li, Grandvalet & Davoine 2018): the pretrained weights as the anchor the
    # penalty pulls towards, instead of zero — keeps what the model knows while it learns.
    anchor = {n: p.detach().clone() for n, p in model.named_parameters()} if cfg["l2sp"] > 0 else None

    history = {"config": cfg, "data": report, "started": now(), "validations": [],
               "best": None, "baseline": None}
    start_epoch = 0
    resume_path = run_dir / "resume.pt"
    if a.resume and resume_path.exists():
        ck = torch.load(resume_path, map_location="cpu", weights_only=False)
        model.load_state_dict(ck["model"])
        optimizer.load_state_dict(ck["optimizer"])
        history = ck["history"]
        start_epoch = ck["epoch"] + 1
        print(f"resumed from epoch {ck['epoch']}")
    else:
        save_json(run_dir / "config.json", {"config": cfg, "data": report, "argv": argv,
                                            "torch": torch.__version__, "device": str(device)})

    dataset = CropDataset(train_entries, cfg["seq_len"], stretch=cfg["stretch"],
                          stretch_prob=cfg["stretch_prob"], seed=cfg["seed"],
                          stretch_min_bpm=cfg["stretch_min_bpm"])
    sampler = CropSampler(train_entries, cfg["seq_len"], cfg["seed"])
    loader = DataLoader(dataset, batch_size=cfg["batch_size"], sampler=sampler,
                        num_workers=cfg["num_workers"], drop_last=True, pin_memory=(device.type == "cuda"),
                        persistent_workers=cfg["num_workers"] > 0)
    pool = ProcessPoolExecutor(max_workers=cfg["dbn_workers"])
    cli, wavs, cli_pool = None, None, None
    if cfg["val_decoder"] == "cli":
        from concurrent.futures import ThreadPoolExecutor
        from common import cli_path
        cli = cli_path()
        wavs = prepare_val_wavs(val_sets, cfg)
        cli_pool = ThreadPoolExecutor(max_workers=cfg["cli_jobs"])
        print(f"  validating through {cli}", flush=True)

    log_path = run_dir / "log.csv"
    if not log_path.exists() or not a.resume:
        with open(log_path, "w", encoding="utf-8", newline="") as f:
            csv.writer(f).writerow(["epoch", "train_loss", "grad_norm", "lr", "seconds", "val_selection",
                                    *[f"{s}_beat_f" for s in val_sets], *[f"{s}_down_f" for s in val_sets]])

    def status(extra):
        save_json(run_dir / "status.json", {"updated": now(), "run": cfg["name"], "best": history["best"],
                                            "baseline": history["baseline"], **extra})

    def run_validation(epoch):
        t = time.time()
        dbn = validate(model, val_sets, cfg, pool, device)
        if cli is not None:
            val = validate_cli(model, val_sets, cfg, cli, wavs, run_dir, cli_pool)
            for s in val:
                val[s]["dbn_beat_f"], val[s]["dbn_down_f"], val[s]["loss"] = \
                    dbn[s]["beat_f"], dbn[s]["down_f"], dbn[s]["loss"]
        else:
            val = dbn
        score = selection_score(val, cfg)
        gate_set = cfg["gate_set"]
        gate_value = val[gate_set]["beat_f"] if gate_set in val else None
        if history["baseline"] is None:
            history["baseline"] = {"epoch": epoch, "selection": score, "gate": gate_value, "val": {
                s: {k: v for k, v in r.items() if k != "tracks"} for s, r in val.items()}}
        gate_ok = gate_value is None or history["baseline"]["gate"] is None or \
            gate_value >= history["baseline"]["gate"] - cfg["gate_margin"]
        record = {"epoch": epoch, "selection": score, "gate": gate_value, "gate_ok": bool(gate_ok),
                  "seconds": round(time.time() - t, 1), "when": now(),
                  "val": {s: {k: v for k, v in r.items() if k != "tracks"} for s, r in val.items()}}
        history["validations"].append(record)
        save_json(run_dir / "val.json", history)
        save_json(run_dir / f"val_tracks_epoch_{epoch:03d}.json", {s: r["tracks"] for s, r in val.items()})
        improved = gate_ok and (history["best"] is None or score > history["best"]["selection"])
        if improved:
            history["best"] = {"epoch": epoch, "selection": score, "gate": gate_value}
            torch.save(model.state_dict(), run_dir / "best.pt")
        print(f"  val epoch {epoch}: selection {score:.4f} gate {gate_set} {gate_value:.4f} "
              f"({'ok' if gate_ok else 'FAILED'}){' *best*' if improved else ''} in {record['seconds']:.0f} s\n"
              f"    {fmt_val(val)}", flush=True)
        return record, improved

    if a.validate_only:
        run_validation(start_epoch)
        pool.shutdown()
        if cli_pool is not None:
            cli_pool.shutdown()
        return

    since_best = 0
    if history["validations"] and history["best"]:
        since_best = sum(1 for v in history["validations"] if v["epoch"] > history["best"]["epoch"])
    if start_epoch == 0 and not history["validations"]:
        run_validation(0)   # the pretrained weights: the baseline every later number is against
    model.train()
    t_run = time.time()
    for epoch in range(max(start_epoch, 1), cfg["max_epochs"] + 1):
        sampler.epoch = epoch
        t0 = time.time()
        losses, norms = [], []
        for feats, cls in loader:
            feats = feats.to(device, non_blocking=True)
            cls = cls.to(device, non_blocking=True)
            optimizer.zero_grad(set_to_none=True)
            logits, _ = model.train_forward(feats)              # (B, 3, T)
            loss = F.cross_entropy(logits, cls, weight=cw)
            if anchor is not None:
                loss = loss + cfg["l2sp"] * sum(((p - anchor[n]) ** 2).sum()
                                                for n, p in model.named_parameters())
            loss.backward()
            if cfg["grad_clip"] > 0:
                norm = torch.nn.utils.clip_grad_norm_(model.parameters(), cfg["grad_clip"])
            else:
                norm = torch.norm(torch.stack([p.grad.norm() for p in model.parameters() if p.grad is not None]))
            optimizer.step()
            losses.append(loss.item())
            norms.append(norm.item())
        train_loss, grad_norm, seconds = float(np.mean(losses)), float(np.mean(norms)), time.time() - t0
        print(f"epoch {epoch}/{cfg['max_epochs']}  loss {train_loss:.4f}  grad {grad_norm:.2f}  "
              f"{seconds:.0f} s  ({(time.time() - t_run) / 60:.0f} min so far)", flush=True)

        record = None
        if epoch % cfg["val_every"] == 0:
            record, improved = run_validation(epoch)
            since_best = 0 if improved else since_best + 1
        torch.save(model.state_dict(), run_dir / "last.pt")
        if epoch % cfg["checkpoint_every"] == 0:
            torch.save(model.state_dict(), run_dir / f"epoch_{epoch:03d}.pt")
        torch.save({"model": model.state_dict(), "optimizer": optimizer.state_dict(),
                    "epoch": epoch, "history": history}, resume_path)
        with open(log_path, "a", encoding="utf-8", newline="") as f:
            csv.writer(f).writerow([epoch, f"{train_loss:.5f}", f"{grad_norm:.4f}", cfg["learning_rate"],
                                    f"{seconds:.1f}", f"{record['selection']:.4f}" if record else "",
                                    *[f"{record['val'][s]['beat_f']:.4f}" if record else "" for s in val_sets],
                                    *[f"{record['val'][s]['down_f']:.4f}" if record else "" for s in val_sets]])
        status({"epoch": epoch, "train_loss": train_loss, "grad_norm": grad_norm, "epoch_seconds": seconds,
                "validations_since_best": since_best, "last_validation": record,
                "elapsed_minutes": round((time.time() - t_run) / 60, 1)})
        if since_best >= cfg["patience"]:
            print(f"early stop: {since_best} validations without improvement", flush=True)
            break
    history["finished"] = now()
    save_json(run_dir / "val.json", history)
    status({"finished": True, "epoch": epoch})
    pool.shutdown()
    if cli_pool is not None:
        cli_pool.shutdown()
    print(f"done; best {history['best']}")


if __name__ == "__main__":
    main(sys.argv[1:])
