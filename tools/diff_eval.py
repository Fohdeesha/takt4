"""Compare two evaluate.py reports, for the accuracy gate.

HANDOFF §5's rule is that anything touching `TempoTracker` or `ParticleFilter` re-runs
the Ballroom evaluation and diffs it against
`tests/data/tracking/evaluation/ballroom-generic-nofold.json`, and that **bit-identical is
the standard, not "close"**. Eyeballing the summary table does not meet it: the seven
figures it prints can all match while a per-genre mean has moved, and a changed
configuration field would not show up there at all.

So this walks both JSON trees to their leaves and compares every key the two have in
common, to full float precision. Keys on only one side are reported rather than skipped —
the committed reports have their per-file `groups` stripped, so the fresh side always has
thousands more, and that asymmetry must stay visible instead of quietly widening.

A few differences are expected against a committed report and mean nothing: `dataset` and
`annotations` are provenance strings, hand-edited when the report was trimmed, `cli` and
`platform` say where it ran, and `held_out` and `held_out_note` name the file --held-out was
given. Any other difference is a real one — `cli_args` included, so the
fresh report has to be made the way the committed one was, which since the decoder's default
became a bar of four alone is with the waltz put back:

    .venv/Scripts/python.exe tools/evaluate.py references/ballroom-audio \
        --annotations references/ballroom-annotations --weights generic \
        --cli-args "--meters 3,4" --jobs 4 --report fresh.json
    .venv/Scripts/python.exe tools/diff_eval.py \
        tests/data/tracking/evaluation/ballroom-generic-nofold.json fresh.json

(The committed reports carried `cli_args ""` from before that default changed until
2026-09-24, so this procedure failed on `cli_args` every time it was followed, and a gate that
is always red teaches people to read past it — the audit's Python-tools list. They were
regenerated that day, with `--meters 3,4` recorded.)

Exit status is 1 when any shared value differs, so it can gate a script.
"""

import argparse
import json
import sys

# Hand-edited when a report is trimmed for committing; not measurements. `held_out` is the
# path --held-out was given, written relative in the committed reports and absolute by
# tools/train/gate.ps1, and `held_out_note` quotes it; which clips were held out is measured
# by `files` and every `per_group.*.files`, which are compared. Until 2026-09-26 the path was
# compared too, so every held-out report came out red (the 2026-09-25 audit's P6).
PROVENANCE = {"dataset", "annotations", "cli", "platform", "held_out", "held_out_note"}


def leaves(node, path=""):
    """Every leaf in the tree, as a path -> value pair."""
    if isinstance(node, dict):
        for key, value in node.items():
            yield from leaves(value, f"{path}.{key}" if path else str(key))
    elif isinstance(node, list):
        for index, value in enumerate(node):
            yield from leaves(value, f"{path}[{index}]")
    else:
        yield path, node


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("committed", help="the report to hold the new one to")
    parser.add_argument("fresh", help="the report just produced")
    parser.add_argument("--strict", action="store_true",
                        help="count provenance fields as differences too")
    args = parser.parse_args()

    with open(args.committed, encoding="utf-8") as handle:
        left = dict(leaves(json.load(handle)))
    with open(args.fresh, encoding="utf-8") as handle:
        right = dict(leaves(json.load(handle)))

    shared = sorted(set(left) & set(right))
    only_committed = sorted(set(left) - set(right))
    only_fresh = sorted(set(right) - set(left))

    differing = [(k, left[k], right[k]) for k in shared if left[k] != right[k]]
    provenance = [d for d in differing if d[0] in PROVENANCE and not args.strict]
    real = [d for d in differing if d not in provenance]

    print(f"committed {len(left)} leaves, fresh {len(right)}, compared {len(shared)}")

    if only_committed:
        print(f"\n!! {len(only_committed)} keys are in the committed report and NOT the fresh one:")
        for key in only_committed[:20]:
            print(f"     {key}")
        if len(only_committed) > 20:
            print(f"     ... and {len(only_committed) - 20} more")

    if only_fresh:
        # Expected: the committed reports have their per-file scores stripped.
        print(f"\n   {len(only_fresh)} keys only in the fresh report "
              f"(expected — per-file scores), e.g. {only_fresh[0]}")

    if provenance:
        print(f"\n   {len(provenance)} provenance fields differ, which is expected:")
        for key, a, b in provenance:
            print(f"     {key}: {a!r} -> {b!r}")

    if real:
        print(f"\n!! {len(real)} MEASURED VALUES DIFFER:")
        for key, a, b in real:
            print(f"     {key}\n       committed {a!r}\n       fresh     {b!r}")
        return 1

    if only_committed:
        return 1

    print(f"\nevery measured value is identical ({len(shared) - len(provenance)} of them).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
