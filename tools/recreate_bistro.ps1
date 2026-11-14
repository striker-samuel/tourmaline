# Recreate the Bistro demo from its ORCA source.
#
# The Bistro demo's model + textures are ~800 MB and come from NVIDIA's
# Open Research Content Archive (CC BY 4.0); they are NOT in the git
# tree. This script verifies the source is present and drives the recipe
# cook, or prints the recreate steps when the source is missing.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\recreate_bistro.ps1
#   powershell ... -Project releases\bistro -ProjectExe out\build\x64-Release\Project.exe
#
# See releases/bistro/README.md and BISTRO-CREDITS.md.

param(
  [string]$Project = "",
  [string]$ProjectExe = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $Project) { $Project = Join-Path $repoRoot "releases\bistro" }
$Project = [System.IO.Path]::GetFullPath($Project)
if (-not $ProjectExe) { $ProjectExe = Join-Path $repoRoot "out\build\x64-Release\Project.exe" }

$recipe = Join-Path $Project "bistro_static.tourrecipe"
$gltf = Join-Path $Project "content\models\bistro_static\bistro_static.gltf"
$modelDir = Join-Path $Project "content\models\bistro_static"

$textureCount = 0
if (Test-Path $modelDir -PathType Container) {
  $textureCount = @(Get-ChildItem $modelDir -Recurse -Filter "*.ktx2" -File).Count
}

if (-not (Test-Path $recipe -PathType Leaf) -or -not (Test-Path $gltf -PathType Leaf) -or $textureCount -eq 0) {
  Write-Host "Bistro source is not fully present under ${Project}:"
  Write-Host "  recipe:  $recipe  ($(if (Test-Path $recipe) {'ok'} else {'MISSING'}))"
  Write-Host "  glTF:    $gltf  ($(if (Test-Path $gltf) {'ok'} else {'MISSING'}))"
  Write-Host "  textures: $textureCount .ktx2 files  (needs ~343)"
  Write-Host ""
  Write-Host "To recreate, first obtain the ORCA Amazon Lumberyard Bistro:"
  Write-Host "  https://developer.nvidia.com/orca/amazon-lumberyard-bistro"
  Write-Host "convert the FBX to glTF and its textures to KTX2, then place"
  Write-Host "  - bistro.bin               (the glTF geometry buffer) and"
  Write-Host "  - the .ktx2 textures       (under objects/ and textures/)"
  Write-Host "beside the committed glTF under content/models/bistro_static/."
  Write-Host "See releases/bistro/README.md for the full flow."
  exit 1
}

if (-not (Test-Path $ProjectExe -PathType Leaf)) {
  Write-Host "Project.exe not found at $ProjectExe - build the Project target first."
  exit 1
}

Write-Host "Cooking the Bistro demo (this is the multi-GB bake; be patient):"
& $ProjectExe build `
  --project $Project `
  --recipe $recipe `
  --publish (Join-Path $Project "build") `
  --manifest-name bistro_static
exit $LASTEXITCODE
