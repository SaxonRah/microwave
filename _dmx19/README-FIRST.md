# MicroWave strict Doom/DMX OPL2 backend

This bundle contains complete files, not patches.

It replaces the first register-native `snd_genmidi_opl` implementation with a
strict Doom 1.9 / Ultimate Doom driver model researched against Chocolate Doom,
DSDA-Doom, Woof, libADLMIDI, FastDoom, and Nuked OPL3.

## Install

From PowerShell, after extracting this bundle:

```powershell
powershell -ExecutionPolicy Bypass -File `
    .\install-microwave-opl-backend.ps1 `
    -Repo "C:\microwave"
```

Then:

```powershell
cd C:\microwave

.\mw.bat test all

Remove-Item -Recurse -Force .\build-tools -ErrorAction SilentlyContinue
.\mw.bat tool build
```

## First fidelity render

```powershell
.\mw.bat tool opl-wav `
    --wad "C:\microconsole\wads\doom.wad" `
    --music D_E1M1 `
    --out .\e1m1-dmx19-284.wav `
    --seconds 60 `
    --freq-split 284 `
    --trace .\e1m1-dmx19-284.csv `
    --gain 256 `
    --accum wide
```

Then compare the newer libADLMIDI reverse-engineered frequency split:

```powershell
.\mw.bat tool opl-wav `
    --wad "C:\microconsole\wads\doom.wad" `
    --music D_E1M1 `
    --out .\e1m1-dmx19-283.wav `
    --seconds 60 `
    --freq-split 283 `
    --trace .\e1m1-dmx19-283.csv `
    --gain 256 `
    --accum wide
```

`284` is the default and matches Chocolate Doom / DSDA-Doom / Woof.
`283` is an explicit A/B experiment based on newer libADLMIDI DMX research.

The old generic `genmidi-wav` renderer remains separate. Do not apply its
40/256 headroom calibration to this Nuked-backed OPL path.
