# Plays a steady sine tone from this PowerShell process for a while, so a capture test has a
# program whose audio is easy to recognise. Pair two of these at different frequencies and use
# tools/check_tones.py on the recording to see which one got in.
#
# usage: powershell -NoProfile -File tools\play_tone.ps1 -Frequency 440 -Seconds 12 [-Amplitude 0.2]
# Prints its own process ID so the capture can be pointed at it.

param(
    [int]$Frequency = 440,
    [int]$Seconds = 12,
    [double]$Amplitude = 0.2
)

$rate = 44100
$samples = $rate          # one second, an integer number of periods, so the loop point is seamless
$dataBytes = $samples * 2

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)
$ascii = [System.Text.Encoding]::ASCII
$bw.Write($ascii.GetBytes("RIFF")); $bw.Write([int32](36 + $dataBytes)); $bw.Write($ascii.GetBytes("WAVE"))
$bw.Write($ascii.GetBytes("fmt ")); $bw.Write([int32]16); $bw.Write([int16]1); $bw.Write([int16]1)
$bw.Write([int32]$rate); $bw.Write([int32]($rate * 2)); $bw.Write([int16]2); $bw.Write([int16]16)
$bw.Write($ascii.GetBytes("data")); $bw.Write([int32]$dataBytes)
$step = 2.0 * [math]::PI * $Frequency / $rate
for ($i = 0; $i -lt $samples; $i++) {
    $bw.Write([int16][math]::Round($Amplitude * 32767.0 * [math]::Sin($step * $i)))
}
$bw.Flush()
$ms.Position = 0

$player = New-Object System.Media.SoundPlayer($ms)
$player.Load()
$player.PlayLooping()
Write-Output "playing $Frequency Hz from PID $PID for $Seconds s"
Start-Sleep -Seconds $Seconds
$player.Stop()
