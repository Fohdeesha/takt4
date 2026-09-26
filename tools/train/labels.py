"""Check that every track's labels are what its manifest beats make, and repair where not.

    python tools/train/labels.py                          # check every set; exit status 1 on a mismatch
    python tools/train/labels.py --sets library osu2beat
    python tools/train/labels.py --adopt-from OLD.json    # the labels are right, the manifest lost them
    python tools/train/labels.py --rebuild                # the manifest is right, the labels are stale

finetune.py trains from `<id>.gt.npy` and validates against the manifest's `beats`, so the
two have to be one labelling, and finetune.py refuses to start when they are not. They
stopped being one on 2026-09-11: `layout.py --sets library` wrote the library's entries
again from the annotations and dropped octave.py's fields, and the `.gt.npy` that octave.py
had halved stayed halved. 254 tracks trained at half time and were scored at full time,
and v4 and v5 were selected that way (TRACKING-PROPOSAL.md §7.16).

Which side to repair is a decision about the labels, not about the files:

  --adopt-from OLD   the labels on disk are right and the manifest lost them. For each
                     mismatched track whose labels are exactly what OLD's entry makes, and
                     whose annotation is the one OLD's entry was made from, OLD's label
                     fields (layout.LABEL_FIELDS) replace the manifest's. The library's 254
                     came back this way on 2026-09-26 from `manifest.before-cross.json`,
                     which octave.py wrote on 2026-09-09; the operator chose half time.
  --rebuild          the manifest is right and the labels are stale: each mismatched
                     `.gt.npy` is rebuilt from the manifest's beats.

Both check again afterwards and say what is still mismatched.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (MANIFEST, feature_paths, label_mismatches, labels_from_beats,  # noqa: E402
                    load_manifest, save_json)
from layout import LABEL_FIELDS, same_annotation  # noqa: E402


def check(manifest, sets):
    items = [(s, t["id"], t["beats"]) for s in sets for t in manifest["sets"][s]["tracks"]
             if feature_paths(s, t["id"])[0].exists()]
    return len(items), label_mismatches(items)


def report(total, bad):
    if not bad:
        print(f"all {total} tracks with features are what their beats make")
        return
    by_set = {}
    for s, _, _ in bad:
        by_set[s] = by_set.get(s, 0) + 1
    print(f"{len(bad)} of {total} tracks are not what their beats make: "
          + ", ".join(f"{s} {n}" for s, n in sorted(by_set.items())))
    for s, tid, why in bad[:20]:
        print(f"  {s}/{tid}: {why}")
    if len(bad) > 20:
        print(f"  ... and {len(bad) - 20} more")


def adopt(manifest, bad, old_path):
    """Take OLD's label fields for each mismatched track whose labels OLD's entry makes."""
    with open(old_path, encoding="utf-8") as f:
        old = json.load(f)
    adopted, refused = [], {}
    for set_name, tid, _ in bad:
        track = next(t for t in manifest["sets"][set_name]["tracks"] if t["id"] == tid)
        before = next((t for t in old.get("sets", {}).get(set_name, {}).get("tracks", [])
                       if t["id"] == tid), None)
        why = None
        if before is None:
            why = "not in it"
        elif not same_annotation(before, track):
            why = "made from another annotation"
        elif not all(Path(before[k]).exists() for k in ("beats", "beats_original", "beats_annotated")
                     if k in before):
            why = "a file it names has gone"
        else:
            stored = np.load(feature_paths(set_name, tid)[1])
            if not np.array_equal(stored, labels_from_beats(before["beats"], len(stored))):
                why = "its beats do not make the labels either"
        if why:
            refused[why] = refused.get(why, 0) + 1
            continue
        for key in LABEL_FIELDS:
            if key in before:
                track[key] = before[key]
            else:
                track.pop(key, None)
        adopted.append((set_name, tid))
    print(f"adopted {old_path}'s label fields for {len(adopted)} tracks"
          + (f"; not for {sum(refused.values())}: {refused}" if refused else ""))
    return adopted


def rebuild(manifest, bad):
    done = 0
    for set_name, tid, _ in bad:
        track = next(t for t in manifest["sets"][set_name]["tracks"] if t["id"] == tid)
        feat_path, gt_path = feature_paths(set_name, tid)
        frames = int(np.load(feat_path, mmap_mode="r").shape[0])
        np.save(gt_path, labels_from_beats(track["beats"], frames))
        done += 1
    print(f"rebuilt {done} .gt.npy from their manifest beats")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sets", nargs="*", default=None)
    what = ap.add_mutually_exclusive_group()
    what.add_argument("--adopt-from", type=Path, default=None, metavar="OLD_MANIFEST")
    what.add_argument("--rebuild", action="store_true")
    a = ap.parse_args(argv)

    manifest = load_manifest()
    sets = a.sets or list(manifest["sets"])
    total, bad = check(manifest, sets)
    report(total, bad)
    if not bad or not (a.adopt_from or a.rebuild):
        return 1 if bad else 0
    if a.adopt_from:
        if adopt(manifest, bad, a.adopt_from):
            save_json(MANIFEST, manifest)
            print(f"wrote {MANIFEST}")
    else:
        rebuild(manifest, bad)
    total, bad = check(manifest, sorted({s for s, _, _ in bad}))
    print("afterwards: ", end="")
    report(total, bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
