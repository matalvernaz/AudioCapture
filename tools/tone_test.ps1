# Proves that a per-process capture records only its target program. Starts two PowerShell
# tone players (440 Hz and 1320 Hz), captures each one twice in a row with capture_smoke.exe,
# captures the whole playback device once as a control, then measures every file with
# tools/check_tones.py. Expect: the a440 files show 440 Hz present and 1320 Hz absent, the
# b1320 files the reverse, and the "all" file both present.
#
# usage (from the repo root, after building the capture_smoke target):
#   powershell -NoProfile -File tools\tone_test.ps1

param(
    [string]$Exe = "build\bin\Release\capture_smoke.exe",
    [string]$OutDir = "$env:TEMP\audiocapture-tonetest"
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
Remove-Item "$OutDir\*.wav" -ErrorAction SilentlyContinue

$playerArgs = '-NoProfile', '-File', 'tools\play_tone.ps1', '-Seconds', '22', '-Frequency'
$a = Start-Process powershell -ArgumentList ($playerArgs + '440') -PassThru -WindowStyle Hidden
$b = Start-Process powershell -ArgumentList ($playerArgs + '1320') -PassThru -WindowStyle Hidden
Write-Output "player A (440 Hz) PID $($a.Id), player B (1320 Hz) PID $($b.Id)"
Start-Sleep -Seconds 5   # both players need a moment to build their tone and start looping

& $Exe $a.Id 2 2 "$OutDir\a440"
& $Exe $b.Id 2 2 "$OutDir\b1320"
& $Exe 0 1 2 "$OutDir\all"

Wait-Process -Id $a.Id, $b.Id -ErrorAction SilentlyContinue
Get-ChildItem "$OutDir\*.wav" | Sort-Object Name | ForEach-Object {
    python -I tools\check_tones.py $_.FullName 440 1320
}
