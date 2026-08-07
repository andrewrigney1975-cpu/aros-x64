# Writes TEST.TXT onto the exFAT test VHD (must already be attached with a
# drive letter -- run make_exfat_vhd.txt via diskpart first). Explicitly
# flushes before you detach: a plain WriteAllText + detach can silently
# lose the write, since Windows may not flush volume metadata to the VHD
# backing file until something forces it.
param(
    [string]$DriveLetter = "Z"
)

$pattern = "The quick brown fox jumps over the lazy dog. "
$marker = "END-OF-FILE-MARKER"
$targetLen = 5000 - $marker.Length
$body = ""
while ($body.Length -lt $targetLen) { $body += $pattern }
$body = $body.Substring(0, $targetLen)
$content = $body + $marker

$path = "${DriveLetter}:\TEST.TXT"
$stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
$bytes = [System.Text.Encoding]::ASCII.GetBytes($content)
$stream.Write($bytes, 0, $bytes.Length)
$stream.Flush($true)
$stream.Close()

Write-Host "Wrote $path ($($bytes.Length) bytes)"
Write-VolumeCache -DriveLetter $DriveLetter
Write-Host "Flushed volume cache for ${DriveLetter}: -- safe to detach now."
