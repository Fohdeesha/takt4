# The three gates of TRACKING-PROPOSAL.md §7.11 for one checkpoint, in order, with the frozen CLI
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
$bin = Join-Path $W "$Name.bin"

"== $(Get-Date -Format s) convert $Checkpoint -> $bin  ($Jobs jobs, idle priority, cores $('0x{0:X}' -f $Cores))"
& $P tools/convert_weights.py $Checkpoint --name $Name --out-dir $W
"== $(Get-Date -Format s) gate (i): Ballroom's 70 held-out clips, bars 3+4 (generic on the same 70, 2026-09-24: beat F 0.9460, downbeat 0.9259)"
& $P tools/evaluate.py references/ballroom-audio --annotations references/ballroom-annotations --weights $bin --jobs $Jobs --report "$W/ballroom-heldout-$Name.json" --cli $env:TAKT4_CLI --cli-args "--meters 3,4" --held-out $Manifest
"-- and all 698, for continuity with earlier logs (committed generic: beat F 0.9555, downbeat 0.9420, acc1 0.983)"
& $P tools/evaluate.py references/ballroom-audio --annotations references/ballroom-annotations --weights $bin --jobs $Jobs --report "$W/ballroom-$Name.json" --cli $env:TAKT4_CLI --cli-args "--meters 3,4"
"== $(Get-Date -Format s) gate (ii): the harness, window off and on, bars of four (the shipped default)"
& $P tools/refeval/run_takt4.py nofold --extra "--weights $bin" --tag "$Name-nofold" --jobs $Jobs
& $P tools/refeval/run_takt4.py fold70 --extra "--weights $bin" --tag "$Name-fold70" --jobs $Jobs
"-- all 23:"
& $P tools/refeval/score.py --ref ref_beatthis --quiet takt4:nofold "takt4:$Name-nofold" takt4:fold70 "takt4:$Name-fold70"
"-- agreed 15:"
& $P tools/refeval/score.py --ref ref_beatthis --quiet --min-agree 0.8 takt4:nofold "takt4:$Name-nofold" takt4:fold70 "takt4:$Name-fold70"
"-- full table, window off:"
& $P tools/refeval/score.py --ref ref_beatthis takt4:nofold "takt4:$Name-nofold" --json "$W/refeval-$Name.json"
"== $(Get-Date -Format s) gate (iii): GiantSteps tempo (generic: acc1 0.768, acc2 0.851)"
& $P tools/train/giantsteps_eval.py $bin --jobs $Jobs
"== $(Get-Date -Format s) done"
