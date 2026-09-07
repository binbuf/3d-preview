param([string]$ShotPath)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$bmp = New-Object System.Drawing.Bitmap($ShotPath)
$w = $bmp.Width
$h = $bmp.Height

function Px($bmp, $x, $y) {
    $c = $bmp.GetPixel($x, $y)
    return @($c.R, $c.G, $c.B)
}

# 1. Toolbar band (should be ~44,44,46 dark gray)
$toolbar = Px $bmp ([int]($w / 2)) 10
Write-Host ("toolbar sample: " + ($toolbar -join ','))

# 2. Plain background (right side, below toolbar) ~28,28,30
$bg = Px $bmp ($w - 60) ([int]($h / 2))
Write-Host ("background sample: " + ($bg -join ','))

# 3. Center pixel (triangle or error card or background)
$center = Px $bmp ([int]($w / 2)) ([int](($h + 52) / 2))
Write-Host ("center sample: " + ($center -join ','))

# 4. Count pixels that differ from the expected background in the viewport
$bgColor = [System.Drawing.Color]::FromArgb(28, 28, 30)
$diff = 0
$total = 0
for ($y = 60; $y -lt $h - 50; $y += 4) {
    for ($x = 8; $x -lt $w - 8; $x += 4) {
        $c = $bmp.GetPixel($x, $y)
        $total++
        if ([Math]::Abs($c.R - $bgColor.R) -gt 18 -or [Math]::Abs($c.G - $bgColor.G) -gt 18 -or [Math]::Abs($c.B - $bgColor.B) -gt 18) { $diff++ }
    }
}
$pct = [Math]::Round(100.0 * $diff / $total, 2)
Write-Host ("non-background pixel share in viewport: $pct%  ($diff of $total samples)")

# 5. Error card detection: large centered rect of ~(34,34,38)
$isErrorCard = ($center[0] -ge 30 -and $center[0] -le 40 -and $center[1] -ge 30 -and $center[1] -le 40 -and $center[2] -ge 32 -and $center[2] -le 42)
Write-Host ("center looks like error card: $isErrorCard")

# 6. Status pill (bottom-left, dark translucent pill over background ~ (22,24,29))
$pill = Px $bmp 40 ($h - 28)
Write-Host ("pill sample: " + ($pill -join ','))

$bmp.Dispose()

if ($isErrorCard) {
    Write-Host 'VERDICT: ERROR CARD VISIBLE (load failed)'
    exit 2
}
if ($pct -lt 1.0) {
    Write-Host 'VERDICT: viewport appears empty'
    exit 3
}
Write-Host 'VERDICT: viewport shows rendered content (grid/model/pill)'
exit 0
