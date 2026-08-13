# Copies the current examples/editor.bas onto the exFAT test VHD (must
# already be attached with a drive letter -- run attach_exfat_vhd.txt via
# diskpart first). Explicitly flushes before you detach, same reasoning
# as write_testfile.ps1: a plain Copy-Item + detach can silently lose the
# write since Windows may not flush volume metadata to the VHD backing
# file until something forces it.
param(
    [string]$DriveLetter = "Z"
)

$src = "F:\Src\aros-x64\examples\editor.bas"
$dst = "${DriveLetter}:\editor.bas"

$bytes = [System.IO.File]::ReadAllBytes($src)
$stream = [System.IO.File]::Open($dst, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
$stream.Write($bytes, 0, $bytes.Length)
$stream.Flush($true)
$stream.Close()

Write-Host "Wrote $dst ($($bytes.Length) bytes)"
Write-VolumeCache -DriveLetter $DriveLetter
Write-Host "Flushed volume cache for ${DriveLetter}: -- safe to detach now."
