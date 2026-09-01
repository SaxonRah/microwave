# Host tests

Standalone. No Pico SDK, no Open Watcom, no DOSBox, no audio device. The shared
mixer is compiled for the host so that the code shipping to DOS and RP2350 can
be unit tested, fuzzed and benchmarked under sanitizers.

```
cd tests
cmake --preset tests
cmake --build --preset tests
ctest --test-dir ../build-tests --output-on-failure
```

## What each binary is for

**`mw_test_unit`** — targeted checks, roughly 110 of them. The important one is
`test_block_size_invariance()`: the same scene rendered at block sizes 1, 3,
16, 64, 255, 256, 257 and 1024 must produce identical output, sample for
sample. It is the audio analogue of MicroRender's tile-seam tests, and it is
what allows each frontend to choose whatever buffer size its hardware prefers.

Also covered: span clipping and clamping, voice rejection by span
intersection, silent-block skipping as a pure optimization, pipelined versus
synchronous equivalence, looping and ping-pong, ADPCM accuracy and
seek-equals-sequential, gain saturation, wide-versus-narrow accumulator
agreement below full scale, stereo panning, envelopes, the voice bank,
sequencer determinism across buffer sizes, demo determinism, and null-argument
safety.

**`mw_test_fuzz`** — generalizes the invariance property. Each iteration builds
a random scene from random clips, voices, pitches, gains, pans and seeks, then
renders it twice at two random block sizes and requires the two results to
match. It also checks the pipelined path and a stereo pass. Everything is
seeded: a failure prints its seed and iteration, and rerunning with that seed
reproduces it exactly. ctest runs four fixed seeds.

**`mw_test_bench`** — informational, with no pass/fail threshold, because a
number from a CI runner is not a hardware promise. Reports the **realtime
ratio**: seconds of audio produced per second of CPU. Built unsanitized and
optimized even in Debug configurations, against a separately compiled copy of
the mixer, so the figure is not quietly measuring ASan.

## Why the property-based tests carry the weight

Three real bugs were caught here rather than on a device:

1. Undefined behaviour — a left shift of a negative value in the 8-bit mix
   path, caught by UBSan.
2. A sequencer row landing mid-block destroyed the outgoing note, which only
   showed up as a divergence between a 1024-frame and a 32-frame buffer.
3. The synth bypassed the wide accumulator and the whole demo rendered silent.

None of the three is the kind of thing an example program would have revealed,
and all three are the kind of thing that would have been blamed on hardware.

See `../DECISIONS.md` for the full write-ups.

## Adding a test

Put it in `mw_test_unit.c`, use `MWT_CHECK`, and give the message enough
context to diagnose the failure without a debugger — the first differing sample
index, not just "mismatch". Use `mwt_rng_t` rather than `rand()`: CI results
must reproduce.

If the thing you are testing can write into a block, check it at more than one
block size. That is where the bugs have actually been.

Assert that your sink received everything you think you sent. `mwt_sink_t`
counts `rejected_drains` for exactly this reason: a comparison against a
reference that silently lost part of itself will keep passing for the wrong
reason.
