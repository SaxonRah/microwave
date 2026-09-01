#ifndef SND_H
#define SND_H

#include "snd_config.h"
#include "snd_fixed.h"
#include "snd_sample.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MicroWave: a compact software mixer built to the same rule as MicroRender.

   The mixer allocates nothing. The caller supplies one working block plus a
   drain callback, and the mixer fills that block over and over. That is what
   lets the same mixing code run on segmented 16-bit DOS, on an RP2350 feeding
   I2S by DMA, and on a desktop host under a sanitizer.

   The correspondence with the renderer is exact and intentional:

     gfx tile         -> snd block          a slice of the output, reused
     gfx flush        -> snd drain          hand the finished slice to hardware
     gfx clip rect    -> snd span           what part of the block may be written
     gfx sprite       -> snd clip           immutable source data
     gfx blit         -> snd mix            composite source into the working slice
     gfx dirty rect   -> snd silent block   skip work when nothing changed
     RGB565           -> S16                one logical format, converted on output

   Anyone who has read gfx.c should be able to read snd.c without a map.
*/

typedef struct snd_mixer snd_mixer_t;

typedef void (*snd_drain_fn)(snd_mixer_t SND_PTR *m, long frame, int frames,
                             const snd_sample_t SND_PTR *samples,
                             void SND_PTR *user);
typedef void (*snd_drain_begin_fn)(snd_mixer_t SND_PTR *m, long frame,
                                   int frames,
                                   const snd_sample_t SND_PTR *samples,
                                   void SND_PTR *user);
typedef void (*snd_drain_wait_fn)(snd_mixer_t SND_PTR *m, void SND_PTR *user);

struct snd_mixer {
  int rate;     /* logical mix rate in Hz */
  int channels; /* 1 = mono, 2 = interleaved stereo */

  long block_frame;  /* absolute frame index of block[0] */
  int block_frames;  /* frames currently in the working block */
  int block_max;     /* frames per block as configured */
  /* Sample capacity of the caller-provided block buffer. snd_init() infers
     this as block_max * channels; call snd_set_block_capacity() if you
     allocated more and intend to use longer blocks later. snd_begin_block()
     clamps against it so a long block can never write past the end of the
     buffer. */
  long block_capacity;
  snd_sample_t SND_PTR *block;

#if SND_WIDE_ACCUM
  int32_t SND_PTR *accum; /* optional wide scratch, block_capacity entries */
#endif

  /* Span clipping, in frames relative to the start of the block. Exactly the
     role gfx clip_x0/clip_x1 plays inside a tile. */
  int span_0;
  int span_1; /* exclusive */

  int16_t master_gain; /* 8.8, applied per voice on the way in */

  /* Set by the mix layer whenever a voice actually contributed to the current
     block. snd_render_blocked() reads it to decide whether the block can be
     drained as silence rather than mixed. */
  int block_touched;

  snd_drain_fn drain;
  snd_drain_begin_fn drain_begin;
  snd_drain_wait_fn drain_wait;
  void SND_PTR *user;
};

/* ------------------------------------------------------------------ */
/* clips: immutable source audio, the counterpart of gfx_sprite_t      */
/* ------------------------------------------------------------------ */

#define SND_CLIP_PCM16 0x01u   /* int16_t samples, native mix format   */
#define SND_CLIP_PCM8 0x02u    /* uint8_t samples, 0x80 centred        */
#define SND_CLIP_ADPCM4 0x04u  /* IMA ADPCM, self-seeking blocks       */
#define SND_CLIP_LOOP 0x08u    /* wrap to loop_start at loop_end       */
#define SND_CLIP_PINGPONG 0x10u /* with LOOP: reverse instead of wrap  */

/* Exactly one of PCM16 / PCM8 / ADPCM4 must be set. */
#define SND_CLIP_FORMAT_MASK                                                   \
  ((uint8_t)(SND_CLIP_PCM16 | SND_CLIP_PCM8 | SND_CLIP_ADPCM4))

typedef struct snd_clip {
  const void SND_PTR *data; /* raw samples, or the ADPCM block pool */
  uint32_t frames;          /* decoded frames, per channel */
  uint32_t bytes;           /* size of data, for validation */
  uint32_t loop_start;      /* frame index */
  uint32_t loop_end;        /* exclusive; 0 means "use frames" */
  int rate;                 /* native rate of the data */
  uint8_t channels;
  uint8_t flags;
} snd_clip_t;

/* IMA ADPCM decoder state. Small enough to live inside every voice, which is
   what lets a voice be seeked and restarted without touching the clip. */
typedef struct snd_adpcm_state {
  int32_t predictor;
  int16_t step_index;
  uint32_t block;      /* ADPCM block currently decoded from */
  uint32_t cursor;     /* frame within that block */
  int16_t cache[2];    /* last decoded frame, per channel */
} snd_adpcm_state_t;

/* ------------------------------------------------------------------ */
/* voices: a clip plus playback state, the counterpart of              */
/* gfx_sprite_instance_t                                               */
/* ------------------------------------------------------------------ */

typedef struct snd_voice {
  const snd_clip_t SND_PTR *clip;
  snd_fixed_t pos;  /* 16.16 frame position inside the clip */
  snd_fixed_t step; /* 16.16 frames advanced per output frame */
  int16_t gain_l;   /* 8.8 */
  int16_t gain_r;   /* 8.8; ignored when the mixer is mono */
  long start_frame; /* absolute output frame this voice becomes audible */
  long end_frame;   /* absolute output frame it goes silent; -1 = open */
  uint8_t active;
  uint8_t reverse; /* pingpong direction */
  int16_t id;      /* caller's tag, never interpreted */
  snd_adpcm_state_t adpcm;
} snd_voice_t;

typedef struct snd_bank {
  snd_voice_t voices[SND_BANK_MAX_VOICES];
  int count;
} snd_bank_t;

typedef struct snd_mix_stats {
  unsigned long frames_written;
  unsigned long frames_clipped;
  unsigned long voices_considered;
  unsigned long voices_rejected;
  unsigned long voices_mixed;
  unsigned long adpcm_blocks_decoded;
  unsigned long blocks_silent;
} snd_mix_stats_t;

/* A half-open range of absolute output frames. The time-domain counterpart of
   gfx_rect_t, and used for the same purpose: cheap rejection before any inner
   loop runs. */
typedef struct snd_span {
  long start;
  long end; /* exclusive */
} snd_span_t;

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void snd_init(snd_mixer_t SND_PTR *m, int rate, int channels,
              snd_sample_t SND_PTR *block_buffer, int block_frames,
              snd_drain_fn drain, void SND_PTR *user);
void snd_set_block_capacity(snd_mixer_t SND_PTR *m, long samples);
void snd_set_master_gain(snd_mixer_t SND_PTR *m, int16_t gain_8_8);

#if SND_WIDE_ACCUM
/* Install the optional wide accumulator. Without it the mixer sums with
   saturating adds directly in the block, which is the DOS and Pico path and
   remains available everywhere. */
void snd_set_accumulator(snd_mixer_t SND_PTR *m, int32_t SND_PTR *scratch);
#endif

/* Reject a clip whose fields are inconsistent, whose data is shorter than its
   frame count claims, or whose loop points are out of range. The hot mix path
   deliberately does not re-check any of this per block, so anything decoded
   from a pack file or any other untrusted source must be passed through here
   once after loading. Returns 1 if the clip is safe to mix, 0 otherwise.

   This is the exact counterpart of gfx_sprite_rle_validate() and exists for
   the same reason. */
int snd_clip_validate(const snd_clip_t SND_PTR *c);

/* Decoded frames a clip of `bytes` bytes in the given format holds. */
uint32_t snd_clip_frames_for_bytes(uint8_t flags, uint8_t channels,
                                   uint32_t bytes);

/* ------------------------------------------------------------------ */
/* block lifecycle: the counterpart of gfx_begin_tile / gfx_flush_tile */
/* ------------------------------------------------------------------ */

void snd_begin_block(snd_mixer_t SND_PTR *m, long frame, int frames);
void snd_flush_block(snd_mixer_t SND_PTR *m);
void snd_clear_block(snd_mixer_t SND_PTR *m);

/* Span state is per-block. snd_begin_block() resets the span to the whole
   block, so a span set before snd_render_blocked*() has no effect. Set it
   inside the mix_scene callback instead. This mirrors gfx clip semantics
   precisely, including the footgun. */
void snd_set_span(snd_mixer_t SND_PTR *m, int frame, int frames);
void snd_reset_span(snd_mixer_t SND_PTR *m);

#define SND_RENDER_CLEAR 0x01u
/* Drain a block that no voice touched without mixing or clearing it. The
   caller's drain must accept a NULL sample pointer as "this many frames of
   silence" -- a DMA target can then repeat a pre-zeroed buffer instead of
   rewriting one. The counterpart of skipping a clean dirty rect. */
#define SND_RENDER_SKIP_SILENT 0x02u

void snd_set_async_drain(snd_mixer_t SND_PTR *m, snd_drain_begin_fn begin_fn,
                         snd_drain_wait_fn wait_fn);

void snd_render_blocked(snd_mixer_t SND_PTR *m, long total_frames,
                        void (*mix_scene)(snd_mixer_t SND_PTR *m,
                                          void SND_PTR *scene_user),
                        void SND_PTR *scene_user);
void snd_render_blocked_ex(snd_mixer_t SND_PTR *m, long total_frames,
                           void (*mix_scene)(snd_mixer_t SND_PTR *m,
                                             void SND_PTR *scene_user),
                           void SND_PTR *scene_user, unsigned flags);
/* Mix block N+1 while the hardware is still consuming block N. The second
   buffer must be the same capacity as the first. Directly analogous to
   gfx_render_tiled_pipelined(). */
void snd_render_blocked_pipelined(
    snd_mixer_t SND_PTR *m, snd_sample_t SND_PTR *second_block,
    long total_frames,
    void (*mix_scene)(snd_mixer_t SND_PTR *m, void SND_PTR *scene_user),
    void SND_PTR *scene_user, unsigned flags);
/* Render one block starting at an arbitrary absolute frame. Used by the
   callback-driven frontends, where the audio device asks for exactly one
   buffer at a time and there is no enclosing loop to own. */
void snd_render_one_block(snd_mixer_t SND_PTR *m, long frame, int frames,
                          void (*mix_scene)(snd_mixer_t SND_PTR *m,
                                            void SND_PTR *scene_user),
                          void SND_PTR *scene_user, unsigned flags);

/* ------------------------------------------------------------------ */
/* voices                                                              */
/* ------------------------------------------------------------------ */

void snd_voice_reset(snd_voice_t SND_PTR *v);
/* Begin playback at `start_frame` absolute output frames. `pitch_8_8` is a
   multiplier on the clip's natural rate: 0x0100 is unity, 0x0200 an octave up.
   Rate conversion between the clip and the mixer is folded in automatically,
   so a 11025 Hz clip in a 22050 Hz mixer at unity pitch is correct. */
void snd_voice_start(snd_voice_t SND_PTR *v, const snd_clip_t SND_PTR *clip,
                     const snd_mixer_t SND_PTR *m, long start_frame,
                     int16_t pitch_8_8, int16_t gain_8_8);
void snd_voice_stop(snd_voice_t SND_PTR *v);
void snd_voice_set_pitch(snd_voice_t SND_PTR *v, const snd_mixer_t SND_PTR *m,
                         int16_t pitch_8_8);
void snd_voice_set_gain(snd_voice_t SND_PTR *v, int16_t gain_8_8);
/* pan_8_8: 0 hard left, 0x0100 centre, 0x0200 hard right. Constant-power is
   deliberately not used; a linear pan law is one shift on a 386 and the
   difference is inaudible against 8-bit DMA output. */
void snd_voice_set_pan(snd_voice_t SND_PTR *v, int16_t gain_8_8,
                       int16_t pan_8_8);
void snd_voice_seek(snd_voice_t SND_PTR *v, uint32_t frame);
/* Absolute output frames this voice can still produce sound over. A voice
   whose span does not intersect the current block is rejected before any
   sample is touched, which is the whole point. */
snd_span_t snd_voice_span(const snd_voice_t SND_PTR *v);

/* ------------------------------------------------------------------ */
/* mixing: the counterpart of the gfx blit family                      */
/* ------------------------------------------------------------------ */

void snd_mix_voice_pcm16(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v);
void snd_mix_voice_pcm8(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v);
#if SND_ENABLE_ADPCM
void snd_mix_voice_adpcm(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v);
#endif
/* Dispatch on the clip's format flags. The counterpart of gfx_blit(). */
void snd_mix_voice(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v);
void snd_mix_voice_counted(snd_mixer_t SND_PTR *m, snd_voice_t SND_PTR *v,
                           snd_mix_stats_t SND_PTR *stats);

/* ------------------------------------------------------------------ */
/* writing into the block from outside the built-in mixers             */
/* ------------------------------------------------------------------ */

/* Anything that generates samples -- the synth, or your own generator -- must
   go through these two rather than touching m->block directly. snd_touch_block
   performs the lazy clear that makes silent-block skipping work, and
   snd_block_add routes the sample through whichever accumulation strategy is
   configured. Writing to m->block by hand works right up until someone enables
   the wide accumulator, at which point the resolve step overwrites it. */
void snd_touch_block(snd_mixer_t SND_PTR *m);
/* `sample_index` is an index into the block in samples, not frames: for a
   stereo mixer, frame f left is 2*f and right is 2*f+1. */
void snd_block_add(snd_mixer_t SND_PTR *m, long sample_index, long value);

void snd_bank_init(snd_bank_t SND_PTR *b);
snd_voice_t SND_PTR *snd_bank_alloc(snd_bank_t SND_PTR *b);
/* Steal the quietest voice if none are free, so a bank never silently drops a
   note. Returns NULL only for a zero-capacity bank. */
snd_voice_t SND_PTR *snd_bank_alloc_or_steal(snd_bank_t SND_PTR *b);
snd_voice_t SND_PTR *snd_bank_find(snd_bank_t SND_PTR *b, int16_t id);
void snd_bank_stop_all(snd_bank_t SND_PTR *b);
int snd_bank_active(const snd_bank_t SND_PTR *b);
/* Mix every voice whose span intersects the current block, and retire the ones
   that ran out. The counterpart of gfx_sprites_draw_intersecting(). */
void snd_bank_mix(snd_mixer_t SND_PTR *m, snd_bank_t SND_PTR *b,
                  snd_mix_stats_t SND_PTR *stats);

/* ------------------------------------------------------------------ */
/* spans and format helpers                                            */
/* ------------------------------------------------------------------ */

snd_span_t snd_span_make(long start, long end);
snd_span_t snd_span_intersection(snd_span_t a, snd_span_t b);
int snd_span_empty(snd_span_t s);
long snd_span_length(snd_span_t s);
int snd_spans_overlap(snd_span_t a, snd_span_t b);

/* Convert a finished S16 block to the 8-bit unsigned form a Sound Blaster or a
   PWM slice wants. Called from a drain callback, never from the mix loop --
   this is the audio half of "DOS quantizes during presentation". */
void snd_pack_u8(const snd_sample_t SND_PTR *src, uint8_t SND_PTR *dst,
                 long samples);
void snd_pack_float(const snd_sample_t SND_PTR *src, float SND_PTR *dst,
                    long samples);
/* Fold interleaved stereo down to mono, for a target that has one channel. */
void snd_fold_mono(const snd_sample_t SND_PTR *src, snd_sample_t SND_PTR *dst,
                   long frames);

int snd_clip_sample(long v);

#ifdef __cplusplus
}
#endif

#endif /* SND_H */
