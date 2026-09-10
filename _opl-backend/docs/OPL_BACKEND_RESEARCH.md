# MicroWave Doom OPL backend research and implementation

## Why the previous WAV had correct rhythm but incorrect instruments

MicroWave's `snd_mus` layer was doing the source-format job correctly: it
validated caller-owned MUS bytes and emitted normalized MIDI channel messages
at absolute output frames. The recognizable rhythm and note timing in the
rendered WAV were consistent with that design.

The failure was after the MIDI boundary. The old path was:

```text
MUS -> normalized MIDI -> GENMIDI converted into generic FM patches
    -> linear oscillators + linear ADSR -> PCM
```

A Doom `GENMIDI` voice is already a collection of YM3812 register values. It
contains operator multiplier/tremolo/vibrato flags, key-scale information,
total level, attack/decay, sustain/release, waveform, channel feedback and
connection. Converting those fields into ordinary waveforms, milliseconds and
one fixed phase-modulation amount discards most of the instrument.

That is why the previous output retained the score while guitars, basses,
drums and synths collapsed into unrelated buzzy tones.

## What FastDoom does

FastDoom separates the same responsibilities that MicroWave already separates:

- `mus2mid.c` converts MUS into a MIDI event stream/file representation.
- `ns_midi.c` owns MIDI sequencing and dispatch.
- `ns_music.c` selects a backend through note/controller/program callbacks.
- `ns_sbmus.c` is the AdLib/Sound Blaster FM backend. It allocates nine OPL
  channels and writes operator/channel registers.
- `ns_mpu401.c` is the external MIDI/MPU-401 backend.

FastDoom therefore does **not** turn GENMIDI into a generic subtractive/FM
software synth. Its AdLib path terminates in OPL register writes.

Source:

- https://github.com/viti95/FastDoom

## What accurate modern Doom ports do

Chocolate Doom's OPL music path likewise keeps MIDI sequencing separate from
the GENMIDI/OPL driver. The driver:

- loads the raw 36-byte GENMIDI instrument records;
- writes the stored operator values to OPL `20/40/60/80/E0` registers;
- applies the DMX MIDI-volume-to-OPL-total-level mapping;
- handles fixed-pitch and percussion instruments;
- handles base-note offsets and the second-voice fine-tuning field;
- uses nine-channel OPL2 allocation for the classic baseline;
- writes frequency/key-on values to `A0/B0` registers.

Current Chocolate Doom includes Nuked OPL3/Nuked-OPL3-fast as a software chip
implementation. Woof and DSDA-Doom use the same class of accurate OPL core.

Sources:

- https://github.com/chocolate-doom/chocolate-doom/blob/master/src/i_oplmusic.c
- https://github.com/chocolate-doom/chocolate-doom/tree/master/opl
- https://github.com/nukeykt/Nuked-OPL3

## Cores considered

### Nuked OPL3

Selected for the first correct backend.

- Plain C and easy to call from MicroWave C99.
- Implements the actual Yamaha operator, logarithmic attenuation, envelope,
  feedback, waveform, tremolo, vibrato, key-scale and register behavior.
- Widely used by Doom source ports and music players.
- Supports direct register writes and output-rate resampling.
- LGPL-2.1-or-later. It is kept as a separate library target with its own
  license and source provenance.

### ymfm

Technically excellent and permissively licensed (BSD), but primarily C++ and
substantially less suitable for MicroWave's C99/Open Watcom direction. It
remains a useful future reference and possible host-only comparison backend.

- https://github.com/aaronsgiles/ymfm

### FastDoom's AdLib driver

Excellent design reference for DOS hardware output, but not a software OPL
emulator for Pico/raylib. FastDoom is also GPL, so MicroWave's new driver was
written independently around documented GENMIDI and OPL register behavior.

### `dos-like` `opl.h`

A convenient single-header C port that combines an ymfm-derived core and a
DOSMid-derived MIDI layer. Its author explicitly warns that the conversion may
contain mistakes and recommends the originals for serious use. It was useful
for architectural comparison, not selected as the correctness baseline.

- https://github.com/mattiasgustavsson/dos-like/blob/main/source/libs/opl.h

## New MicroWave architecture

```text
caller-owned MUS bytes
        |
        v
     snd_mus
        |
        v
     snd_midi          normalized messages at absolute frames
        |
        v
 snd_genmidi_opl       raw GENMIDI + DMX-shaped allocation/register driver
        |
        v
  Nuked OPL3 core      OPL2 compatibility mode, nine channels
        |
        v
 MicroWave mix block   stereo PCM at the configured output rate
```

The previous generic path remains available:

```text
snd_genmidi -> snd_midi_fm
```

It is useful as a small generic FM experiment and for A/B tests, but it is no
longer the Doom-fidelity path.

## Files added

- `shared/src/snd_genmidi_opl.h`
- `shared/src/snd_genmidi_opl.c`
- `tools/mw_opl_wav.c`
- `scripts/mw_vendor_nuked_opl3.ps1`

The replacement CMake and host-tool dispatcher files expose these as an
optional target and command.

## Third-party source policy

`scripts/mw_vendor_nuked_opl3.ps1` downloads a pinned `opl3.c`/`opl3.h` pair
from the researched Chocolate Doom revision and preserves Nuked OPL3's license
under:

```text
third_party/nuked-opl3/
```

The core is compiled as `microwave_nuked_opl3`, separate from MicroWave's own
`snd_genmidi_opl.c` driver.

## First validation goal

The first check is intentionally audible rather than another headroom scan:

```powershell
.\mw.bat tool opl-wav `
    --wad "C:\microconsole\wads\doom.wad" `
    --music D_E1M1 `
    --out .\e1m1-nuked-opl.wav `
    --seconds 60 `
    --gain 256 `
    --accum wide
```

Expected qualitative change:

- Doom's recognizable AdLib/OPL instrument identities rather than generic
  buzz/organ timbres;
- centered output with mean sample near zero instead of the earlier +0.126
  full-scale bias;
- chip envelope and feedback behavior rather than linear ADSR;
- no waveform approximation artifacts from the old generic synth.

Only after the music sounds correct should `--scan-music` and gain calibration
be added to the register-native tool. The generic backend's `40/256` setting is
not transferable to Nuked OPL output.
