#requires -Version 5.1
# Build a self-contained Tourmaline Setup installer.
# Usage: .\packaging\build_windows_installer.ps1 -NativeBinDir out/build/x64-Release
param(
    [Parameter(Mandatory=$true)][string]$NativeBinDir,
    [string]$Iscc = 'ISCC.exe',
    [string]$Python = 'py',
    [switch]$SkipPyInstallerInstall,
    [string]$Version = '0.1'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$native = (Resolve-Path $NativeBinDir).Path
if (-not (Test-Path (Join-Path $native 'Project.exe') -PathType Leaf)) {
    throw "NativeBinDir must contain the built Project.exe (native Project init is required)"
}
Push-Location $root
try {
    if (-not $SkipPyInstallerInstall) {
        & $Python -3 -m pip install 'pyinstaller>=6,<7'
        if ($LASTEXITCODE -ne 0) { throw 'PyInstaller installation failed' }
    }
    $dist = Join-Path $root 'out/setup-dist'
    $build = Join-Path $root 'out/setup-build'
    $stage = Join-Path $root 'out/setup-stage'
    New-Item -ItemType Directory -Force $dist, $build, $stage | Out-Null
    $wizardArgs = @('-3','-m','PyInstaller','--noconfirm','--clean','--onefile',
        '--windowed','--name','TourmalineSetupWizard','--distpath',$dist,
        '--workpath',$build,'--specpath',$build,'tools/setup_wizard.py')
    & $Python @wizardArgs
    if ($LASTEXITCODE -ne 0) { throw 'Wizard executable build failed' }
    $serverArgs = @('-3','-m','PyInstaller','--noconfirm','--clean','--onefile',
        '--name','TourmalineAssetServer','--distpath',$dist,'--workpath',$build,
        '--specpath',$build,'--add-data','tools/asset_consumers_dashboard.html;tools',
        'tools/shared_asset_server.py')
    & $Python @serverArgs
    if ($LASTEXITCODE -ne 0) { throw 'Asset server executable build failed' }
    Copy-Item (Join-Path $dist 'TourmalineSetupWizard.exe') $stage -Force
    Copy-Item (Join-Path $dist 'TourmalineAssetServer.exe') $stage -Force
    Copy-Item (Join-Path $native '*') $stage -Recurse -Force
    $maya = Join-Path $stage 'tools/maya'
    New-Item -ItemType Directory -Force $maya | Out-Null
    Copy-Item (Join-Path $root 'tools/maya/tourmaline_asset_cache.py') $maya
    Copy-Item (Join-Path $root 'tools/maya/tourmaline_asset_versions.py') $maya
    if (-not (Get-Command $Iscc -ErrorAction SilentlyContinue)) {
        $candidates = @(
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 6/ISCC.exe'))
        foreach ($candidate in $candidates) {
            if (Test-Path $candidate) { $Iscc = $candidate; break }
        }
    }
    $output = Join-Path $root 'out/installers'
    New-Item -ItemType Directory -Force $output | Out-Null
    if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed' }
    Write-Host "Installer created in $output"
}
finally { Pop-Location }
) { throw 'Version must contain 3 or 4 numeric components' }
    & $Iscc "/DStageDir=$stage" "/DOutputDir=$output" "/DAppVersion=$Version" (Join-Path $root 'packaging/tourmaline_setup.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed' }
    Write-Host "Installer created in $output"
}
finally { Pop-Location }
