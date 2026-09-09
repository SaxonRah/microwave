# MicroWave MUS source adapter

`snd_mus` is a zero-allocation reader/player for the DMX MUS music format used
by Doom-family games.  It stops at the MIDI boundary:

```text
caller-owned MUS bytes
        |
        v
     snd_mus
        |
        v
     snd_midi
        |
        +---- future OPL backend
        +---- future sample/GM backend
        `---- hardware MIDI transport
```

It does **not** synthesize audio, read WAD files, parse Standard MIDI Files,
load instrument banks, or know about MicroConsole/FastDoom.

## Why it is separate from `snd_seq`

`snd_seq` is MicroWave's compact pattern/tracker-like song representation.
MUS is a source file/event format.  Both can eventually drive generators that
mix through `snd_mixer`, but they are not forced into one sequencer model.

Likewise, MOD/XM/S3M/IT support is not implied by MUS support.  Those formats
have different timing, instruments, effects, and playback semantics and should
remain separate adapters if a game ever needs them.

## Ownership and memory

The MUS bytes stay owned by the caller. `snd_mus_open()` validates them and
stores only offsets/metadata; it never copies the score. `snd_mus_player_t` is
fully caller-owned state and advances a cursor through those bytes while
playing.

There is no malloc, stdio, clock, WAD API, audio device, or mixer dependency.
The only runtime dependency is `snd_midi`.

## Timing

MUS delay ticks are scheduled directly onto absolute output frames. Doom-family
MUS playback uses 140 ticks/second; Raptor uses 70. The tick rate is an argument
to `snd_mus_player_init()` instead of being baked into the parser.

Fractional frame remainder is carried across event groups and internal loops.
For example, at 22050 Hz and 140 MUS ticks/sec, successive one-tick boundaries
land at frames 157, 315, 472, 630 rather than accumulating a half-frame error.

`snd_mus_process_until()` uses a half-open frame boundary: an event exactly at
`end_frame` is left for the next call. This makes processing independent of the
frontend's audio block size.

## Translation rules

The adapter follows classic Doom MUS-to-MIDI behavior:

- MUS channel 15 maps to MIDI percussion channel 10 (zero-based channel 9).
- Other MUS channels are assigned MIDI channels in first-use order, skipping
  percussion channel 9.
- A newly allocated melodic MIDI channel receives All Notes Off before its
  first translated event. This also prevents notes leaking over a song loop.
- Note-on velocity is cached per MUS channel; if a Play Note event omits a new
  velocity, the previous value is reused. Initial velocity is 127.
- MUS pitch 128 maps to MIDI pitch-bend center (8192).
- Program changes become MIDI Program Change messages.
- MUS controllers map to ordinary MIDI controllers.

Valued controller mapping:

| MUS | MIDI CC | Meaning |
| ---: | ---: | --- |
| 1 | 32 | Bank select LSB |
| 2 | 1 | Modulation |
| 3 | 7 | Volume |
| 4 | 10 | Pan |
| 5 | 11 | Expression |
| 6 | 91 | Reverb |
| 7 | 93 | Chorus |
| 8 | 64 | Sustain |
| 9 | 67 | Soft pedal |

MUS controller 0 is a Program Change, not a MIDI CC.

System-event mapping:

| MUS | MIDI CC | Meaning |
| ---: | ---: | --- |
| 10 | 120 | All Sound Off |
| 11 | 123 | All Notes Off |
| 12 | 126 | Mono |
| 13 | 127 | Poly |
| 14 | 121 | Reset All Controllers |

The documented End-of-Measure event is accepted as a no-op. The unused type-7
event consumes its compatibility byte and emits nothing. This is intentionally
more tolerant than old converters that rejected those event types outright.

## Validation

`snd_mus_open()` scans the score once before playback and rejects:

- bad/truncated headers;
- invalid score/instrument ranges;
- truncated event payloads;
- invalid controller numbers;
- overflowing/malformed variable-length delays;
- scores with no Finish event.

A zero-duration score is valid for one-shot playback but is refused if asked to
loop, preventing an audio callback from spinning forever at one frame.

## API sketch

```c
snd_mus_song_t song;
snd_mus_player_t player;
snd_midi_t midi;

if (snd_mus_open(&song, lump_data, lump_bytes) != SND_MUS_OK)
    return;

snd_mus_player_init(&player, &song,
                    22050, SND_MUS_DOOM_TICK_HZ,
                    first_audio_frame, 1);

/* Before mixing/presenting each half-open audio window: */
snd_mus_process_until(&player, &midi, block_end_frame);
```

The next layer is a MIDI synthesis backend. `snd_mus` should not change when
that backend is added.

## Tests

`mw_test_mus` uses synthetic, non-copyrighted MUS buffers and covers:

- header/instrument metadata;
- every truncated prefix of a valid multi-event score;
- malformed event/controller/delay cases;
- all 16 MUS channels and MIDI percussion reservation;
- cached note velocity;
- all valued/system controller mappings;
- pitch conversion;
- 140 Hz and 70 Hz timing;
- fractional-frame carry;
- half-open event scheduling;
- processing-window invariance;
- looping and zero-duration loop rejection;
- documented no-op/unused events.
