Add-Type -AssemblyName System.Drawing

$W = 8
$H = 16
$bmp = New-Object System.Drawing.Bitmap($W, $H)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor

$font = New-Object System.Drawing.Font("Consolas", 12, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$brushBg = [System.Drawing.Brushes]::Black
$brushFg = [System.Drawing.Brushes]::White
$fmt = New-Object System.Drawing.StringFormat
$fmt.FormatFlags = [System.Drawing.StringFormatFlags]::NoClip -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces

$lines = @()
$lines += "; Auto-generated 8x16 monochrome bitmap font, ASCII 32..126."
$lines += "; One row = one glyph = 16 bytes (1 byte per scanline, MSB = leftmost pixel)."
$lines += "font8x16:"

for ($c = 32; $c -le 126; $c++) {
    $g.Clear([System.Drawing.Color]::Black)
    $ch = [char]$c
    $g.DrawString($ch, $font, $brushFg, -1.0, -2.0, $fmt)
    $rows = @()
    for ($y = 0; $y -lt $H; $y++) {
        $byte = 0
        for ($x = 0; $x -lt $W; $x++) {
            $px = $bmp.GetPixel($x, $y)
            $on = ($px.R -gt 100)
            if ($on) { $byte = $byte -bor (0x80 -shr $x) }
        }
        $rows += $byte
    }
    $hex = ($rows | ForEach-Object { "0x{0:X2}" -f $_ }) -join ", "
    $comment = $ch
    if ($c -eq 32) { $comment = "space" }
    $lines += "    db $hex  ; $c '$comment'"
}

$lines -join "`r`n" | Out-File -FilePath "F:\Src\aros-x64\kernel\font8x16.inc" -Encoding ascii
Write-Host "Wrote font8x16.inc"
