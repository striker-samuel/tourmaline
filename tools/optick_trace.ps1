# Optick .opt trace analyzer (PowerShell, no external deps).
#
# Parses the binary capture format written by Optick's save callback (the
# engine's --bench runs write bench.opt next to the exe):
#
#   file:    [magic u32 = 0xB50FB50F][version u16][flags u16]
#            flags bit0 (1) = gzip stream, bit1 (2) = zlib stream
#   stream:  sequence of DataResponse messages:
#            [version u32 = 26][payload size u32][type u16][application u16 = 0xB50F][payload bytes]
#   type 0 (FrameDescriptionBoard): board number, QPC frequency, thread
#            names, event-description names (the string table).
#   type 1 (EventFrame): per-thread scope: board + thread ids, then the
#            category + event arrays of {start i64, finish i64, descIndex u32}.
#   other types (sampling/tags/sync/summary/frames packs): skipped.
#
# Usage:
#   powershell -File tools/optick_trace.ps1 -Path bench.opt -Scope GfxUploadSeed
#   powershell -File tools/optick_trace.ps1 -Path bench.opt -Scope GfxUploadSeed -Top 20 -Neighbors
#   powershell -File tools/optick_trace.ps1 -Path bench.opt -Scope UploadFlush
#
# -Scope      event name to summarize (exact match; prints a hint list of
#             Seed/Upload/Flush names when nothing matches).
# -Top N      print the N slowest instances (default 10).
# -Neighbors  for the single worst instance: dump every same-thread event
#             overlapping its interval (parents show as "<== CONTAINS"),
#             which answers "what was this thread blocked on / inside".
#
# Duration histogram thresholds are always printed (1us .. 10ms), plus
# the per-second rate of each, so before/after A/B is one glance.
param(
    [Parameter(Mandatory=$true)][string]$Path,
    [string]$Scope = "GfxUploadSeed",
    [int]$Top = 10,
    [switch]$Neighbors
)

$ErrorActionPreference = "Stop"
function Read-Str([System.IO.BinaryReader]$r) {
    $len = $r.ReadUInt32()
    if ($len -gt 0) {
        $b = $r.ReadBytes([int]$len)
        return [System.Text.Encoding]::ASCII.GetString($b)
    }
    return ""
}

$raw = [System.IO.File]::ReadAllBytes($Path)
if ($raw.Length -lt 16) { throw "too small" }
$magic = [BitConverter]::ToUInt32($raw, 0)
$magicWant = [uint32]::Parse("B50FB50F", [System.Globalization.NumberStyles]::HexNumber)
if ($magic -ne $magicWant) { throw ("bad magic: 0x{0:X}" -f $magic) }
$flags = [BitConverter]::ToUInt16($raw, 6)

$ms = New-Object System.IO.MemoryStream(,$raw)
$ms.Seek(8, 'Begin') | Out-Null
$decomp = $null
if (($flags -band 1) -ne 0) {
    # flag 1: gzip stream (1F 8B magic)
    $gz = New-Object System.IO.Compression.GZipStream($ms, [System.IO.Compression.CompressionMode]::Decompress)
    $out = New-Object System.IO.MemoryStream
    $gz.CopyTo($out)
    $gz.Dispose()
    $decomp = $out
}
elseif (($flags -band 2) -ne 0) {
    # flag 2: zlib stream (78 xx magic + raw deflate + adler32).
    # .NET Framework DeflateStream wants raw deflate: skip the 2-byte
    # zlib header, ignore the 4-byte trailing checksum.
    $ms.Seek(2, 'Current') | Out-Null
    $df = New-Object System.IO.Compression.DeflateStream($ms, [System.IO.Compression.CompressionMode]::Decompress)
    $out = New-Object System.IO.MemoryStream
    $df.CopyTo($out)
    $df.Dispose()
    $decomp = $out
}
else {
    $decomp = $ms
}
$decomp.Seek(0, 'Begin') | Out-Null
$r = New-Object System.IO.BinaryReader($decomp)

$boards = @{}          # boardNumber -> @{ freq; threads=[names]; descs=[names] }
$events = New-Object System.Collections.Generic.List[object]  # all events
$msgCount = 0
while ($decomp.Position -lt $decomp.Length) {
    if ($decomp.Length - $decomp.Position -lt 12) { break }
    $ver = $r.ReadUInt32()
    if ($ver -ne 26) { throw "bad protocol version $ver at pos $($decomp.Position)" }
    $size = $r.ReadUInt32()
    $type = $r.ReadUInt16()
    $app = $r.ReadUInt16()
    $msgCount++
    $payloadStart = $decomp.Position
    if ($type -eq 0) {
        # FrameDescriptionBoard
        $bn = $r.ReadUInt32()
        $freq = $r.ReadUInt64()
        $origin = $r.ReadUInt64()
        $prec = $r.ReadUInt32()
        [void]$r.ReadInt64(); [void]$r.ReadInt64()  # timeSlice
        $tCount = $r.ReadUInt32()
        $threads = New-Object System.Collections.Generic.List[string]
        for ($i = 0; $i -lt $tCount; $i++) {
            [void]$r.ReadUInt64()      # threadID
            [void]$r.ReadUInt32()      # processID
            $threads.Add((Read-Str $r)) | Out-Null
            [void]$r.ReadInt32(); [void]$r.ReadInt32(); [void]$r.ReadUInt32()
        }
        $fCount = $r.ReadUInt32()
        for ($i = 0; $i -lt $fCount; $i++) { [void]$r.ReadUInt64() }
        [void]$r.ReadUInt32()          # forcedMainThreadIndex
        $dCount = $r.ReadUInt32()
        $descs = New-Object System.Collections.Generic.List[string]
        for ($i = 0; $i -lt $dCount; $i++) {
            $name = Read-Str $r
            [void](Read-Str $r)        # file
            [void]$r.ReadUInt32()      # line
            [void]$r.ReadUInt32()      # filter
            [void]$r.ReadUInt32()      # color
            [void]$r.ReadSingle()      # float pad
            [void]$r.ReadByte()        # flags
            $descs.Add($name) | Out-Null
        }
        [void]$r.ReadUInt32(); [void]$r.ReadUInt32(); [void]$r.ReadUInt32(); [void]$r.ReadUInt32()  # tags/run/filters/threaddescs
        [void]$r.ReadUInt32()          # mode
        $pCount = $r.ReadUInt32()
        for ($i = 0; $i -lt $pCount; $i++) {
            [void]$r.ReadUInt32(); [void](Read-Str $r); [void]$r.ReadUInt64()
        }
        $tdCount = $r.ReadUInt32()
        for ($i = 0; $i -lt $tdCount; $i++) {
            [void]$r.ReadUInt64(); [void]$r.ReadUInt32(); [void](Read-Str $r)
            [void]$r.ReadInt32(); [void]$r.ReadInt32(); [void]$r.ReadUInt32()
        }
        [void]$r.ReadUInt32(); [void]$r.ReadUInt32()  # pid, cores
        $boards[[int]$bn] = @{ freq = $freq; threads = $threads; descs = $descs }
        $leftover = $size - ($decomp.Position - $payloadStart)
        if ($leftover -ne 0) { Write-Host ("board {0}: leftover {1} bytes (parse drift!)" -f $bn, $leftover) -ForegroundColor Yellow }
    }
    elseif ($type -eq 1) {
        # EventFrame (ScopeData)
        $bn = $r.ReadUInt32()
        $tn = $r.ReadInt32()
        [void]$r.ReadInt32()           # fiberNumber
        [void]$r.ReadInt64(); [void]$r.ReadInt64()  # header event time
        [void]$r.ReadInt32()           # frame type
        foreach ($isCat in @($true, $false)) {
            $cnt = $r.ReadUInt32()
            for ($i = 0; $i -lt $cnt; $i++) {
                $s = $r.ReadInt64(); $f = $r.ReadInt64(); $d = $r.ReadUInt32()
                if (-not $isCat) {
                    $events.Add([pscustomobject]@{ bn = [int]$bn; tn = [int]$tn; s = $s; f = $f; d = [int]$d })
                }
            }
        }
    }
    else {
        # skip payload (Sampling/Tags/Sync/Summary/Frames packs etc.)
        [void]$decomp.Seek($size, 'Current')
    }
    $consumed = $decomp.Position - $payloadStart
    if ($consumed -lt $size) { [void]$decomp.Seek($size - $consumed, 'Current') }
}

# Resolve names
$boardList = $boards.Keys | Sort-Object
$descCache = @{}  # bn -> descs list
foreach ($k in $boardList) { $descCache[$k] = $boards[$k].descs }
$threadCache = @{}
foreach ($k in $boardList) { $threadCache[$k] = $boards[$k].threads }

function Get-Desc($bn, $d) {
    $list = $descCache[$bn]
    if ($null -eq $list) { return "?(board $bn)" }
    if ($d -lt $list.Count) { return $list[$d] }
    return "?(desc $d)"
}
function Get-Thread($bn, $tn) {
    $list = $threadCache[$bn]
    if ($null -eq $list) { return "?(board $bn)" }
    if ($tn -ge 0 -and $tn -lt $list.Count) { return $list[$tn] }
    return "?(thread $tn)"
}

$freq = if ($boards.Count -gt 0) { $boards[($boardList[0])].freq } else { 10000000 }
$msPerTick = 1000.0 / $freq

Write-Host ("=== {0} ===" -f (Split-Path $Path -Leaf))
Write-Host ("messages: {0}  boards: {1}  events: {2}  freq: {3}" -f $msgCount, ($boardList -join ','), $events.Count, $freq)
foreach ($b in $boardList) {
    Write-Host ("board {0}: {1} threads [{2}], {3} descriptions" -f $b, $boards[$b].threads.Count, (($boards[$b].threads | Select-Object -First 6) -join ', '), $boards[$b].descs.Count)
}

# Scope stats
$hits = New-Object System.Collections.Generic.List[object]
foreach ($e in $events) {
    $n = Get-Desc $e.bn $e.d
    if ($n -eq $Scope) {
        $hits.Add([pscustomobject]@{
            Name = $n; Thread = (Get-Thread $e.bn $e.tn)
            StartMs = [double]$e.s * $msPerTick; DurMs = ([double]$e.f - [double]$e.s) * $msPerTick
        })
    }
}
if ($hits.Count -eq 0) {
    Write-Host ("no events named '{0}' found" -f $Scope)
    # list scope names containing 'Seed' or 'Upload' to help
    $names = @{}
    foreach ($e in $events) { $names[(Get-Desc $e.bn $e.d)] = $true }
    $interesting = $names.Keys | Where-Object { $_ -match 'Seed|Upload|Flush' } | Sort-Object
    Write-Host ("scopes matching Seed/Upload/Flush: " + ($interesting -join ', '))
    return
}
$durs = $hits | ForEach-Object { $_.DurMs }
$avg = ($durs | Measure-Object -Average).Average
$min = ($durs | Measure-Object -Minimum).Minimum
$max = ($durs | Measure-Object -Maximum).Maximum
Write-Host ("--- '{0}': n={1}  min={2:N4} ms  avg={3:N4} ms  max={4:N4} ms" -f $Scope, $hits.Count, $min, $avg, $max)
if ($hits.Count -gt 0) {
    $span = ($hits | Measure-Object StartMs -Maximum).Maximum - ($hits | Measure-Object StartMs -Minimum).Minimum
    foreach ($th in @(0.001, 0.01, 0.1, 1.0, 5.0, 10.0)) {
        $c = @($durs | Where-Object { $_ -ge $th }).Count
        Write-Host ("  >= {0,6:N3} ms : {1,5}  ({2:N3}/s)" -f $th, $c, $(if ($span -gt 0) { $c / ($span / 1000.0) } else { 0 }))
    }
    Write-Host ("  span: {0:N1} s" -f ($span / 1000.0))
}
$hits | Sort-Object DurMs -Descending | Select-Object -First $Top |
    ForEach-Object { Write-Host ("  {0,10:N4} ms  @ {1,12:N3} ms  [{2}]" -f $_.DurMs, $_.StartMs, $_.Thread) }

if ($Neighbors) {
    # For the worst hit: show same-thread events containing/adjacent to it
    $worst = $hits | Sort-Object DurMs -Descending | Select-Object -First 1
    Write-Host ("--- neighbors of worst '{0}' ({1:N4} ms @ {2:N3} ms, thread {3})" -f $Scope, $worst.DurMs, $worst.StartMs, $worst.Thread)
    $wStart = $worst.StartMs; $wEnd = $worst.StartMs + $worst.DurMs
    $sameThread = $events | Where-Object { (Get-Thread $_.bn $_.tn) -eq $worst.Thread }
    $near = $sameThread | ForEach-Object {
        $s = [double]$_.s * $msPerTick; $f = [double]$_.f * $msPerTick
        [pscustomobject]@{ Name = (Get-Desc $_.bn $_.d); StartMs = $s; EndMs = $f; DurMs = $f - $s }
    } | Where-Object { $_.StartMs -le $wEnd -and $_.EndMs -ge ($wStart - 0.5) } |
        Sort-Object StartMs
    $near | ForEach-Object {
        $rel = ""
        if ($_.StartMs -le $wStart -and $_.EndMs -ge $wEnd) { $rel = " <== CONTAINS" }
        Write-Host ("  {0,12:N3} -> {1,12:N3}  {2,9:N3} ms  {3}{4}" -f $_.StartMs, $_.EndMs, $_.DurMs, $_.Name, $rel)
    }
}
