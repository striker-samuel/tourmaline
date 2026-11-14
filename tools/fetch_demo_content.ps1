# Fetch the Tourmaline demo content - the rolling 'demo-content' GitHub
# release carrying the cooked sponza_10 project (the .tapp runtime
# manifest + its content-addressed .pak) - into a build's content/paks
# directory.
#
# Why a release and not the git tree: the demo pak is ~100 MB of cooked
# binary content whose CONTENT-ADDRESSED name changes on every recook.
# The release is rolling (republished per recook); this script is the
# stable acquisition path.
#
# What it does:
#   1. queries the release metadata by tag (public API, no auth),
#   2. downloads the demo assets + the SHA256SUMS.txt manifest,
#   3. verifies every downloaded file against the published SHA-256,
#   4. installs them into -Dest (default: the conventional Windows
#      build dir under the repo root, wherever the caller's CWD is),
#   5. removes STALE sponza_10-*.pak files left by earlier fetches
#      (recooks change the content-addressed name; leaving the old pak
#      behind would just accumulate dead weight - the .tapp only ever
#      names the current one).
#
# Invoked by the CMake `demo_content` target (which passes -Dest), or
# directly:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_demo_content.ps1
#   powershell ... -Dest out\build\x64-Release\content\paks
#
# The demo file set is deliberately explicit (sponza_10.tapp +
# sponza_10-*.pak): the destination also hosts engine-shaders.pak and
# the user's own cooked paks, so the install/cleanup NEVER touches
# files it does not own. If the demo project ever changes identity,
# update the patterns here and in fetch_demo_content.sh alongside the
# release.

param(
  [string]$Dest = "",
  [string]$Repo = "striker-samuel/tourmaline",
  [string]$Tag = "demo-content"
)

$ErrorActionPreference = "Stop"

# Default destination: <repo-root>\out\build\x64-Release\content\paks
# (resolved from the script's own location so CWD does not matter).
if (-not $Dest) {
  $Dest = Join-Path (Split-Path $PSScriptRoot -Parent) "out\build\x64-Release\content\paks"
}

function Fail([string]$Message) {
  Write-Host "error: $Message" -ForegroundColor Red
  exit 1
}

$releaseUrl = "https://api.github.com/repos/$Repo/releases/tags/$Tag"
try {
  $release = Invoke-RestMethod -Uri $releaseUrl -TimeoutSec 30 `
    -Headers @{ "User-Agent" = "tourmaline-demo-fetch" }
} catch {
  Fail "could not query the '$Tag' release ($releaseUrl): $($_.Exception.Message). Check the network, or browse https://github.com/$Repo/releases/tag/$Tag and download the assets manually into '$Dest'."
}

# The demo assets: the manifest + the content-addressed pak, plus the
# checksum manifest. Anything else on the release is ignored.
$assets = @($release.assets)
$sumsAsset = $assets | Where-Object { $_.name -eq "SHA256SUMS.txt" }
$contentAssets = @($assets | Where-Object {
  $_.name -eq "sponza_10.tapp" -or $_.name -like "sponza_10-*.pak"
})
if ($contentAssets.Count -eq 0) { Fail "the '$Tag' release carries no demo assets (sponza_10.tapp / sponza_10-*.pak)." }
if (-not $sumsAsset)            { Fail "the '$Tag' release carries no SHA256SUMS.txt manifest - cannot verify downloads." }

Write-Host "demo-content release '$($release.name)': downloading $($contentAssets.Count) file(s) to a temp dir..."

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("tourmaline-demo-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
  foreach ($a in $contentAssets) {
    Invoke-WebRequest -Uri $a.browser_download_url -OutFile (Join-Path $tmp $a.name) -TimeoutSec 600
  }
  Invoke-WebRequest -Uri $sumsAsset.browser_download_url -OutFile (Join-Path $tmp "SHA256SUMS.txt") -TimeoutSec 60

  # Verify: every downloaded demo file must have a matching published
  # checksum (and every checksum line must correspond to a downloaded
  # file - a half-updated release fails loudly, never installs).
  $expected = @{}
  foreach ($line in (Get-Content (Join-Path $tmp "SHA256SUMS.txt"))) {
    if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
      $expected[$matches[2].Trim()] = $matches[1].ToLowerInvariant()
    }
  }
  foreach ($a in $contentAssets) {
    if (-not $expected.ContainsKey($a.name)) { Fail "no SHA-256 checksum recorded for '$($a.name)' in SHA256SUMS.txt." }
    $actual = (Get-FileHash (Join-Path $tmp $a.name) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected[$a.name]) {
      Fail "SHA-256 mismatch for '$($a.name)': expected $($expected[$a.name]), got $actual."
    }
    Write-Host ("  verified {0}  {1}" -f $a.name, $actual.Substring(0, 16))
  }

  # Install (overwrite in place) + drop stale demo paks from earlier
  # fetches whose content-addressed names are no longer current.
  New-Item -ItemType Directory -Force -Path $Dest | Out-Null
  $keep = @($contentAssets | ForEach-Object { $_.name })
  foreach ($a in $contentAssets) {
    Copy-Item (Join-Path $tmp $a.name) (Join-Path $Dest $a.name) -Force
  }
  # Keep the repository's model provenance/license notice alongside the
  # downloaded release assets (the rolling GitHub release currently carries
  # only the pak, manifest and checksums).
  $creditsSource = Join-Path (Split-Path $PSScriptRoot -Parent) "SPONZA-CREDITS.md"
  if (-not (Test-Path $creditsSource -PathType Leaf)) {
    Fail "SPONZA-CREDITS.md is missing from this checkout; cannot stage demo attribution."
  }
  Copy-Item -LiteralPath $creditsSource -Destination (Join-Path $Dest "SPONZA-CREDITS.md") -Force
  foreach ($stale in (Get-ChildItem $Dest -Filter "sponza_10-*.pak" -File)) {
    if ($keep -notcontains $stale.Name) {
      Write-Host "  removing stale demo pak: $($stale.Name)"
      Remove-Item $stale.FullName -Force
    }
  }

  $tapp = $contentAssets | Where-Object { $_.name -like "*.tapp" } | Select-Object -First 1
  Write-Host ""
  Write-Host "demo content installed into ${Dest} (credits: SPONZA-CREDITS.md):"
  foreach ($a in $contentAssets) {
    Write-Host ("  {0}  ({1:N1} MB)" -f $a.name, ($a.size / 1MB))
  }
  if ($tapp) {
    $relDest = [System.IO.Path]::GetFullPath($Dest)
    Write-Host ""
    Write-Host "run it from the build directory:"
    Write-Host "  App --project content/paks/$($tapp.name)"
  }
} finally {
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
