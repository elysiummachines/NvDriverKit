# Regenerates res/app.ico.
#
# Windows picks the closest stored size rather than scaling one bitmap, so a
# purpose-drawn image is emitted at every size the shell asks for. Sizes up to
# 64 are stored as 32-bit DIBs; 128 and 256 as PNG, which is what the shell
# expects for the large ones and keeps the file small.
#
#   powershell -ExecutionPolicy Bypass -File res\make-icon.ps1
#
# Then rebuild so windres picks the new icon up.

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$OutFile = Join-Path $PSScriptRoot 'app.ico'
$Sizes   = @(16, 20, 24, 32, 40, 48, 64, 128, 256)

function New-RoundRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x,       $y,       $d, $d, 180, 90)
    $p.AddArc($x+$w-$d, $y,       $d, $d, 270, 90)
    $p.AddArc($x+$w-$d, $y+$h-$d, $d, $d,   0, 90)
    $p.AddArc($x,       $y+$h-$d, $d, $d,  90, 90)
    $p.CloseFigure()
    return $p
}

# A green rounded chip with a white download arrow: a bold silhouette that
# still reads at 16px, where fine detail turns to mush.
function Draw-Icon([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $pad  = $s * 0.06
    $side = $s - 2*$pad
    $body = New-RoundRect $pad $pad $side $side ($s * 0.22)

    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
                (New-Object System.Drawing.PointF(0,0)),
                (New-Object System.Drawing.PointF(0,[single]$s)),
                [System.Drawing.Color]::FromArgb(255,63,185,80),
                [System.Drawing.Color]::FromArgb(255,26,116,54))
    $g.FillPath($grad, $body)

    # The rim only helps once there are pixels to spare for it.
    if ($s -ge 32) {
        $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(70,0,0,0), [single]($s*0.035))
        $g.DrawPath($pen, $body)
        $pen.Dispose()
    }

    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
    $cx    = $s/2
    $stemW = $s*0.17
    $top   = $s*0.25
    $hTop  = $s*0.52
    $bot   = $s*0.79
    $aw    = $s*0.46
    $g.FillRectangle($white, [single]($cx-$stemW/2), [single]$top, [single]$stemW, [single]($hTop-$top))
    $g.FillPolygon($white, @(
        (New-Object System.Drawing.PointF([single]($cx-$aw/2), [single]$hTop)),
        (New-Object System.Drawing.PointF([single]($cx+$aw/2), [single]$hTop)),
        (New-Object System.Drawing.PointF([single]$cx,         [single]$bot))
    ))

    $white.Dispose(); $grad.Dispose(); $body.Dispose(); $g.Dispose()
    return $bmp
}

# 32bpp bottom-up DIB: BITMAPINFOHEADER with doubled height, BGRA pixels,
# then an all-zero AND mask (the alpha channel does the masking).
function Get-Dib([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([int]40); $bw.Write([int]$w); $bw.Write([int]($h*2))
    $bw.Write([int16]1); $bw.Write([int16]32); $bw.Write([int]0)
    $bw.Write([int]($w*$h*4)); $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]0)
    for ($y = $h-1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $w; $x++) {
            $c = $bmp.GetPixel($x,$y)
            $bw.Write([byte]$c.B); $bw.Write([byte]$c.G); $bw.Write([byte]$c.R); $bw.Write([byte]$c.A)
        }
    }
    $stride = [int][math]::Floor(($w + 31) / 32) * 4
    $zero = New-Object byte[] $stride
    for ($y = 0; $y -lt $h; $y++) { $bw.Write($zero) }
    $bw.Flush()
    $bytes = $ms.ToArray(); $bw.Dispose(); $ms.Dispose()
    return ,$bytes
}

$images = @()
foreach ($s in $Sizes) {
    $b = Draw-Icon $s
    if ($s -ge 128) {
        $ms = New-Object System.IO.MemoryStream
        $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $images += ,@{ Size = $s; Data = $ms.ToArray() }
        $ms.Dispose()
    } else {
        $images += ,@{ Size = $s; Data = (Get-Dib $b) }
    }
    $b.Dispose()
}

$out = New-Object System.IO.MemoryStream
$w2  = New-Object System.IO.BinaryWriter($out)
$w2.Write([int16]0); $w2.Write([int16]1); $w2.Write([int16]$images.Count)   # ICONDIR
$offset = 6 + 16*$images.Count
foreach ($im in $images) {
    $d = if ($im.Size -ge 256) { 0 } else { $im.Size }   # 256 is encoded as 0
    $w2.Write([byte]$d); $w2.Write([byte]$d); $w2.Write([byte]0); $w2.Write([byte]0)
    $w2.Write([int16]1); $w2.Write([int16]32)
    $w2.Write([int]$im.Data.Length); $w2.Write([int]$offset)
    $offset += $im.Data.Length
}
foreach ($im in $images) { $w2.Write($im.Data) }
$w2.Flush()
[System.IO.File]::WriteAllBytes($OutFile, $out.ToArray())
$w2.Dispose(); $out.Dispose()

# The README header shows the same icon, so write the 256px image out on its
# own as well. It is the PNG already built above, so the two cannot drift.
$png256 = ($images | Where-Object { $_.Size -eq 256 }).Data
[System.IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'icon.png'), $png256)

$f = Get-Item $OutFile
"Wrote {0} ({1:N0} bytes, {2} sizes: {3})" -f $f.Name, $f.Length, $images.Count, ($Sizes -join ', ')
