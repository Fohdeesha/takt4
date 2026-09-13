"""Lay the annotated sets out for BeatNet+'s training code (TRACKING-PROPOSAL.md §7.5, step 2).

    python tools/train/layout.py               # all four sets
    python tools/train/layout.py --seed 42     # the split's seed (42 is the default)

Reads each set's own annotation format and writes one `<id>.beats` per track in the
Ballroom layout (`<seconds> TAB <beat in bar>`, downbeat = 1) into
`references/datasets/<set dir>/annotations/`, beside the audio — which is exactly
`prepare_data.py`'s `{raw}/{dataset}/audio/*.wav` + `{raw}/{dataset}/annotations/*.beats`
with `raw = references/datasets`. Ballroom is already in that layout under
`references/ballroom-*`; it gets a `references/datasets/ballroom/` directory whose
`audio` and `annotations` are junctions to those, so all four sets read alike.

    Raveform    structures/beats/<index>.<youtube id>.beat.csv: time, beat in bar, section
    osu2beat    annotations/<md5>_<beatmap set>_beats_metered.txt, already Ballroom-shaped;
                an audio with several annotations keeps the first by name (the rest are
                listed under `alternates`)
    Harmonix    dataset/beats_and_downbeats/<file>.txt: time, beat in bar, bar number;
                genre and the original audio's duration from dataset/metadata.csv
    Ballroom    <genre>/<file>.wav + <file>.beats

**The octave is left as annotated** — §3.1's data decision, settled by §7.7: the fast
octave for drum and bass is the genre's own convention, and the window halves the number
at the operator's request. No relabelling.

Then WORK/manifest.json: every track with audio, its duration, tempo, meter, a seeded
90/10 train/validation split per set, and `flags` that keep it out of training:

    held_out             it is one of references/audio's tracks (common.HELD_OUT)
    duration_mismatch    Raveform: the fetched video's length is more than 2 s from the
                         length tracks.jsonl records, so it is not the video annotated
    few_beats            fewer than four beats or two downbeats — prepare_data.py's rule

...and, on a pseudo-labelled set, three about whether the label can be believed:

    teacher_disagree        Beat This!'s own three seeds agree below 0.7 beat F
    teacher_disagree_cross  a second teacher of another lineage (madmom, teacher_agree.py)
                            agrees with the label below 0.7 beat F
    teacher_octave_cross    ...and reads a whole octave from it, which is §7.13.2's central
                            failure mode and the one the seed filter is blindest to

check.py adds the activation-based flags later. A flag only records what was measured:
`finetune.py` drops a track when the recipe's `exclude_flags` names the flag, so adding
one changes no existing run until a config asks for it.
"""
import argparse
import csv
import json
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (BALLROOM_ANNOTATIONS, BALLROOM_AUDIO, DATASETS, HELD_OUT, MANIFEST,  # noqa: E402
                    OWN_SETS, SETS, WORK, meter_of, now, read_beats, save_json, tempo_of, write_beats)


def wav_seconds(path):
    import soundfile as sf
    info = sf.info(str(path))
    return info.frames / info.samplerate


def junction(link: Path, target: Path):
    """A directory junction (no privilege needed on Windows), or a symlink elsewhere.

    Only a convenience for prepare_data.py's layout: the manifest records the real paths.
    A network share (references/ lives on one here) cannot hold a reparse point, so a
    failure is reported and not fatal."""
    if link.exists():
        return
    link.parent.mkdir(parents=True, exist_ok=True)
    try:
        if sys.platform == "win32":
            import _winapi
            _winapi.CreateJunction(str(target), str(link))
        else:
            link.symlink_to(target, target_is_directory=True)
    except OSError as e:
        print(f"(no junction {link} -> {target}: {e}; the manifest uses the real paths) ", end="")


def track_record(set_name, track_id, wav, beats_path, times, positions, **extra):
    rec = {
        "id": track_id,
        "wav": str(wav),
        "beats": str(beats_path),
        "seconds": round(wav_seconds(wav), 3),
        "n_beats": int(len(times)),
        "n_downbeats": int(np.sum(positions == 1)),
        "first_beat": round(float(times[0]), 3) if len(times) else None,
        "last_beat": round(float(times[-1]), 3) if len(times) else None,
        "bpm": round(tempo_of(times), 2),
        "meter": meter_of(positions),
        "flags": [],
    }
    rec.update(extra)
    if len(times) < 4 or rec["n_downbeats"] < 2:
        rec["flags"].append("few_beats")
    if track_id in HELD_OUT.get(set_name, {}):
        rec["flags"].append("held_out")
        rec["held_out_reason"] = HELD_OUT[set_name][track_id]
    return rec


def layout_raveform():
    base = DATASETS / SETS["raveform"][0]
    audio, ann = base / SETS["raveform"][1], base / SETS["raveform"][2]
    ann.mkdir(exist_ok=True)
    durations = {}
    with open(base / "tracks.jsonl", encoding="utf-8") as f:
        for line in f:
            row = json.loads(line)
            if row.get("id"):
                try:
                    durations[str(row["id"])] = float(row.get("duration"))
                except (TypeError, ValueError):
                    pass
    tracks, missing = [], 0
    for src in sorted((base / "structures" / "beats").glob("*.beat.csv")):
        index, vid = src.name.split(".")[0], src.name.split(".")[1]
        wav = audio / f"{vid}.wav"
        if not wav.exists():
            missing += 1
            continue
        times, positions = [], []
        with open(src, encoding="utf-8") as f:
            for row in csv.DictReader(f):
                times.append(float(row["time"]))
                positions.append(int(float(row["downbeat"])))
        times, positions = np.asarray(times), np.asarray(positions)
        order = np.argsort(times, kind="stable")
        times, positions = times[order], positions[order]
        out = ann / f"{vid}.beats"
        write_beats(out, times, positions)
        rec = track_record("raveform", vid, wav, out, times, positions,
                           raveform_index=index, meta_seconds=durations.get(vid))
        if rec["meta_seconds"] is not None and abs(rec["seconds"] - rec["meta_seconds"]) > 2.0:
            rec["flags"].append("duration_mismatch")
        tracks.append(rec)
    return tracks, {"annotated_without_audio": missing}


def layout_osu2beat():
    base = DATASETS / SETS["osu2beat"][0]
    audio, ann = base / SETS["osu2beat"][1], base / SETS["osu2beat"][2]
    by_md5 = {}
    for src in sorted(ann.glob("*_beats_metered.txt")):
        by_md5.setdefault(src.name.split("_")[0].lower(), []).append(src)
    tracks, missing = [], 0
    for md5, sources in sorted(by_md5.items()):
        wav = audio / f"{md5}.wav"
        if not wav.exists():
            missing += 1
            continue
        times, positions = read_beats(sources[0])
        out = ann / f"{md5}.beats"
        write_beats(out, times, positions)
        tracks.append(track_record("osu2beat", md5, wav, out, times, positions,
                                   source=sources[0].name,
                                   alternates=[s.name for s in sources[1:]],
                                   beatmap_set=sources[0].name.split("_")[1]))
    return tracks, {"audios_without_audio": missing,
                    "audios_with_several_annotations": sum(len(v) > 1 for v in by_md5.values())}


def layout_harmonix():
    base = DATASETS / SETS["harmonix"][0]
    audio, ann = base / SETS["harmonix"][1], base / SETS["harmonix"][2]
    ann.mkdir(exist_ok=True)
    meta = {}
    with open(base / "dataset" / "metadata.csv", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            meta[row["File"]] = row
    scores = {}
    with open(base / "dataset" / "youtube_alignment_scores.csv", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            scores[row["File"]] = float(row["score"])
    tracks, missing = [], 0
    for src in sorted((base / "dataset" / "beats_and_downbeats").glob("*.txt")):
        stem = src.stem
        wav = audio / f"{stem}.wav"
        if not wav.exists():
            missing += 1
            continue
        times, positions = read_beats(src)
        out = ann / f"{stem}.beats"
        write_beats(out, times, positions)
        m = meta.get(stem, {})
        try:
            meta_seconds = float(m.get("Duration", ""))
        except ValueError:
            meta_seconds = None
        rec = track_record("harmonix", stem, wav, out, times, positions,
                           genre=m.get("Genre", ""), meta_seconds=meta_seconds,
                           meta_bpm=m.get("BPM", ""), time_signature=m.get("Time Signature", ""),
                           youtube_alignment_score=scores.get(stem))
        # metadata.csv's Duration is the annotated audio's; a YouTube version of another
        # length is another edit, and the beats past the edit point are wrong.
        if meta_seconds is not None and abs(rec["seconds"] - meta_seconds) > 2.0:
            rec["flags"].append("duration_mismatch")
        tracks.append(rec)
    return tracks, {"annotated_without_audio": missing}


def layout_ballroom():
    base = DATASETS / SETS["ballroom"][0]
    junction(base / "audio", BALLROOM_AUDIO)
    junction(base / "annotations", BALLROOM_ANNOTATIONS)
    tracks, missing = [], 0
    for wav in sorted(BALLROOM_AUDIO.rglob("*.wav")):
        beats = BALLROOM_ANNOTATIONS / f"{wav.stem}.beats"
        if not beats.exists():
            missing += 1
            continue
        times, positions = read_beats(beats)
        tracks.append(track_record("ballroom", wav.stem, wav, beats, times, positions,
                                   genre=wav.parent.name))
    return tracks, {"audio_without_annotation": missing}


def _normalised(name):
    return "".join(ch for ch in name.lower() if ch.isalnum())


def _harness_titles():
    """The 23 tracks of references/audio by normalised title, the leading track number
    dropped: `09 - Defang.flac` is `defang`, so a library copy named `Defang.flac` or
    `9 Defang.flac` is recognised too. Titles shorter than five characters keep their
    number, or `rale` would hold out every `morale`."""
    titles = set()
    for p in (DATASETS.parent / "audio").iterdir():
        if not p.is_file():
            continue
        full = _normalised(p.stem)
        title = _normalised(re.sub(r"^[\d\s.\-_]+", "", p.stem))
        titles.add(title if len(title) >= 5 else full)
    return titles


#: Below this beat F-measure between the teacher's seeds (distil.py's agreement.tsv, the
#: least of the pairwise scores), a pseudo-label is a guess the seeds themselves do not
#: share, and the track is flagged `teacher_disagree` rather than taught.
MIN_TEACHER_AGREEMENT = 0.7

#: The same question asked of a *different lineage* — madmom's RNN + DBN, from
#: teacher_agree.py's teacher_cross.tsv. Seeds of one model share that model's bias, so
#: their agreement measures variance and not correctness: on 2026-09-09 the sampled tracks
#: where madmom sat a whole octave from the teacher had a mean seed agreement of 0.806, and
#: the seed filter caught three of ten. The two filters correlate only 0.681.
#:
#: madmom is *weaker* than Beat This! on electronic music, so a disagreement does not mean
#: the label is wrong — it means the label has no independent confirmation. These flags are
#: for **excluding or downweighting, never for relabelling** (TRACKING-PROPOSAL.md §7.15).
MIN_TEACHER_AGREEMENT_CROSS = 0.7


def read_agreement(path):
    """{stem: (min, mean, ensemble tempo, [seed tempi])} from distil.py's agreement.tsv."""
    out = {}
    if not path.exists():
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 4:
                out[parts[0]] = (float(parts[1]), float(parts[2]), float(parts[3]),
                                 [float(p) for p in parts[4:]])
    return out


def read_teacher_cross(path):
    """{stem: (madmom bpm, teacher bpm, beat F, downbeat F, bpm ratio)} from
    teacher_agree.py's teacher_cross.tsv. Skips the header and any line a killed run
    truncated; the file is written as the run goes, so a partial one is expected."""
    out = {}
    if not path.exists():
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 6:
                try:
                    out[parts[0]] = tuple(float(p) for p in parts[1:6])
                except ValueError:
                    continue                           # the header, or a truncated tail
    return out


def is_octave(ratio):
    """madmom a whole octave from the teacher — teacher_agree.py's own test. These are the
    labels worth dropping first: the octave is §7.13.2's central failure mode, and the seed
    filter is at its blindest exactly here."""
    return abs(ratio - 2.0) < 0.1 or abs(ratio - 0.5) < 0.05


def layout_own(set_name, source, min_agreement=MIN_TEACHER_AGREEMENT,
               min_agreement_cross=MIN_TEACHER_AGREEMENT_CROSS):
    """A set of the operator's own — the library distil.py labelled, or the tracks tapped in
    through tools/annotate.py — as audio/<stem>.wav and annotations/<stem>.beats. Any track
    that is one of references/audio's, the 23 the harness scores, is flagged held_out,
    matched by title (`_harness_titles`); a pseudo-labelled track whose teacher seeds
    disagree below `min_agreement` is flagged `teacher_disagree`.

    Two further flags come from teacher_agree.py's second teacher, when it has been run:
    `teacher_disagree_cross` below `min_agreement_cross`, and `teacher_octave_cross` where
    madmom reads a whole octave from the label. They are *separate* from the seed flag
    because they catch different tracks — the two filters correlate 0.681 — and separate
    from each other because the octave ones are the ones to drop first."""
    base = DATASETS / SETS[set_name][0]
    audio, ann = base / SETS[set_name][1], base / SETS[set_name][2]
    if not audio.is_dir():
        return [], {"note": f"{audio} does not exist; nothing laid out"}
    harness = _harness_titles()
    agreement = read_agreement(base / "agreement.tsv")
    cross = read_teacher_cross(base / "teacher_cross.tsv")
    tracks, missing, disagree = [], 0, 0
    disagree_cross, octave_cross = 0, 0
    for wav in sorted(audio.glob("*.wav")):
        beats = ann / f"{wav.stem}.beats"
        if not beats.exists():
            missing += 1
            continue
        times, positions = read_beats(beats)
        rec = track_record(set_name, wav.stem, wav, beats, times, positions, source=source)
        n = _normalised(wav.stem)
        if any(h and h in n for h in harness):
            rec["flags"].append("held_out")
            rec["held_out_reason"] = "one of references/audio's 23: the harness scores it"
        if wav.stem in agreement:
            low, mean, _, tempi = agreement[wav.stem]
            rec["teacher_agreement"] = round(low, 4)
            rec["teacher_agreement_mean"] = round(mean, 4)
            rec["teacher_tempi"] = tempi
            if low < min_agreement:
                rec["flags"].append("teacher_disagree")
                disagree += 1
        if wav.stem in cross:
            mad_bpm, _, beat_f, _, ratio = cross[wav.stem]
            rec["teacher_cross_f"] = round(beat_f, 4)
            rec["teacher_cross_ratio"] = round(ratio, 3)
            rec["teacher_cross_bpm"] = round(mad_bpm, 2)
            if beat_f < min_agreement_cross:
                rec["flags"].append("teacher_disagree_cross")
                disagree_cross += 1
            if is_octave(ratio):
                rec["flags"].append("teacher_octave_cross")
                octave_cross += 1
        tracks.append(rec)
    notes = {"audio_without_annotation": missing}
    if agreement:
        notes["teacher_disagree"] = disagree
        notes["min_teacher_agreement"] = min_agreement
    if cross:
        notes["teacher_cross_scored"] = len(cross)
        notes["teacher_disagree_cross"] = disagree_cross
        notes["teacher_octave_cross"] = octave_cross
        notes["min_teacher_agreement_cross"] = min_agreement_cross
    return tracks, notes


def layout_library():
    return layout_own("library", "beat_this final0 (tools/train/distil.py)")


def layout_operator():
    return layout_own("operator", "the operator, tapped (tools/annotate.py)")


LAYOUTS = {"raveform": layout_raveform, "osu2beat": layout_osu2beat,
           "harmonix": layout_harmonix, "ballroom": layout_ballroom,
           "library": layout_library, "operator": layout_operator}


def assign_split(tracks, seed, val_fraction):
    """A seeded, per-set split. Flagged tracks are split too, so that lifting a flag
    later does not move any other track between train and validation."""
    rng = np.random.RandomState(seed)
    ids = sorted(t["id"] for t in tracks)
    rng.shuffle(ids)
    n_val = int(round(len(ids) * val_fraction))
    val = set(ids[:n_val])
    for t in tracks:
        t["split"] = "val" if t["id"] in val else "train"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sets", nargs="*", choices=list(SETS),
                    default=[s for s in SETS if s not in OWN_SETS],
                    help="default: the four public sets; name `library` once distil.py has run, "
                         "`operator` once tools/annotate.py --dataset has")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--val-fraction", type=float, default=0.1)
    a = ap.parse_args(argv)

    manifest = {"created": now(), "seed": a.seed, "val_fraction": a.val_fraction, "sets": {}}
    if MANIFEST.exists():
        with open(MANIFEST, encoding="utf-8") as f:
            manifest["sets"] = json.load(f).get("sets", {})
    for name in a.sets:
        print(f"{name}: ", end="", flush=True)
        tracks, notes = LAYOUTS[name]()
        assign_split(tracks, a.seed, a.val_fraction)
        flagged = sum(1 for t in tracks if t["flags"])
        hours = sum(t["seconds"] for t in tracks) / 3600
        manifest["sets"][name] = {
            "dir": str(DATASETS / SETS[name][0]), "notes": notes, "tracks": tracks,
            "n": len(tracks), "hours": round(hours, 2),
            "n_train": sum(t["split"] == "train" for t in tracks),
            "n_val": sum(t["split"] == "val" for t in tracks),
        }
        print(f"{len(tracks)} tracks, {hours:.1f} h, {manifest['sets'][name]['n_train']} train / "
              f"{manifest['sets'][name]['n_val']} val, {flagged} flagged; {notes}")
        for t in tracks:
            if t["flags"]:
                print(f"    {t['id']}: {', '.join(t['flags'])}"
                      + (f" ({t['held_out_reason']})" if "held_out" in t["flags"] else "")
                      + (f" fetched {t['seconds']:.1f} s, listed {t['meta_seconds']:.1f} s"
                         if "duration_mismatch" in t["flags"] else ""))
    WORK.mkdir(parents=True, exist_ok=True)
    save_json(MANIFEST, manifest)
    print(f"wrote {MANIFEST}")


if __name__ == "__main__":
    main(sys.argv[1:])
