# MicroWave MIDI FM backend

`snd_midi_fm` is MicroWave's first MIDI synthesis backend. It is a small,
deterministic, two-operator FM renderer shaped around the parts of OPL music
that are useful to the project, without pretending to be a register-accurate
YM3812/YMF262 emulator.

```text
MUS / future SMF / live MIDI
            |
            v
         snd_midi
            |
            v
       snd_midi_fm
            |
            v
        snd_mixer
```

It uses no malloc, stdio, clock, WAD API, Raylib, Pico SDK, or DOS API. Patch
banks, synth state, event queues, and mixer buffers are all caller-owned.

## Why this is not called an OPL emulator

A hardware-accurate OPL implementation is a different project: operator rate
generators, logarithmic envelopes, waveform quirks, tremolo/vibrato behavior,
register timing, OPL2/OPL3 differences, and chip-specific edge cases all add
substantial code and verification work.

The immediate MicroConsole requirement is smaller: turn normalized MIDI events
into deterministic FM audio while preserving a clean boundary where a more
accurate backend can be substituted later.

So this layer implements an **OPL-shaped two-operator synth**:

- modulator + carrier operators;
- FM or additive connection;
- operator frequency multiples;
- operator levels;
- ADSR-like attack/decay/sustain/release envelopes;
- phase modulation depth;
- feedback;
- sine, square, saw, triangle, and noise waveforms from `snd_synth`;
- up to 18 software voices, with 9 as the OPL2-shaped default.

That is intentionally finite.

## MIDI behavior

The backend consumes normalized `snd_midi_message_t` events and implements the
parts that materially affect game music:

- Note On / Note Off;
- Program Change;
- channel volume;
- expression;
- pan;
- sustain pedal;
- pitch bend (currently a fixed +/-2-semitone range);
- Reset All Controllers;
- All Notes Off;
- All Sound Off;
- MIDI channel 10 percussion through a note-indexed percussion bank.

Unknown/irrelevant controllers remain harmless because `snd_midi` is still the
protocol/state authority.

## Instruments and Doom's future GENMIDI adapter

The bank is generic and caller-owned:

```text
program 0..127 -> snd_fm_instrument_t
percussion key -> snd_fm_instrument_t
```

An instrument contains one or two FM layers. Two layers are deliberate: DMX
GENMIDI has double-voice instruments, so a future `snd_genmidi` adapter can
represent them without changing this synth API or throwing away the second
voice.

Each layer has a whole-semitone transpose plus signed 1/128-semitone detune.
This also leaves enough resolution for GENMIDI's second-voice fine tuning.
Fixed-note instruments are supported at the instrument level for percussion.

No GENMIDI parser exists in this layer. The intended future boundary is:

```text
GENMIDI lump bytes
       |
       v
 future snd_genmidi
       |
       v
snd_midi_fm_bank_t
```

FastDoom should eventually provide the GENMIDI lump bytes; MicroWave should own
the adapter and synthesis.

## Exact event timing

MIDI callbacks do not mutate active oscillators immediately. They enqueue a
small, bounded event record tagged with its absolute output frame.

`snd_midi_fm_mix_block()` subdivides the current mixer span at those event
boundaries:

```text
block start -------- note on -------- pitch bend ------ note off ----- end
        mix old state | mix new note | mix bent note | release state
```

This is the same rule already used by `snd_seq`: subdivide, do not approximate.
A 37-frame frontend and a 256/1024-frame frontend therefore hear MIDI events on
the same samples.

The queue defaults to 128 events. Source adapters should process one audio
window at a time. Overflow or out-of-order insertion is rejected and counted
in `dropped_events` rather than silently changing event order.

## Voice allocation

The default limit is 9 voices to resemble OPL2 resource pressure. Callers may
select up to `SND_MIDI_FM_MAX_VOICES` (18 by default) for an OPL3-shaped budget.

When full, allocation is deterministic:

1. prefer an already-releasing voice;
2. otherwise steal the oldest active voice.

Layered instruments consume one FM voice per layer. This is important for
future GENMIDI double-voice instruments and makes the resource cost explicit.

## What remains separate

This backend does not imply or implement:

- Standard MIDI File parsing;
- SoundFonts or General MIDI wavetable instruments;
- exact OPL register emulation;
- Doom WAD access;
- DMX GENMIDI parsing;
- MOD/XM/S3M/IT playback.

Those remain independent source/bank/backend layers if the project actually
needs them.

## Tests

`mw_test_midi_fm` covers:

- default state and late binding to existing MIDI channel state;
- bounded/in-order event queue behavior;
- half-open block scheduling;
- exact block-size invariance;
- audible FM output;
- sustain;
- deterministic voice stealing;
- layered instruments;
- program/percussion bank selection;
- pan and pitch state;
- All Sound Off;
- synthetic end-to-end MUS -> MIDI -> FM rendering;
- the same end-to-end render at different mixer block sizes.
