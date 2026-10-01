# The three gates for one checkpoint, in order, with the frozen CLI
# (the decoder that ships: the forward filter, bars of four alone since 2026-09-09).
#
#   powershell -File tools/train/gate.ps1 -Checkpoint build/training/runs/<run>/epoch_024.pt -Name <name> [-Jobs 2]
#
# In git since 2026-09-24; it had lived in build/training/, where nothing tracked it and its
# paths still named C:\build, which moved to D: on 2026-09-09 (the audit's open items).
#
# Ballroom is measured with --meters 3,4 so that it goes on measuring the *model* against the
# committed reports (made with 3+4) rather than the bar prior, and **on the clips the fine-tune
# held out** — nine in ten of Ballroom are in its training split, so a whole-set score is a
# score on its own training data. The whole set is still printed, for continuity with the
# earlier runs' logs. The harness is measured as the rig runs, bars of four alone.
#
# Runs at idle priority on cores 8-15 (children inherit both), so the desktop's audio is never
# waiting on it. The affinity is this 16-thread machine's; change -Cores elsewhere.
#
# Every step's exit status is checked. A conversion that fails, or leaves a .bin that was not
# made from -Checkpoint, stops the script before anything is measured: it used to carry on, and
# the three gates scored whatever .bin of that name an earlier conversion had left (the
# 2026-09-25 audit's P5). A gate that fails to run is reported, the others still run, and the
# script ends with exit status 1.
param(
    [Parameter(Mandatory)][string]$Checkpoint,
    [Parameter(Mandatory)][string]$Name,
    [int]$Jobs = 2,
    [long]$Cores = 0xFF00
)
$ErrorActionPreference = "Continue"
$me = Get-Process -Id $PID
$me.PriorityClass = 'Idle'
$me.ProcessorAffinity = [IntPtr]$Cores
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $Root
$Work = if ($env:TAKT4_TRAIN_WORK) { $env:TAKT4_TRAIN_WORK } else { Join-Path $Root "build\training" }
# The frozen CLI: a copy that a rebuild of the tree cannot change under a run (the memory note
# on the fine-tune pipeline).
if (-not $env:TAKT4_CLI) { $env:TAKT4_CLI = Join-Path $Work "bin\takt4-cli.exe" }
$P = Join-Path $Root ".venv\Scripts\python.exe"
$W = Join-Path $Work "weights"
$Manifest = Join-Path $Work "manifest.json"
# What the checkpoint's run validated on is held out of its training whatever today's
# manifest says; the manifest is the fallback for a checkpoint kept outside its run.
$Run = Split-Path -Parent $Checkpoint
$HeldOut = if (Test-Path (Join-Path $Run "val_tracks_epoch_000.json")) { $Run } else { $Manifest }
$bin = Join-Path $W "$Name.bin"
$failed = @()
# Records a step that did not exit 0. Run straight after the step: $LASTEXITCODE is the last
# native command's.
function Check([string]$Step) {
    if ($LASTEXITCODE -ne 0) {
        "!! $Step failed (exit status $LASTEXITCODE)"
        $script:failed += $Step
    }
}

"== $(Get-Date -Format s) convert $Checkpoint -> $bin  ($Jobs jobs, idle priority, cores $('0x{0:X}' -f $Cores))"
& $P tools/convert_weights.py $Checkpoint --name $Name --out-dir $W
Check "the conversion"
# convert_weights.py records the checkpoint's SHA-256 beside the .bin; a .bin whose record
# names another checkpoint is an earlier conversion's.
$made, $want = $null, $null
if (-not $failed -and (Test-Path $Checkpoint)) {
    $want = (Get-FileHash -Algorithm SHA256 $Checkpoint).Hash.ToLower()
    if ((Test-Path $bin) -and (Test-Path "$W\$Name.json")) {
        $made = (Get-Content -Raw -Encoding UTF8 "$W\$Name.json" | ConvertFrom-Json).source_sha256
    }
}
if ($failed -or -not $want -or $made -ne $want) {
    "!! $bin was not made from $Checkpoint; nothing is measured"
    exit 1
}
"== $(Get-Date -Format s) gate (i): Ballroom's 70 held-out clips, bars 3+4 (generic on the same 70, 2026-09-24: beat F 0.9460, downbeat 0.9259)"
& $P tools/evaluate.py references/ballroom-audio --annotations references/ballroom-annotations --weights $bin --jobs $Jobs --report "$W/ballroom-heldout-$Name.json" --cli $env:TAKT4_CLI --cli-args "--meters 3,4" --held-out $HeldOut
Check "gate (i), Ballroom held out"
"-- and all 698, for continuity with earlier logs (committed generic: beat F 0.9555, downbeat 0.9420, acc1 0.983)"
& $P tools/evaluate.py references/ballroom-audio --annotations references/ballroom-annotations --weights $bin --jobs $Jobs --report "$W/ballroom-$Name.json" --cli $env:TAKT4_CLI --cli-args "--meters 3,4"
Check "gate (i), all of Ballroom"
"== $(Get-Date -Format s) gate (ii): the harness, window off and on, bars of four (the shipped default)"
& $P tools/refeval/run_takt4.py nofold --extra "--weights $bin" --tag "$Name-nofold" --jobs $Jobs
Check "gate (ii), the harness with the window off"
& $P tools/refeval/run_takt4.py fold70 --extra "--weights $bin" --tag "$Name-fold70" --jobs $Jobs
Check "gate (ii), the harness with the window on"
"-- all 23:"
& $P tools/refeval/score.py --ref ref_beatthis --quiet takt4:nofold "takt4:$Name-nofold" takt4:fold70 "takt4:$Name-fold70"
Check "gate (ii), scoring all 23"
"-- agreed 15:"
& $P tools/refeval/score.py --ref ref_beatthis --quiet --min-agree 0.8 takt4:nofold "takt4:$Name-nofold" takt4:fold70 "takt4:$Name-fold70"
Check "gate (ii), scoring the agreed 15"
"-- full table, window off:"
& $P tools/refeval/score.py --ref ref_beatthis takt4:nofold "takt4:$Name-nofold" --json "$W/refeval-$Name.json"
Check "gate (ii), the full table"
"== $(Get-Date -Format s) gate (iii): GiantSteps tempo (generic: acc1 0.768, acc2 0.851)"
& $P tools/train/giantsteps_eval.py $bin --jobs $Jobs
Check "gate (iii), GiantSteps"
if ($failed) {
    "== $(Get-Date -Format s) done, but $($failed.Count) step(s) did not run to the end: $($failed -join '; ')"
    exit 1
}
"== $(Get-Date -Format s) done"
