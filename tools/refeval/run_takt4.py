"""Run `takt4-cli track` over every decoded WAV, eight at a time: a trace and a beat file
per track per configuration.

    python tools/refeval/run_takt4.py            # nofold only
    python tools/refeval/run_takt4.py nofold fold70 gmain afnp

Configurations, by tag:

    nofold   --bpm off                     the app as it ships: the default decoder, no window
    fold70   --bpm 70-140                  the operator's window on
    gmain    --bpm off --weights generic-main
    afnp     --bpm off --weights af-non-percussive
    pf, pf70                               the particle filter by name
    fwd, fwd70, fwdmap, fwdmean, pf100     the forward filter and its variants; see CONFIGS

Output goes to references/refeval-work/takt4/<stem>.<tag>.{trace,beats}. Existing files
are **deleted before each run**, and any run that fails makes the script exit non-zero:
this is the script that measures the build, so it must never hand back a stale result.
`--only NAME` restricts it to tracks whose stem contains NAME.

Beside them, `<tag>.provenance.json` records, per track, the build that wrote its files —
the CLI's path, SHA-256 and `--version` — and the options. A run with `--only` leaves the
other tracks' files and records as they were, so gate.py reads the record to refuse a
scoring that mixes builds (the 2026-09-25 audit's P7).
"""
import argparse
import datetime
import json
import os
import shlex
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from common import AUDIO, TAKT4, cli_identity, cli_path, load_provenance, provenance_path

CONFIGS = {
    "nofold": ["--bpm", "off"],
    "fold70": ["--bpm", "70-140"],
    "gmain": ["--bpm", "off", "--weights", "generic-main"],
    "afnp": ["--bpm", "off", "--weights", "af-non-percussive"],
    # The particle filter on a 100 fps state space (tools/dump_statespace.py --fps 100
    # --name fps100), the activations interpolated by the engine: the finer grid applied
    # to the particle filter. Measured worse than at 50.
    "pf100": ["--bpm", "off", "--decoder", "pf", "--statespace", "fps100"],
    "pf100fold70": ["--bpm", "70-140", "--decoder", "pf", "--statespace", "fps100"],
    # The exact forward filter as it ships — the peak emission rule — window
    # off and on, and the two other emission rules it was measured against.
    "fwd": ["--bpm", "off", "--decoder", "forward"],
    "fwd70": ["--bpm", "70-140", "--decoder", "forward"],
    "fwdmap": ["--bpm", "off", "--decoder", "forward", "--emission", "map"],
    "fwdmap70": ["--bpm", "70-140", "--decoder", "forward", "--emission", "map"],
    "fwdmean": ["--bpm", "off", "--decoder", "forward", "--emission", "mean"],
    "fwdmean70": ["--bpm", "70-140", "--decoder", "forward", "--emission", "mean"],
    # And the particle filter by name, for when it is no longer the default.
    "pf": ["--bpm", "off", "--decoder", "pf"],
    "pf70": ["--bpm", "70-140", "--decoder", "pf"],
}


def run(cli: Path, wav: Path, tag: str, extra):
    """One track through one configuration: (line to print, whether it worked)."""
    trace = TAKT4 / f"{wav.stem}.{tag}.trace"
    beats = TAKT4 / f"{wav.stem}.{tag}.beats"
    # Gone before the run, not overwritten by it: a run that fails partway, or never starts,
    # must leave nothing that gate.py could score as this build's. The audit found exactly
    # that — a failed run left the last build's files in place, the script exited 0, and the
    # gate reported every value identical.
    trace.unlink(missing_ok=True)
    beats.unlink(missing_ok=True)
    cmd = [str(cli), "track", str(wav), "--trace", str(trace), "--out", str(beats)] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not beats.is_file():
        why = r.stderr.strip()[-200:] or "no beat file written"
        return f"{wav.stem:<45} {tag:<8} FAILED ({r.returncode}): {why}", False
    last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    return f"{wav.stem:<45} {tag:<8} {last}", True


def split_extra(text):
    """`--extra` as arguments. On Windows the POSIX rules would eat every backslash in a path
    (`--weights D:\\x\\y.bin` arrived as `D:xy.bin`), so there the quotes are honoured and
    then removed, and the backslashes kept."""
    if os.name != "nt":
        return shlex.split(text)
    return [t[1:-1] if len(t) >= 2 and t[0] == t[-1] and t[0] in "\"'" else t
            for t in shlex.split(text, posix=False)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("tags", nargs="*", default=["nofold"], choices=list(CONFIGS))
    ap.add_argument("--only", nargs="*", default=None, help="stems containing any of these")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--extra", default="",
                    help='more takt4-cli options for every configuration, quoted: "--meter-margin 3"')
    ap.add_argument("--tag", default=None,
                    help="write the files under this tag instead of the configuration's; for "
                         "one configuration with --extra, so the experiment keeps its own name")
    a = ap.parse_args()
    if a.tag and len(a.tags) != 1:
        raise SystemExit("--tag names one configuration's output; give one")

    cli = cli_path()
    TAKT4.mkdir(parents=True, exist_ok=True)
    wavs = sorted(AUDIO.glob("*.wav"))
    if a.only:
        wavs = [w for w in wavs if any(n in w.stem for n in a.only)]
    if not wavs:
        raise SystemExit(f"no WAVs under {AUDIO}; run decode_all.py first")
    extra = split_extra(a.extra) if a.extra else []
    print(f"{cli}\n{len(wavs)} tracks x {a.tags} {' '.join(extra)}\n", flush=True)
    jobs = [(cli, w, a.tag or tag, CONFIGS[tag] + extra) for w in wavs for tag in a.tags]
    identity = cli_identity(cli)
    when = datetime.datetime.now().isoformat(timespec="seconds")
    records = {tag: load_provenance(tag) for tag in {j[2] for j in jobs}}
    failed = 0
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for (_, wav, tag, options), (line, ok) in zip(jobs, pool.map(lambda j: run(*j), jobs)):
            print(line, flush=True)
            failed += 0 if ok else 1
            if ok:
                records[tag][wav.stem] = {**identity, "options": options, "when": when}
            else:
                records[tag].pop(wav.stem, None)
    for tag, record in records.items():
        with open(provenance_path(tag), "w", encoding="utf-8", newline="\n") as f:
            json.dump(record, f, indent=1, sort_keys=True)
            f.write("\n")
    if failed:
        raise SystemExit(f"{failed} of {len(jobs)} runs failed; their files are gone, not stale")


if __name__ == "__main__":
    main()
