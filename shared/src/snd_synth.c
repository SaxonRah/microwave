/* MicroWave procedural oscillators.
 *
 * The audio counterpart of gfx_font5x7.c: a small ROM table and a couple of
 * loops that let any frontend produce output before any asset exists.
 */

#include "snd_synth.h"

const int16_t snd_sine_table[256] = {
         0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
      6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
     12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
     18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
     23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
     27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
     30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
     32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
     32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
     32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
     30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
     27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
     23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
     18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
     12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
      6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
         0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
     -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
    -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
    -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
    -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
    -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
    -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
    -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
     -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804
};

/* Twelve equal-tempered semitones of the octave starting at C0, in
   millihertz, so the octave shift below stays exact integer math. */
static const long snd_note_mhz[12] = {16352L, 17324L, 18354L, 19445L,
                                      20602L, 21827L, 23125L, 24500L,
                                      25957L, 27500L, 29135L, 30868L};

int snd_note_hz(int midi_note) {
  int octave, semi;
  long mhz;

  if (midi_note < 0)
    midi_note = 0;
  if (midi_note > 127)
    midi_note = 127;

  /* MIDI note 12 is C0 in this table's numbering. */
  octave = (midi_note / 12) - 1;
  semi = midi_note % 12;
  mhz = snd_note_mhz[semi];

  if (octave > 0)
    mhz <<= octave;
  else if (octave < 0)
    mhz >>= (-octave);

  return (int)((mhz + 500L) / 1000L);
}

int snd_wave_sample(snd_wave_t wave, snd_fixed_t phase, uint32_t SND_PTR *lfsr) {
  uint32_t frac = SND_FIXED_FRAC(phase);

  switch (wave) {
  case SND_WAVE_SQUARE:
    return (frac < 0x8000u) ? SND_FULL_SCALE : -SND_FULL_SCALE;

  case SND_WAVE_SAW:
    /* -1 .. +1 across the cycle. */
    return (int)(((long)frac * (2L * SND_FULL_SCALE) >> 16) - SND_FULL_SCALE);

  case SND_WAVE_TRIANGLE: {
    long v;
    if (frac < 0x8000u)
      v = ((long)frac * (2L * SND_FULL_SCALE) >> 15) - SND_FULL_SCALE;
    else
      v = SND_FULL_SCALE -
          (((long)(frac - 0x8000u) * (2L * SND_FULL_SCALE)) >> 15);
    return (int)v;
  }

  case SND_WAVE_SINE: {
    int idx = (int)(frac >> 8);
    long v = (long)snd_sine_table[idx & 0xFF];
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
    v >>= 8;
#endif
    return (int)v;
  }

  case SND_WAVE_NOISE: {
    /* 32-bit xorshift. Deterministic and seeded by the caller, because the
       stress test and the fuzz seeds must reproduce byte for byte. */
    uint32_t x;
    if (!lfsr)
      return 0;
    x = *lfsr ? *lfsr : 0x1234567u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *lfsr = x;
    return (int)((long)(x >> 16) % (2L * SND_FULL_SCALE + 1L)) - SND_FULL_SCALE;
  }

  case SND_WAVE_COUNT:
  default:
    return 0;
  }
}

/* ------------------------------------------------------------------ */
/* envelope                                                            */
/* ------------------------------------------------------------------ */

void snd_env_init(snd_env_t SND_PTR *e, long attack, long decay,
                  int16_t sustain, long release) {
  if (!e)
    return;
  e->attack = (attack < 0) ? 0 : attack;
  e->decay = (decay < 0) ? 0 : decay;
  e->sustain = (sustain < 0) ? 0 : sustain;
  e->release = (release < 0) ? 0 : release;
}

int16_t snd_env_level(const snd_env_t SND_PTR *e, long since_start,
                      long since_release) {
  long level;

  if (!e)
    return SND_GAIN_UNITY;
  if (since_start < 0)
    return 0;

  if (since_release >= 0) {
    /* Release ramps from wherever sustain left it down to zero. */
    if (e->release <= 0)
      return 0;
    if (since_release >= e->release)
      return 0;
    level = ((long)e->sustain * (e->release - since_release)) / e->release;
    return (int16_t)level;
  }

  if (e->attack > 0 && since_start < e->attack) {
    level = ((long)SND_GAIN_UNITY * since_start) / e->attack;
    return (int16_t)level;
  }

  since_start -= e->attack;
  if (e->decay > 0 && since_start < e->decay) {
    long span = (long)SND_GAIN_UNITY - (long)e->sustain;
    level = (long)SND_GAIN_UNITY - (span * since_start) / e->decay;
    return (int16_t)level;
  }

  return e->sustain;
}

/* ------------------------------------------------------------------ */
/* tones                                                               */
/* ------------------------------------------------------------------ */

void snd_tone_reset(snd_tone_t SND_PTR *t) {
  if (!t)
    return;
  t->phase = 0;
  t->phase_step = 0;
  t->gain = SND_GAIN_UNITY;
  t->start_frame = 0;
  t->end_frame = -1;
  t->release_from = -1;
  t->noise = 0x1234567u;
  t->wave = (uint8_t)SND_WAVE_SQUARE;
  t->active = 0;
  t->id = 0;
  snd_env_init(&t->env, 0, 0, SND_GAIN_UNITY, 0);
}

static snd_fixed_t snd_tone_step_for(const snd_mixer_t SND_PTR *m, int hz) {
  long step;
  if (!m || m->rate <= 0)
    return 0;
  if (hz < 0)
    hz = 0;
  step = ((long)hz << SND_FIXED_SHIFT) / (long)m->rate;
  return (snd_fixed_t)step;
}

void snd_tone_start(snd_tone_t SND_PTR *t, const snd_mixer_t SND_PTR *m,
                    snd_wave_t wave, int hz, int16_t gain_8_8,
                    long start_frame, long frames) {
  if (!t)
    return;
  snd_tone_reset(t);
  t->wave = (uint8_t)wave;
  t->phase_step = snd_tone_step_for(m, hz);
  t->gain = (gain_8_8 < 0) ? 0 : gain_8_8;
  t->start_frame = start_frame;
  t->end_frame = (frames > 0) ? (start_frame + frames) : -1;
  t->active = 1;
  /* Seed per note so two tones started on the same frame do not produce
     identical noise. */
  t->noise = (uint32_t)(0x9E3779B9u + (uint32_t)start_frame * 2654435761u +
                        (uint32_t)hz);
  if (!t->noise)
    t->noise = 1u;
}

void snd_tone_set_hz(snd_tone_t SND_PTR *t, const snd_mixer_t SND_PTR *m,
                     int hz) {
  if (!t)
    return;
  t->phase_step = snd_tone_step_for(m, hz);
}

void snd_tone_set_env(snd_tone_t SND_PTR *t, const snd_env_t SND_PTR *e) {
  if (!t || !e)
    return;
  t->env = *e;
}

void snd_tone_release(snd_tone_t SND_PTR *t, long frame) {
  if (!t || !t->active)
    return;
  if (t->release_from < 0)
    t->release_from = frame;
}

void snd_tone_stop(snd_tone_t SND_PTR *t) {
  if (!t)
    return;
  t->active = 0;
}

snd_span_t snd_tone_span(const snd_tone_t SND_PTR *t) {
  snd_span_t s;
  s.start = 0;
  s.end = 0;
  if (!t || !t->active)
    return s;
  s.start = t->start_frame;
  if (t->release_from >= 0)
    s.end = t->release_from + t->env.release;
  else if (t->end_frame >= 0)
    s.end = t->end_frame + t->env.release;
  else
    s.end = 0x7FFFFFFFL;
  return s;
}

void snd_mix_tone(snd_mixer_t SND_PTR *m, snd_tone_t SND_PTR *t) {
  long block_start, lo, hi;
  snd_span_t ts;
  int f, f0, f1;
  long g;

  if (!m || !m->block || !t || !t->active)
    return;

  block_start = m->block_frame;
  lo = block_start + (long)m->span_0;
  hi = block_start + (long)m->span_1;
  ts = snd_tone_span(t);
  if (ts.start > lo)
    lo = ts.start;
  if (ts.end < hi)
    hi = ts.end;
  if (hi <= lo)
    return;

  f0 = (int)(lo - block_start);
  f1 = (int)(hi - block_start);

  /* Only clear once we know we will write. */
  snd_touch_block(m);

  /* Tone gain only; the mixer scales the finished block by master volume. */
  g = (long)t->gain;

  for (f = f0; f < f1; ++f) {
    long abs_frame = block_start + (long)f;
    long since_start = abs_frame - t->start_frame;
    long since_release = -1;
    long lvl, s, out;

    if (t->release_from >= 0)
      since_release = abs_frame - t->release_from;
    else if (t->end_frame >= 0 && abs_frame >= t->end_frame)
      since_release = abs_frame - t->end_frame;

    lvl = (long)snd_env_level(&t->env, since_start, since_release);
    s = (long)snd_wave_sample((snd_wave_t)t->wave, t->phase, &t->noise);
    out = (((s * g) >> SND_GAIN_SHIFT) * lvl) >> SND_GAIN_SHIFT;

    /* Through snd_block_add, never straight into m->block: the mixer may be
       accumulating somewhere else entirely. */
    if (m->channels == 2) {
      long i = (long)f * 2L;
      snd_block_add(m, i, out);
      snd_block_add(m, i + 1, out);
    } else {
      snd_block_add(m, (long)f, out);
    }

    t->phase += t->phase_step;
    t->phase &= (SND_FIXED_ONE - 1); /* keep phase in [0,1) */
  }

  /* Retire once the release has finished. */
  {
    long done_at = ts.end;
    if (done_at < 0x7FFFFFFFL && (block_start + (long)f1) >= done_at)
      t->active = 0;
  }
}

/* ------------------------------------------------------------------ */
/* offline generation                                                  */
/* ------------------------------------------------------------------ */

int snd_synth_fill(snd_sample_t SND_PTR *buf, uint32_t frames, snd_wave_t wave,
                   int rate, int hz, int16_t gain_8_8) {
  uint32_t i;
  snd_fixed_t phase = 0;
  snd_fixed_t step;
  uint32_t lfsr = 0x1234567u;

  if (!buf || frames == 0u || rate <= 0)
    return 0;
  if (gain_8_8 < 0)
    gain_8_8 = 0;

  step = (snd_fixed_t)(((long)hz << SND_FIXED_SHIFT) / (long)rate);

  for (i = 0u; i < frames; ++i) {
    long s = (long)snd_wave_sample(wave, phase, &lfsr);
    s = (s * (long)gain_8_8) >> SND_GAIN_SHIFT;
    buf[i] = (snd_sample_t)SND_MIX_TO_SAMPLE(snd_clip_sample(s));
    phase += step;
    phase &= (SND_FIXED_ONE - 1);
  }
  return 1;
}

int snd_synth_clip(snd_clip_t SND_PTR *out, snd_sample_t SND_PTR *buf,
                   uint32_t frames, int rate, snd_wave_t wave, int hz,
                   int16_t gain_8_8) {
  if (!out)
    return 0;
  if (!snd_synth_fill(buf, frames, wave, rate, hz, gain_8_8))
    return 0;

  out->data = buf;
  out->frames = frames;
  out->bytes = frames * (uint32_t)sizeof(snd_sample_t);
  out->loop_start = 0u;
  out->loop_end = frames;
  out->rate = rate;
  out->channels = 1u;
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
  out->flags = (uint8_t)SND_CLIP_PCM8;
#else
  out->flags = (uint8_t)SND_CLIP_PCM16;
#endif
  return snd_clip_validate(out);
}
