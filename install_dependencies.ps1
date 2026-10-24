# install_dependencies.ps1 - fetches the non-submodule third-party trees into external/.
#
# The vendored trees moved OUT of git: the repo keeps only the small integration
# files (hand-written CMakeLists, pinned-modified sources, manifests) tracked on
# disk; everything else under the affected external/ folders arrives here, from
# pinned upstream archives / raw files, byte-checked by SHA256.
#
# Idempotent: re-running skips deps whose .depstamp already matches. Use
# -Force to re-fetch everything, -IncludeUnused to also fetch the two
# intentionally-unwired trees (box3d, zlib).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File install_dependencies.ps1
#   powershell -ExecutionPolicy Bypass -File install_dependencies.ps1 -Force
#   powershell -ExecutionPolicy Bypass -File install_dependencies.ps1 -IncludeUnused
#
# Linux: run install_dependencies.sh instead (same pins; skips the
# Windows-only binary packages: SDL3 prebuilt, dxc, Streamline).

param(
    [switch]$Force,
    [switch]$IncludeUnused
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Ext = Join-Path $RepoRoot 'external'
$Deps = Join-Path $RepoRoot 'tools\deps'
$Cache = Join-Path $Ext '.downloads'
New-Item -ItemType Directory -Force -Path $Cache | Out-Null
$UA = @{ 'User-Agent' = 'tourmaline-install-deps' }

# ---------------------------------------------------------------------------
# Pin table. type: raw (single file), tar (codeload archive, strip 1), zip.
# subset: only copy these top-level entries from the extracted archive.
# exclude/overlay: per-dep manifests under tools/deps/ (forward-slash paths).
#   - <dep>.exclude : upstream-only cruft (CI, docs, tests, samples) NOT copied
#   - <dep>.overlay : tracked files that the fetch must never overwrite
# ---------------------------------------------------------------------------
$Pins = @(
    @{ name='VMA';        type='raw';  target='VulkanMemoryAllocator';
       url='https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/68d1c9733f56f681613d6b3d728c1af4dca8c2b6/include/vk_mem_alloc.h';
       file='vk_mem_alloc.h';
       sha256='af2e8ed9cabc33bd5a027d480aaac14309aad741763427fa39ffdcf98561c52b' }
    @{ name='stb-image';  type='raw';  target='stb';
       url='https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h';
       file='stb_image.h';
       sha256='594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3' }
    @{ name='stb-write'; type='raw';  target='stb';
       url='https://raw.githubusercontent.com/nothings/stb/1ee679ca2ef753a528db5ba6801e1067b40481b8/stb_image_write.h';
       file='stb_image_write.h';
       sha256='cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05' }
    @{ name='tinygltf';  type='raw';  target='tinygltf';
       url='https://raw.githubusercontent.com/syoyo/tinygltf/v2.8.10/tiny_gltf.h';
       file='tiny_gltf.h';
       sha256='777b94a0d7b4876b521cea53630a5bbd55aff693ef49586a55fb273278e96bf5' }
    @{ name='lz4';        type='raw';  target='lz4'; multi=$true;
       urls=@('https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/lz4.h',
              'https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/lz4.c');
       files=@('lz4.h','lz4.c');
       sha256=@('26b82efc53d1570f3b54eef02e9c4764c1ad374ff03cac04e2ced5ea4d4c552f',
                '9396f7de527bc8435de9c7569fb7998e56545a84b4f3c2d808c0235c01774539') }
    @{ name='SDL3';       type='zip';  target='SDL3-3.4.16'; windows=$true;
       url='https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-devel-3.4.16-VC.zip';
       sha256='1a784cb2a5c64d56fe7a62090fe9d242d9865f235e4ea9678f1a6ba4e693e7de';
       # the official MSVC development package: cmake configs + headers +
       # prebuilt import libs/DLLs (the plain SDL3-3.4.16.zip is the SOURCE tree)
       subset=@('cmake','include','lib','.git-hash','INSTALL.md','LICENSE.txt','README.md') }
    @{ name='dxc';        type='zip';  target='dxc'; windows=$true;
       url='https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/dxc_2026_07_29.zip';
       sha256='a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241' }
    @{ name='streamline'; type='zip';  target='streamline'; windows=$true;
       url='https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.1/streamline-sdk-v2.14.1.zip';
       sha256='92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b';
       # include/ + the production bin/x64 runtime set + the top licenses
       subset=@('include','bin/x64','license.txt','3rd-party-licenses.md');
       drop=@('bin/x64/development') }
    @{ name='sqlite-amalgamation'; type='zip'; target='SQLiteCpp'; into='sqlite3';
       url='https://sqlite.org/2026/sqlite-amalgamation-3530400.zip';
       sha256='1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d';
       subset=@('sqlite3.c','sqlite3.h') }
    @{ name='SQLiteCpp';  type='tar';  target='SQLiteCpp';
       url='https://codeload.github.com/SRombauts/SQLiteCpp/tar.gz/8a63915c06988988b8f20b434fcb34666ab72e38';
       sha256='ae81b10191346114fbeb4b937943f65accf8647050426ae71dd0e33973acb144' }
    @{ name='embree';     type='tar';  target='embree';
       url='https://codeload.github.com/embree/embree/tar.gz/3d9cb89b9ea099c630e6272d37767e7dd4e78e74';
       sha256='65a7d6014f546844a69201ff1ed0e892c49aa74171db8a7eeffa86f6ef0be78f' }
    @{ name='optick';     type='tar';  target='optick';
       url='https://codeload.github.com/bombomby/optick/tar.gz/362d3952d35982f2037e4a74df4706486d590cfe';
       sha256='771582d71050630c76aecb1a759443eeb902cef3a16620b8167e5223cd892466' }
    @{ name='imgui';      type='tar';  target='imgui';
       # docking branch, "Version 1.92.3" sync commit
       url='https://codeload.github.com/ocornut/imgui/tar.gz/1c544ee941';
       sha256='d56a25a7b0594e391b79e7d3cb47a46cb87387de20a8dd1b6dd28b3b7def4dc7' }
    @{ name='xatlas';     type='tar';  target='xatlas';
       url='https://codeload.github.com/jpcy/xatlas/tar.gz/1f101e2983b0bb91f5cebff80306d0ee8710f11e';
       sha256='5c7c3dfff9e40e32c31f1ecc3e8f6af376545008a03dfdb1fddbf5491f1b4dcb' }
    @{ name='box3d';      type='tar';  target='box3d'; unused=$true;
       url='https://codeload.github.com/erincatto/box3d/tar.gz/954cf879e717334eb96da9a5c255788fdc9f5f87';
       sha256='d9f15a080708120d4c9dfcc589bfb629f999646402c604fe27d2d4d1f8266504' }
    @{ name='zlib';       type='tar';  target='zlib'; unused=$true;
       url='https://codeload.github.com/madler/zlib/tar.gz/51b2667cb4e02a583a8be55abd995421dccbfec0';
       sha256='bbe3653640497bde28c86c99ca33bbdb38a3b3ef15bd99905b97c8b2863ac80a' }
)

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
function Read-Manifest([string]$path) {
    if (Test-Path $path) { return @(Get-Content $path | Where-Object { $_ -ne '' }) }
    return @()
}

function Is-Excluded([string]$rel, $excludeList) {
    foreach ($e in $excludeList) {
        if ($rel -eq $e) { return $true }
        if ($rel.StartsWith($e + '/')) { return $true }
    }
    return $false
}

function Get-FileSHA([string]$path) {
    return (Get-FileHash $path -Algorithm SHA256).Hash.ToLower()
}

function Download([string]$url, [string]$dst) {
    $name = Split-Path -Leaf $dst
    if ((Test-Path $dst) -and ((Get-FileSHA $dst) -eq $script:expectedSHA)) {
        Write-Host "  cached: $name"
        return
    }
    Write-Host "  downloading: $url"
    Invoke-WebRequest -Uri $url -OutFile $dst -UseBasicParsing -TimeoutSec 1800 -Headers $UA
}

# Copy an extracted source tree into the target, honoring subset/include,
# exclude and overlay manifests. Never overwrites overlay files.
function Copy-Tree([string]$src, [string]$dst, $subset, $drop, $excludeList, $overlayList) {
    $overlaySet = @{}; $overlayList | ForEach-Object { $overlaySet[$_] = $true }
    $targets = @()
    if ($subset -and $subset.Count -gt 0) {
        foreach ($s in $subset) {
            $p = Join-Path $src ($s -replace '/','\')
            # a missing subset entry means the pin is stale (wrong archive
            # variant, upstream renamed something) - fail the dep instead of
            # stamping a partial tree as installed
            if (Test-Path $p) { $targets += ,@($p, $s) } else { throw "subset entry missing in archive: $s (stale pin?)" }
        }
    } else {
        $targets += ,@($src, '')
    }
    $copied = 0; $skipped = 0
    foreach ($t in $targets) {
        $tSrc, $tRel = $t
        if (Test-Path $tSrc -PathType Leaf) {
            # file entry (e.g. .git-hash, sqlite3.c): lands at the target root
            $rel = [IO.Path]::GetFileName($tSrc)
            $relDst = Join-Path $dst $rel
            Copy-Item $tSrc $relDst -Force
            $copied++
            continue
        }
        $rootLen = $tSrc.TrimEnd('\').Length + 1
        $files = Get-ChildItem $tSrc -Recurse -File
        foreach ($f in $files) {
            $rel = ($f.FullName.Substring($rootLen) -replace '\\','/')
            if ($tRel -ne '') { $rel = "$tRel/$rel" }
            if ($drop) {
                $dropped = $false
                foreach ($d in $drop) { if ($rel -eq $d -or $rel.StartsWith($d + '/')) { $dropped = $true; break } }
                if ($dropped) { $skipped++; continue }
            }
            if ((Is-Excluded $rel $excludeList)) { $skipped++; continue }
            # overlay-listed files are tracked in git and owned by it - the fetch
            # NEVER writes them, even when absent (a wiped tree gets restored by
            # git checkout, not by re-importing upstream over our pinned edits)
            if ($overlaySet.ContainsKey($rel)) { $skipped++; continue }
            $relDst = Join-Path $dst ($rel -replace '/','\')
            New-Item -ItemType Directory -Force -Path (Split-Path $relDst) | Out-Null
            Copy-Item $f.FullName $relDst -Force
            $copied++
        }
    }
    Write-Host "  copied $copied file(s), skipped $skipped (excluded/overlay)"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
$failures = @()
foreach ($pin in $Pins) {
    $name = $pin.name
    if ($pin.unused -and -not $IncludeUnused) {
        Write-Host "[$name] skipped (not wired into the build; pass -IncludeUnused to fetch)"
        continue
    }
    if ($pin.windows) {
        $isWin = ($env:OS -eq 'Windows_NT')
        if (-not $isWin) {
            Write-Host "[$name] skipped (Windows-only binary package; run install_dependencies.ps1 on Windows)"
            continue
        }
    }
    $targetDir = Join-Path $Ext $pin.target
    # per-dep stamp (external/SQLiteCpp/ is touched by TWO pins: the tree + the
    # amalgamation - a shared .depstamp would make them re-fetch each other)
    $stampFile = Join-Path $targetDir ".depstamp-$name"
    $stampId = "$($pin.url)|$($pin.sha256)"
    $upToDate = (Test-Path $stampFile) -and ((Get-Content $stampFile -Raw) -eq ($stampId + "`n"))
    if ($upToDate -and $pin.type -eq 'raw') {
        # raw pins: the stamp alone is not enough - the target file must also
        # exist (a manually deleted file with a valid stamp would otherwise
        # report "up to date" forever and starve the build)
        $wanted = if ($pin.multi) { @($pin.files) } else { @($pin.file) }
        foreach ($w in $wanted) { if (-not (Test-Path (Join-Path $targetDir $w))) { $upToDate = $false } }
    }
    if (-not $Force -and $upToDate) {
        Write-Host "[$name] up to date"
        continue
    }
    Write-Host "[$name] installing into external/$($pin.target)/"
    try {
        New-Item -ItemType Directory -Force -Path $targetDir | Out-Null

        if ($pin.type -eq 'raw') {
            if ($pin.multi) {
                for ($i = 0; $i -lt $pin.urls.Count; $i++) {
                    $u = $pin.urls[$i]; $fn = $pin.files[$i]
                    $script:expectedSHA = [string]$pin.sha256[$i]
                    $dst = Join-Path $targetDir $fn
                    Download $u $dst
                    $actual = Get-FileSHA $dst
                    if ($actual -ne $script:expectedSHA) { throw "sha256 mismatch for ${fn}: got $actual" }
                }
            } else {
                $script:expectedSHA = [string]$pin.sha256
                $dst = Join-Path $targetDir $pin.file
                Download $pin.url $dst
                $actual = Get-FileSHA $dst
                if ($actual -ne $script:expectedSHA) { throw "sha256 mismatch for $($pin.file): got $actual" }
            }
        } else {
            # archive (zip or tar): download + verify + extract + filtered copy
            $archiveName = "$name." + $(if ($pin.type -eq 'zip') { 'zip' } else { 'tar.gz' })
            $archivePath = Join-Path $Cache $archiveName
            $script:expectedSHA = [string]$pin.sha256
            Download $pin.url $archivePath
            $actual = Get-FileSHA $archivePath
            if ($actual -ne $script:expectedSHA) { throw "sha256 mismatch for $archiveName`: got $actual" }

            $extractDir = Join-Path $Cache ("extract-$name")
            if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }
            New-Item -ItemType Directory -Force -Path $extractDir | Out-Null

            if ($pin.type -eq 'zip') {
                $tmpZip = Join-Path $extractDir 'a.zip'
                Copy-Item $archivePath $tmpZip
                Expand-Archive -Path $tmpZip -DestinationPath (Join-Path $extractDir 'x') -Force
                $xdir = @(Get-ChildItem (Join-Path $extractDir 'x') -Directory)
                $xfiles = @(Get-ChildItem (Join-Path $extractDir 'x') -File)
                # single wrapper dir (e.g. SDL3-3.4.16/) -> strip; flat zip -> use as-is
                if ($xdir.Count -eq 1 -and $xfiles.Count -eq 0) {
                    $treeDir = $xdir[0].FullName
                } else {
                    $treeDir = Join-Path $extractDir 'x'
                }
            } else {
                $treeDir = Join-Path $extractDir 't'
                New-Item -ItemType Directory -Force -Path $treeDir | Out-Null
                # exit codes 1/2 are warnings: upstream tarballs carry symlinks
                # (e.g. optick samples/) that Windows tar cannot create without
                # developer mode - those trees are excluded from the copy anyway.
                tar -xzf $archivePath -C $treeDir --strip-components=1
                if ($LASTEXITCODE -gt 2) { throw "tar extraction failed for $name (exit $LASTEXITCODE)" }
            }

            # @( ) around the reads: an absent manifest must stay an empty array,
            # not $null (a $null pipeline would feed ForEach-Object one null item)
            $excludeList = @(Read-Manifest (Join-Path $Deps "$name.exclude"))
            $overlayList = @(Read-Manifest (Join-Path $Deps "$name.overlay"))
            $dst = $targetDir
            if ($pin.into) { $dst = Join-Path $dst $pin.into }
            Copy-Tree $treeDir $dst $pin.subset $pin.drop $excludeList $overlayList
        }

        [IO.File]::WriteAllText($stampFile, $stampId + "`n")
        Write-Host "[$name] done"
    } catch {
        Write-Warning "[$name] FAILED: $($_.Exception.Message)"
        Write-Warning "[$name] at line $($_.InvocationInfo.ScriptLineNumber): $($_.InvocationInfo.Line.Trim())"
        $failures += $name
    }
}

""
if ($failures.Count -gt 0) {
    Write-Warning "Failed deps: $($failures -join ', ')"
    exit 1
}
Write-Host "All dependencies installed."
Write-Host "Note: nvngx_dlss.dll (the NGX DLSS snippet) arrives with the Streamline"
Write-Host "zip; it is gitignored and never tracked. If DLSS is unwanted, just"
Write-Host "delete external/streamline/bin/x64/nvngx_dlss.dll - the engine falls"
Write-Host "back to the native render extent."
exit 0
