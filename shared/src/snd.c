/* MicroWave software mixer.
 *
 * Structured to match shared/src/gfx.c in MicroRender function for function.
 * If you are looking for the audio equivalent of a clipped colorkey blit, it is
 * snd_mix_voice_pcm8() and it is clipped the same way: by intersecting spans
 * before the inner loop, never by testing inside it.
 */

#include "snd.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

int snd_clip_sample(long v) {
  if (v > (long)SND_MIX_MAX)
    return SND_MIX_MAX;
  if (v < (long)SND_MIX_MIN)
    return SND_MIX_MIN;
  return (int)v;
}

static long snd_clip_end_frame(const snd_clip_t SND_PTR *c) {
  if ((c->flags & SND_CLIP_LOOP) && c->loop_end > 0u)
    return (long)c->loop_end;
  return (long)c->frames;
}

static long snd_clip_loop_start(const snd_clip_t SND_PTR *c) {
  return (long)c->loop_start;
}

/* Source data is stored at its own natural width, which is not necessarily the
   mixer's. A PCM16 clip is int16 whatever snd_sample_t happens to be, so the
   legacy U8 build has to bring 16-bit sources down into its own working range
   rather than clamp them flat. These are the only two places a source width is
   converted; everything downstream is already in mix units. */
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
#define SND_SRC_FROM_S16(v) ((long)(v) / 256L)
#define SND_SRC_FROM_U8(b) ((long)(b) - 128L)
#else
#define SND_SRC_FROM_S16(v) ((long)(v))
#define SND_SRC_FROM_U8(b) (((long)(b) - 128L) * 256L)
#endif

/* ------------------------------------------------------------------ */
/* IMA ADPCM                                                           */
/* ------------------------------------------------------------------ */

#if SND_ENABLE_ADPCM

static const int16_t snd_adpcm_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t snd_adpcm_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                           -1, -1, -1, -1, 2, 4, 6, 8};

/* Bytes in one self-contained mono ADPCM block: a four-byte restart header
   followed by one nibble for every frame after the first. With the default
   505 frames that is exactly 256 bytes, which is a convenient DMA quantum and
   not a coincidence. */
#define SND_ADPCM_BLOCK_BYTES (4u + ((SND_ADPCM_BLOCK_FRAMES - 1u + 1u) / 2u))

static int snd_adpcm_step_once(snd_adpcm_state_t SND_PTR *st, uint8_t nib) {
  long step = (long)snd_adpcm_step[st->step_index];
  long diff = step >> 3;
  long pred;

  if (nib & 1u)
    diff += step >> 2;
  if (nib & 2u)
    diff += step >> 1;
  if (nib & 4u)
    diff += step;
  if (nib & 8u)
    diff = -diff;

  pred = (long)st->predictor + diff;
  if (pred > 32767L)
    pred = 32767L;
  if (pred < -32768L)
    pred = -32768L;
  st->predictor = (int32_t)pred;

  st->step_index = (int16_t)(st->step_index + snd_adpcm_index[nib & 0x0Fu]);
  if (st->step_index < 0)
    st->step_index = 0;
  if (st->step_index > 88)
    st->step_index = 88;

  return (int)pred;
}

/* Restart the decoder at the head of an ADPCM block. Every block carries its
   own predictor, so this is O(1) no matter how far into the clip it is; the
   walk from a block head to an arbitrary frame is the only linear part. */
static int snd_adpcm_open_block(const snd_clip_t SND_PTR *c,
                                snd_adpcm_state_t SND_PTR *st, uint32_t block) {
  const uint8_t SND_PTR *p = (const uint8_t SND_PTR *)c->data;
  uint32_t off = block * SND_ADPCM_BLOCK_BYTES;

  if (off + 4u > c->bytes)
    return 0;

  p += off;
  st->predictor = (int32_t)(int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
  st->step_index = (int16_t)p[2];
  if (st->step_index < 0)
    st->step_index = 0;
  if (st->step_index > 88)
    st->step_index = 88;
  st->block = block;
  st->cursor = 0u;
  st->cache[0] = (int16_t)st->predictor;
  st->cache[1] = (int16_t)st->predictor;
  return 1;
}

/* Decoded sample at absolute frame `frame`. Walks forward when it can and
   restarts from the containing block head when it cannot, which is exactly how
   the row-start RLE path decides between continuing a row and reseeking. */
static int snd_adpcm_sample_at(const snd_clip_t SND_PTR *c,
                               snd_adpcm_state_t SND_PTR *st, long frame,
                               unsigned long SND_PTR *blocks_decoded) {
  uint32_t want_block;
  uint32_t want_cursor;

  if (frame < 0)
    frame = 0;
  want_block = (uint32_t)frame / (uint32_t)SND_ADPCM_BLOCK_FRAMES;
  want_cursor = (uint32_t)frame % (uint32_t)SND_ADPCM_BLOCK_FRAMES;

  if (st->block != want_block || st->cursor > want_cursor) {
    if (!snd_adpcm_open_block(c, st, want_block))
      return 0;
    if (blocks_decoded)
      ++*blocks_decoded;
  }

  while (st->cursor < want_cursor) {
    const uint8_t SND_PTR *p = (const uint8_t SND_PTR *)c->data;
    uint32_t base = st->block * SND_ADPCM_BLOCK_BYTES + 4u;
    uint32_t nib_index = st->cursor; /* frame 0 is the header predictor */
    uint32_t byte_index = base + (nib_index >> 1);
    uint8_t nib;

    if (byte_index >= c->bytes)
      break;

    nib = p[byte_index];
    nib = (uint8_t)((nib_index & 1u) ? (nib >> 4) : (nib & 0x0Fu));
    st->cache[0] = (int16_t)snd_adpcm_step_once(st, nib);
    ++st->cursor;
  }

  return (int)st->cache[0];
}

#endif /* SND_ENABLE_ADPCM */

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void snd_init(snd_mixer_t SND_PTR *m, int rate, int channels,
              snd_sample_t SND_PTR *block_buffer, int block_frames,
              snd_drain_fn drain, void SND_PTR *user) {
  if (!m)
    return;
  if (rate <= 0)
    rate = SND_DEFAULT_RATE;
  if (channels != 2)
    channels = 1;
  if (block_frames <= 0)
    block_frames = SND_DEFAULT_BLOCK_FRAMES;

  m->rate = rate;
  m->channels = channels;
  m->block = block_buffer;
  m->block_max = block_frames;
  m->block_frames = 0;
  m->block_frame = 0;
  m->block_capacity = (long)block_frames * (long)channels;
  m->span_0 = 0;
  m->span_1 = 0;
  m->master_vol = SND_VOL_UNITY;
  m->master_vol_cur = SND_VOL_UNITY;
  m->master_vol_step = (SND_VOL_RAMP_FRAMES > 0)
                           ? (SND_VOL_UNITY / (int32_t)SND_VOL_RAMP_FRAMES)
                           : 0;
  m->block_touched = 0;
  m->drain = drain;
  m->drain_begin = 0;
  m->drain_wait = 0;
  m->user = user;
#if SND_WIDE_ACCUM
  m->accum = 0;
#endif
}

void snd_set_block_capacity(snd_mixer_t SND_PTR *m, long samples) {
  if (!m)
    return;
  if (samples < 0)
    samples = 0;
  m->block_capacity = samples;
}

/* ------------------------------------------------------------------ */
/* master volume                                                       */
/* ------------------------------------------------------------------ */

static int32_t snd_vol_clamp(int32_t v) {
  if (v < SND_VOL_SILENT)
    return SND_VOL_SILENT;
  if (v > SND_VOL_UNITY)
    return SND_VOL_UNITY;
  return v;
}

void snd_set_master_volume(snd_mixer_t SND_PTR *m, int32_t vol_16_16) {
  if (!m)
    return;
  m->master_vol = snd_vol_clamp(vol_16_16);
  if (m->master_vol_step <= 0)
    m->master_vol_cur = m->master_vol;
}

void snd_set_master_volume_now(snd_mixer_t SND_PTR *m, int32_t vol_16_16) {
  if (!m)
    return;
  m->master_vol = snd_vol_clamp(vol_16_16);
  m->master_vol_cur = m->master_vol;
}

void snd_set_volume_ramp(snd_mixer_t SND_PTR *m, int frames) {
  if (!m)
    return;
  if (frames <= 0) {
    m->master_vol_step = 0;
    m->master_vol_cur = m->master_vol; /* nothing left to ramp toward */
    return;
  }
  m->master_vol_step = SND_VOL_UNITY / (int32_t)frames;
  if (m->master_vol_step <= 0)
    m->master_vol_step = 1; /* a ramp longer than 65536 frames still moves */
}

int32_t snd_master_volume(const snd_mixer_t SND_PTR *m) {
  return m ? m->master_vol : SND_VOL_SILENT;
}

int32_t snd_master_volume_current(const snd_mixer_t SND_PTR *m) {
  return m ? m->master_vol_cur : SND_VOL_SILENT;
}

int snd_master_is_silent(const snd_mixer_t SND_PTR *m) {
  if (!m)
    return 1;
  return (m->master_vol == SND_VOL_SILENT &&
          m->master_vol_cur == SND_VOL_SILENT);
}

/* pct*pct/10000 in 16.16. The intermediate is at most 100*100*65536, which
   needs 30 bits, so it is computed in int32_t and not in int. */
int32_t snd_vol_from_percent(int pct) {
  int32_t p;

  if (pct <= 0)
    return SND_VOL_SILENT;
  if (pct >= 100)
    return SND_VOL_UNITY;
  p = (int32_t)pct;
  return (p * p * SND_VOL_UNITY) / 10000L;
}

/* The inverse, by integer square root, so a UI can round-trip its own slider
   position without keeping a shadow copy. */
int snd_vol_to_percent(int32_t vol_16_16) {
  int32_t target, lo, hi;

  if (vol_16_16 <= SND_VOL_SILENT)
    return 0;
  if (vol_16_16 >= SND_VOL_UNITY)
    return 100;
  target = vol_16_16;
  lo = 0;
  hi = 100;
  while (lo < hi) {
    int32_t mid = (lo + hi + 1) / 2;
    if ((mid * mid * SND_VOL_UNITY) / 10000L <= target)
      lo = mid;
    else
      hi = mid - 1;
  }
  return (int)lo;
}

void snd_set_master_gain(snd_mixer_t SND_PTR *m, int16_t gain_8_8) {
  int32_t v;

  if (!m)
    return;
  if (gain_8_8 < 0)
    gain_8_8 = 0;
  /* 8.8 -> 16.16 is an exact shift, and the old API had no ramp, so this
     jumps. Callers that want the ramp have snd_set_master_volume(). */
  v = (int32_t)gain_8_8 << (SND_VOL_SHIFT - SND_GAIN_SHIFT);
  snd_set_master_volume_now(m, v);
}

/* Move one output frame's worth toward the target. Deliberately a function of
   frames elapsed and nothing else: if this depended on how many frames
   happened to be in the current block, two block sizes would produce
   different ramps and test_volume_ramp_block_size_invariance() would catch
   it. Same discipline as carrying a resample phase across a block edge. */
static int32_t snd_vol_advance(int32_t cur, int32_t target, int32_t step) {
  if (step <= 0 || cur == target)
    return target;
  if (cur < target) {
    cur += step;
    if (cur > target)
      cur = target;
  } else {
    cur -= step;
    if (cur < target)
      cur = target;
  }
  return cur;
}

/* Scale one sample by a 16.16 volume.

   The multiply is bounded: this runs on the block after the clamp, so the
   magnitude is at most SND_MIX_MAX, and volume is at most SND_VOL_UNITY.
   32767 * 65536 is 2147418112, which fits in int32_t with 65535 to spare.
   That headroom is the reason master volume is capped at unity and applied
   post-clamp rather than into the wide accumulator -- doing it pre-clamp
   would need a 64-bit intermediate that DOS does not have. */
static snd_sample_t snd_vol_scale(snd_sample_t s, int32_t vol) {
  int32_t v = (int32_t)SND_SAMPLE_TO_MIX(s);
  v = (v * vol) >> SND_VOL_SHIFT;
  return (snd_sample_t)SND_MIX_TO_SAMPLE(snd_clip_sample((long)v));
}

/* Advance the ramp across frames whose samples are not being scaled, because
   they are already known to be silence. Silence times any volume is silence,
   so the output is identical either way -- but the ramp position must still
   move, or a muted stretch would leave the volume where it was and the next
   audible block would start from the wrong place. */
static void snd_master_skip(snd_mixer_t SND_PTR *m, int frames) {
  int f;

  if (!m || m->master_vol_cur == m->master_vol)
    return;
  for (f = 0; f < frames; ++f)
    m->master_vol_cur =
        snd_vol_advance(m->master_vol_cur, m->master_vol, m->master_vol_step);
}

/* The output stage. Everything that generates sound has already been summed
   and clamped into m->block by the time this runs, which is exactly why one
   pass here covers voices, synth tones and a caller's own generator without
   any of them knowing it exists. */
static void snd_master_apply(snd_mixer_t SND_PTR *m) {
  long i, n;
  int f, c;
  int32_t vol;

  if (!m || !m->block || m->block_frames <= 0)
    return;

  n = (long)m->block_frames * (long)m->channels;

  if (m->master_vol_cur == m->master_vol) {
    /* Settled. Two cases are free and one is a single multiply per sample. */
    if (m->master_vol_cur == SND_VOL_UNITY)
      return; /* the default costs nothing, which is the point */
    if (m->master_vol_cur == SND_VOL_SILENT) {
      for (i = 0; i < n; ++i)
        m->block[i] = SND_SAMPLE_SILENCE;
      return;
    }
    vol = m->master_vol_cur;
    for (i = 0; i < n; ++i)
      m->block[i] = snd_vol_scale(m->block[i], vol);
    return;
  }

  /* Ramping: the gain changes once per frame, and both samples of a stereo
     frame get the same one. Interpolating within a frame would put a
     different gain on left and right and shift the stereo image. */
  vol = m->master_vol_cur;
  for (f = 0; f < m->block_frames; ++f) {
    vol = snd_vol_advance(vol, m->master_vol, m->master_vol_step);
    for (c = 0; c < m->channels; ++c) {
      long idx = (long)f * (long)m->channels + (long)c;
      m->block[idx] = snd_vol_scale(m->block[idx], vol);
    }
  }
  m->master_vol_cur = vol;
}

#if SND_WIDE_ACCUM
void snd_set_accumulator(snd_mixer_t SND_PTR *m, int32_t SND_PTR *scratch) {
  if (!m)
    return;
  m->accum = scratch;
}
#endif

void snd_set_async_drain(snd_mixer_t SND_PTR *m, snd_drain_begin_fn begin_fn,
                         snd_drain_wait_fn wait_fn) {
  if (!m)
    return;
  m->drain_begin = begin_fn;
  m->drain_wait = wait_fn;
}

uint32_t snd_clip_frames_for_bytes(uint8_t flags, uint8_t channels,
                                   uint32_t bytes) {
  uint32_t ch = (channels == 2u) ? 2u : 1u;

  if (flags & SND_CLIP_PCM16)
    return bytes / (2u * ch);
  if (flags & SND_CLIP_PCM8)
    return bytes / ch;
#if SND_ENABLE_ADPCM
  if (flags & SND_CLIP_ADPCM4) {
    uint32_t whole = bytes / SND_ADPCM_BLOCK_BYTES;
    uint32_t rest = bytes % SND_ADPCM_BLOCK_BYTES;
    uint32_t frames = whole * (uint32_t)SND_ADPCM_BLOCK_FRAMES;
    if (rest > 4u)
      frames += 1u + ((rest - 4u) * 2u);
    else if (rest == 4u)
      frames += 1u;
    return frames;
  }
#endif
  return 0u;
}

int snd_clip_validate(const snd_clip_t SND_PTR *c) {
  uint8_t fmt;
  uint32_t need;
  long end;

  if (!c || !c->data)
    return 0;
  if (c->frames == 0u || c->bytes == 0u)
    return 0;
  if (c->rate <= 0)
    return 0;
  if (c->channels != 1u && c->channels != 2u)
    return 0;

  /* Exactly one format bit. */
  fmt = (uint8_t)(c->flags & SND_CLIP_FORMAT_MASK);
  if (fmt != SND_CLIP_PCM16 && fmt != SND_CLIP_PCM8 && fmt != SND_CLIP_ADPCM4)
    return 0;

#if !SND_ENABLE_ADPCM
  if (fmt == SND_CLIP_ADPCM4)
    return 0;
#else
  /* The ADPCM decoder is forward-only by construction: a nibble cannot be
     decoded without the one before it. Reverse playback would have to reseek
     to a block head on every single output frame, so it is refused here rather
     than silently sounding wrong. */
  if (fmt == SND_CLIP_ADPCM4 && (c->flags & SND_CLIP_PINGPONG))
    return 0;
  /* Interleaved stereo ADPCM would need a second decoder state per voice and
     a second header per block. Not shipped; convert to two mono clips. */
  if (fmt == SND_CLIP_ADPCM4 && c->channels != 1u)
    return 0;
#endif

  if ((long)c->frames > SND_MAX_CLIP_FRAMES)
    return 0;

  need = snd_clip_frames_for_bytes(fmt, c->channels, c->bytes);
  if (need < c->frames)
    return 0;

  end = snd_clip_end_frame(c);
  if (end <= 0 || end > (long)c->frames)
    return 0;
  if (c->flags & SND_CLIP_LOOP) {
    if ((long)c->loop_start >= end)
      return 0;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* block lifecycle                                                     */
/* ------------------------------------------------------------------ */

void snd_begin_block(snd_mixer_t SND_PTR *m, long frame, int frames) {
  long want;

  if (!m)
    return;
  if (frames < 0)
    frames = 0;
  if (frames > m->block_max)
    frames = m->block_max;

  want = (long)frames * (long)m->channels;
  if (want > m->block_capacity)
    frames = (int)(m->block_capacity / (long)m->channels);

  m->block_frame = frame;
  m->block_frames = frames;
  m->span_0 = 0;
  m->span_1 = frames;
  m->block_touched = 0;
}

void snd_clear_block(snd_mixer_t SND_PTR *m) {
  long i, n;

  if (!m || !m->block)
    return;
  n = (long)m->block_frames * (long)m->channels;
  for (i = 0; i < n; ++i)
    m->block[i] = SND_SAMPLE_SILENCE;
#if SND_WIDE_ACCUM
  if (m->accum) {
    for (i = 0; i < n; ++i)
      m->accum[i] = 0;
  }
#endif
}

/* Called by every mix entry point before it writes a sample. A block that no
   voice ever touches is never zeroed, which is where the silent-block saving
   actually comes from. */
void snd_touch_block(snd_mixer_t SND_PTR *m) {
  if (!m || m->block_touched)
    return;
  snd_clear_block(m);
  m->block_touched = 1;
}

/* Bring the block into its final form: collapse the wide accumulator if there
   is one, then apply the output-stage volume. Both the synchronous and the
   pipelined loop go through here, so there is one definition of "finished
   block" and the two cannot drift apart. */
static void snd_resolve_block(snd_mixer_t SND_PTR *m) {
#if SND_WIDE_ACCUM
  if (m->accum && m->block) {
    long i, n = (long)m->block_frames * (long)m->channels;
    for (i = 0; i < n; ++i)
      m->block[i] =
          (snd_sample_t)SND_MIX_TO_SAMPLE(snd_clip_sample((long)m->accum[i]));
  }
#endif
  snd_master_apply(m);
}

void snd_flush_block(snd_mixer_t SND_PTR *m) {
  if (!m || m->block_frames <= 0)
    return;

  /* Guarantee the caller's drain always sees a defined block. */
  if (!m->block_touched)
    snd_clear_block(m);

  snd_resolve_block(m);

  if (m->drain)
    m->drain(m, m->block_frame, m->block_frames, m->block, m->user);
}

void snd_set_span(snd_mixer_t SND_PTR *m, int frame, int frames) {
  int lo, hi;

  if (!m)
    return;
  lo = frame;
  hi = frame + frames;
  if (lo < 0)
    lo = 0;
  if (hi > m->block_frames)
    hi = m->block_frames;
  if (hi < lo)
    hi = lo;
  m->span_0 = lo;
  m->span_1 = hi;
}

void snd_reset_span(snd_mixer_t SND_PTR *m) {
  if (!m)
    return;
  m->span_0 = 0;
  m->span_1 = m->block_frames;
}

/* ------------------------------------------------------------------ */
/* render loops                                                        */
/* ------------------------------------------------------------------ */

void snd_render_one_block(snd_mixer_t SND_PTR *m, long frame, int frames,
                          void (*mix_scene)(snd_mixer_t SND_PTR *m,
                                            void SND_PTR *scene_user),
                          void SND_PTR *scene_user, unsigned flags) {
  if (!m)
    return;

  snd_begin_block(m, frame, frames);
  if (flags & SND_RENDER_CLEAR)
    snd_touch_block(m);
  if (mix_scene)
    mix_scene(m, scene_user);

  /* The scene still runs when the mixer is muted. Skipping it would stop the
     sequencer and freeze every voice's position, so unmuting would resume
     where the music left off instead of where it should have got to -- mute
     is a volume setting, not a pause button. What muting does save is the
     resolve pass and the drain's format conversion, which on DOS is the
     packing loop and on the Pico is a DMA transfer. */
  if ((flags & SND_RENDER_SKIP_SILENT) &&
      (!m->block_touched || snd_master_is_silent(m))) {
    snd_master_skip(m, m->block_frames);
    if (m->drain)
      m->drain(m, m->block_frame, m->block_frames, 0, m->user);
    return;
  }
  snd_flush_block(m);
}

void snd_render_blocked_ex(snd_mixer_t SND_PTR *m, long total_frames,
                           void (*mix_scene)(snd_mixer_t SND_PTR *m,
                                             void SND_PTR *scene_user),
                           void SND_PTR *scene_user, unsigned flags) {
  long done = 0;

  if (!m || total_frames <= 0)
    return;

  while (done < total_frames) {
    long left = total_frames - done;
    int n = (left > (long)m->block_max) ? m->block_max : (int)left;
    snd_render_one_block(m, done, n, mix_scene, scene_user, flags);
    if (m->block_frames <= 0)
      break; /* zero-capacity block: refuse to spin */
    done += (long)m->block_frames;
  }
}

void snd_render_blocked(snd_mixer_t SND_PTR *m, long total_frames,
                        void (*mix_scene)(snd_mixer_t SND_PTR *m,
                                          void SND_PTR *scene_user),
                        void SND_PTR *scene_user) {
  snd_render_blocked_ex(m, total_frames, mix_scene, scene_user, 0u);
}

void snd_render_blocked_pipelined(
    snd_mixer_t SND_PTR *m, snd_sample_t SND_PTR *second_block,
    long total_frames,
    void (*mix_scene)(snd_mixer_t SND_PTR *m, void SND_PTR *scene_user),
    void SND_PTR *scene_user, unsigned flags) {
  snd_sample_t SND_PTR *buffers[2];
  int which = 0;
  int in_flight = 0;
  long done = 0;

  if (!m || total_frames <= 0)
    return;
  if (!second_block || !m->drain_begin || !m->drain_wait) {
    /* No asynchronous drain installed: degrade to the synchronous loop rather
       than pretend to overlap. Same contract as gfx_render_tiled_pipelined. */
    snd_render_blocked_ex(m, total_frames, mix_scene, scene_user, flags);
    return;
  }

  buffers[0] = m->block;
  buffers[1] = second_block;

  while (done < total_frames) {
    long left = total_frames - done;
    int n = (left > (long)m->block_max) ? m->block_max : (int)left;

    m->block = buffers[which];
    snd_begin_block(m, done, n);
    if (flags & SND_RENDER_CLEAR)
      snd_touch_block(m);
    if (mix_scene)
      mix_scene(m, scene_user);
    if (m->block_frames <= 0)
      break;

    if (!m->block_touched)
      snd_clear_block(m);
    snd_resolve_block(m);

    /* Wait for the previous transfer only now, so rasterizing this block
       overlapped it. */
    if (in_flight)
      m->drain_wait(m, m->user);
    m->drain_begin(m, m->block_frame, m->block_frames, m->block, m->user);
    in_flight = 1;

    done += (long)m->block_frames;
    which ^= 1;
  }

  if (in_flight)
    m->drain_wait(m, m->user);
  m->block = buffers[0];
}

/* ------------------------------------------------------------------ */
/* voices                                                              */
/* ------------------------------------------------------------------ */

void snd_voice_reset(snd_voice_t SND_PTR *v) {
  if (!v)
    return;
  v->clip = 0;
  v->pos = 0;
  v->step = SND_FIXED_ONE;
  v->gain_l = SND_GAIN_UNITY;
  v->gain_r = SND_GAIN_UNITY;
  v->start_frame = 0;
  v->end_frame = -1;
  v->active = 0;
  v->reverse = 0;
  v->id = 0;
  v->adpcm.predictor = 0;
  v->adpcm.step_index = 0;
  v->adpcm.block = 0xFFFFFFFFu;
  v->adpcm.cursor = 0u;
  v->adpcm.cache[0] = 0;
  v->adpcm.cache[1] = 0;
}

/* The clip's natural rate and the mixer's rate are reconciled here, once, so
   the inner loop never divides. */
static snd_fixed_t snd_voice_compute_step(const snd_clip_t SND_PTR *clip,
                                          const snd_mixer_t SND_PTR *m,
                                          int16_t pitch_8_8) {
  long ratio;

  if (!clip || !m || m->rate <= 0)
    return SND_FIXED_ONE;
  if (pitch_8_8 <= 0)
    pitch_8_8 = SND_GAIN_UNITY;

  /* step = (clip_rate / mix_rate) * pitch, in 16.16. Done in one long
     expression so a 16-bit int target never holds the intermediate. */
  ratio = ((long)clip->rate << SND_FIXED_SHIFT) / (long)m->rate;
  ratio = (ratio * (long)pitch_8_8) >> SND_GAIN_SHIFT;
  if (ratio <= 0)
    ratio = 1;
  return (snd_fixed_t)ratio;
}

void snd_voice_start(snd_voice_t SND_PTR *v, const snd_clip_t SND_PTR *clip,
                     const snd_mixer_t SND_PTR *m, long start_frame,
                     int16_t pitch_8_8, int16_t gain_8_8) {
  if (!v)
    return;
  snd_voice_reset(v);
  if (!clip)
    return;

  v->clip = clip;
  v->pos = 0;
  v->step = snd_voice_compute_step(clip, m, pitch_8_8);
  v->gain_l = gain_8_8;
  v->gain_r = gain_8_8;
  v->start_frame = start_frame;
  v->end_frame = -1;
  v->active = 1;

  if (!(clip->flags & SND_CLIP_LOOP) && v->step > 0) {
    /* A non-looping voice has a knowable last frame, so it can be rejected by
       span intersection long before its data is touched. */
    long frames = (long)clip->frames;
    long out = ((frames << SND_FIXED_SHIFT) + v->step - 1) / v->step;
    v->end_frame = start_frame + out;
  }
}

void snd_voice_stop(snd_voice_t SND_PTR *v) {
  if (!v)
    return;
  v->active = 0;
}

void snd_voice_set_pitch(snd_voice_t SND_PTR *v, const snd_mixer_t SND_PTR *m,
                         int16_t pitch_8_8) {
  if (!v || !v->clip)
    return;
  v->step = snd_voice_compute_step(v->clip, m, pitch_8_8);
  /* The precomputed end frame is no longer meaningful once the rate changes;
     let the mix loop retire the voice when it runs off the end instead. */
  v->end_frame = -1;
}

void snd_voice_set_gain(snd_voice_t SND_PTR *v, int16_t gain_8_8) {
  if (!v)
    return;
  if (gain_8_8 < 0)
    gain_8_8 = 0;
  v->gain_l = gain_8_8;
  v->gain_r = gain_8_8;
}

void snd_voice_set_pan(snd_voice_t SND_PTR *v, int16_t gain_8_8,
                       int16_t pan_8_8) {
  long l, r;

  if (!v)
    return;
  if (gain_8_8 < 0)
    gain_8_8 = 0;
  if (pan_8_8 < 0)
    pan_8_8 = 0;
  if (pan_8_8 > (SND_GAIN_UNITY * 2))
    pan_8_8 = (int16_t)(SND_GAIN_UNITY * 2);

  r = ((long)gain_8_8 * (long)pan_8_8) / (long)(SND_GAIN_UNITY * 2);
  l = (long)gain_8_8 - r;
  /* Linear pan sums to unity, so a centred voice is half amplitude in each
     channel. Scale back up so centre matches the mono case. */
  v->gain_l = (int16_t)(l * 2L);
  v->gain_r = (int16_t)(r * 2L);
}

void snd_voice_seek(snd_voice_t SND_PTR *v, uint32_t frame) {
  if (!v)
    return;
  v->pos = SND_TO_FIXED((long)frame);
  v->reverse = 0;
}

snd_span_t snd_voice_span(const snd_voice_t SND_PTR *v) {
  snd_span_t s;
  s.start = 0;
  s.end = 0;
  if (!v || !v->active || !v->clip)
    return s;
  s.start = v->start_frame;
  s.end = (v->end_frame >= 0) ? v->end_frame : 0x7FFFFFFFL;
  return s;
}

/* ------------------------------------------------------------------ */
/* accumulate                                                          */
/* ------------------------------------------------------------------ */

#if SND_WIDE_ACCUM
#define SND_ACC(m, i, v)                                                       \
  do {                                                                         \
    if ((m)->accum)                                                            \
      (m)->accum[(i)] += (int32_t)(v);                                         \
    else                                                                       \
      (m)->block[(i)] = (snd_sample_t)SND_MIX_TO_SAMPLE(snd_clip_sample(       \
          (long)SND_SAMPLE_TO_MIX((m)->block[(i)]) + (long)(v)));              \
  } while (0)
#else
#define SND_ACC(m, i, v)                                                       \
  do {                                                                         \
    (m)->block[(i)] = (snd_sample_t)SND_MIX_TO_SAMPLE(snd_clip_sample(         \
        (long)SND_SAMPLE_TO_MIX((m)->block[(i)]) + (long)(v)));                \
  } while (0)
#endif

/* The out-of-line form of SND_ACC, for generators that live outside this
   translation unit. The synth uses it; so should anything you write. */
void snd_block_add(snd_mixer_t SND_PTR *m, long sample_index, long value) {
  if (!m || !m->block)
    return;
  if (sample_index < 0 || sample_index >= m->block_capacity)
    return;
  SND_ACC(m, sample_index, value);
}

/* Where the mix for this voice may write: the block's span intersected with
   the frames the voice is actually alive for. Everything after this point in
   the mix loops is unconditional, which is the same bargain gfx makes when it
   splits clipped and unclipped blitters. */
static int snd_mix_window(const snd_mixer_t SND_PTR *m,
                          const snd_voice_t SND_PTR *v, int SND_PTR *out_f0,
                          int SND_PTR *out_f1) {
  long block_start = m->block_frame;
  long lo = block_start + (long)m->span_0;
  long hi = block_start + (long)m->span_1;
  snd_span_t vs = snd_voice_span(v);

  if (vs.start > lo)
    lo = vs.start;
  if (vs.end < hi)
    hi = vs.end;
  if (hi <= lo)
    return 0;

  *out_f0 = (int)(lo - block_start);
  *out_f1 = (int)(hi - block_start);
  return 1;
}

/* Advance a voice's position by one output frame, applying loop policy.
   Returns 0 when the voice has run out and should be retired. */
static int snd_voice_advance(snd_voice_t SND_PTR *v,
                             const snd_clip_t SND_PTR *c) {
  long end = snd_clip_end_frame(c);
  long loop = snd_clip_loop_start(c);
  long idx;

  if (v->reverse)
    v->pos -= v->step;
  else
    v->pos += v->step;

  idx = SND_FROM_FIXED(v->pos);

  if (!(c->flags & SND_CLIP_LOOP)) {
    if (idx >= (long)c->frames || idx < 0)
      return 0;
    return 1;
  }

  if (c->flags & SND_CLIP_PINGPONG) {
    if (idx >= end) {
      v->reverse = 1;
      v->pos = SND_TO_FIXED(end - 1);
    } else if (idx < loop) {
      v->reverse = 0;
      v->pos = SND_TO_FIXED(loop);
    }
    return 1;
  }

  if (idx >= end) {
    long len = end - loop;
    if (len <= 0)
      return 0;
    /* Subtract rather than assign, so a step longer than the loop keeps its
       fractional phase and the loop stays in tune. */
    while (SND_FROM_FIXED(v->pos) >= end)
      v->pos -= SND_TO_FIXED(len);
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* mix loops, one per source format                                    */
/* ------------------------------------------------------------------ */

static void snd_emit(snd_mixer_t SND_PTR *m, int f, long sl, long sr,
                     long gl, long gr) {
  if (m->channels == 2) {
    long i = (long)f * 2L;
    SND_ACC(m, i, (sl * gl) >> SND_GAIN_SHIFT);
    SND_ACC(m, i + 1, (sr * gr) >> SND_GAIN_SHIFT);
  } else {
    long v = ((sl * gl) + (sr * gr)) >> (SND_GAIN_SHIFT + 1);
    SND_ACC(m, (long)f, v);
  }
}

void snd_mix_voice_pcm16(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v) {
  const snd_clip_t SND_PTR *c;
  const int16_t SND_PTR *src;
  int f, f0, f1;
  long gl, gr;
  int stereo_src;

  if (!m || !m->block || !v || !v->active || !v->clip)
    return;
  c = v->clip;
  if (!snd_mix_window(m, v, &f0, &f1))
    return;

  snd_touch_block(m);
  src = (const int16_t SND_PTR *)c->data;
  stereo_src = (c->channels == 2u);
  /* Voice gain only. Master volume is an output-stage gain now; folding it
     in here would compose two fixed-point gains and lose the low bits. */
  gl = (long)v->gain_l;
  gr = (long)v->gain_r;

  for (f = f0; f < f1; ++f) {
    long idx = SND_FROM_FIXED(v->pos);
    long sl, sr;

    if (idx < 0 || idx >= (long)c->frames)
      break;

    if (stereo_src) {
      sl = SND_SRC_FROM_S16(src[idx * 2]);
      sr = SND_SRC_FROM_S16(src[idx * 2 + 1]);
    } else {
      sl = SND_SRC_FROM_S16(src[idx]);
      sr = sl;
    }

#if SND_ENABLE_LERP
    {
      uint32_t frac = SND_FIXED_FRAC(v->pos);
      if (frac && (idx + 1) < (long)c->frames && !v->reverse) {
        long nl, nr;
        if (stereo_src) {
          nl = SND_SRC_FROM_S16(src[(idx + 1) * 2]);
          nr = SND_SRC_FROM_S16(src[(idx + 1) * 2 + 1]);
        } else {
          nl = SND_SRC_FROM_S16(src[idx + 1]);
          nr = nl;
        }
        sl += ((nl - sl) * (long)frac) >> SND_FIXED_SHIFT;
        sr += ((nr - sr) * (long)frac) >> SND_FIXED_SHIFT;
      }
    }
#endif

    snd_emit(m, f, sl, sr, gl, gr);

    if (!snd_voice_advance(v, c)) {
      v->active = 0;
      break;
    }
  }
}

void snd_mix_voice_pcm8(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v) {
  const snd_clip_t SND_PTR *c;
  const uint8_t SND_PTR *src;
  int f, f0, f1;
  long gl, gr;
  int stereo_src;

  if (!m || !m->block || !v || !v->active || !v->clip)
    return;
  c = v->clip;
  if (!snd_mix_window(m, v, &f0, &f1))
    return;

  snd_touch_block(m);
  src = (const uint8_t SND_PTR *)c->data;
  stereo_src = (c->channels == 2u);
  /* Voice gain only. Master volume is an output-stage gain now; folding it
     in here would compose two fixed-point gains and lose the low bits. */
  gl = (long)v->gain_l;
  gr = (long)v->gain_r;

  for (f = f0; f < f1; ++f) {
    long idx = SND_FROM_FIXED(v->pos);
    long sl, sr;

    if (idx < 0 || idx >= (long)c->frames)
      break;

    /* 0x80-centred bytes widened to the mix range. The widening is a multiply
       and not a shift: the value is signed after centring, and left-shifting a
       negative value is undefined in C99. Every compiler emits the shift. */
    if (stereo_src) {
      sl = SND_SRC_FROM_U8(src[idx * 2]);
      sr = SND_SRC_FROM_U8(src[idx * 2 + 1]);
    } else {
      sl = SND_SRC_FROM_U8(src[idx]);
      sr = sl;
    }

    snd_emit(m, f, sl, sr, gl, gr);

    if (!snd_voice_advance(v, c)) {
      v->active = 0;
      break;
    }
  }
}

#if SND_ENABLE_ADPCM
void snd_mix_voice_adpcm(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v) {
  const snd_clip_t SND_PTR *c;
  int f, f0, f1;
  long gl, gr;

  if (!m || !m->block || !v || !v->active || !v->clip)
    return;
  c = v->clip;
  if (!snd_mix_window(m, v, &f0, &f1))
    return;

  snd_touch_block(m);
  /* Voice gain only. Master volume is an output-stage gain now; folding it
     in here would compose two fixed-point gains and lose the low bits. */
  gl = (long)v->gain_l;
  gr = (long)v->gain_r;

  for (f = f0; f < f1; ++f) {
    long idx = SND_FROM_FIXED(v->pos);
    long s;

    if (idx < 0 || idx >= (long)c->frames)
      break;

    /* The IMA decoder always reconstructs at 16 bits, whatever the mixer's
       own sample format is. */
    s = SND_SRC_FROM_S16(snd_adpcm_sample_at(c, &v->adpcm, idx, 0));
    snd_emit(m, f, s, s, gl, gr);

    if (!snd_voice_advance(v, c)) {
      v->active = 0;
      break;
    }
  }
}
#endif

void snd_mix_voice(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v) {
  uint8_t fmt;

  if (!v || !v->clip)
    return;
  fmt = (uint8_t)(v->clip->flags & SND_CLIP_FORMAT_MASK);

  if (fmt & SND_CLIP_PCM16) {
    snd_mix_voice_pcm16(m, v);
    return;
  }
  if (fmt & SND_CLIP_PCM8) {
    snd_mix_voice_pcm8(m, v);
    return;
  }
#if SND_ENABLE_ADPCM
  if (fmt & SND_CLIP_ADPCM4)
    snd_mix_voice_adpcm(m, v);
#endif
}

void snd_mix_voice_counted(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v,
                           snd_mix_stats_t SND_PTR *stats) {
  int f0, f1;

  if (stats)
    ++stats->voices_considered;

  if (!m || !v || !v->active || !v->clip) {
    if (stats)
      ++stats->voices_rejected;
    return;
  }
  if (!snd_mix_window(m, v, &f0, &f1)) {
    if (stats)
      ++stats->voices_rejected;
    return;
  }

  snd_mix_voice(m, v);

  if (stats) {
    ++stats->voices_mixed;
    stats->frames_written += (unsigned long)(f1 - f0);
  }
}

/* ------------------------------------------------------------------ */
/* banks                                                               */
/* ------------------------------------------------------------------ */

void snd_bank_init(snd_bank_t SND_PTR *b) {
  int i;
  if (!b)
    return;
  b->count = SND_BANK_MAX_VOICES;
  for (i = 0; i < b->count; ++i)
    snd_voice_reset(&b->voices[i]);
}

snd_voice_t SND_PTR *snd_bank_alloc(snd_bank_t SND_PTR *b) {
  int i;
  if (!b)
    return 0;
  for (i = 0; i < b->count; ++i) {
    if (!b->voices[i].active)
      return &b->voices[i];
  }
  return 0;
}

snd_voice_t SND_PTR *snd_bank_alloc_or_steal(snd_bank_t SND_PTR *b) {
  int i, best = -1;
  long best_gain = 0;

  if (!b || b->count <= 0)
    return 0;

  for (i = 0; i < b->count; ++i) {
    if (!b->voices[i].active)
      return &b->voices[i];
  }
  for (i = 0; i < b->count; ++i) {
    long g = (long)b->voices[i].gain_l + (long)b->voices[i].gain_r;
    if (best < 0 || g < best_gain) {
      best = i;
      best_gain = g;
    }
  }
  return &b->voices[best];
}

snd_voice_t SND_PTR *snd_bank_find(snd_bank_t SND_PTR *b, int16_t id) {
  int i;
  if (!b)
    return 0;
  for (i = 0; i < b->count; ++i) {
    if (b->voices[i].active && b->voices[i].id == id)
      return &b->voices[i];
  }
  return 0;
}

void snd_bank_stop_all(snd_bank_t SND_PTR *b) {
  int i;
  if (!b)
    return;
  for (i = 0; i < b->count; ++i)
    b->voices[i].active = 0;
}

int snd_bank_active(const snd_bank_t SND_PTR *b) {
  int i, n = 0;
  if (!b)
    return 0;
  for (i = 0; i < b->count; ++i)
    if (b->voices[i].active)
      ++n;
  return n;
}

void snd_bank_mix(snd_mixer_t SND_PTR *m, snd_bank_t SND_PTR *b,
                  snd_mix_stats_t SND_PTR *stats) {
  int i;
  if (!m || !b)
    return;
  for (i = 0; i < b->count; ++i) {
    if (b->voices[i].active)
      snd_mix_voice_counted(m, &b->voices[i], stats);
  }
}

/* ------------------------------------------------------------------ */
/* spans                                                               */
/* ------------------------------------------------------------------ */

snd_span_t snd_span_make(long start, long end) {
  snd_span_t s;
  s.start = start;
  s.end = (end < start) ? start : end;
  return s;
}

snd_span_t snd_span_intersection(snd_span_t a, snd_span_t b) {
  snd_span_t s;
  s.start = (a.start > b.start) ? a.start : b.start;
  s.end = (a.end < b.end) ? a.end : b.end;
  if (s.end < s.start)
    s.end = s.start;
  return s;
}

int snd_span_empty(snd_span_t s) { return s.end <= s.start; }

long snd_span_length(snd_span_t s) {
  return (s.end > s.start) ? (s.end - s.start) : 0;
}

int snd_spans_overlap(snd_span_t a, snd_span_t b) {
  return (a.start < b.end) && (b.start < a.end);
}

/* ------------------------------------------------------------------ */
/* output format helpers                                               */
/* ------------------------------------------------------------------ */

void snd_pack_u8(const snd_sample_t SND_PTR *src, uint8_t SND_PTR *dst,
                 long samples) {
  long i;
  if (!src || !dst)
    return;
  for (i = 0; i < samples; ++i) {
    int v = SND_SAMPLE_TO_MIX(src[i]);
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_S16
    dst[i] = (uint8_t)(((v >> 8) + 128) & 0xFF);
#else
    dst[i] = (uint8_t)((v + 128) & 0xFF);
#endif
  }
}

void snd_pack_float(const snd_sample_t SND_PTR *src, float SND_PTR *dst,
                    long samples) {
  long i;
  if (!src || !dst)
    return;
  for (i = 0; i < samples; ++i)
    dst[i] = (float)SND_SAMPLE_TO_MIX(src[i]) / (float)(SND_FULL_SCALE + 1);
}

void snd_fold_mono(const snd_sample_t SND_PTR *src, snd_sample_t SND_PTR *dst,
                   long frames) {
  long i;
  if (!src || !dst)
    return;
  for (i = 0; i < frames; ++i) {
    long l = (long)SND_SAMPLE_TO_MIX(src[i * 2]);
    long r = (long)SND_SAMPLE_TO_MIX(src[i * 2 + 1]);
    dst[i] = (snd_sample_t)SND_MIX_TO_SAMPLE(snd_clip_sample((l + r) / 2));
  }
}
