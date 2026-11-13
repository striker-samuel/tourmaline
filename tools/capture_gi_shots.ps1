# Capture matched GI shots (beauty + specular-only + irradiance) for a
# bistro viewpoint across the SG9 / Oct8 / Compact permutations, at fixed
# exposure. Each permutation writes:
#   <out>/<view>/<perm>/beauty.png       tonemapped render
#   <out>/<view>/<perm>/specular.png     tonemapped env-specular slice
#   <out>/<view>/<perm>/irradiance.png   tonemapped diffuse irradiance
param(
    [Parameter(Mandatory=$true)][string]$View,
    [Parameter(Mandatory=$true)][string]$CamPos,
    [Parameter(Mandatory=$true)][string]$CamFwd,
    [string]$Out = "F:\REPOS\tourmaline\out\screenshots"
)

$repo = "F:\REPOS\tourmaline"
$exe = Join-Path $repo "out\build\x64-Release\App.exe"
$exeDir = Split-Path $exe  # the renderer CWD; screenshots land in $exeDir\exports
Push-Location $exeDir       # App.exe resolves content/paks + exports relative to CWD

$env:TOURMALINE_CAM_POS = $CamPos
$env:TOURMALINE_CAM_FWD = $CamFwd
$env:TOURMALINE_AUTO_EXPOSURE = '0'
$env:TOURMALINE_AA = 'off'

foreach ($perm in @('sg9','oct8','compact')) {
    Remove-Item Env:\TOURMALINE_GI_OCTAHEDRAL -ErrorAction SilentlyContinue
    Remove-Item Env:\TOURMALINE_GI_COMPACT_SPECULAR -ErrorAction SilentlyContinue
    if ($perm -eq 'oct8') { $env:TOURMALINE_GI_OCTAHEDRAL = '1' }
    elseif ($perm -eq 'compact') { $env:TOURMALINE_GI_COMPACT_SPECULAR = '1' }

    $dir = Join-Path $Out "$View\$perm"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null

    # 1. beauty
    $env:TOURMALINE_CAPTURE = '12'
    & $exe --pak content/paks/bistro.pak --disable_control --bench 16 *> "$env:TEMP\shot_${View}_${perm}.txt"
    Remove-Item Env:\TOURMALINE_CAPTURE -ErrorAction SilentlyContinue
    $shot = (Select-String -Path "$env:TEMP\shot_${View}_${perm}.txt" -Pattern "screenshot exported: (.*)" | Select-Object -Last 1).Matches[0].Groups[1].Value.Trim()
    Copy-Item (Join-Path $exeDir $shot) (Join-Path $dir "beauty.png") -Force

    # 2. specular-only
    $env:TOURMALINE_CAPTURE = '12'
    $env:TOURMALINE_GI_EXPORT_SPECULAR = "$dir\spec"
    & $exe --pak content/paks/bistro.pak --disable_control --bench 16 *> "$env:TEMP\spec_${View}_${perm}.txt"
    Remove-Item Env:\TOURMALINE_GI_EXPORT_SPECULAR -ErrorAction SilentlyContinue
    Remove-Item Env:\TOURMALINE_CAPTURE -ErrorAction SilentlyContinue
    python "$repo\tools\tonemap_specular.py" --dir "$dir\spec" --output "$dir\specular.png"

    # 3. irradiance (diffuse GI)
    $env:TOURMALINE_CAPTURE = '12'
    $env:TOURMALINE_GI_EXPORT_IRRADIANCE = "$dir\irrad"
    & $exe --pak content/paks/bistro.pak --disable_control --bench 16 *> "$env:TEMP\irr_${View}_${perm}.txt"
    Remove-Item Env:\TOURMALINE_GI_EXPORT_IRRADIANCE -ErrorAction SilentlyContinue
    Remove-Item Env:\TOURMALINE_CAPTURE -ErrorAction SilentlyContinue
    python "$repo\tools\tonemap_specular.py" --dir "$dir\irrad" --output "$dir\irradiance.png"

    Write-Output "$perm -> $dir (beauty/specular/irradiance.png)"
}

Remove-Item Env:\TOURMALINE_GI_OCTAHEDRAL -ErrorAction SilentlyContinue
Remove-Item Env:\TOURMALINE_GI_COMPACT_SPECULAR -ErrorAction SilentlyContinue
Pop-Location
