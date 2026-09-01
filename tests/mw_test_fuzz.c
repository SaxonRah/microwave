/* MicroWave deterministic fuzz harness.
 *
 * Every iteration builds a random scene and renders it twice at two different
 * block sizes, then requires the two renders to be identical sample for
 * sample. That single property is worth more than a pile of range assertions:
 * almost any state that leaks across a block boundary, any off-by-one in span
 * intersection, and any resample phase that is recomputed rather than carried
 * will break it.
 *
 * Everything is seeded. A failure prints its seed and iteration, and rerunning
 * with that seed reproduces it exactly.
 */

#include "mw_test_support.h"
#include "snd_seq.h"
#include <stdlib.h>

int mwt_checks = 0;
int mwt_failures = 0;

#define RATE 22050
#define MAX_BLOCK 512
#define RENDER_FRAMES 3072
#define MAX_VOICES 6
#define CLIP16_FRAMES 977
#define CLIP8_FRAMES 613
#define ADPCM_FRAMES 1300

static snd_sample_t g_block[MAX_BLOCK * 2];
static snd_sample_t g_block2[MAX_BLOCK * 2];

static int16_t g_pcm16[CLIP16_FRAMES];
static int16_t g_pcm16_st[CLIP16_FRAMES * 2];
static uint8_t g_pcm8[CLIP8_FRAMES];
static int16_t g_adpcm_src[ADPCM_FRAMES];
static uint8_t g_adpcm[8192];

static snd_clip_t g_clips[4];
static int g_clip_count;

typedef struct {
  snd_voice_t voices[MAX_VOICES];
  snd_tone_t tones[2];
  int voice_count;
  int tone_count;
} fscene_t;

static void fscene_mix(snd_mixer_t *m, void *user) {
  fscene_t *s = (fscene_t *)user;
  int i;
  for (i = 0; i < s->voice_count; ++i)
    snd_mix_voice(m, &s->voices[i]);
  for (i = 0; i < s->tone_count; ++i)
    snd_mix_tone(m, &s->tones[i]);
}

static void build_clips(mwt_rng_t *rng) {
  uint32_t bytes;
  int i;

  for (i = 0; i < CLIP16_FRAMES; ++i)
    g_pcm16[i] = (int16_t)(mwt_rand(rng, -32000, 32000));
  for (i = 0; i < CLIP16_FRAMES * 2; ++i)
    g_pcm16_st[i] = (int16_t)(mwt_rand(rng, -32000, 32000));
  for (i = 0; i < CLIP8_FRAMES; ++i)
    g_pcm8[i] = (uint8_t)mwt_rand(rng, 0, 255);
  /* ADPCM is differential, so give it something it can actually track. */
  for (i = 0; i < ADPCM_FRAMES; ++i)
    g_adpcm_src[i] = (int16_t)(((i * 53) % 6001) - 3000);

  g_clip_count = 0;

  mwt_clip_pcm16(&g_clips[g_clip_count], g_pcm16, CLIP16_FRAMES, RATE, 0u);
  ++g_clip_count;

  mwt_clip_pcm8(&g_clips[g_clip_count], g_pcm8, CLIP8_FRAMES, 11025, 0u);
  ++g_clip_count;

  memset(&g_clips[g_clip_count], 0, sizeof(g_clips[0]));
  g_clips[g_clip_count].data = g_pcm16_st;
  g_clips[g_clip_count].frames = CLIP16_FRAMES;
  g_clips[g_clip_count].bytes = CLIP16_FRAMES * 4u;
  g_clips[g_clip_count].rate = 44100;
  g_clips[g_clip_count].channels = 2u;
  g_clips[g_clip_count].flags = (uint8_t)SND_CLIP_PCM16;
  ++g_clip_count;

#if SND_ENABLE_ADPCM
  bytes = mwt_adpcm_encode(g_adpcm_src, ADPCM_FRAMES, g_adpcm, sizeof(g_adpcm));
  if (bytes > 0u) {
    memset(&g_clips[g_clip_count], 0, sizeof(g_clips[0]));
    g_clips[g_clip_count].data = g_adpcm;
    g_clips[g_clip_count].bytes = bytes;
    g_clips[g_clip_count].frames = ADPCM_FRAMES;
    g_clips[g_clip_count].rate = RATE;
    g_clips[g_clip_count].channels = 1u;
    g_clips[g_clip_count].flags = (uint8_t)SND_CLIP_ADPCM4;
    ++g_clip_count;
  }
#else
  (void)bytes;
  (void)g_adpcm;
#endif

  for (i = 0; i < g_clip_count; ++i) {
    if (!snd_clip_validate(&g_clips[i])) {
      printf("  FAIL fixture clip %d did not validate\n", i);
      ++mwt_failures;
    }
    ++mwt_checks;
  }
}

/* Populate a scene from the RNG. Called twice with the same seed so both
   renders start from byte-identical state. */
static void build_scene(fscene_t *sc, mwt_rng_t *rng, const snd_mixer_t *m) {
  int i;

  memset(sc, 0, sizeof(*sc));
  sc->voice_count = mwt_rand(rng, 1, MAX_VOICES);
  sc->tone_count = mwt_rand(rng, 0, 2);

  for (i = 0; i < sc->voice_count; ++i) {
    const snd_clip_t *clip = &g_clips[mwt_rand(rng, 0, g_clip_count - 1)];
    snd_clip_t local = *clip;
    long start = mwt_rand(rng, -200, RENDER_FRAMES + 200);
    int pitch = mwt_rand(rng, 0x0020, 0x0600);
    int gain = mwt_rand(rng, 0, SND_GAIN_UNITY);

    /* Loop flags are randomized per voice by copying the clip, but the copy
       must live as long as the voice, so loop state is applied to the shared
       fixture instead. Keep it simple: use the clip as authored. */
    (void)local;

    snd_voice_reset(&sc->voices[i]);
    snd_voice_start(&sc->voices[i], clip, m, start, (int16_t)pitch,
                    (int16_t)gain);
    if (mwt_rand(rng, 0, 3) == 0)
      snd_voice_set_pan(&sc->voices[i], (int16_t)gain,
                        (int16_t)mwt_rand(rng, 0, SND_GAIN_UNITY * 2));
    if (mwt_rand(rng, 0, 4) == 0)
      snd_voice_seek(&sc->voices[i],
                     (uint32_t)mwt_rand(rng, 0, (int)clip->frames - 1));
    sc->voices[i].id = (int16_t)i;
  }

  for (i = 0; i < sc->tone_count; ++i) {
    snd_env_t env;
    snd_tone_reset(&sc->tones[i]);
    snd_tone_start(&sc->tones[i], m,
                   (snd_wave_t)mwt_rand(rng, 0, SND_WAVE_COUNT - 1),
                   mwt_rand(rng, 20, 8000), (int16_t)mwt_rand(rng, 0, 256),
                   mwt_rand(rng, 0, RENDER_FRAMES),
                   mwt_rand(rng, 0, RENDER_FRAMES));
    snd_env_init(&env, mwt_rand(rng, 0, 500), mwt_rand(rng, 0, 500),
                 (int16_t)mwt_rand(rng, 0, 256), mwt_rand(rng, 0, 500));
    snd_tone_set_env(&sc->tones[i], &env);
  }
}

static void render(mwt_sink_t *sink, int block_frames, int channels,
                   unsigned long long seed, unsigned flags, int pipelined) {
  snd_mixer_t m;
  fscene_t sc;
  mwt_rng_t rng;

  mwt_sink_reset(sink);
  snd_init(&m, RATE, channels, g_block, block_frames, mwt_sink_drain, sink);
  snd_set_master_gain(&m, SND_GAIN_UNITY);

  mwt_rng_seed(&rng, seed);
  build_scene(&sc, &rng, &m);

  if (pipelined) {
    snd_set_async_drain(&m, NULL, NULL); /* no async: falls back, still valid */
    snd_render_blocked_pipelined(&m, g_block2, RENDER_FRAMES, fscene_mix, &sc,
                                 flags);
  } else {
    snd_render_blocked_ex(&m, RENDER_FRAMES, fscene_mix, &sc, flags);
  }
}

int main(int argc, char **argv) {
  int iterations = (argc > 1) ? atoi(argv[1]) : 200;
  unsigned long long seed =
      (argc > 2) ? strtoull(argv[2], NULL, 0) : 0x5EEDULL;
  mwt_rng_t meta;
  mwt_sink_t a, b;
  int it;

  printf("MicroWave fuzz: %d iterations, seed 0x%llX\n", iterations, seed);

  mwt_rng_seed(&meta, seed);
  build_clips(&meta);

  mwt_sink_init(&a, RENDER_FRAMES, 1);
  mwt_sink_init(&b, RENDER_FRAMES, 1);

  for (it = 0; it < iterations; ++it) {
    unsigned long long s = seed + (unsigned long long)it * 0x9E3779B97F4A7C15ULL;
    int bs_a = 1 + (int)(s % MAX_BLOCK);
    int bs_b = 1 + (int)((s >> 17) % MAX_BLOCK);
    unsigned flags = ((s >> 33) & 1u) ? SND_RENDER_SKIP_SILENT : 0u;
    long diff = -1;

    render(&a, bs_a, 1, s, 0u, 0);
    render(&b, bs_b, 1, s, flags, 0);

    ++mwt_checks;
    if (!mwt_sink_equal(&a, &b, &diff)) {
      ++mwt_failures;
      printf("  FAIL iter %d seed 0x%llX: block %d vs %d diverged at %ld\n", it,
             s, bs_a, bs_b, diff);
      if (mwt_failures > 5) {
        printf("  (stopping after 6 failures)\n");
        break;
      }
    }

    ++mwt_checks;
    if (a.rejected_drains != 0 || b.rejected_drains != 0) {
      ++mwt_failures;
      printf("  FAIL iter %d seed 0x%llX: %ld/%ld drains fell outside the "
             "buffer\n",
             it, s, a.rejected_drains, b.rejected_drains);
    }

    /* The pipelined path must also agree, and must restore its buffer. */
    ++mwt_checks;
    render(&b, bs_a, 1, s, 0u, 1);
    diff = -1;
    if (!mwt_sink_equal(&a, &b, &diff)) {
      ++mwt_failures;
      printf("  FAIL iter %d seed 0x%llX: pipelined diverged at %ld\n", it, s,
             diff);
    }

    /* Stereo is a separate set of loops and gets its own pass. */
    {
      mwt_sink_t sa, sb;
      mwt_sink_init(&sa, RENDER_FRAMES, 2);
      mwt_sink_init(&sb, RENDER_FRAMES, 2);
      render(&sa, bs_a, 2, s, 0u, 0);
      render(&sb, bs_b, 2, s, flags, 0);
      ++mwt_checks;
      diff = -1;
      if (!mwt_sink_equal(&sa, &sb, &diff)) {
        ++mwt_failures;
        printf("  FAIL iter %d seed 0x%llX: stereo %d vs %d diverged at %ld\n",
               it, s, bs_a, bs_b, diff);
      }
      mwt_sink_free(&sa);
      mwt_sink_free(&sb);
    }
  }

  mwt_sink_free(&a);
  mwt_sink_free(&b);

  printf("%d checks, %d failures\n", mwt_checks, mwt_failures);
  return mwt_failures ? 1 : 0;
}
