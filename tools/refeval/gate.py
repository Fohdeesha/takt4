"""The electronic-material gate: TRACKING-PROPOSAL.md's Appendix A, held to a committed
baseline the way tools/diff_eval.py holds Ballroom to tests/data/tracking/evaluation/.

    python tools/refeval/run_takt4.py nofold fold70     # the traces, from the build under test
    python tools/refeval/gate.py                        # score them and compare
    python tools/refeval/gate.py --write                # replace the baseline, deliberately

Ballroom says whether a tracker change is right on music the model was trained on; this
says whether it is right on the music the app is for. Twenty-three full electronic tracks
(`references/audio`, 91 minutes, not vendored) are scored against Beat This! with madmom
as the second opinion, with the fold off and with the operator's 70-140 window on, and
the numbers are summarised three ways: over all 23, over the fifteen the two references
agree on (beat F >= 0.8 against each other — the only set on which a score means
"correct" rather than "agrees with a convention"), and over the eight they do not.

**What fails it.** Any summary mean lower than the baseline's by more than `--tolerance`
(0.0005: the fourth decimal, which is what "unchanged to four decimals" means), or any
single track's beat F lower by more than `--track-tolerance` (0.02: more than a beat or
two, on a track of a few hundred). Every difference is printed either way, including the
ones that improved, because a change that moves a number it had no business moving is
worth a look even when the number went up. A missing track or system is a failure too.

The forward filter that `nofold` and `fold70` run (the default decoder) is deterministic, so on
one machine two runs of the same build agree to the last bit and the tolerances exist for other
compilers, not for noise. If they ever have to be widened, that is a finding.

The baseline lives beside the reference beats it is measured against:
`tests/data/tracking/refeval/baseline.json`. It was written from the build described in
its `provenance` field; `--write` records the build whose files it scored.

**One build at a time.** run_takt4.py records which build wrote each track's files, and the
gate refuses to score files no record accounts for or files from more than one build. It
used to score whatever was on disk: after `run_takt4.py --only` on a new build the rest were
the old build's, and `--write` then named the CLI of the moment rather than the one measured
(the 2026-09-25 audit's P7).
"""
import argparse
import datetime
import json
import sys

from common import HARD_EIGHT, REFS, load_provenance
from score import METRICS, score_systems, summarise

BASELINE = REFS / "baseline.json"
SYSTEMS = ("takt4:nofold", "takt4:fold70")
SUMMARIES = (("all 23", 0.0, ()), ("agreed 15", 0.8, ()), ("hard 8", 0.0, "not-hard"))


def summaries(scored):
    out = {}
    for label, min_agree, exclude in SUMMARIES:
        if exclude == "not-hard":
            exclude = tuple(s for s in scored["tracks"] if s not in HARD_EIGHT)
        out[label] = summarise(scored, min_agree, exclude)
    return out


def one_build(stems):
    """The identity of the one build that wrote every scored file; SystemExit otherwise."""
    builds, unrecorded = {}, []
    for spec in SYSTEMS:
        tag = spec.partition(":")[2]
        record = load_provenance(tag)
        for stem in stems:
            made = record.get(stem)
            if made is None:
                unrecorded.append(f"{stem}.{tag}")
                continue
            builds.setdefault(made["sha256"], (made, []))[1].append(f"{stem}.{tag}")
    if unrecorded:
        raise SystemExit(f"{len(unrecorded)} files have no record of the build that wrote them "
                         f"(e.g. {unrecorded[0]}); run run_takt4.py nofold fold70 again")
    if len(builds) > 1:
        raise SystemExit("the files were written by more than one build; run run_takt4.py nofold "
                         "fold70 again with one:\n  " +
                         "\n  ".join(f"{made['version'] or made['cli']} ({digest[:12]}): {len(files)} "
                                     f"files, e.g. {files[0]}" for digest, (made, files) in builds.items()))
    made = next(iter(builds.values()))[0]
    return {k: made[k] for k in ("cli", "sha256", "version")}


def fmt(value):
    return f"{value:.4f}" if isinstance(value, float) else str(value)


def compare(baseline, fresh, tolerance, track_tolerance):
    """Prints every difference; returns the list of failures."""
    failures = []
    changes = 0

    for label in baseline["summaries"]:
        for spec in baseline["summaries"][label]:
            old = baseline["summaries"][label][spec]
            new = fresh["summaries"][label].get(spec)
            if new is None:
                failures.append(f"{label}: {spec} is missing from the fresh scoring")
                continue
            for k in METRICS:
                delta = new[k] - old[k]
                if abs(delta) > 1e-9:
                    changes += 1
                    verdict = ""
                    if delta < -tolerance:
                        verdict = "  <-- WORSE"
                        failures.append(f"{label} {spec} {k}: {old[k]:.4f} -> {new[k]:.4f}")
                    print(f"  {label:<10} {spec:<14} {k:<5} {old[k]:.4f} -> {new[k]:.4f} "
                          f"({delta:+.4f}){verdict}")
            if new["n"] != old["n"]:
                failures.append(f"{label} {spec}: {old['n']} tracks became {new['n']}")

    for stem, old_row in baseline["tracks"].items():
        new_row = fresh["tracks"].get(stem)
        if new_row is None:
            failures.append(f"{stem}: missing from the fresh scoring")
            continue
        for spec, old in old_row["systems"].items():
            new = new_row["systems"].get(spec)
            if new is None:
                failures.append(f"{stem} {spec}: no beat file")
                continue
            moved = [k for k in ("bpm", "F", "CMLt", "AMLt", "dF", "beats") if
                     abs(new[k] - old[k]) > (0.05 if k == "bpm" else 1e-9)]
            if moved:
                changes += 1
                verdict = ""
                if new["F"] < old["F"] - track_tolerance:
                    verdict = "  <-- WORSE"
                    failures.append(f"{stem} {spec} beat F: {old['F']:.3f} -> {new['F']:.3f}")
                print(f"  {stem[:34]:<34} {spec:<14} " +
                      "  ".join(f"{k} {fmt(old[k])}->{fmt(new[k])}" for k in moved) + verdict)
    return failures, changes


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--write", action="store_true", help="replace the committed baseline")
    ap.add_argument("--tolerance", type=float, default=0.0005,
                    help="a summary mean may fall by this much (default 0.0005)")
    ap.add_argument("--track-tolerance", type=float, default=0.02,
                    help="one track's beat F may fall by this much (default 0.02)")
    a = ap.parse_args(argv)

    fresh = score_systems(list(SYSTEMS))
    missing = [s for s in fresh["tracks"] if any(spec not in fresh["tracks"][s]["systems"]
                                                for spec in SYSTEMS)]
    if len(fresh["tracks"]) < 23 or missing:
        raise SystemExit(f"only {len(fresh['tracks'])} tracks scored, {len(missing)} without "
                         f"both systems; run decode_all.py and run_takt4.py nofold fold70 first")
    build = one_build(sorted(fresh["tracks"]))
    print(f"scoring {build['version'] or build['cli']} (sha256 {build['sha256'][:12]})")
    fresh["summaries"] = summaries(fresh)

    print("summary, fresh:")
    for label, per_system in fresh["summaries"].items():
        for spec, t in per_system.items():
            print(f"  {label:<10} {spec:<14} n={t['n']:2d}  F {t['F']:.4f}  CMLt {t['CMLt']:.4f}  "
                  f"AMLt {t['AMLt']:.4f}  dF {t['dF']:.4f}  a1 {t['a1']:.4f}  a2 {t['a2']:.4f}")

    if a.write:
        fresh["provenance"] = {
            "written": datetime.date.today().isoformat(),
            "cli": build["cli"],
            "cli_sha256": build["sha256"],
            "cli_version": build["version"],
            "note": "tools/refeval/gate.py --write; see tests/data/tracking/refeval/README.md",
        }
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
            json.dump(fresh, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"\nwrote {BASELINE}")
        return 0

    if not BASELINE.exists():
        raise SystemExit(f"no baseline at {BASELINE}; write one with --write")
    with open(BASELINE, encoding="utf-8") as f:
        baseline = json.load(f)
    print(f"\nagainst {BASELINE.name} (written {baseline.get('provenance', {}).get('written')}):")
    failures, changes = compare(baseline, fresh, a.tolerance, a.track_tolerance)
    if not changes:
        print("  every value is identical.")
    if failures:
        print(f"\n!! {len(failures)} WORSE than the baseline:")
        for line in failures:
            print(f"   {line}")
        return 1
    print(f"\nnothing worse than the baseline ({changes} values moved).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
