param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$Destination = Join-Path $Root "third_party\nuked-opl3"

# This is the Chocolate Doom revision researched for the integration. Its
# opl3.c/opl3.h pair is Nuked-OPL3-fast 1.8-fast.3 and supports
# OPL_WF_TABLE_RUNTIME=1, so wf_rom.h is not required.
$ChocolateCommit = "895f581c5d91497bdda0516612da803fe5843e28"
$CoreBase = "https://raw.githubusercontent.com/chocolate-doom/chocolate-doom/$ChocolateCommit/opl"

$Files = @(
    @{
        Name = "opl3.c"
        Url = "$CoreBase/opl3.c"
    },
    @{
        Name = "opl3.h"
        Url = "$CoreBase/opl3.h"
    },
    @{
        Name = "LICENSE"
        Url = "https://raw.githubusercontent.com/nukeykt/Nuked-OPL3/master/LICENSE"
    }
)

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

foreach ($File in $Files) {
    $Target = Join-Path $Destination $File.Name

    if ((Test-Path $Target) -and -not $Force) {
        Write-Host "  present $Target"
        continue
    }

    $Temporary = "$Target.download"
    Remove-Item -Force $Temporary -ErrorAction SilentlyContinue

    Write-Host "  downloading $($File.Url)"
    Invoke-WebRequest `
        -UseBasicParsing `
        -Uri $File.Url `
        -OutFile $Temporary

    if (-not (Test-Path $Temporary)) {
        throw "Download did not produce $Temporary"
    }

    $Length = (Get-Item $Temporary).Length
    if ($Length -lt 100) {
        throw "Downloaded file is unexpectedly short: $Temporary ($Length bytes)"
    }

    Move-Item -Force $Temporary $Target
}

$CText = Get-Content (Join-Path $Destination "opl3.c") -Raw
$HText = Get-Content (Join-Path $Destination "opl3.h") -Raw

if ($CText -notmatch "Nuked OPL3" -or
    $CText -notmatch "OPL3_GenerateResampled" -or
    $HText -notmatch "typedef struct _opl3_chip") {
    throw "Vendored files do not look like the expected Nuked OPL3 source."
}

$SourceNote = @"
Nuked OPL3 source used by MicroWave's optional register-native OPL backend.

Source project:
  Chocolate Doom
  https://github.com/chocolate-doom/chocolate-doom

Pinned revision:
  $ChocolateCommit

Pinned files:
  opl/opl3.c
  opl/opl3.h

Upstream core:
  Nuked OPL3 by Nuke.YKT
  https://github.com/nukeykt/Nuked-OPL3

The core remains licensed under GNU LGPL version 2.1 or later.
MicroWave builds it as a separate static-library target. The MicroWave
GENMIDI register driver is separate source under shared/src.

Build configuration:
  OPL_WF_TABLE_RUNTIME=1
  OPL_ENABLE_STEREOEXT=0

The first option builds the waveform table at runtime and avoids requiring
Chocolate Doom's generated wf_rom.h. The second keeps the validation backend
on the nine-channel OPL2-compatible path.
"@

Set-Content `
    -Path (Join-Path $Destination "SOURCE-MICROWAVE.txt") `
    -Value $SourceNote `
    -Encoding UTF8

Write-Host ""
Write-Host "Nuked OPL3 ready:"
Get-FileHash `
    (Join-Path $Destination "opl3.c"), `
    (Join-Path $Destination "opl3.h"), `
    (Join-Path $Destination "LICENSE") `
    -Algorithm SHA256 |
    Format-Table -AutoSize
