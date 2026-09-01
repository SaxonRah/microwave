/* MicroWave host test support.
 *
 * A buffer-backed drain target plus a tiny assertion and RNG layer, shared by
 * the unit, fuzz and benchmark binaries. Host-only: this file is never
 * compiled into a DOS or Pico build.
 */
#ifndef MW_TEST_SUPPORT_H
#define MW_TEST_SUPPORT_H

#include "snd.h"
#include "snd_synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define MWT_UNUSED __attribute__((unused))
#else
#define MWT_UNUSED
#endif

/* A PCM16 clip holds int16 data whatever snd_sample_t is, so a fixture that
 * builds one must scale against this and not against SND_FULL_SCALE. */
#define MWT_S16_FULL 32767

/* ------------------------------------------------------------------ */
/* assertions                                                          */
/* ------------------------------------------------------------------ */

extern int mwt_checks;
extern int mwt_failures;

#define MWT_CHECK(cond, ...)                                                   \
  do {                                                                         \
    ++mwt_checks;                                                              \
    if (!(cond)) {                                                             \
      ++mwt_failures;                                                          \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);                            \
      printf(__VA_ARGS__);                                                     \
      printf("\n");                                                            \
    }                                                                          \
  } while (0)

#define MWT_CHECK_EQ_INT(got, want, what)                                      \
  MWT_CHECK((long)(got) == (long)(want), "%s: got %ld, want %ld", (what),       \
            (long)(got), (long)(want))

/* ------------------------------------------------------------------ */
/* deterministic RNG (never rand(); results must reproduce in CI)      */
/* ------------------------------------------------------------------ */

typedef struct {
  unsigned long long s;
} mwt_rng_t;

static MWT_UNUSED void mwt_rng_seed(mwt_rng_t *g, unsigned long long seed) {
  g->s = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

static MWT_UNUSED int mwt_rand(mwt_rng_t *g, int lo, int hi) {
  g->s = g->s * 6364136223846793005ULL + 1442695040888963407ULL;
  if (hi <= lo)
    return lo;
  return lo + (int)((g->s >> 33) % (unsigned long long)(hi - lo + 1));
}

/* ------------------------------------------------------------------ */
/* capture sink                                                        */
/* ------------------------------------------------------------------ */

/* The capture buffer is heap-allocated on purpose. Under AddressSanitizer the
 * redzones around it turn any stray write from a drain into a hard failure
 * instead of silent corruption of an adjacent global. */
typedef struct {
  snd_sample_t *samples;
  long frames;
  int channels;
  long drain_calls;
  long silent_drains;    /* drains that arrived with a NULL sample pointer */
  long rejected_drains;  /* drains whose window fell outside the buffer */
  long frames_written;
} mwt_sink_t;

static MWT_UNUSED void mwt_sink_init(mwt_sink_t *s, long frames, int channels) {
  s->frames = frames;
  s->channels = channels;
  s->samples =
      (snd_sample_t *)calloc((size_t)(frames * channels), sizeof(snd_sample_t));
  s->drain_calls = 0;
  s->silent_drains = 0;
  s->rejected_drains = 0;
  s->frames_written = 0;
}

static MWT_UNUSED void mwt_sink_free(mwt_sink_t *s) {
  free(s->samples);
  s->samples = NULL;
}

static MWT_UNUSED void mwt_sink_reset(mwt_sink_t *s) {
  long i, n = s->frames * (long)s->channels;
  for (i = 0; i < n; ++i)
    s->samples[i] = SND_SAMPLE_SILENCE;
  s->drain_calls = 0;
  s->silent_drains = 0;
  s->rejected_drains = 0;
  s->frames_written = 0;
}

/* A NULL sample pointer means "this many frames of silence" and must be
 * honoured, otherwise SND_RENDER_SKIP_SILENT would silently change output. */
static MWT_UNUSED void mwt_sink_drain(snd_mixer_t *m, long frame, int frames,
                                      const snd_sample_t *samples, void *user) {
  mwt_sink_t *s = (mwt_sink_t *)user;
  long i, n;

  (void)m;
  ++s->drain_calls;

  if (frames <= 0 || frame < 0 || frame + (long)frames > s->frames) {
    ++s->rejected_drains;
    return;
  }

  n = (long)frames * (long)s->channels;

  if (!samples) {
    ++s->silent_drains;
    for (i = 0; i < n; ++i)
      s->samples[frame * (long)s->channels + i] = SND_SAMPLE_SILENCE;
    s->frames_written += frames;
    return;
  }

  for (i = 0; i < n; ++i)
    s->samples[frame * (long)s->channels + i] = samples[i];
  s->frames_written += frames;
}

static MWT_UNUSED snd_sample_t mwt_sink_get(const mwt_sink_t *s, long frame,
                                            int channel) {
  if (frame < 0 || frame >= s->frames)
    return SND_SAMPLE_SILENCE;
  return s->samples[frame * (long)s->channels + channel];
}

static MWT_UNUSED long mwt_sink_count_not(const mwt_sink_t *s,
                                          snd_sample_t quiet) {
  long i, n = s->frames * (long)s->channels, c = 0;
  for (i = 0; i < n; ++i)
    if (s->samples[i] != quiet)
      ++c;
  return c;
}

/* Peak absolute deviation from silence, in mix units. */
static MWT_UNUSED long mwt_sink_peak(const mwt_sink_t *s) {
  long i, n = s->frames * (long)s->channels, peak = 0;
  for (i = 0; i < n; ++i) {
    long v = (long)SND_SAMPLE_TO_MIX(s->samples[i]);
    if (v < 0)
      v = -v;
    if (v > peak)
      peak = v;
  }
  return peak;
}

static MWT_UNUSED int mwt_sink_equal(const mwt_sink_t *a, const mwt_sink_t *b,
                                     long *first_diff) {
  long i, n;
  if (a->frames != b->frames || a->channels != b->channels)
    return 0;
  n = a->frames * (long)a->channels;
  for (i = 0; i < n; ++i) {
    if (a->samples[i] != b->samples[i]) {
      if (first_diff)
        *first_diff = i;
      return 0;
    }
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* clip construction                                                   */
/* ------------------------------------------------------------------ */

/* A ramp whose value encodes its own frame index, so a mix can be checked for
 * correct source alignment rather than just "wrote something". */
static MWT_UNUSED void mwt_make_ramp16(int16_t *dst, uint32_t frames) {
  uint32_t i;
  for (i = 0u; i < frames; ++i)
    dst[i] = (int16_t)((i % 512u) * 32u);
}

static MWT_UNUSED void mwt_make_ramp8(uint8_t *dst, uint32_t frames) {
  uint32_t i;
  for (i = 0u; i < frames; ++i)
    dst[i] = (uint8_t)(i & 0xFFu);
}

static MWT_UNUSED void mwt_clip_pcm16(snd_clip_t *c, const int16_t *data,
                                      uint32_t frames, int rate,
                                      uint8_t extra_flags) {
  memset(c, 0, sizeof(*c));
  c->data = data;
  c->frames = frames;
  c->bytes = frames * 2u;
  c->rate = rate;
  c->channels = 1u;
  c->flags = (uint8_t)(SND_CLIP_PCM16 | extra_flags);
  c->loop_start = 0u;
  c->loop_end = frames;
}

static MWT_UNUSED void mwt_clip_pcm8(snd_clip_t *c, const uint8_t *data,
                                     uint32_t frames, int rate,
                                     uint8_t extra_flags) {
  memset(c, 0, sizeof(*c));
  c->data = data;
  c->frames = frames;
  c->bytes = frames;
  c->rate = rate;
  c->channels = 1u;
  c->flags = (uint8_t)(SND_CLIP_PCM8 | extra_flags);
  c->loop_start = 0u;
  c->loop_end = frames;
}

/* ------------------------------------------------------------------ */
/* IMA ADPCM encoder, test-side only                                   */
/* ------------------------------------------------------------------ */

/* The library only decodes. Encoding lives here and in the Python packer, so
 * a decoder bug cannot be masked by a matching encoder bug in the same file.
 * Block layout: 4-byte restart header then one nibble per subsequent frame. */

static const int16_t mwt_ima_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int mwt_ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                      -1, -1, -1, -1, 2, 4, 6, 8};

#define MWT_ADPCM_BLOCK_FRAMES SND_ADPCM_BLOCK_FRAMES
#define MWT_ADPCM_BLOCK_BYTES (4u + ((MWT_ADPCM_BLOCK_FRAMES - 1u + 1u) / 2u))

/* Returns bytes written, or 0 if the destination is too small. */
static MWT_UNUSED uint32_t mwt_adpcm_encode(const int16_t *pcm, uint32_t frames,
                                            uint8_t *dst, uint32_t max_bytes) {
  uint32_t block, blocks, out = 0;

  blocks = (frames + MWT_ADPCM_BLOCK_FRAMES - 1u) / MWT_ADPCM_BLOCK_FRAMES;
  if (blocks * MWT_ADPCM_BLOCK_BYTES > max_bytes)
    return 0u;

  for (block = 0u; block < blocks; ++block) {
    uint32_t first = block * MWT_ADPCM_BLOCK_FRAMES;
    uint32_t n = frames - first;
    int32_t pred;
    int index = 0;
    uint32_t i;
    uint8_t *hdr = dst + out;

    if (n > MWT_ADPCM_BLOCK_FRAMES)
      n = MWT_ADPCM_BLOCK_FRAMES;

    pred = pcm[first];
    hdr[0] = (uint8_t)((uint16_t)pred & 0xFFu);
    hdr[1] = (uint8_t)(((uint16_t)pred >> 8) & 0xFFu);
    hdr[2] = (uint8_t)index;
    hdr[3] = 0u;
    /* Zero the nibble area so a partial final block is deterministic. */
    memset(hdr + 4, 0, MWT_ADPCM_BLOCK_BYTES - 4u);

    for (i = 1u; i < n; ++i) {
      long step = (long)mwt_ima_step[index];
      long diff = (long)pcm[first + i] - (long)pred;
      long vpdiff;
      uint8_t nib = 0u;

      if (diff < 0) {
        nib = 8u;
        diff = -diff;
      }
      vpdiff = step >> 3;
      if (diff >= step) {
        nib = (uint8_t)(nib | 4u);
        diff -= step;
        vpdiff += step;
      }
      step >>= 1;
      if (diff >= step) {
        nib = (uint8_t)(nib | 2u);
        diff -= step;
        vpdiff += step;
      }
      step >>= 1;
      if (diff >= step) {
        nib = (uint8_t)(nib | 1u);
        vpdiff += step;
      }

      if (nib & 8u)
        pred -= (int32_t)vpdiff;
      else
        pred += (int32_t)vpdiff;
      if (pred > 32767)
        pred = 32767;
      if (pred < -32768)
        pred = -32768;

      index += mwt_ima_index[nib];
      if (index < 0)
        index = 0;
      if (index > 88)
        index = 88;

      {
        uint32_t nib_index = i - 1u;
        uint8_t *slot = hdr + 4u + (nib_index >> 1);
        if (nib_index & 1u)
          *slot = (uint8_t)(*slot | (uint8_t)(nib << 4));
        else
          *slot = (uint8_t)(*slot | nib);
      }
    }
    out += MWT_ADPCM_BLOCK_BYTES;
  }
  return out;
}

#endif /* MW_TEST_SUPPORT_H */
