# Generate res/icon.ico from res/icon64.png (the Linux/desktop master icon).
#
# Windows Explorer/taskbar show a per-file icon only when it is embedded as an
# RT_GROUP_ICON resource in the .exe. The source is a 512x512 PNG; an .ico is a
# container of fixed-size images, so we resample to the standard Windows sizes
# and pack them into one ICO. Uses only .NET System.Drawing (present on every
# Windows PowerShell host) - no ImageMagick/Pillow dependency, so regenerating
# the asset needs nothing but Windows itself.
#
#   powershell -ExecutionPolicy Bypass -File scripts/make_icon.ps1
#
# The produced res/icon.ico is a tracked build asset: commit it, don't gitignore
# it. Re-run this only when res/icon64.png changes.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root   = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$root   = Split-Path -Parent $root
$src    = Join-Path $root 'res\icon64.png'
$dst    = Join-Path $root 'res\icon.ico'
$sizes  = 16, 32, 48, 64, 128, 256

if (-not (Test-Path $src)) { throw "source icon not found: $src" }

# Encode one resized bitmap as PNG bytes (PNG-compressed ICO entries are valid
# on Vista+ and keep the file small for the larger sizes).
$master = [System.Drawing.Bitmap]::new($src)
$pngs   = New-Object 'System.Collections.Generic.List[byte[]]'
foreach ($s in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.DrawImage($master, 0, 0, $s, $s)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $pngs.Add($ms.ToArray())
    $ms.Dispose()
}
$master.Dispose()

$count  = $sizes.Count
$out    = New-Object System.IO.MemoryStream
$bw     = New-Object System.IO.BinaryWriter $out

# ICONDIR: reserved=0, type=1 (icon), count
$bw.Write([UInt16]0)
$bw.Write([UInt16]1)
$bw.Write([UInt16]$count)

# Image data starts after ICONDIR (6) + count * ICONDIRENTRY (16)
$offset = 6 + 16 * $count
for ($i = 0; $i -lt $count; $i++) {
    $px    = $sizes[$i]
    $bytes = $pngs[$i]
    $bw.Write([Byte]$(if ($px -ge 256) { 0 } else { $px }))  # width  (0 == 256)
    $bw.Write([Byte]$(if ($px -ge 256) { 0 } else { $px }))  # height (0 == 256)
    $bw.Write([Byte]0)      # color count (truecolor)
    $bw.Write([Byte]0)      # reserved
    $bw.Write([UInt16]1)    # planes
    $bw.Write([UInt16]32)   # bit depth
    $bw.Write([UInt32]$bytes.Length)  # size of this entry's data
    $bw.Write([UInt32]$offset)        # offset of data from file start
    $offset += $bytes.Length
}
foreach ($bytes in $pngs) { $bw.Write($bytes) }

$bw.Flush()
[System.IO.File]::WriteAllBytes($dst, $out.ToArray())
$bw.Dispose(); $out.Dispose()

Write-Host "Wrote $dst ($((Get-Item $dst).Length) bytes, sizes: $($sizes -join ', '))"
