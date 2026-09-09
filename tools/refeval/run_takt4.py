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
are **overwritten**: this is the script that measures the build, so it must never hand
back a stale result. `--only NAME` restricts it to tracks whose stem contains NAME.
"""
import argparse
import shlex
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from common import AUDIO, TAKT4, cli_path

CONFIGS = {
    "nofold": ["--bpm", "off"],
    "fold70": ["--bpm", "70-140"],
    "gmain": ["--bpm", "off", "--weights", "generic-main"],
    "afnp": ["--bpm", "off", "--weights", "af-non-percussive"],
    # The particle filter on a 100 fps state space (tools/dump_statespace.py --fps 100
    # --name fps100), the activations interpolated by the engine: option B of
    # TRACKING-PROPOSAL.md §4 applied to the particle filter. Measured worse than at 50.
    "pf100": ["--bpm", "off", "--decoder", "pf", "--statespace", "fps100"],
    "pf100fold70": ["--bpm", "70-140", "--decoder", "pf", "--statespace", "fps100"],
    # The exact forward filter (option C) as it ships — the peak emission rule — window
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
    trace = TAKT4 / f"{wav.stem}.{tag}.trace"
    beats = TAKT4 / f"{wav.stem}.{tag}.beats"
    cmd = [str(cli), "track", str(wav), "--trace", str(trace), "--out", str(beats)] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return f"{wav.stem:<45} {tag:<8} FAILED ({r.returncode}): {r.stderr.strip()[-200:]}"
    last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
    return f"{wav.stem:<45} {tag:<8} {last}"


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
    extra = shlex.split(a.extra) if a.extra else []
    print(f"{cli}\n{len(wavs)} tracks x {a.tags} {' '.join(extra)}\n", flush=True)
    jobs = [(cli, w, a.tag or tag, CONFIGS[tag] + extra) for w in wavs for tag in a.tags]
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for line in pool.map(lambda j: run(*j), jobs):
            print(line, flush=True)


if __name__ == "__main__":
    main()
