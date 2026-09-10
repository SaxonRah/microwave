# Install the register-native Doom OPL backend

This bundle contains complete files, not patches.

From PowerShell, with the ZIP extracted somewhere such as
`C:\Users\Jupiter\Downloads\microwave-register-native-opl-full-files`:

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

The first tool build downloads the pinned Nuked OPL3 source into
`third_party\nuked-opl3`, retains its LGPL license, and builds both validators.

Render E1M1 through the new backend:

```powershell
.\mw.bat tool opl-wav `
    --wad "C:\microconsole\wads\doom.wad" `
    --music D_E1M1 `
    --out .\e1m1-nuked-opl.wav `
    --seconds 60 `
    --gain 256 `
    --accum wide
```

The old generic renderer is still available as `genmidi-wav` for comparison.
Do not reuse its `40/256` gain calibration for the new OPL backend.
