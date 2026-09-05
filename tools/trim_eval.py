"""Trim an evaluate.py report down to what is worth committing.

`tests/data/tracking/evaluation/*.json` are the accuracy gate's baselines. A full report
carries a per-file score for every one of the 698 clips, which is megabytes of detail
nobody diffs; what a reader and `tools/diff_eval.py` need is the configuration, the
summary and the per-genre means.

The paths are replaced with provenance strings, because the raw ones say where the audio
happened to sit on the machine that ran it and would differ on every other one --
`diff_eval.py` knows to expect exactly those two fields to differ and reports them apart
from the measurements.

    .venv/Scripts/python.exe tools/trim_eval.py eval.json \
        tests/data/tracking/evaluation/ballroom-generic-nofold.json

Until this existed the trimming was done by hand, which is why the two provenance strings
in the committed reports do not match anything a fresh run produces.
"""

import argparse
import json

# What a fresh report calls them, and what a committed one should say instead.
PROVENANCE = {
    "dataset": "Ballroom (references/ballroom-audio), not vendored",
    "annotations": "CPJKU/BallroomAnnotations @ 1db08914a8ae15edb01f104046e30bad88effe67",
}

# Per-file scores: the bulk of the file and the part nobody reads. Both of these hold one
# entry per clip — `per_file` the scores, `groups` the same keyed for the per-genre means —
# so dropping only one leaves 550 KB behind and a diff nobody can read.
DROP = ("groups", "per_file")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("fresh", help="the report evaluate.py --report just wrote")
    parser.add_argument("out", help="where to write the trimmed one")
    parser.add_argument("--keep-paths", action="store_true",
                        help="leave dataset/annotations as the machine's own paths")
    args = parser.parse_args()

    with open(args.fresh, encoding="utf-8") as handle:
        report = json.load(handle)

    dropped = [key for key in DROP if key in report]
    for key in dropped:
        del report[key]
    if not args.keep_paths:
        for key, value in PROVENANCE.items():
            if key in report:
                report[key] = value

    # newline="\n" so a run on Windows does not rewrite every line ending and turn a
    # twenty-line change of numbers into a whole-file diff.
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, indent=2)
        handle.write("\n")

    print(f"dropped {', '.join(dropped) if dropped else 'nothing'}; wrote {args.out}")
    summary = report.get("summary", {})
    for measure in ("F-measure", "Downbeat F-measure"):
        if measure in summary:
            print(f"  {measure}: {summary[measure]['mean']:.4f}")


if __name__ == "__main__":
    main()
