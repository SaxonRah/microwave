# MicroWave

A C99 software audio mixer for DOS 16-bit, RP2350, and desktop. It is the audio
counterpart of [MicroRender](https://github.com/SaxonRah/microrender), built to
the same rule and with the same shape, and it runs standalone or alongside it.

MicroRender's rule is that **the renderer allocates nothing**: the caller hands
it a tile buffer and a flush callback, and the same rasterizer runs on
segmented real-mode DOS, on a Pico driving an ILI9341 over SPI, and on a
desktop under a sanitizer.

MicroWave's rule is the same sentence with two words changed. **The mixer
allocates nothing.** The caller hands it a block buffer and a drain callback.

```c
snd_sample_t block[256];          /* yours, not the mixer's */
snd_mixer_t  mixer;

snd_init(&mixer, 22050, 1, block, 256, my_drain, &my_device);
snd_render_blocked(&mixer, total_frames, mix_scene, &game);
```

That is the whole contract. Everything below follows from it.

## The correspondence

The two libraries are deliberately isomorphic. Anyone who has read `gfx.c`
should be able to read `snd.c` without a map.

| MicroRender | MicroWave | what it is |
| --- | --- | --- |
| tile | block | a slice of the output, reused |
| `gfx_flush_fn` | `snd_drain_fn` | hand the finished slice to hardware |
| clip rectangle | span | which part of the slice may be written |
| `gfx_sprite_t` | `snd_clip_t` | immutable source data |
| `gfx_blit` | `snd_mix_voice` | composite source into the slice |
| dirty rectangles | silent-block skip | do no work when nothing changed |
| RGB565 | S16 @ 22050 Hz | one logical format, converted on output |
| RLE + per-row seek index | IMA ADPCM self-restarting blocks | compressed, still seekable |
| `gfx_font5x7` | `snd_synth` tones | output with zero assets |
| `gfx_draw_tilemap` | `snd_player_mix_block` | an index array walked against a cursor |
| `gfx_render_tiled_pipelined` | `snd_render_blocked_pipelined` | mix N+1 while DMA drains N |
| `GFX_FAST_WORD_COPY` | `SND_WIDE_ACCUM` | a 32-bit-only optimization |

There is one logical mix format — signed 16-bit at 22050 Hz — and it is
converted only in the drain callback. DOS calls `snd_pack_u8()` to get the
bytes a Sound Blaster wants; the Pico does the same for PWM; Raylib calls
`snd_pack_float()`. That is exactly where and why MicroRender quantizes RGB565
to a VGA palette during presentation, and nowhere else.

## Standalone or combined

MicroWave has no dependency on MicroRender. It builds, tests and runs on its
own.

But if you already use MicroRender, the two share an asset container.
MicroRender's `MRP1` pack already reserves entry kind 10 for 8-bit audio, so
`snd_pack_open()` accepts both magics and `snd_pack_load_clip()` reads a
`GAME.MRP` directly:

```c
snd_pack_t pack;
snd_clip_t pickup;
static unsigned char pool[64 * 1024];      /* still your memory, not ours */

snd_pack_open(&pack, "GAME.MRP");          /* MicroRender's own pack file */
snd_pack_load_clip(&pack, "pickup", pool, sizeof pool, &pickup);
```

No second asset pipeline, no duplicated samples, no converter step. Anything
`mr_pack.py --wav` produced is already in a format this library mixes. CI proves
it: the `asset-pipeline` job clones MicroRender, builds a pack with *their*
packer, and reads it with *this* reader.

Going the other way, `mw_pack.py` writes `MWP1` packs that additionally carry
S16 and ADPCM audio and song data. Those kinds are numbered from 32 up, above
anything `mr_pack.py` emits, so MicroRender's reader reports them as unknown
rather than misreading them.

## Zero assets required

The bundled demo song is synthesized from `snd_synth`'s oscillators, so it
plays on a Pico with empty flash, in a DOS box with no `GAME.MWP` next to the
executable, and in CI with nothing checked out. That is the same reason
MicroRender carries a 5x7 font: the frontend can prove itself before the asset
pipeline exists.

## The test that holds the library up

`test_block_size_invariance()` renders the same scene at block sizes 1, 3, 16,
64, 255, 256, 257 and 1024, and requires the results to be **identical sample
for sample**. It is the audio analogue of MicroRender's tile-seam tests, and it
is the reason every frontend here is free to pick whatever buffer size its
hardware wants.

The fuzz harness generalizes it: build a random scene, render it at two random
block sizes, require identical output. Almost any state that leaks across a
block boundary, any off-by-one in span intersection, and any resample phase
that is recomputed rather than carried will break that property.

It has already earned its keep. Three real bugs, all caught here rather than on
a device:

1. **Undefined behaviour** — left-shifting a negative value in the 8-bit mix
   path. Caught by UBSan, fixed with a multiply.
2. **A sequencer row landing mid-block destroyed the outgoing note.** A 1024-frame
   buffer diverged from a 32-frame buffer at frame 5120, because triggering a
   row replaced the channel's note before the old one had been mixed for the
   earlier part of that block. Fixed by subdividing the block at row boundaries
   with `snd_set_span()` — the same manoeuvre the renderer makes when a sprite
   straddles a tile edge. Subdivide, do not approximate.
3. **The synth bypassed the wide accumulator**, writing straight into
   `m->block`, which the resolve step then overwrote with silence. The whole
   demo was silent with `SND_WIDE_ACCUM` on. Fixed by routing every generator
   through `snd_block_add()`. The accumulator test had only ever used voices;
   it now covers tones too.

## Quick start

```
git clone https://github.com/SaxonRah/microwave.git
cd microwave

.\mw.bat test all          # unit + fuzz, ASan+UBSan, every sample format
.\mw.bat build headless    # frontend with the device compiled out
.\mw.bat run headless --seconds 10 --wav demo.wav
.\mw.bat bench             # informational realtime ratio
```

On Linux or macOS, skip `mw.bat`:

```
cd tests
cmake --preset tests && cmake --build --preset tests
ctest --test-dir ../build-tests --output-on-failure
```

## Platform quick starts

### Desktop (Raylib)

Raylib owns the device; MicroWave owns the mix. The mixer is driven by the
stream's appetite rather than a frame timer:

```c
while (IsAudioStreamProcessed(stream) && produced < total) {
    snd_render_one_block(&mixer, produced, MW_BLOCK,
                         mw_demo_mix, &demo, SND_RENDER_SKIP_SILENT);
    produced += MW_BLOCK;
}
```

The same `main.c` compiles with `-DMW_HEADLESS=1` into a binary that renders to
a WAV with no audio device, which is what CI runs.

### RP2350

The target the pipelined path exists for. Install an async drain and one call
overlaps every DMA transfer with the mix of the next block:

```c
snd_set_async_drain(&mixer, mw_drain_begin, mw_drain_wait);
snd_render_blocked_pipelined(&mixer, second_block, frames,
                             mw_demo_mix, &demo, SND_RENDER_SKIP_SILENT);
```

No audio thread, no ring buffer, no lock.

### DOS

8-bit auto-init DMA to a Sound Blaster. `snd_pack_u8()` in the drain is the
entire target-specific transformation; `SND_RENDER_SKIP_SILENT` turns a silent
block into a `memset`, which on a 386 is worth having.

## Repository layout

```
shared/src/          the library
  snd_config.h       build-time knobs and target detection
  snd_sample.h       sample format selection (S16 default, U8 legacy)
  snd_fixed.h        16.16 phase accumulator
  snd.h  snd.c       the mixer: blocks, spans, voices, clips, ADPCM, banks
  snd_synth.h/.c     oscillators and envelopes, no assets required
  snd_seq.h/.c       pattern sequencer, rows scheduled on absolute frames
  snd_pack.h/.c      MWP1 and MRP1 pack reader
  mw_music_demo.h/.c the deterministic demo every frontend plays
shared/tools/
  mw_pack.py         asset packer, WAV in, MWP1 out, ADPCM encoder
tests/               unit, fuzz and benchmark harnesses + CMake presets
microwave/           RP2350 frontend
microwave_dos/       DOS Sound Blaster frontend
microwave_raylib/    desktop frontend (and the headless build CI uses)
scripts/             build, run and clean dispatchers; mw_tools.bat locates toolchains
mw.bat               single entry point, sibling of MicroRender's mr.bat
```

## Layering

Three targets, deliberately separate:

- `microwave::snd` — `snd.c` + `snd_synth.c`. No stdio, no malloc, no clock, no
  song. CI asserts this by checking the archive's undefined symbols.
- `microwave::seq` — the sequencer. An application can use the mixer without
  accepting a tracker's idea of what a song is.
- `microwave::pack` — the pack reader. Needs stdio, which a bare-metal build
  may not want.

## Known constraints

These are real limits, chosen deliberately, not oversights.

- **A 16.16 phase caps one voice at 65535 frames**, about three seconds at
  22050 Hz. This keeps the inner loop to a 32-bit add on a 386. Long-form audio
  wants streaming, not a resident clip. `SND_FIXED_WIDE` lifts it on 64-bit
  hosts.
- **Voices are summed with a saturating add by default.** No second buffer,
  which matters on DOS and RP2350, and it is what period trackers did.
  Saturating adds are order-dependent once a mix genuinely clips;
  `SND_WIDE_ACCUM=1` sums into int32 and clamps once, which is
  order-independent and costs one int32 per frame of block. The two agree
  exactly below full scale, and a test asserts it.
- **ADPCM is forward-only.** Reverse playback would reseek to a block head on
  every output frame, so `snd_clip_validate()` refuses `ADPCM4 | PINGPONG`
  rather than letting it sound wrong. Interleaved stereo ADPCM is refused for
  the same reason — convert to two mono clips.
- **The pan law is linear, not constant-power.** One shift on a 386, and the
  difference is inaudible against 8-bit DMA output.
- **Tempo is quantized to whole frames per row.** That is what keeps two
  targets at the same mix rate sample-identical instead of drifting apart over
  a few minutes.
- **`snd_clip_validate()` is not optional.** The mix loops do not re-check
  bounds per block, by design. Anything decoded from a pack or any other
  untrusted source must go through it once after loading. `snd_pack_load_clip()`
  does this for you.
- **The DOS and Pico frontends are not covered by CI**, because CI has no Sound
  Blaster and no silicon. The mixer they call is covered exhaustively. Both
  files say so in their own header comments, and both are kept small
  specifically so the untested part stays legible.

## Benchmark

Informational, with no threshold, for the same reason MicroRender's has none: a
number from a CI runner is not a hardware promise. Run it on the target you
care about. The figure that matters for audio is the **realtime ratio** —
seconds of audio produced per second of CPU.

Host reference, 22050 Hz, 256-frame blocks, `-O2`, no sanitizers:

| case | voices | realtime ratio |
| --- | --- | --- |
| PCM16 mono | 1 | ~10500x |
| PCM16 mono | 8 | ~1175x |
| PCM16 mono | 32 | ~305x |
| PCM8 mono | 32 | ~400x |
| PCM16, stereo out | 32 | ~290x |

A target must stay above 1.0x with the voice count it actually uses.

## License

MIT. See `LICENSE`.
