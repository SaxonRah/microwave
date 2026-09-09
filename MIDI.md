# MicroWave MIDI foundation

This layer intentionally stops at the MIDI message boundary.

## What `snd_midi` is

`snd_midi.h/.c` provides:

- MIDI 1.0 channel-voice messages for 16 channels;
- raw-byte parsing with running status;
- safe handling of interspersed real-time bytes;
- safe skipping of System Common and SysEx traffic;
- normalized Note-On velocity 0 -> Note-Off;
- cached program, bank, volume, expression, pan, sustain, pressure and 14-bit pitch-bend state;
- one sink callback that receives the normalized event and the new channel state;
- no allocation, stdio, clock or audio dependency.

It deliberately does **not** parse a `.mid` file, synthesize General MIDI instruments, load a SoundFont, emulate OPL, or understand MOD/XM/S3M/IT. Doom MUS parsing now lives in the separate `snd_mus` adapter.

Those are different jobs and should stay different jobs.

## Intended layering

```text
                     source formats / transports

        Doom MUS            Standard MIDI File          live MIDI bytes
           |                       |                         |
          snd_mus            future snd_smf            snd_midi_feed_byte
           |                       |                         |
           +-----------------------+-------------------------+
                                   |
                                   v
                              snd_midi
                       message + channel state
                                   |
          +------------------------+------------------------+
          |                        |                        |
          v                        v                        v
   future OPL backend      future sample synth       hardware MIDI out
          |                        |
          +------------+-----------+
                       |
                       v
                    snd_mixer
```

The existing `snd_seq` pattern sequencer is not replaced. It is a compact tracker-like song representation for games that want one. MIDI is a protocol and state model, so it lives beside `snd_seq`, not inside it.

## Why this boundary matters for Doom

Doom's IWAD music is commonly stored as MUS. FastDoom currently converts MUS to MIDI and gives that to its music driver. For MicroConsole, the clean path is instead:

```text
WAD lump -> MUS reader -> normalized MIDI messages -> chosen synth backend -> MicroWave mixer
```

The FastDoom port should not grow a MIDI synth. It should only hand the WAD music data to MicroWave and ask it to play/stop/pause/resume.

Likewise, `snd_midi` should not know what a WAD is.

## Deliberately deferred

`snd_mus` now implements the first source adapter as a zero-allocation reader over caller-owned MUS bytes; see `MUS.md`. The remaining milestones stay separate:

1. **A real synthesis backend** -- probably OPL-compatible first for Doom, with the MIDI core making a later sample/wavetable backend possible without changing MUS parsing.
2. **Standard MIDI File (`.mid`) adapter** -- only if MicroConsole actually needs arbitrary SMF playback.
3. **Tracker formats** -- only as independent adapters if a game needs them. MOD/XM/S3M/IT are not "more MIDI" and should not be pulled into the MIDI layer.

That keeps the undertaking finite: one source format and one renderer can be added at a time around a stable event boundary.
