# MicroWave host tools

These are hosted validation/conversion programs around the allocation-free core.
They may use `stdio` and `malloc`; none of that leaks into `shared/src`.

## Real Doom MUS + GENMIDI validator

```powershell
.\mw.bat tool genmidi-wav `
    --wad "C:\microconsole\wads\doom.wad" `
    --music D_E1M1 `
    --out .\e1m1-microwave.wav `
    --seconds 30
```

The first run builds `build-tools`; later runs use the existing executable.
After changing tool/core source, rebuild explicitly:

```powershell
.\mw.bat tool build
```

`mw_genmidi_wav` keeps all file-format convenience at the host edge. It reads a
WAD directory, extracts `GENMIDI` and one requested MUS lump, passes those bytes
through `snd_genmidi` / `snd_mus` / `snd_midi` / `snd_midi_fm`, and writes the
finished MicroWave PCM samples as a stereo 16-bit WAV.

No WAD or WAV API is added to the reusable music/synthesis layers, and no Doom
copyrighted data is checked into this repository.


### Mix-headroom diagnostic

The validator defaults to the host-only wide accumulator so its clipping
report measures the final summed signal rather than order-dependent per-voice
saturation. It reports the raw pre-clamp peak and how many samples would clip
before the mixer resolves the block.

Use the shipping-style saturating path explicitly when you want to compare it:

```powershell
.\mw.bat tool genmidi-wav `
    --wad "C:\microconsole\wads\doom.wad" `
    --music D_E1M1 `
    --out .\e1m1-saturating.wav `
    --seconds 30 `
    --accum saturating
```

The wide mode is diagnostic only; it does not change `snd_mus`, `snd_genmidi`,
or `snd_midi_fm`.

### Full-WAD music headroom scan

Use `--scan-music` to measure every effective `D_*` MUS lump in a WAD without
writing WAV files:

```powershell
.\mw.bat tool genmidi-wav `
    --wad "C:\microconsole\wads\doom.wad" `
    --scan-music `
    --voices 9 `
    --accum wide
```

Each valid music lump is rendered through its complete first pass using the
same `snd_mus -> snd_midi -> snd_midi_fm -> snd_mixer` path as normal WAV
validation. Duplicate WAD names follow Doom's last-definition-wins semantics.
The report shows duration, raw peak, percent of full scale, would-clip sample
count, and voice steals, then names the worst peak across the WAD.

Wide accumulation is the useful mode for headroom work because it observes the
true summed signal before the S16 clamp. Saturating mode remains available for
shipping-path comparisons.
