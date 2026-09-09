# MicroWave DMX GENMIDI adapter

`snd_genmidi` translates the packed OPL instrument bank used by Doom-family
`GENMIDI` lumps into MicroWave's generic `snd_midi_fm_bank_t`.

```text
caller-owned GENMIDI bytes
        |
        v
    snd_genmidi
        |
        v
 snd_midi_fm_bank_t
        |
        v
    snd_midi_fm
```

It deliberately has no WAD API, malloc, stdio, clock, MIDI-file parser, or
FastDoom dependency. A game locates a lump and passes the bytes in; the adapter
only understands the bank format.

## Layout accepted

The canonical bank begins with `#OPL_II#`, followed by 175 packed 36-byte
instruments: 128 melodic programs and 47 percussion instruments for MIDI notes
35..81. Canonical Doom banks then contain 175 32-byte debug names.

The names are not needed for synthesis, so `snd_genmidi_load()` requires the
6308-byte header+instrument table and reports whether the canonical name area is
present. It does not copy names into runtime state.

## Translation, not emulation

GENMIDI stores raw OPL register-shaped data. `snd_midi_fm` is intentionally a
small deterministic two-operator FM renderer, not a YM3812 emulator, so the
adapter translates rather than pretending register accuracy:

- OPL frequency multiples become 8.8 operator ratios;
- total-level attenuation becomes 8.8 linear operator gain;
- attack/decay/release nibbles become approximate millisecond envelope times;
- sustain attenuation becomes an 8.8 sustain level;
- the OPL connection bit selects FM vs additive mode;
- three feedback bits become a bounded generic feedback amount;
- OPL waveforms are mapped to the closest built-in MicroWave waveform;
- unsupported OPL tremolo/vibrato/key-scaling quirks are not synthesized.

The stored carrier total level is intentionally not used as a fixed carrier
gain because DMX drives carrier level from MIDI note/channel volume. The
MicroWave MIDI-FM backend already applies MIDI gain outside the operator.

## Pitch behavior

GENMIDI `base_note_offset` is translated into the FM layer's 1/128-semitone
pitch offset. The second layer's `fine_tuning` follows the DMX relationship and
is also represented in that domain.

Fixed-pitch instruments ignore `base_note_offset`, matching DMX behavior.
Non-fixed percussion uses note 60 as its base before per-layer offsets, matching
Doom's OPL player rather than using the incoming MIDI drum key as pitch.

## Memory and ownership

`snd_genmidi_bank_t` is caller-owned and contains the converted 128+47
instruments directly. There is no allocation and the source bytes do not need
to remain alive after a successful load.

This costs more static memory than retaining the packed bank, but keeps the hot
synth path independent of GENMIDI byte parsing and is still small enough for the
intended DOS/RP2350 targets. If that storage later becomes material, a compact
bank representation can be added without changing MUS or MIDI.

## Scope boundary

This adapter does not add:

- WAD lookup;
- Standard MIDI File parsing;
- SoundFonts;
- register-accurate OPL2/OPL3;
- MOD/XM/S3M/IT;
- FastDoom-specific playback APIs.

The intended Doom path is now:

```text
MUS lump --------> snd_mus -----> snd_midi --+
                                             |
GENMIDI lump ----> snd_genmidi ----------------+--> snd_midi_fm --> snd_mixer
```

Once this path is tested with a real Doom `GENMIDI` lump, the remaining
FastDoom work is integration rather than another music subsystem.
