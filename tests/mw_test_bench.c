/* MicroWave host benchmark.
 *
 * Informational, with no pass/fail threshold, for the same reason
 * MicroRender's benchmark has none: a number from a CI runner is not a
 * hardware promise. Run it on the target you care about.
 *
 * The figure that matters for audio is not frames per second, it is the
 * realtime ratio: how many seconds of audio the mixer produces per second of
 * CPU. A 386 needs that number to stay above 1.0 with the voice count the game
 * actually uses, and everything else is detail.
 */

#include "mw_test_support.h"
#include <time.h>

int mwt_checks = 0;
int mwt_failures = 0;

#define RATE 22050
#define BLOCK 256
#define CLIP_FRAMES 4096

static snd_sample_t g_block[BLOCK * 2];
static int16_t g_pcm16[CLIP_FRAMES];
static uint8_t g_pcm8[CLIP_FRAMES];
static snd_sample_t g_junk[BLOCK * 2];

/* A drain that touches the data, so the optimizer cannot delete the mix. */
static void bench_drain(snd_mixer_t *m, long frame, int frames,
                        const snd_sample_t *samples, void *user) {
  long *acc = (long *)user;
  int i;
  (void)m;
  (void)frame;
  if (!samples)
    return;
  for (i = 0; i < frames; ++i)
    *acc += (long)SND_SAMPLE_TO_MIX(samples[i]);
}

typedef struct {
  snd_voice_t *voices;
  int count;
} bscene_t;

static void bench_mix(snd_mixer_t *m, void *user) {
  bscene_t *s = (bscene_t *)user;
  int i;
  for (i = 0; i < s->count; ++i) {
    if (!s->voices[i].active) {
      /* Restart rather than fall silent, so voice count stays honest. */
      s->voices[i].active = 1;
      s->voices[i].pos = 0;
      s->voices[i].start_frame = m->block_frame;
      s->voices[i].end_frame = -1;
    }
    snd_mix_voice(m, &s->voices[i]);
  }
}

static double run_case(const char *label, const snd_clip_t *clip, int voices,
                       int channels, long seconds) {
  snd_mixer_t m;
  snd_voice_t pool[64];
  bscene_t sc;
  long acc = 0;
  long frames = (long)RATE * seconds;
  clock_t t0, t1;
  double cpu, ratio;
  int i;

  if (voices > 64)
    voices = 64;

  snd_init(&m, RATE, channels, g_block, BLOCK, bench_drain, &acc);
  for (i = 0; i < voices; ++i) {
    snd_voice_reset(&pool[i]);
    snd_voice_start(&pool[i], clip, &m, 0, (int16_t)(0x0100 + i * 3),
                    (int16_t)(SND_GAIN_UNITY / 4));
  }
  sc.voices = pool;
  sc.count = voices;

  t0 = clock();
  snd_render_blocked(&m, frames, bench_mix, &sc);
  t1 = clock();

  cpu = (double)(t1 - t0) / (double)CLOCKS_PER_SEC;
  ratio = (cpu > 0.0) ? ((double)seconds / cpu) : 0.0;

  printf("  %-28s %3d voices  %2dch  %6.1f s audio in %6.3f s cpu  "
         "%9.1fx realtime\n",
         label, voices, channels, (double)seconds, cpu, ratio);

  /* Keep the accumulator observable so the mix cannot be optimized away. */
  if (acc == 0x7FFFFFFFL)
    printf("  (accumulator sentinel)\n");
  return ratio;
}

int main(int argc, char **argv) {
  long seconds = (argc > 1) ? atol(argv[1]) : 10;
  snd_clip_t c16, c8;
  int i;

  if (seconds < 1)
    seconds = 1;

  for (i = 0; i < CLIP_FRAMES; ++i) {
    g_pcm16[i] = (int16_t)(((i * 37) % 40001) - 20000);
    g_pcm8[i] = (uint8_t)(i & 0xFFu);
  }
  mwt_clip_pcm16(&c16, g_pcm16, CLIP_FRAMES, RATE, SND_CLIP_LOOP);
  mwt_clip_pcm8(&c8, g_pcm8, CLIP_FRAMES, RATE, SND_CLIP_LOOP);
  (void)g_junk;

  printf("MicroWave benchmark: %ld s of audio per case, %d Hz, %d-frame "
         "blocks\n",
         seconds, RATE, BLOCK);
  printf("  (informational; no threshold. Realtime ratio is the number that "
         "matters.)\n\n");

  run_case("PCM16 mono", &c16, 1, 1, seconds);
  run_case("PCM16 mono", &c16, 8, 1, seconds);
  run_case("PCM16 mono", &c16, 32, 1, seconds);
  run_case("PCM8 mono", &c8, 8, 1, seconds);
  run_case("PCM8 mono", &c8, 32, 1, seconds);
  run_case("PCM16 stereo out", &c16, 8, 2, seconds);
  run_case("PCM16 stereo out", &c16, 32, 2, seconds);

  printf("\nA target must stay above 1.0x with the voice count it actually "
         "uses.\n");
  return 0;
}
