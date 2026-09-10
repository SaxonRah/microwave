param(
    [string]$Repo = "C:\microwave",
    [switch]$NoBackup
)

$ErrorActionPreference = "Stop"

$BundleRoot = $PSScriptRoot
$Repo = [System.IO.Path]::GetFullPath($Repo)

if (-not (Test-Path (Join-Path $Repo "mw.bat"))) {
    throw "MicroWave repository not found at: $Repo"
}

$Files = @(
    "shared\src\snd_genmidi_opl.h",
    "shared\src\snd_genmidi_opl.c",
    "shared\CMakeLists.txt",
    "tools\mw_opl_wav.c",
    "tools\CMakeLists.txt",
    "scripts\mw_vendor_nuked_opl3.ps1",
    "scripts\mw_tool.bat",
    "docs\OPL_BACKEND_RESEARCH.md"
)

$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$BackupRoot = Join-Path $Repo ".mw-backup-opl-$Stamp"

if (-not $NoBackup) {
    New-Item -ItemType Directory -Force -Path $BackupRoot | Out-Null
}

foreach ($Relative in $Files) {
    $Source = Join-Path $BundleRoot $Relative
    $Destination = Join-Path $Repo $Relative

    if (-not (Test-Path $Source)) {
        throw "Bundle is missing: $Relative"
    }

    if ((Test-Path $Destination) -and -not $NoBackup) {
        $Backup = Join-Path $BackupRoot $Relative
        New-Item -ItemType Directory -Force `
            -Path (Split-Path -Parent $Backup) | Out-Null
        Copy-Item -Force $Destination $Backup
    }

    New-Item -ItemType Directory -Force `
        -Path (Split-Path -Parent $Destination) | Out-Null
    Copy-Item -Force $Source $Destination
    Write-Host "  installed $Relative"
}

Write-Host ""
if (-not $NoBackup) {
    Write-Host "Backup of replaced files: $BackupRoot"
}
Write-Host ""
Write-Host "Next commands:"
Write-Host "  cd $Repo"
Write-Host "  .\mw.bat test all"
Write-Host "  Remove-Item -Recurse -Force .\build-tools -ErrorAction SilentlyContinue"
Write-Host "  .\mw.bat tool build"
