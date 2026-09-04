/* MicroWave unit tests.
 *
 * The load-bearing test in this file is test_block_size_invariance(). It is the
 * audio equivalent of MicroRender's tile-seam equivalence tests: the same
 * scene, rendered with wildly different block sizes, must produce output that
 * is identical sample for sample. If it does not, some piece of state is
 * leaking across a block boundary, and every frontend that picks a different
 * buffer size will sound subtly different from CI.
 */

#include "mw_test_support.h"
#include "mw_music_demo.h"
#include "snd_seq.h"

int mwt_checks = 0;
int mwt_failures = 0;

#define RATE 22050
#define MAXBLK 1024

static snd_sample_t g_block[MAXBLK * 2];
static snd_sample_t g_block2[MAXBLK * 2];
#if SND_WIDE_ACCUM
static int32_t g_accum[MAXBLK * 2];
#endif

/* ------------------------------------------------------------------ */

typedef struct {
  snd_voice_t voices[4];
  int count;
} scene_t;

static void scene_mix(snd_mixer_t *m, void *user) {
  scene_t *s = (scene_t *)user;
  int i;
  for (i = 0; i < s->count; ++i)
    snd_mix_voice(m, &s->voices[i]);
}

/* ------------------------------------------------------------------ */

static void test_clip_validate(void) {
  static int16_t pcm[64];
  snd_clip_t c;

  printf("clip validation\n");
  mwt_make_ramp16(pcm, 64u);

  mwt_clip_pcm16(&c, pcm, 64u, RATE, 0u);
  MWT_CHECK(snd_clip_validate(&c) == 1, "a well-formed clip validates");

  /* Claiming more frames than the byte count can hold is the exact shape of a
     truncated pack entry, and it must be refused. */
  c.frames = 4096u;
  MWT_CHECK(snd_clip_validate(&c) == 0, "frames beyond bytes is refused");

  mwt_clip_pcm16(&c, pcm, 64u, RATE, 0u);
  c.flags = (uint8_t)(SND_CLIP_PCM16 | SND_CLIP_PCM8);
  MWT_CHECK(snd_clip_validate(&c) == 0, "two format bits is refused");

  mwt_clip_pcm16(&c, pcm, 64u, RATE, 0u);
  c.flags = (uint8_t)(SND_CLIP_PCM16 & 0u);
  MWT_CHECK(snd_clip_validate(&c) == 0, "no format bit is refused");

  mwt_clip_pcm16(&c, pcm, 64u, RATE, SND_CLIP_LOOP);
  c.loop_start = 100u;
  c.loop_end = 64u;
  MWT_CHECK(snd_clip_validate(&c) == 0, "loop start past loop end is refused");

  mwt_clip_pcm16(&c, pcm, 64u, RATE, 0u);
  c.rate = 0;
  MWT_CHECK(snd_clip_validate(&c) == 0, "zero rate is refused");

  mwt_clip_pcm16(&c, pcm, 64u, RATE, 0u);
  c.channels = 3u;
  MWT_CHECK(snd_clip_validate(&c) == 0, "three channels is refused");

  mwt_clip_pcm16(&c, pcm, 64u, RATE, 0u);
  c.data = NULL;
  MWT_CHECK(snd_clip_validate(&c) == 0, "null data is refused");

#if SND_ENABLE_ADPCM
  mwt_clip_pcm16(&c, pcm, 64u, RATE, SND_CLIP_LOOP | SND_CLIP_PINGPONG);
  c.flags = (uint8_t)(SND_CLIP_ADPCM4 | SND_CLIP_LOOP | SND_CLIP_PINGPONG);
  MWT_CHECK(snd_clip_validate(&c) == 0,
            "reverse ADPCM is refused rather than sounding wrong");
#endif
}

/* ------------------------------------------------------------------ */

static void test_span_and_window(void) {
  static int16_t pcm[512];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t sink;
  snd_voice_t v;
  long i, touched;

  printf("span clipping\n");
  mwt_make_ramp16(pcm, 512u);
  mwt_clip_pcm16(&clip, pcm, 512u, RATE, 0u);

  mwt_sink_init(&sink, 256, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);

  /* A span is per block and must survive nothing. Write only the middle
     64 frames and check the edges stayed silent. */
  snd_begin_block(&m, 0, 256);
  snd_set_span(&m, 96, 64);
  snd_voice_reset(&v);
  snd_voice_start(&v, &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
  snd_mix_voice(&m, &v);
  snd_flush_block(&m);

  touched = 0;
  for (i = 0; i < 256; ++i)
    if (mwt_sink_get(&sink, i, 0) != SND_SAMPLE_SILENCE)
      ++touched;

  MWT_CHECK(touched > 0 && touched <= 64,
            "span limited the write to at most 64 frames, wrote %ld", touched);
  MWT_CHECK_EQ_INT(mwt_sink_get(&sink, 95, 0), SND_SAMPLE_SILENCE,
                   "frame before the span is untouched");
  MWT_CHECK_EQ_INT(mwt_sink_get(&sink, 160, 0), SND_SAMPLE_SILENCE,
                   "frame after the span is untouched");

  /* An out-of-range span must clamp, not wrap or write past the block. */
  mwt_sink_reset(&sink);
  snd_begin_block(&m, 0, 256);
  snd_set_span(&m, -50, 10000);
  MWT_CHECK_EQ_INT(m.span_0, 0, "negative span start clamps to 0");
  MWT_CHECK_EQ_INT(m.span_1, 256, "oversized span end clamps to block");
  snd_flush_block(&m);
  MWT_CHECK_EQ_INT(sink.rejected_drains, 0, "no drain fell outside the sink");

  mwt_sink_free(&sink);
}

/* ------------------------------------------------------------------ */

static void test_voice_span_rejection(void) {
  static int16_t pcm[128];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t sink;
  snd_voice_t v;
  snd_mix_stats_t stats;

  printf("voice span rejection\n");
  mwt_make_ramp16(pcm, 128u);
  mwt_clip_pcm16(&clip, pcm, 128u, RATE, 0u);

  mwt_sink_init(&sink, 1024, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);

  snd_voice_reset(&v);
  snd_voice_start(&v, &clip, &m, 5000, SND_GAIN_UNITY, SND_GAIN_UNITY);

  memset(&stats, 0, sizeof(stats));
  snd_begin_block(&m, 0, 256);
  snd_mix_voice_counted(&m, &v, &stats);

  MWT_CHECK_EQ_INT(stats.voices_rejected, 1,
                   "a voice that starts later is rejected before mixing");
  MWT_CHECK_EQ_INT(m.block_touched, 0,
                   "a rejected voice never dirties the block");

  /* And a voice that has already finished. */
  snd_voice_reset(&v);
  snd_voice_start(&v, &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
  MWT_CHECK(v.end_frame > 0, "a non-looping voice has a knowable end frame");

  memset(&stats, 0, sizeof(stats));
  snd_begin_block(&m, 4096, 256);
  snd_mix_voice_counted(&m, &v, &stats);
  MWT_CHECK_EQ_INT(stats.voices_rejected, 1,
                   "a voice that already ended is rejected");

  mwt_sink_free(&sink);
}

/* ------------------------------------------------------------------ */

static void test_silent_block_skip(void) {
  static int16_t pcm[64];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t skip_sink, plain_sink;
  scene_t sc;
  long diff = -1;

  printf("silent block skipping\n");
  mwt_make_ramp16(pcm, 64u);
  mwt_clip_pcm16(&clip, pcm, 64u, RATE, 0u);

  /* One short voice near the start; everything after it is silence. */
  memset(&sc, 0, sizeof(sc));
  sc.count = 1;
  mwt_sink_init(&skip_sink, 2048, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &skip_sink);
  snd_voice_reset(&sc.voices[0]);
  snd_voice_start(&sc.voices[0], &clip, &m, 10, SND_GAIN_UNITY, SND_GAIN_UNITY);
  snd_render_blocked_ex(&m, 2048, scene_mix, &sc, SND_RENDER_SKIP_SILENT);

  MWT_CHECK(skip_sink.silent_drains > 0,
            "silent blocks were drained without being mixed (%ld of %ld)",
            skip_sink.silent_drains, skip_sink.drain_calls);

  /* Skipping must be a pure optimization: identical output either way. */
  memset(&sc, 0, sizeof(sc));
  sc.count = 1;
  mwt_sink_init(&plain_sink, 2048, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &plain_sink);
  snd_voice_reset(&sc.voices[0]);
  snd_voice_start(&sc.voices[0], &clip, &m, 10, SND_GAIN_UNITY, SND_GAIN_UNITY);
  snd_render_blocked_ex(&m, 2048, scene_mix, &sc, 0u);

  MWT_CHECK(mwt_sink_equal(&skip_sink, &plain_sink, &diff),
            "skipping silent blocks changed nothing (first diff at %ld)", diff);

  mwt_sink_free(&skip_sink);
  mwt_sink_free(&plain_sink);
}

/* ------------------------------------------------------------------ */

/* Render the same scene at several block sizes and require sample-identical
   output. This is the audio tile-seam test and it is the reason the rest of
   the library is allowed to keep state in the voice rather than the mixer. */
static void render_at_block_size(mwt_sink_t *sink, int block_frames,
                                 const snd_clip_t *a, const snd_clip_t *b,
                                 unsigned flags) {
  snd_mixer_t m;
  scene_t sc;

  mwt_sink_reset(sink);
  snd_init(&m, RATE, 1, g_block, block_frames, mwt_sink_drain, sink);

  memset(&sc, 0, sizeof(sc));
  sc.count = 2;
  snd_voice_reset(&sc.voices[0]);
  snd_voice_reset(&sc.voices[1]);
  /* Deliberately awkward: a fractional resample ratio and an odd start frame,
     so any per-block rounding shows up immediately. */
  snd_voice_start(&sc.voices[0], a, &m, 7, (int16_t)0x0143, SND_GAIN_UNITY);
  snd_voice_start(&sc.voices[1], b, &m, 331, (int16_t)0x00B7,
                  (int16_t)(SND_GAIN_UNITY / 2));

  snd_render_blocked_ex(&m, sink->frames, scene_mix, &sc, flags);
}

static void test_block_size_invariance(void) {
  static int16_t pcm[2000];
  static uint8_t pcm8[3000];
  snd_clip_t a, b;
  mwt_sink_t ref, alt;
  static const int sizes[] = {1, 3, 16, 64, 255, 256, 257, 1024};
  size_t k;
  long diff = -1;

  printf("block size invariance\n");
  mwt_make_ramp16(pcm, 2000u);
  mwt_make_ramp8(pcm8, 3000u);
  mwt_clip_pcm16(&a, pcm, 2000u, RATE, SND_CLIP_LOOP);
  mwt_clip_pcm8(&b, pcm8, 3000u, 11025, 0u);
  MWT_CHECK(snd_clip_validate(&a) && snd_clip_validate(&b),
            "invariance fixtures are valid clips");

  mwt_sink_init(&ref, 4096, 1);
  mwt_sink_init(&alt, 4096, 1);

  render_at_block_size(&ref, 256, &a, &b, 0u);
  MWT_CHECK(mwt_sink_peak(&ref) > 0, "the reference render is not silence");

  for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
    render_at_block_size(&alt, sizes[k], &a, &b, 0u);
    diff = -1;
    MWT_CHECK(mwt_sink_equal(&ref, &alt, &diff),
              "block size %d matches the 256-frame reference (first diff %ld)",
              sizes[k], diff);
  }

  /* And with silent-block skipping on, which changes the code path but must
     not change a single sample. */
  for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
    render_at_block_size(&alt, sizes[k], &a, &b, SND_RENDER_SKIP_SILENT);
    diff = -1;
    MWT_CHECK(mwt_sink_equal(&ref, &alt, &diff),
              "block size %d with skip-silent matches (first diff %ld)",
              sizes[k], diff);
  }

  mwt_sink_free(&ref);
  mwt_sink_free(&alt);
}

/* ------------------------------------------------------------------ */

static void begin_async(snd_mixer_t *m, long frame, int frames,
                        const snd_sample_t *samples, void *user) {
  /* A real target hands this to DMA and returns. The test copies immediately,
     which is the strictest possible interpretation of "the buffer is in use
     until wait returns". */
  mwt_sink_drain(m, frame, frames, samples, user);
}

static void wait_async(snd_mixer_t *m, void *user) {
  (void)m;
  (void)user;
}

static void test_pipelined_equivalence(void) {
  static int16_t pcm[1500];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t sync_sink, pipe_sink;
  scene_t sc;
  long diff = -1;

  printf("pipelined equivalence\n");
  mwt_make_ramp16(pcm, 1500u);
  mwt_clip_pcm16(&clip, pcm, 1500u, RATE, SND_CLIP_LOOP);

  mwt_sink_init(&sync_sink, 4096, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sync_sink);
  memset(&sc, 0, sizeof(sc));
  sc.count = 1;
  snd_voice_reset(&sc.voices[0]);
  snd_voice_start(&sc.voices[0], &clip, &m, 13, (int16_t)0x0121,
                  SND_GAIN_UNITY);
  snd_render_blocked_ex(&m, 4096, scene_mix, &sc, 0u);

  mwt_sink_init(&pipe_sink, 4096, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &pipe_sink);
  snd_set_async_drain(&m, begin_async, wait_async);
  memset(&sc, 0, sizeof(sc));
  sc.count = 1;
  snd_voice_reset(&sc.voices[0]);
  snd_voice_start(&sc.voices[0], &clip, &m, 13, (int16_t)0x0121,
                  SND_GAIN_UNITY);
  snd_render_blocked_pipelined(&m, g_block2, 4096, scene_mix, &sc, 0u);

  MWT_CHECK(mwt_sink_equal(&sync_sink, &pipe_sink, &diff),
            "the double-buffered path matches the synchronous one "
            "(first diff at %ld)",
            diff);
  MWT_CHECK(m.block == g_block, "pipelining restored the original block");

  mwt_sink_free(&sync_sink);
  mwt_sink_free(&pipe_sink);
}

/* ------------------------------------------------------------------ */

static void test_looping(void) {
  static int16_t pcm[100];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t sink;
  snd_voice_t v;
  int i;

  printf("looping\n");
  for (i = 0; i < 100; ++i)
    pcm[i] = (int16_t)(i * 100);

  mwt_sink_init(&sink, 1024, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);

  /* A looping voice must still be alive well past its own length. */
  mwt_clip_pcm16(&clip, pcm, 100u, RATE, SND_CLIP_LOOP);
  snd_voice_reset(&v);
  snd_voice_start(&v, &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
  MWT_CHECK_EQ_INT(v.end_frame, -1, "a looping voice has no end frame");
  for (i = 0; i < 4; ++i) {
    snd_begin_block(&m, i * 256, 256);
    snd_mix_voice(&m, &v);
    snd_flush_block(&m);
  }
  MWT_CHECK(v.active == 1, "a looping voice survives many block boundaries");

  /* A one-shot must retire itself. */
  mwt_clip_pcm16(&clip, pcm, 100u, RATE, 0u);
  snd_voice_reset(&v);
  snd_voice_start(&v, &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
  snd_begin_block(&m, 0, 256);
  snd_mix_voice(&m, &v);
  snd_flush_block(&m);
  MWT_CHECK(v.active == 0, "a one-shot voice retires when it runs out");

  /* Pingpong reverses instead of wrapping, so it must also survive. */
  mwt_clip_pcm16(&clip, pcm, 100u, RATE,
                 (uint8_t)(SND_CLIP_LOOP | SND_CLIP_PINGPONG));
  snd_voice_reset(&v);
  snd_voice_start(&v, &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
  for (i = 0; i < 4; ++i) {
    snd_begin_block(&m, i * 256, 256);
    snd_mix_voice(&m, &v);
    snd_flush_block(&m);
  }
  MWT_CHECK(v.active == 1, "a pingpong voice survives many block boundaries");

  mwt_sink_free(&sink);
}

/* ------------------------------------------------------------------ */

#if SND_ENABLE_ADPCM
static void test_adpcm(void) {
  static int16_t pcm[1600];
  static uint8_t enc[8192];
  static int16_t decoded_seq[1600];
  static int16_t decoded_seek[1600];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t sink;
  uint32_t bytes;
  long err = 0, peak = 0;
  int i;

  printf("ADPCM decode and seek\n");

  /* A smooth signal, because ADPCM is a differential coder and white noise is
     not a fair test of it. */
  for (i = 0; i < 1600; ++i) {
    long t = i;
    pcm[i] = (int16_t)(((t * 37) % 4001) - 2000);
  }

  bytes = mwt_adpcm_encode(pcm, 1600u, enc, sizeof(enc));
  MWT_CHECK(bytes > 0u, "the test encoder produced output");

  memset(&clip, 0, sizeof(clip));
  clip.data = enc;
  clip.bytes = bytes;
  clip.frames = 1600u;
  clip.rate = RATE;
  clip.channels = 1u;
  clip.flags = (uint8_t)SND_CLIP_ADPCM4;
  MWT_CHECK(snd_clip_validate(&clip) == 1, "the encoded clip validates");

  /* Straight sequential playback. */
  mwt_sink_init(&sink, 2048, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);
  {
    snd_voice_t v;
    snd_voice_reset(&v);
    snd_voice_start(&v, &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
    for (i = 0; i < 8; ++i) {
      snd_begin_block(&m, i * 256, 256);
      snd_mix_voice(&m, &v);
      snd_flush_block(&m);
    }
  }
  for (i = 0; i < 1600; ++i)
    decoded_seq[i] = (int16_t)mwt_sink_get(&sink, i, 0);

  for (i = 0; i < 1600; ++i) {
    /* Compare in the mixer's own units: a U8 build reconstructs the same
       waveform eight bits narrower, and comparing it against int16 source
       would only be measuring the format change. */
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
    long want = (long)pcm[i] / 256L;
    long d = (long)SND_SAMPLE_TO_MIX((snd_sample_t)decoded_seq[i]) - want;
#else
    long d = (long)decoded_seq[i] - (long)pcm[i];
#endif
    if (d < 0)
      d = -d;
    err += d;
    if (d > peak)
      peak = d;
  }
  MWT_CHECK(err / 1600 < (SND_FULL_SCALE / 80 + 2),
            "mean ADPCM error is small (%ld, peak %ld)", err / 1600, peak);

  /* Now decode the same clip while seeking backwards between blocks. The
     decoder must restart from a block head rather than carry stale state, so
     the result has to match the sequential decode exactly. */
  mwt_sink_reset(&sink);
  {
    snd_voice_t v;
    int order[8] = {5, 0, 3, 1, 7, 2, 6, 4};
    int k;
    snd_voice_reset(&v);
    snd_voice_start(&v, &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
    for (k = 0; k < 8; ++k) {
      int blk = order[k];
      snd_voice_seek(&v, (uint32_t)(blk * 256));
      v.active = 1;
      v.start_frame = blk * 256;
      v.end_frame = -1;
      snd_begin_block(&m, blk * 256, 256);
      snd_mix_voice(&m, &v);
      snd_flush_block(&m);
    }
  }
  for (i = 0; i < 1600; ++i)
    decoded_seek[i] = (int16_t)mwt_sink_get(&sink, i, 0);

  {
    int mismatches = 0;
    for (i = 0; i < 1536; ++i)
      if (decoded_seq[i] != decoded_seek[i])
        ++mismatches;
    MWT_CHECK_EQ_INT(mismatches, 0,
                     "seeking to a block head reproduces sequential decode");
  }

  mwt_sink_free(&sink);
}
#endif

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* master volume                                                       */
/* ------------------------------------------------------------------ */

/* A caller's own generator: it goes through snd_block_add() and knows nothing
   about master volume, which is the whole point. Before the volume moved to
   the output stage this was immune to it, and a muted mixer still produced
   full-scale output here. */
static void gen_scene(snd_mixer_t *m, void *user) {
  int f;
  (void)user;
  snd_touch_block(m);
  for (f = 0; f < m->block_frames; ++f)
    snd_block_add(m, (long)f, SND_FULL_SCALE / 2);
}

/* Render `frames` with a volume that is set once up front, and report peak. */
static long render_at_volume(mwt_sink_t *sink, int32_t vol, int block_frames) {
  snd_mixer_t m;

  mwt_sink_reset(sink);
  snd_init(&m, RATE, 1, g_block, block_frames, mwt_sink_drain, sink);
  snd_set_master_volume_now(&m, vol);
  snd_render_blocked_ex(&m, sink->frames, gen_scene, 0, 0u);
  return mwt_sink_peak(sink);
}

static void test_master_volume(void) {
  mwt_sink_t sink;
  long full, half, quarter, muted;

  printf("master volume\n");
  mwt_sink_init(&sink, 1024, 1);

  full = render_at_volume(&sink, SND_VOL_UNITY, 256);
  half = render_at_volume(&sink, SND_VOL_UNITY / 2, 256);
  quarter = render_at_volume(&sink, SND_VOL_UNITY / 4, 256);
  muted = render_at_volume(&sink, SND_VOL_SILENT, 256);

  /* The generator never consults the mixer's volume, so if these differ the
     scaling is happening at the output stage, which is what we want. */
  MWT_CHECK(full > 0, "a caller generator at full volume is audible (%ld)",
            full);
  MWT_CHECK_EQ_INT(muted, 0, "a muted mixer silences a caller generator");
  MWT_CHECK(half > full / 2 - 2 && half < full / 2 + 2,
            "half volume halves a caller generator (%ld vs %ld)", half, full);
  MWT_CHECK(quarter > full / 4 - 2 && quarter < full / 4 + 2,
            "quarter volume quarters it (%ld vs %ld)", quarter, full);

  /* Monotone all the way down, with no plateau above the format's own
     quantization floor. Composing an 8.8 master into an 8.8 voice gain used to
     give four identical steps and then a cliff well above that floor.

     How far down the steps stay distinct is a property of the output word, not
     of the mixer: the test signal is half scale, so the quietest volume that
     survives at all is the one where (SND_FULL_SCALE/2) * vol >> 16 is still
     1. S16 resolves every step of a 100-step square-law control; U8 has 256
     codes total and runs out partway down, which is the same 8-bit floor the
     DOS frontend's hardware/software split exists to stay above. Asserting the
     computed floor rather than a hardcoded percentage keeps this test honest
     in both formats. */
  {
    int pct;
    long prev = -1;
    int never_increases = 1;
    int lowest_audible = 0;

    for (pct = 100; pct >= 1; --pct) {
      long p = render_at_volume(&sink, snd_vol_from_percent(pct), 256);
      if (prev >= 0 && p > prev)
        never_increases = 0;
      if (p > 0)
        lowest_audible = pct;
      prev = p;
    }
    MWT_CHECK(never_increases,
              "the volume curve never rises as the control comes down");

    {
      /* The quietest step the format can carry, derived not assumed. */
      long signal = SND_FULL_SCALE / 2;
      int floor_pct = 100;
      for (pct = 1; pct <= 100; ++pct) {
        if ((signal * (long)snd_vol_from_percent(pct)) >> SND_VOL_SHIFT) {
          floor_pct = pct;
          break;
        }
      }
      MWT_CHECK_EQ_INT(lowest_audible, floor_pct,
                       "the control resolves every step down to the format's "
                       "own quantization floor");
    }
  }

  /* Percent round-trips, so a UI can keep its slider position in the mixer. */
  {
    int pct, ok = 1;
    for (pct = 0; pct <= 100; ++pct)
      if (snd_vol_to_percent(snd_vol_from_percent(pct)) != pct)
        ok = 0;
    MWT_CHECK(ok, "percent survives a round trip through 16.16");
  }

  /* The old 8.8 entry point still means what it used to mean. */
  {
    snd_mixer_t m;
    snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);
    snd_set_master_gain(&m, SND_GAIN_UNITY);
    MWT_CHECK_EQ_INT(snd_master_volume(&m), SND_VOL_UNITY,
                     "8.8 unity maps to 16.16 unity");
    snd_set_master_gain(&m, (int16_t)(SND_GAIN_UNITY / 2));
    MWT_CHECK_EQ_INT(snd_master_volume(&m), SND_VOL_UNITY / 2,
                     "8.8 half maps to 16.16 half");
    snd_set_master_gain(&m, (int16_t)-5);
    MWT_CHECK_EQ_INT(snd_master_volume(&m), SND_VOL_SILENT,
                     "a negative 8.8 gain clamps to silence");
  }

  mwt_sink_free(&sink);
}

/* Order the volume change before the first block, then render the whole thing
   in one call at the given block size.

   The change is deliberately NOT ordered from inside the render loop. Master
   volume is applied at resolve, after mix_scene has returned, so a target set
   partway through a render takes effect at the next block boundary -- and
   block boundaries are by definition in different places at different block
   sizes. Setting it up front removes the trigger from the comparison and
   leaves exactly the thing that could actually be wrong: whether the ramp
   advances per frame or per block. test_volume_change_is_block_quantized()
   pins the trigger granularity separately, so neither property is left to
   folklore. */
static void render_with_volume_change(mwt_sink_t *sink, int block_frames,
                                      const snd_clip_t *clip) {
  snd_mixer_t m;
  scene_t sc;

  mwt_sink_reset(sink);
  snd_init(&m, RATE, 1, g_block, block_frames, mwt_sink_drain, sink);
  snd_set_master_volume_now(&m, SND_VOL_UNITY);
  snd_set_volume_ramp(&m, 2048); /* long enough to still be moving at the end */
  snd_set_master_volume(&m, snd_vol_from_percent(30));

  memset(&sc, 0, sizeof(sc));
  sc.count = 1;
  snd_voice_reset(&sc.voices[0]);
  snd_voice_start(&sc.voices[0], clip, &m, 0, (int16_t)0x0143, SND_GAIN_UNITY);

  snd_render_blocked_ex(&m, sink->frames, scene_mix, &sc,
                        SND_RENDER_SKIP_SILENT);
}

/* The invariant the whole library is built on, extended to cover the volume
   ramp. A ramp that advanced per block rather than per frame would settle
   after a fixed number of blocks whatever their size, so a 1024-frame render
   would fade over 16 times as many frames as a 64-frame one -- and this test
   would fail at the first block size that is not 256. */
static void test_volume_ramp_block_size_invariance(void) {
  static int16_t pcm[2000];
  snd_clip_t clip;
  mwt_sink_t ref, alt;
  static const int sizes[] = {1, 3, 16, 64, 255, 256, 257, 1024};
  size_t k;
  long diff = -1;

  printf("volume ramp block size invariance\n");
  mwt_make_ramp16(pcm, 2000u);
  mwt_clip_pcm16(&clip, pcm, 2000u, RATE, SND_CLIP_LOOP);

  mwt_sink_init(&ref, 4096, 1);
  mwt_sink_init(&alt, 4096, 1);

  render_with_volume_change(&ref, 256, &clip);
  MWT_CHECK(mwt_sink_peak(&ref) > 0, "the ramped reference is not silence");

  for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
    render_with_volume_change(&alt, sizes[k], &clip);
    diff = -1;
    MWT_CHECK(mwt_sink_equal(&ref, &alt, &diff),
              "ramped volume at block size %d matches 256 (first diff %ld)",
              sizes[k], diff);
  }

  /* The ramp must actually have moved, or the test above proves nothing: two
     renders of a constant volume would match trivially. */
  {
    long early = 0, late = 0;
    long i;
    /* Through SND_SAMPLE_TO_MIX, because in the legacy U8 format a sample is
       0x80-centred and its raw magnitude says nothing about loudness. */
    for (i = 0; i < 400; ++i) {
      long v = (long)SND_SAMPLE_TO_MIX(mwt_sink_get(&ref, i, 0));
      if (v < 0)
        v = -v;
      if (v > early)
        early = v;
    }
    for (i = 3000; i < 3400; ++i) {
      long v = (long)SND_SAMPLE_TO_MIX(mwt_sink_get(&ref, i, 0));
      if (v < 0)
        v = -v;
      if (v > late)
        late = v;
    }
    MWT_CHECK(late < early / 2,
              "the volume actually came down (%ld early, %ld late)", early,
              late);
  }

  mwt_sink_free(&ref);
  mwt_sink_free(&alt);
}

/* Pin the granularity of the trigger, so that the limit is a tested property
   and not a surprise someone hits later.

   A volume set partway through a render takes effect at the start of the next
   block, because the output stage runs after mix_scene has returned. At the
   default 256-frame block that is 11.6 ms of latency on a control a person is
   dragging, which is well under the ~20 ms at which a UI feels laggy. What it
   is NOT good enough for is automating volume as a musical event -- a fade
   scheduled on a sequencer row would land on the block boundary rather than
   the row. Use a voice gain or an envelope for that; this control belongs to
   the listener, not to the song. */
static void test_volume_change_is_block_quantized(void) {
  snd_mixer_t m;
  mwt_sink_t sink;
  long done = 0;
  long i, first_change = -1;

  printf("volume change granularity\n");
  mwt_sink_init(&sink, 1024, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);
  snd_set_volume_ramp(&m, 0); /* no ramp, so the change is a clean edge */
  snd_set_master_volume_now(&m, SND_VOL_UNITY);

  while (done < sink.frames) {
    /* Ordered at frame 300, which is 44 frames into the second block. */
    if (done > 300 && snd_master_volume(&m) == SND_VOL_UNITY)
      snd_set_master_volume(&m, SND_VOL_SILENT);
    snd_render_one_block(&m, done, 256, gen_scene, 0, 0u);
    done += 256;
  }

  for (i = 0; i < sink.frames; ++i) {
    if (mwt_sink_get(&sink, i, 0) == SND_SAMPLE_SILENCE) {
      first_change = i;
      break;
    }
  }
  MWT_CHECK_EQ_INT(first_change, 512,
                   "a volume set mid-block lands on the next block boundary");

  mwt_sink_free(&sink);
}

/* A ramp exists to stop a volume change from being a step discontinuity, so
   assert the absence of the step rather than just the presence of the ramp. */
static void test_volume_ramp_has_no_step(void) {
  snd_mixer_t m;
  mwt_sink_t sink;
  long i, worst_ramped = 0, worst_instant = 0;
  int pass;

  printf("volume ramp smoothness\n");
  mwt_sink_init(&sink, 1024, 1);

  for (pass = 0; pass < 2; ++pass) {
    long done = 0;
    long worst = 0;

    mwt_sink_reset(&sink);
    snd_init(&m, RATE, 1, g_block, 64, mwt_sink_drain, &sink);
    snd_set_volume_ramp(&m, (pass == 0) ? 256 : 0);
    snd_set_master_volume_now(&m, SND_VOL_UNITY);

    /* Constant full-scale DC: every sample-to-sample difference in the output
       is therefore caused by the volume control and nothing else. */
    while (done < sink.frames) {
      if (done == 256)
        snd_set_master_volume(&m, SND_VOL_SILENT);
      snd_render_one_block(&m, done, 64, gen_scene, 0, 0u);
      done += 64;
    }

    for (i = 1; i < sink.frames; ++i) {
      long d = (long)SND_SAMPLE_TO_MIX(mwt_sink_get(&sink, i, 0)) -
               (long)SND_SAMPLE_TO_MIX(mwt_sink_get(&sink, i - 1, 0));
      if (d < 0)
        d = -d;
      if (d > worst)
        worst = d;
    }
    if (pass == 0)
      worst_ramped = worst;
    else
      worst_instant = worst;
  }

  MWT_CHECK(worst_instant > worst_ramped * 4,
            "an unramped change steps hard (%ld) and a ramped one does not "
            "(%ld)",
            worst_instant, worst_ramped);

  mwt_sink_free(&sink);
}

static void test_gain_and_saturation(void) {
  static int16_t pcm[256];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t sink;
  snd_voice_t v[8];
  int i;

  printf("gain and saturation\n");
  for (i = 0; i < 256; ++i)
    pcm[i] = (int16_t)(MWT_S16_FULL / 2);
  mwt_clip_pcm16(&clip, pcm, 256u, RATE, SND_CLIP_LOOP);

  mwt_sink_init(&sink, 256, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);

  /* Eight half-scale voices sum to four times full scale. Whatever the mix
     strategy, the output must be clamped and must never wrap. */
  snd_begin_block(&m, 0, 256);
  for (i = 0; i < 8; ++i) {
    snd_voice_reset(&v[i]);
    snd_voice_start(&v[i], &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
    snd_mix_voice(&m, &v[i]);
  }
  snd_flush_block(&m);

  MWT_CHECK(mwt_sink_peak(&sink) <= SND_FULL_SCALE + 1,
            "an overdriven mix clamps instead of wrapping (peak %ld)",
            mwt_sink_peak(&sink));
  MWT_CHECK(mwt_sink_peak(&sink) > SND_FULL_SCALE / 2,
            "an overdriven mix is actually loud (peak %ld)",
            mwt_sink_peak(&sink));

  /* Silence in must be silence out. */
  mwt_sink_reset(&sink);
  snd_begin_block(&m, 0, 256);
  snd_voice_reset(&v[0]);
  snd_voice_start(&v[0], &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_SILENT);
  snd_mix_voice(&m, &v[0]);
  snd_flush_block(&m);
  MWT_CHECK_EQ_INT(mwt_sink_peak(&sink), 0, "zero gain produces silence");

  mwt_sink_free(&sink);
}

/* ------------------------------------------------------------------ */

#if SND_WIDE_ACCUM
static void test_accumulator_agreement(void) {
  static int16_t pcm[512];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t narrow, wide;
  scene_t sc;
  long diff = -1;

  printf("wide accumulator agreement\n");
  /* Quiet enough that nothing clips, where the two strategies must agree
     exactly. Above full scale they are allowed to differ, and do. */
  {
    int i;
    for (i = 0; i < 512; ++i)
      pcm[i] = (int16_t)(((i * 61) % 2001) - 1000);
  }
  mwt_clip_pcm16(&clip, pcm, 512u, RATE, SND_CLIP_LOOP);

  mwt_sink_init(&narrow, 2048, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &narrow);
  memset(&sc, 0, sizeof(sc));
  sc.count = 2;
  snd_voice_reset(&sc.voices[0]);
  snd_voice_reset(&sc.voices[1]);
  snd_voice_start(&sc.voices[0], &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
  snd_voice_start(&sc.voices[1], &clip, &m, 31, (int16_t)0x0180,
                  SND_GAIN_UNITY);
  snd_render_blocked(&m, 2048, scene_mix, &sc);

  mwt_sink_init(&wide, 2048, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &wide);
  snd_set_accumulator(&m, g_accum);
  memset(&sc, 0, sizeof(sc));
  sc.count = 2;
  snd_voice_reset(&sc.voices[0]);
  snd_voice_reset(&sc.voices[1]);
  snd_voice_start(&sc.voices[0], &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
  snd_voice_start(&sc.voices[1], &clip, &m, 31, (int16_t)0x0180,
                  SND_GAIN_UNITY);
  snd_render_blocked(&m, 2048, scene_mix, &sc);

  MWT_CHECK(mwt_sink_equal(&narrow, &wide, &diff),
            "below full scale the narrow and wide mixes agree "
            "(first diff at %ld)",
            diff);

  /* Tones are generated rather than read from a clip, so they reach the block
     by a different route than voices do. An earlier version wrote them
     straight into m->block, which the wide accumulator's resolve step then
     overwrote with silence -- and the voice-only version of this test did not
     notice. Anything that can write to a block gets checked here. */
  {
    snd_tone_t tone;
    int blk;

    mwt_sink_reset(&narrow);
    snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &narrow);
    snd_tone_reset(&tone);
    snd_tone_start(&tone, &m, SND_WAVE_TRIANGLE, 330,
                   (int16_t)(SND_GAIN_UNITY / 4), 0, 2048);
    for (blk = 0; blk < 8; ++blk) {
      snd_begin_block(&m, blk * 256, 256);
      snd_mix_tone(&m, &tone);
      snd_flush_block(&m);
    }

    mwt_sink_reset(&wide);
    snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &wide);
    snd_set_accumulator(&m, g_accum);
    snd_tone_reset(&tone);
    snd_tone_start(&tone, &m, SND_WAVE_TRIANGLE, 330,
                   (int16_t)(SND_GAIN_UNITY / 4), 0, 2048);
    for (blk = 0; blk < 8; ++blk) {
      snd_begin_block(&m, blk * 256, 256);
      snd_mix_tone(&m, &tone);
      snd_flush_block(&m);
    }

    MWT_CHECK(mwt_sink_peak(&wide) > 0,
              "a tone survives the wide accumulator (peak %ld)",
              mwt_sink_peak(&wide));
    diff = -1;
    MWT_CHECK(mwt_sink_equal(&narrow, &wide, &diff),
              "a tone mixes identically with and without the accumulator "
              "(first diff at %ld)",
              diff);
  }

  mwt_sink_free(&narrow);
  mwt_sink_free(&wide);
}
#endif

/* ------------------------------------------------------------------ */

static void test_stereo(void) {
  static int16_t pcm[256];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t sink;
  snd_voice_t v;
  long l, r;
  int i;

  printf("stereo and panning\n");
  for (i = 0; i < 256; ++i)
    pcm[i] = (int16_t)(MWT_S16_FULL / 4);
  mwt_clip_pcm16(&clip, pcm, 256u, RATE, SND_CLIP_LOOP);

  mwt_sink_init(&sink, 256, 2);
  snd_init(&m, RATE, 2, g_block, 256, mwt_sink_drain, &sink);

  snd_voice_reset(&v);
  snd_voice_start(&v, &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
  snd_voice_set_pan(&v, SND_GAIN_UNITY, 0); /* hard left */
  snd_begin_block(&m, 0, 256);
  snd_mix_voice(&m, &v);
  snd_flush_block(&m);

  l = (long)SND_SAMPLE_TO_MIX(mwt_sink_get(&sink, 10, 0));
  r = (long)SND_SAMPLE_TO_MIX(mwt_sink_get(&sink, 10, 1));
  MWT_CHECK(l != 0 && r == 0, "hard left is silent on the right (l=%ld r=%ld)",
            l, r);

  mwt_sink_reset(&sink);
  snd_voice_reset(&v);
  snd_voice_start(&v, &clip, &m, 0, SND_GAIN_UNITY, SND_GAIN_UNITY);
  snd_voice_set_pan(&v, SND_GAIN_UNITY, (int16_t)(SND_GAIN_UNITY * 2));
  snd_begin_block(&m, 0, 256);
  snd_mix_voice(&m, &v);
  snd_flush_block(&m);

  l = (long)SND_SAMPLE_TO_MIX(mwt_sink_get(&sink, 10, 0));
  r = (long)SND_SAMPLE_TO_MIX(mwt_sink_get(&sink, 10, 1));
  MWT_CHECK(r != 0 && l == 0, "hard right is silent on the left (l=%ld r=%ld)",
            l, r);

  mwt_sink_free(&sink);
}

/* ------------------------------------------------------------------ */

static void test_synth_and_envelope(void) {
  snd_env_t e;
  snd_mixer_t m;
  mwt_sink_t sink;
  snd_tone_t t;
  int i;

  printf("synth and envelope\n");

  snd_env_init(&e, 100, 100, (int16_t)(SND_GAIN_UNITY / 2), 100);
  MWT_CHECK_EQ_INT(snd_env_level(&e, 0, -1), 0, "attack starts at zero");
  MWT_CHECK_EQ_INT(snd_env_level(&e, 100, -1), SND_GAIN_UNITY,
                   "attack reaches unity");
  MWT_CHECK_EQ_INT(snd_env_level(&e, 200, -1), SND_GAIN_UNITY / 2,
                   "decay settles at sustain");
  MWT_CHECK_EQ_INT(snd_env_level(&e, 400, -1), SND_GAIN_UNITY / 2,
                   "sustain holds");
  MWT_CHECK_EQ_INT(snd_env_level(&e, 400, 100), 0, "release reaches zero");

  MWT_CHECK(snd_note_hz(69) >= 439 && snd_note_hz(69) <= 441,
            "MIDI 69 is A440, got %d", snd_note_hz(69));
  MWT_CHECK(snd_note_hz(81) >= 879 && snd_note_hz(81) <= 881,
            "an octave up doubles, got %d", snd_note_hz(81));

  /* Every waveform must actually produce signal and must stay in range. */
  {
    static const snd_wave_t waves[5] = {SND_WAVE_SQUARE, SND_WAVE_SAW,
                                        SND_WAVE_TRIANGLE, SND_WAVE_SINE,
                                        SND_WAVE_NOISE};
    mwt_sink_init(&sink, 2048, 1);
    for (i = 0; i < 5; ++i) {
      mwt_sink_reset(&sink);
      snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);
      snd_tone_reset(&t);
      snd_tone_start(&t, &m, waves[i], 440, SND_GAIN_UNITY, 0, 2048);
      {
        int blk;
        for (blk = 0; blk < 8; ++blk) {
          snd_begin_block(&m, blk * 256, 256);
          snd_mix_tone(&m, &t);
          snd_flush_block(&m);
        }
      }
      MWT_CHECK(mwt_sink_peak(&sink) > SND_FULL_SCALE / 4,
                "wave %d produced signal (peak %ld)", i, mwt_sink_peak(&sink));
      MWT_CHECK(mwt_sink_peak(&sink) <= SND_FULL_SCALE + 1,
                "wave %d stayed in range (peak %ld)", i, mwt_sink_peak(&sink));
    }
    mwt_sink_free(&sink);
  }

  /* A tone with no assets must be deterministic across runs, noise included. */
  {
    mwt_sink_t a, b;
    long diff = -1;
    int blk;
    mwt_sink_init(&a, 1024, 1);
    mwt_sink_init(&b, 1024, 1);

    snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &a);
    snd_tone_reset(&t);
    snd_tone_start(&t, &m, SND_WAVE_NOISE, 220, SND_GAIN_UNITY, 0, 1024);
    for (blk = 0; blk < 4; ++blk) {
      snd_begin_block(&m, blk * 256, 256);
      snd_mix_tone(&m, &t);
      snd_flush_block(&m);
    }

    snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &b);
    snd_tone_reset(&t);
    snd_tone_start(&t, &m, SND_WAVE_NOISE, 220, SND_GAIN_UNITY, 0, 1024);
    for (blk = 0; blk < 4; ++blk) {
      snd_begin_block(&m, blk * 256, 256);
      snd_mix_tone(&m, &t);
      snd_flush_block(&m);
    }

    MWT_CHECK(mwt_sink_equal(&a, &b, &diff),
              "noise is reproducible from its seed (first diff %ld)", diff);
    mwt_sink_free(&a);
    mwt_sink_free(&b);
  }
}

/* ------------------------------------------------------------------ */

static const snd_event_t demo_rows[] = {
    /* two channels, four rows */
    {SND_NOTE(60), 0u, SND_VOL_KEEP, SND_VOL_KEEP},
    {SND_NOTE(48), 1u, SND_VOL_KEEP, SND_VOL_KEEP},
    {SND_NOTE_NONE, 0u, SND_VOL_KEEP, SND_VOL_KEEP},
    {SND_NOTE_NONE, 0u, SND_VOL_KEEP, SND_VOL_KEEP},
    {SND_NOTE(64), 0u, 32u, SND_VOL_KEEP},
    {SND_NOTE_NONE, 0u, SND_VOL_KEEP, SND_VOL_KEEP},
    {SND_NOTE_OFF, 0u, SND_VOL_KEEP, SND_VOL_KEEP},
    {SND_NOTE_OFF, 0u, SND_VOL_KEEP, SND_VOL_KEEP}};

static void test_sequencer(void) {
  snd_pattern_t pattern;
  snd_instrument_t instruments[2];
  snd_song_t song;
  uint8_t order[2] = {0u, 0u};
  snd_mixer_t m;
  mwt_sink_t big, small;
  snd_player_t p;
  long diff = -1;

  printf("sequencer\n");

  pattern.events = demo_rows;
  pattern.row_count = 4;
  pattern.channel_count = 2;

  memset(instruments, 0, sizeof(instruments));
  instruments[0].clip = NULL;
  instruments[0].wave = (uint8_t)SND_WAVE_SQUARE;
  instruments[0].gain = (int16_t)(SND_GAIN_UNITY / 2);
  snd_env_init(&instruments[0].env, 50, 200, (int16_t)(SND_GAIN_UNITY / 3),
               400);
  instruments[1].clip = NULL;
  instruments[1].wave = (uint8_t)SND_WAVE_TRIANGLE;
  instruments[1].gain = (int16_t)(SND_GAIN_UNITY / 2);
  snd_env_init(&instruments[1].env, 20, 100, (int16_t)(SND_GAIN_UNITY / 2),
               300);

  song.patterns = &pattern;
  song.pattern_count = 1;
  song.order = order;
  song.order_length = 2;
  song.instruments = instruments;
  song.instrument_count = 2;
  song.bpm = 120;
  song.rows_per_beat = 4;

  mwt_sink_init(&big, 32768, 1);
  snd_init(&m, RATE, 1, g_block, 1024, mwt_sink_drain, &big);
  MWT_CHECK(snd_song_length_frames(&song, &m) > 0, "the song has a length");
  MWT_CHECK_EQ_INT(snd_seq_frames_per_row(&song, RATE), (RATE * 60) / (120 * 4),
                   "frames per row matches the tempo");

  snd_player_init(&p, &song, &m, 0, 0);
  {
    long done = 0;
    while (done < 32768) {
      snd_begin_block(&m, done, 1024);
      snd_player_mix_block(&p, &m, NULL);
      snd_flush_block(&m);
      done += 1024;
    }
  }
  MWT_CHECK(mwt_sink_peak(&big) > 0, "the song produced sound");
  MWT_CHECK(p.rows_played >= 8uL, "every row in the order list was played (%lu)",
            p.rows_played);

  /* The whole point of scheduling rows on absolute frames: a target with a
     32-frame buffer must hear the identical song as one with 1024. */
  mwt_sink_init(&small, 32768, 1);
  snd_init(&m, RATE, 1, g_block, 32, mwt_sink_drain, &small);
  snd_player_init(&p, &song, &m, 0, 0);
  {
    long done = 0;
    while (done < 32768) {
      snd_begin_block(&m, done, 32);
      snd_player_mix_block(&p, &m, NULL);
      snd_flush_block(&m);
      done += 32;
    }
  }

  MWT_CHECK(mwt_sink_equal(&big, &small, &diff),
            "a 32-frame buffer plays the identical song to a 1024-frame one "
            "(first diff at %ld)",
            diff);

  mwt_sink_free(&big);
  mwt_sink_free(&small);
}

/* ------------------------------------------------------------------ */

static void test_bank(void) {
  static int16_t pcm[64];
  snd_clip_t clip;
  snd_mixer_t m;
  mwt_sink_t sink;
  snd_bank_t bank;
  int i;

  printf("voice bank\n");
  mwt_make_ramp16(pcm, 64u);
  mwt_clip_pcm16(&clip, pcm, 64u, RATE, SND_CLIP_LOOP);

  mwt_sink_init(&sink, 512, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);
  snd_bank_init(&bank);

  MWT_CHECK_EQ_INT(snd_bank_active(&bank), 0, "a fresh bank is idle");

  for (i = 0; i < SND_BANK_MAX_VOICES; ++i) {
    snd_voice_t *v = snd_bank_alloc(&bank);
    MWT_CHECK(v != NULL, "voice %d allocated", i);
    if (v) {
      snd_voice_start(v, &clip, &m, 0, SND_GAIN_UNITY,
                      (int16_t)(SND_GAIN_UNITY - i));
      v->id = (int16_t)(100 + i);
    }
  }
  MWT_CHECK(snd_bank_alloc(&bank) == NULL, "a full bank refuses to allocate");
  MWT_CHECK(snd_bank_alloc_or_steal(&bank) != NULL,
            "a full bank still steals rather than dropping a note");
  MWT_CHECK(snd_bank_find(&bank, 100) != NULL, "a voice is findable by id");
  MWT_CHECK(snd_bank_find(&bank, 999) == NULL, "an absent id is not found");

  snd_bank_stop_all(&bank);
  MWT_CHECK_EQ_INT(snd_bank_active(&bank), 0, "stop_all silences the bank");

  mwt_sink_free(&sink);
}

/* ------------------------------------------------------------------ */

static void test_null_safety(void) {
  snd_mixer_t m;
  mwt_sink_t sink;

  printf("null and degenerate arguments\n");

  /* None of these may crash. A frontend that has not finished initializing
     will call into the mixer with exactly this kind of half-state. */
  snd_init(NULL, RATE, 1, NULL, 256, NULL, NULL);
  snd_begin_block(NULL, 0, 10);
  snd_flush_block(NULL);
  snd_clear_block(NULL);
  snd_set_span(NULL, 0, 0);
  snd_mix_voice(NULL, NULL);
  snd_voice_reset(NULL);
  snd_voice_stop(NULL);
  snd_bank_init(NULL);
  MWT_CHECK(snd_bank_alloc(NULL) == NULL, "allocating from a null bank is safe");
  MWT_CHECK(snd_clip_validate(NULL) == 0, "a null clip does not validate");

  mwt_sink_init(&sink, 256, 1);
  snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &sink);

  /* A zero-length render must terminate rather than spin. */
  snd_render_blocked(&m, 0, NULL, NULL);
  MWT_CHECK_EQ_INT(sink.drain_calls, 0, "a zero-length render drains nothing");

  /* A block longer than the buffer must clamp rather than overrun. */
  snd_set_block_capacity(&m, 64);
  snd_begin_block(&m, 0, 256);
  MWT_CHECK(m.block_frames <= 64, "an oversized block clamps to capacity (%d)",
            m.block_frames);

  mwt_sink_free(&sink);
}

/* ------------------------------------------------------------------ */

/* The shared demo is what every frontend plays, so it gets the same treatment
   as any other scene: render it at two very different block sizes and require
   the results to be identical. If this passes, "DOS sounds different from the
   host" is a frontend bug and never a mixer bug. */
static void test_demo_determinism(void) {
  snd_mixer_t m;
  mwt_sink_t big, small;
  mw_demo_t demo;
  long len, done;
  long diff = -1;

  printf("shared demo determinism\n");

  mwt_sink_init(&big, 40000, 1);
  snd_init(&m, RATE, 1, g_block, 1024, mwt_sink_drain, &big);
  mw_demo_init(&demo, &m, 0, 0);
  len = mw_demo_length_frames(&demo, &m);
  /* Roughly eleven seconds at 132 BPM. The render below covers the opening
     40000 frames of it, which is enough to span several patterns. */
  MWT_CHECK(len > RATE && len < (long)RATE * 60L,
            "the demo has a sane length (%ld frames)", len);

  /* Clamp the final block. A loop that always asks for a full block would
     overrun the capture buffer on the last one, and the drain would drop it --
     which silently makes the tail of the reference render into silence and
     turns this into a much weaker test than it looks. */
  for (done = 0; done < 40000; done += 1024) {
    int n = (40000 - done < 1024) ? (int)(40000 - done) : 1024;
    snd_begin_block(&m, done, n);
    mw_demo_mix(&m, &demo);
    snd_flush_block(&m);
  }
  MWT_CHECK(mwt_sink_peak(&big) > SND_FULL_SCALE / 8,
            "the demo actually makes a noise (peak %ld)", mwt_sink_peak(&big));
  MWT_CHECK_EQ_INT(big.rejected_drains, 0,
                   "every block of the reference render reached the buffer");

  mwt_sink_init(&small, 40000, 1);
  snd_init(&m, RATE, 1, g_block, 37, mwt_sink_drain, &small);
  mw_demo_init(&demo, &m, 0, 0);
  for (done = 0; done < 40000; done += 37) {
    int n = (40000 - done < 37) ? (int)(40000 - done) : 37;
    snd_begin_block(&m, done, n);
    mw_demo_mix(&m, &demo);
    snd_flush_block(&m);
  }

  MWT_CHECK(mwt_sink_equal(&big, &small, &diff),
            "the demo is identical at 37- and 1024-frame blocks "
            "(first diff at %ld)",
            diff);

  /* And through the render loop with silent-block skipping, which the quiet
     bridge pattern in the middle of the song will actually exercise. */
  {
    mwt_sink_t skipped;
    mwt_sink_init(&skipped, 40000, 1);
    snd_init(&m, RATE, 1, g_block, 256, mwt_sink_drain, &skipped);
    mw_demo_init(&demo, &m, 0, 0);
    snd_render_blocked_ex(&m, 40000, mw_demo_mix, &demo,
                          SND_RENDER_SKIP_SILENT);
    diff = -1;
    MWT_CHECK(mwt_sink_equal(&big, &skipped, &diff),
              "the demo is unchanged by silent-block skipping "
              "(first diff at %ld)",
              diff);
    mwt_sink_free(&skipped);
  }

  mwt_sink_free(&big);
  mwt_sink_free(&small);
}

/* ------------------------------------------------------------------ */

int main(void) {
  printf("MicroWave unit tests (format=%s, wide_accum=%d, lerp=%d)\n",
         (SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_S16) ? "S16" : "U8",
         SND_WIDE_ACCUM, SND_ENABLE_LERP);

  test_clip_validate();
  test_span_and_window();
  test_voice_span_rejection();
  test_silent_block_skip();
  test_block_size_invariance();
  test_pipelined_equivalence();
  test_looping();
#if SND_ENABLE_ADPCM
  test_adpcm();
#endif
  test_gain_and_saturation();
  test_master_volume();
  test_volume_ramp_block_size_invariance();
  test_volume_ramp_has_no_step();
  test_volume_change_is_block_quantized();
#if SND_WIDE_ACCUM
  test_accumulator_agreement();
#endif
  test_stereo();
  test_synth_and_envelope();
  test_sequencer();
  test_bank();
  test_demo_determinism();
  test_null_safety();

  printf("\n%d checks, %d failures\n", mwt_checks, mwt_failures);
  return mwt_failures ? 1 : 0;
}
