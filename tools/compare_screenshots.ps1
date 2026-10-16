# Copyright (c) 2026 Samuel Martin
# SPDX-License-Identifier: MIT

# Screenshot A/B comparison (PowerShell, System.Drawing only — no external
# deps). Built for the TOURMALINE_CAPTURE=<frame> bench flow:
#
#   1. run A/B  (e.g. TOURMALINE_SHADERLOD=0 vs 1, camera frozen with
#      --disable_control, TOURMALINE_AA=off so T2x jitter cannot shift
#      pixels), let each run export exports/screenshot_*.png,
#   2. diff the pair with this script.
#
# Determinism check first: two IDENTICAL runs should report a tiny
# baseline (a few hundred pixels of exposure-readback noise at most).
# Anything far above that baseline in the A/B pair is the feature under
# test (e.g. farthest-LOD vertex-lit shading repainting distant
# geometry).
#
# Usage:
#   powershell -File tools/compare_screenshots.ps1 -Reference a.png -Compare b.png
#   powershell -File tools/compare_screenshots.ps1 -Reference a.png -Compare b.png -Stride 1 -Threshold 12
#   powershell -File tools/compare_screenshots.ps1 -Reference f5.png -WhiteCount          # near-white pixel census (F5 shader-LOD overlay check)
#
# -Reference   first PNG (required).
# -Compare     second PNG for diff mode (omit with -WhiteCount for the
#              single-file census).
# -Threshold   sum-of-channels |delta| that counts a pixel as different
#              (default 6; 0..765 scale, ACES-tonemapped noise sits well
#              below it).
# -Stride      sample every Nth pixel row/column (default 2; 1 = full
#              resolution, 2 = 4x faster with the same signal).
# -WhiteCount  count near-white pixels in -Reference instead of diffing
#              (-WhiteMin 0..255, default 230). Note ACES maps authored
#              white to ~224 sRGB, so lower -WhiteMin (e.g. 200) when the
#              pixels ride the HDR -> tonemap chain.
# -DiffOut     optional path: write a PNG where differing pixels are red
#              and the rest are dimmed (visual "what changed" map).
#
# Exit codes: 0 = identical within threshold, 1 = differences found
# (or non-white pixels counted), 2 = usage/IO error. Scriptable A/B.
param(
    [Parameter(Mandatory=$true)][string]$Reference,
    [string]$Compare,
    [int]$Threshold = 6,
    [int]$Stride = 2,
    [switch]$WhiteCount,
    [int]$WhiteMin = 230,
    [string]$DiffOut = ""
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

function Lock-Rgb([System.Drawing.Bitmap]$bmp, [ref]$strideOut) {
    # 24bpp RGB copy of the bitmap + its stride (LockBits memory order is
    # BGRA on little-endian; 24bpp makes the R,G,B triplets contiguous).
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $bytes = New-Object byte[] ($data.Stride * $data.Height)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data)
    $strideOut.Value = $data.Stride
    return $bytes
}

try { $a = [System.Drawing.Bitmap]::FromFile($Reference) } catch { Write-Host "cannot open -Reference: $Reference"; exit 2 }

if ($WhiteCount) {
    # ---- Single-file near-white census --------------------------------
    $sa = 0; $ba = Lock-Rgb $a ([ref]$sa)
    $white = 0; $n = 0
    for ($y = 0; $y -lt $a.Height; $y += $Stride) {
        $row = $y * $sa
        for ($x = 0; $x -lt $a.Width; $x += $Stride) {
            $i = $row + $x * 3
            $n++
            if ($ba[$i+2] -ge $WhiteMin -and $ba[$i+1] -ge $WhiteMin -and $ba[$i] -ge $WhiteMin) { $white++ }
        }
    }
    Write-Host ("{0}: near-white(>={1}) px {2}/{3} ({4:P2})" -f `
        (Split-Path $Reference -Leaf), $WhiteMin, $white, $n, ($white / [double]$n))
    $a.Dispose()
    if ($white -gt 0) { exit 1 } else { exit 0 }
}

if (-not $Compare) { Write-Host "-Compare required unless -WhiteCount is set"; $a.Dispose(); exit 2 }
try { $b = [System.Drawing.Bitmap]::FromFile($Compare) } catch { Write-Host "cannot open -Compare: $Compare"; $a.Dispose(); exit 2 }

if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) {
    Write-Host ("size mismatch: {0}x{1} vs {2}x{3}" -f $a.Width, $a.Height, $b.Width, $b.Height)
    $a.Dispose(); $b.Dispose(); exit 2
}

# ---- Diff mode ----------------------------------------------------------
$sa = 0; $sb = 0
$ba = Lock-Rgb $a ([ref]$sa)
$bb = Lock-Rgb $b ([ref]$sb)

$diff = 0; $sum = 0.0; $n = 0
$diffBytes = $null
if ($DiffOut) { $diffBytes = New-Object byte[] ($sa * $a.Height) }  # black base, red pixels

for ($y = 0; $y -lt $a.Height; $y += $Stride) {
    $rowA = $y * $sa; $rowB = $y * $sb
    for ($x = 0; $x -lt $a.Width; $x += $Stride) {
        $iA = $rowA + $x * 3; $iB = $rowB + $x * 3
        $d = [Math]::Abs($ba[$iA] - $bb[$iB]) + [Math]::Abs($ba[$iA+1] - $bb[$iB+1]) `
            + [Math]::Abs($ba[$iA+2] - $bb[$iB+2])
        $sum += $d; $n++
        $isDiff = ($d -gt $Threshold)
        if ($isDiff) {
            $diff++
            if ($diffBytes) {
                # mark the sampled pixel red in the visualization
                $i = $rowA + $x * 3
                $diffBytes[$i] = 0; $diffBytes[$i+1] = 0; $diffBytes[$i+2] = 255
            }
        }
        if ($diffBytes -and -not $isDiff) {
            # dim survivors so changed regions pop
            $i = $rowA + $x * 3
            $diffBytes[$i] = $ba[$iA] / 4; $diffBytes[$i+1] = $ba[$iA+1] / 4; $diffBytes[$i+2] = $ba[$iA+2] / 4
        }
    }
}

Write-Host ("{0} vs {1}: size {2}x{3}" -f (Split-Path $Reference -Leaf), (Split-Path $Compare -Leaf), $a.Width, $a.Height)
Write-Host ("  sampled {0}  differing(>{1}/765) {2}  ({3:P2})  avg delta {4:F2}" -f `
    $n, $Threshold, $diff, ($diff / [double]$n), ($sum / $n))

if ($DiffOut -and $diffBytes) {
    $out = New-Object System.Drawing.Bitmap($a.Width, $a.Height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $out.Width, $out.Height)
    $data = $out.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly,
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    [System.Runtime.InteropServices.Marshal]::Copy($diffBytes, 0, $data.Scan0, $diffBytes.Length)
    $out.UnlockBits($data)
    $out.Save($DiffOut, [System.Drawing.Imaging.ImageFormat]::Png)
    $out.Dispose()
    Write-Host ("  diff map: {0}" -f $DiffOut)
}

$a.Dispose(); $b.Dispose()
if ($diff -gt 0) { exit 1 } else { exit 0 }
