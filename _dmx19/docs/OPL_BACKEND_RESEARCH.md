# MicroWave strict Doom/DMX OPL research

This note records the open-source implementations used to design the strict
register-native MicroWave backend. The goal is behavioral understanding and an
independent MicroWave implementation, not copying a complete GPL driver.

## Result

The correct architecture is:

```text
MUS bytes
  -> snd_mus timing / channel mapping
  -> snd_midi normalized events
  -> strict DMX 1.9 driver semantics
  -> YM3812 register writes
  -> Nuked OPL3 in OPL2 compatibility mode
  -> MicroWave mixer
```

The existing `snd_mus` layer already matches the classic MUS conversion rules
well enough that it should not be changed for this work. The remaining
differences are in the DMX-to-OPL driver.

## Open-source implementations reviewed

### Chocolate Doom

Repository:

https://github.com/chocolate-doom/chocolate-doom

Primary files:

- `src/i_oplmusic.c`
- `src/mus2mid.c`
- `opl/opl.c`
- `opl/opl_sdl.c`
- `opl/opl3.c`

Chocolate Doom explicitly targets accurate reproduction of DOS Doom, including
historical bugs. It is the strongest behavioral oracle for Doom 1.9 OPL
playback.

Important behaviors reproduced in MicroWave strict mode:

- internal MIDI channel 9 is mapped back to DMX channel 15 for percussion;
- channels start at instrument 0, volume 100, bend 0, pan bits 0x30;
- only the pitch-bend MSB is used;
- the historical DMX frequency lookup curve is used;
- fixed-note instruments skip base-note offsets;
- the second GENMIDI voice applies `(fine_tuning / 2) - 64`;
- physical OPL voices use FIFO free-list reuse;
- released physical voices go to the end of the free list;
- pitch-bent voices move to the end of the allocated list;
- Doom 1.9 steals at most one voice before a note-on;
- a double-voice instrument drops its second voice if no second channel remains;
- second GENMIDI voices are preferred steal victims;
- otherwise higher-numbered DMX channels are lower priority;
- operator data is not rewritten when the same instrument/layer remains cached
  on a reused physical OPL voice;
- carrier is loaded first at minimum output, then modulator;
- operator write order is level, tremolo/multiple, attack/decay,
  sustain/release, waveform;
- carrier volume uses the historical DMX volume map rather than the GENMIDI
  carrier TL;
- additive/AM instruments retain the historical DMX modulator-volume quirk;
- the strict OPL2 path ignores expression, sustain, all-sound-off, reset
  controllers, reverb, chorus, etc.;
- startup uses Doom's unusually broad OPL register initialization sequence;
- Chocolate's SDL backend uses Nuked's buffered register writes.

Chocolate/DSDA/Woof use frequency split index 284.

### DSDA-Doom

Repository:

https://github.com/kraflab/dsda-doom

Primary file:

- `prboom2/src/MUSIC/oplplayer.c`

DSDA's OPL player is Chocolate-derived and independently confirms the main
Doom 1.9 frequency, volume, instrument-loading, and voice-stealing behavior.
It also uses frequency split index 284.

### Woof

Repository:

https://github.com/fabiangreffrath/woof

Primary file:

- `src/i_oplmusic.c`

Woof preserves the same Chocolate-lineage register driver while adding modern
features around it. Its classic path also uses the 284 frequency split. Modern
multi-chip/gain extensions are not part of the MicroWave strict baseline.

### libADLMIDI

Repository:

https://github.com/Wohlstand/libADLMIDI

Primary file:

- `src/models/model_dmx.c`

libADLMIDI is especially useful because its current DMX model is an independent,
recent reverse-engineering effort rather than another Chocolate-derived copy.

Its DMX model confirms:

- the historical 128-entry volume mapping;
- the non-generic DMX volume formula;
- the original buggy AM/additive interpretation;
- a dedicated DMX frequency model and lookup table.

The current `model_dmx.c` is MIT licensed. MicroWave's exact frequency table is
cross-checked against that source and the MIT notice is retained next to the
table in `snd_genmidi_opl.c`.

One useful disagreement remains: libADLMIDI deliberately uses split index 283
to reproduce a reported DMX off-by-one side bug, while
Chocolate/DSDA/Woof use 284. MicroWave therefore exposes both:

```text
--freq-split 284   default; Chocolate/DSDA/Woof Doom 1.9 behavior
--freq-split 283   libADLMIDI reverse-engineered side-bug experiment
```

Do not guess which sounds more authentic: render both and compare.

### FastDoom

Repository:

https://github.com/viti95/FastDoom

Relevant files:

- `FASTDOOM/ns_music.c`
- `FASTDOOM/ns_midi.c`
- `FASTDOOM/ns_sbmus.c`
- `FASTDOOM/mus2mid.c`

FastDoom confirms the embedded architecture: the sequencer is separate from the
device backend and the AdLib/Sound Blaster music path performs actual register
programming. Its particular allocation/pitch implementation differs from the
Chocolate Doom 1.9 reconstruction, so it is an architecture reference rather
than the strict behavioral oracle for this backend.

### Nuked OPL3

Upstream:

https://github.com/nukeykt/Nuked-OPL3

MicroWave pins the version carried by Chocolate Doom and builds it separately.
The core remains LGPL-2.1-or-later.

Nuked is not where we should tune Doom instrument semantics. Once the register
stream is correct, the emulator owns YM3812/YMF262 operator behavior.

## MUS layer cross-check

MicroWave `snd_mus.c` was compared with Chocolate Doom `mus2mid.c`.

The important mappings agree:

```text
MUS controller 1..9:
32, 1, 7, 10, 11, 91, 93, 64, 67

MUS system 10..14:
120, 123, 126, 127, 121
```

Also matching:

- MUS percussion channel 15 -> MIDI channel 9;
- lazy melodic MIDI-channel allocation skips channel 9;
- first use of a melodic channel emits All Notes Off;
- MUS pitch byte becomes the same MIDI pitch-wheel representation;
- controller values with bit 7 set are clamped to 127.

For strict Chocolate-style OPL playback, the backend then intentionally ignores
most of those MIDI controllers because the vanilla OPL driver did.

## Major bugs in MicroWave's first register-native backend

The first Nuked-backed version was already dramatically better than the generic
FM synth, but still differed from DMX in several audible ways:

1. It used a mathematically generated F-number curve instead of the DMX table.
2. It folded the GENMIDI carrier TL into note volume. DMX replaces the carrier
   low six TL bits with its calculated note/channel volume.
3. It scaled additive modulator volume incorrectly.
4. It used generic MIDI channel defaults (volume 127) rather than DMX volume
   100.
5. It honored expression and sustain even though the strict OPL driver ignores
   them.
6. It selected the first inactive physical OPL channel rather than maintaining
   DMX's FIFO free list.
7. It could steal twice to make room for both halves of a double-voice patch.
8. It did not preserve instrument/layer cache state across physical voice reuse.
9. It did not move pitch-bent voices to the back of the allocated list.
10. It used normalized MIDI channel 9 for percussion priority instead of
    restoring DMX channel 15.
11. It did not reproduce Doom's full startup register initialization.

The strict backend fixes all eleven.

## Register tracing

The host renderer now supports:

```text
--trace FILE.csv
```

Each row is:

```text
sequence,frame,register,value
```

The trace records driver calls before Nuked's buffered-write delay. That makes
it possible to compare the deterministic DMX register stream independently of
the emulator's audio output.

A trace-enabled render also replays startup initialization after attaching the
trace, so the CSV begins with the initialization register sequence.

## Validation sequence

First render the default 284 model:

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

Then render the newer reverse-engineered 283 interpretation:

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

The 284 render is the default strict Chocolate/DSDA/Woof target. The 283 render
is an A/B diagnostic for the newer libADLMIDI DMX frequency finding.

Do not apply EQ, DC filtering, normalization, or generic-FM gain calibration
while comparing driver fidelity. Those would hide register-stream errors.
