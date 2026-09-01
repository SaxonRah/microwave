# Decisions

The same log MicroRender keeps, for the same reason: a record of what was
predicted, what was measured, and what the measurement actually justified.
Rejections are recorded as carefully as changes, because "we tried it and it
did not help" is the most expensive information in the repository and the
easiest to lose.

Host figures below come from `tests/mw_test_bench.c` at `-O2` with no
sanitizers, 22050 Hz, 256-frame blocks. Run-to-run variance on the reference
host is roughly ±7%, which is larger than several of the effects measured, and
that is itself a result.

---

## 1. Saturating mix versus a wide accumulator — BOTH KEPT, and the benchmark was the wrong question

**Predicted.** Summing voices into an `int32` scratch and clamping once at the
end of the block should cost measurable time versus a saturating 16-bit add per
voice, because it doubles the traffic through the block. The saturating path
was expected to be meaningfully faster and therefore the right default for DOS
and RP2350.

**Measured.** Nothing. On the reference host, 32 voices of PCM16:

| build | run 1 | run 2 | run 3 |
| --- | --- | --- | --- |
| `SND_WIDE_ACCUM=1` | 240.7x | 238.9x | 245.2x |
| `SND_WIDE_ACCUM=0` | 248.5x | 261.7x | 244.1x |

The spread within a single build is larger than the gap between builds. The
branch on `m->accum` predicts perfectly and the extra `int32` array stays in
L1 at this block size, so there is no signal to find.

**What that actually justified.** Not the change the benchmark was run to
support. The real argument for the saturating path was never cycles, it was
**memory**: `SND_WIDE_ACCUM` costs one `int32` per frame of block, which is
1 KiB for a 256-frame block. That is nothing on a desktop, noticeable on an
RP2350, and a genuine consideration on a 386 where it competes with sample
data for a 64 KiB segment.

So both paths stayed, the default is chosen by target rather than by speed, and
the benchmark was demoted to what it actually is: a check that neither path is
pathological. The quality argument is separate and real — saturating adds are
order-dependent once a mix genuinely clips, and `test_accumulator_agreement()`
asserts the two agree exactly below full scale.

**Lesson.** The benchmark answered a question nobody needed answered. The
constraint was always the memory budget, and no amount of host timing was going
to reveal that.

---

## 2. Mid-block row triggers — FIXED, and it was a real bug

**Symptom.** `test_sequencer()` renders the same song through a 1024-frame
buffer and a 32-frame buffer and requires identical output. It diverged at
frame 5120, with a constant offset that alternated sign — the signature of one
channel's square wave being exactly half a cycle out.

**Cause.** Rows were triggered for the whole block up front, then every channel
was mixed once across the whole block. When a row landed *inside* a block,
triggering it replaced the channel's note before the outgoing note had been
mixed for the frames before the trigger point. With a 1024-frame buffer the
lost region was 392 frames; with a 32-frame buffer it was nearly zero. Two
targets with different buffer sizes played different music.

**Fix.** `snd_player_mix_block()` now subdivides the block at row boundaries and
mixes each segment with `snd_set_span()` set to that segment. This is precisely
what spans are for, and precisely the manoeuvre the renderer makes when a
sprite straddles a tile edge.

> Subdivide, do not approximate.

**Cost.** One extra span assignment per row per block. Unmeasurable: rows are
thousands of frames apart, so the common case is a single segment.

**Lesson.** The class of bug — "state applied at block granularity instead of
frame granularity" — is exactly what the block-size invariance property exists
to catch, and it caught it on the first run. A sequencer that had only ever
been tested at one buffer size would have shipped this.

---

## 3. Generators writing directly to the block — FIXED, and the test suite had a hole

**Symptom.** With `SND_WIDE_ACCUM` enabled and an accumulator installed, the
entire demo song rendered as silence. The unit tests were green.

**Cause.** `snd_mix_tone()` read and wrote `m->block[]` directly. The voice
mixers went through the `SND_ACC` macro and therefore into the accumulator; the
synth did not. `snd_flush_block()` then resolved the (all-zero) accumulator over
the block, erasing every tone. The demo is built entirely from tones, so it was
entirely silent.

**Why the tests missed it.** `test_accumulator_agreement()` existed and passed,
but it only ever mixed *voices*. The accumulator was never exercised against a
generator.

**Fix.** `snd_touch_block()` and `snd_block_add()` are now part of the public
API, and every generator goes through them. The header says plainly that
writing to `m->block` by hand works right up until someone enables the wide
accumulator. The accumulator test now covers tones as well as voices.

**Lesson.** A test that covers a feature is not the same as a test that covers
every route into that feature. The hole was not "no accumulator test", it was
"the accumulator test only knew about one kind of writer".

---

## 4. Reverse ADPCM — REJECTED, refused in validation instead

**Considered.** Supporting `SND_CLIP_ADPCM4 | SND_CLIP_PINGPONG`, so compressed
clips could ping-pong loop like raw ones.

**Rejected.** IMA ADPCM is differential: a nibble cannot be decoded without the
one before it. Playing backwards means restarting from a block head and
decoding forward to the target frame *on every single output frame*, up to 504
nibble decodes per sample. That is not a slow path, it is a different
algorithm wearing the same name.

**What shipped instead.** `snd_clip_validate()` returns 0 for the combination.
Interleaved stereo ADPCM is refused for the same reason — it would need a
second decoder state per voice and a second header per block. Convert to two
mono clips.

**Reasoning.** A library that silently sounds wrong is worse than one that says
no. Refusing at validation time means the failure surfaces once, at load, with
a clear cause, rather than as an intermittent artefact on one target.

---

## 5. Linear pan law — KEPT over constant-power

**Considered.** Constant-power panning, which keeps perceived loudness even as
a source moves across the stereo field.

**Rejected.** It needs a sine or a table lookup per pan change. Linear panning
is a subtract and a shift. Against 8-bit DMA output on a Sound Blaster, the
difference is inaudible; against 16-bit output it is a small dip in the centre
that no one has ever complained about in a game.

**Note.** `snd_voice_set_pan()` scales back up by two so that a centred voice
matches the mono case, rather than being 6 dB quieter than the same voice
before panning was applied. That is the part people actually notice.

---

## 6. Nearest-neighbour resampling by default — KEPT, with linear available

**Predicted.** Linear interpolation should cost roughly one multiply and one
add per frame per voice, measurable at high voice counts.

**Measured.** Within noise on the host at 32 voices; the loop is memory-bound
long before the arithmetic matters.

**Kept anyway, as a compile-time option.** The reason is the same as decision 1:
the host benchmark is not the constraint. On a 386 the extra multiply is real,
and `SND_ENABLE_LERP=0` exists for exactly that target. Correctness does not
depend on it, output quality does. Compare `GFX_ENABLE_TRIANGLES`.

---

## 7. Specializing the per-format inner mix loops — NOT DONE, and deliberately

**Considered.** The mix loops currently fetch through a small inline helper and
branch on mono-versus-stereo output per frame. Splitting into fully specialized
loops — mono clip to mono out, mono clip to stereo out, stereo clip to stereo
out, each with and without interpolation — would remove those branches. This is
what MicroRender does when it separates clipped and unclipped blitters.

**Not done.** Twelve near-identical loops is twelve places for a divergence
bug to hide, and the block-size invariance property would have to hold across
all of them. The current structure is one loop per source format, which is
already the split that matters, because the source format determines the fetch
and the fetch is the expensive part.

**Revisit when.** A real 386 measurement shows the branch costing something. Not
before. Recorded here so the option is not rediscovered from scratch.

---

## 8. Silent-block skipping — KEPT, and it is free

**Predicted.** Skipping the clear and the mix for blocks no voice touches
should help on targets where a `memset` of the block is a real fraction of the
budget.

**Implementation detail that made it work.** The clear is lazy.
`snd_touch_block()` clears the block on the *first* write, so a block nothing
writes to is never zeroed at all. Without that, `SND_RENDER_SKIP_SILENT` would
still pay for a clear it then threw away.

**Correctness requirement.** The drain must accept a `NULL` sample pointer as
"this many frames of silence". A DMA target can then repeat a pre-zeroed buffer
instead of rewriting one. Both the unit test and the fuzz harness assert that
enabling the flag changes no output sample anywhere, so it is a pure
optimization or it is a bug.

---

## Process failures worth recording

**The first benchmark measured the wrong thing.** It reported blocks per second,
which is a number that means nothing without also knowing the block size. It
was changed to report the realtime ratio — seconds of audio per second of CPU —
which is the only figure that answers the actual question, "will this keep up
on the target".

**The U8 sample mode was silently broken and passing its tests.** Reading a
PCM16 clip in `SND_SAMPLE_U8=1` mode clamped every sample flat instead of
scaling it down, because the source width and the mixer's width were conflated.
The tests passed because the *fixtures* were also built against
`SND_FULL_SCALE`, so both sides were wrong in the same direction. Fixed by
introducing `SND_SRC_FROM_S16` / `SND_SRC_FROM_U8` as the only two places a
source width is converted, and by giving the tests an explicit `MWT_S16_FULL`
so fixture data is always authored at its own natural width.

The general shape — a test and the code under test sharing a wrong assumption —
is the failure mode that a variant matrix catches and a single build never
will.

**A determinism test was quietly weaker than it looked.** The demo comparison
rendered its reference with a loop that always asked for a full 1024-frame
block, including the last one, which ran past the end of the capture buffer.
The capture sink correctly refused that write, so the final 64 frames of the
reference were silence rather than audio — and the comparison against them
still passed, because at that point in the song every voice happened to have
finished its release. The gap only surfaced when the `u8` variant's rounding
made those frames non-silent by one count.

Fixed by clamping the final block and, more usefully, by asserting
`rejected_drains == 0`. A test that discards part of its own reference will
keep passing for the wrong reason until something unrelated perturbs it.

---

## Summary

| # | Decision | Outcome |
| --- | --- | --- |
| 1 | Saturating mix vs wide accumulator | Both kept; chosen by memory budget, not speed |
| 2 | Mid-block row triggers | Fixed; block now subdivided at row boundaries |
| 3 | Generators writing to the block directly | Fixed; `snd_block_add()` is the only route |
| 4 | Reverse / stereo ADPCM | Rejected; refused in `snd_clip_validate()` |
| 5 | Constant-power pan | Rejected; linear kept |
| 6 | Linear interpolation | Kept as a compile-time option, default on |
| 7 | Specialized per-format mix loops | Not done; recorded so it is not rediscovered |
| 8 | Silent-block skipping | Kept; lazy clear is what makes it free |

Three of the eight entries are bugs the test suite found before any hardware
was involved. That ratio is the argument for the test suite.
