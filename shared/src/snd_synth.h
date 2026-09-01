#ifndef SND_SYNTH_H
#define SND_SYNTH_H

#include "snd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Procedural sound with no assets at all.
 *
 * MicroRender carries a 5x7 font so that any frontend can put an FPS counter
 * on screen without loading anything. This is the same idea for audio: a
 * handful of oscillators and an envelope, driven straight into the mix block
 * from a ROM table, so any frontend can make a noise before the asset pipeline
 * exists. The stress test and the HUD "tick" both use it.
 *
 * A snd_tone_t is deliberately not a snd_voice_t. A voice reads a clip; a tone
 * has no source data at all, exactly as gfx_draw_text5x7() writes pixels
 * without a sprite.
 */

typedef enum snd_wave {
  SND_WAVE_SQUARE = 0,
  SND_WAVE_SAW = 1,
  SND_WAVE_TRIANGLE = 2,
  SND_WAVE_SINE = 3,
  SND_WAVE_NOISE = 4,
  SND_WAVE_COUNT = 5
} snd_wave_t;

/* ADSR in 8.8 gain and whole output frames. Kept integer end to end so the
   envelope a DOS build hears is bit-identical to the one CI hears. */
typedef struct snd_env {
  long attack;   /* frames */
  long decay;    /* frames */
  int16_t sustain; /* 8.8 level */
  long release;  /* frames */
} snd_env_t;

typedef struct snd_tone {
  snd_fixed_t phase;
  snd_fixed_t phase_step;
  int16_t gain;      /* 8.8 peak */
  long start_frame;  /* absolute output frame */
  long end_frame;    /* absolute; -1 for sustain until stopped */
  long release_from; /* absolute frame the release began, -1 if not released */
  uint32_t noise;    /* LFSR state; never rand() */
  uint8_t wave;
  uint8_t active;
  int16_t id;
  snd_env_t env;
} snd_tone_t;

/* 256-entry full-cycle sine, the audio counterpart of the font glyph table. */
extern const int16_t snd_sine_table[256];

/* Raw oscillator sample for a 16.16 phase in [0,1). Exposed so an offline
   tool can bake the same waveform a runtime tone would have produced. */
int snd_wave_sample(snd_wave_t wave, snd_fixed_t phase, uint32_t SND_PTR *lfsr);

void snd_env_init(snd_env_t SND_PTR *e, long attack, long decay,
                  int16_t sustain, long release);
/* Envelope level in 8.8 at `frame` frames after the note started. */
int16_t snd_env_level(const snd_env_t SND_PTR *e, long since_start,
                      long since_release);

void snd_tone_reset(snd_tone_t SND_PTR *t);
void snd_tone_start(snd_tone_t SND_PTR *t, const snd_mixer_t SND_PTR *m,
                    snd_wave_t wave, int hz, int16_t gain_8_8,
                    long start_frame, long frames);
void snd_tone_set_hz(snd_tone_t SND_PTR *t, const snd_mixer_t SND_PTR *m,
                     int hz);
void snd_tone_set_env(snd_tone_t SND_PTR *t, const snd_env_t SND_PTR *e);
/* Begin the release segment at an absolute output frame. */
void snd_tone_release(snd_tone_t SND_PTR *t, long frame);
void snd_tone_stop(snd_tone_t SND_PTR *t);
snd_span_t snd_tone_span(const snd_tone_t SND_PTR *t);

/* Mix one tone into the current block. Obeys the block span exactly as the
   voice mixers do. */
void snd_mix_tone(snd_mixer_t SND_PTR *m, snd_tone_t SND_PTR *t);

/* ------------------------------------------------------------------ */
/* offline generation                                                  */
/* ------------------------------------------------------------------ */

/* Fill a caller-owned buffer with one waveform and describe it as a clip. The
   synth allocates nothing, for the same reason the mixer does not. `frames`
   samples are written to `buf`. Returns 1 on success. */
int snd_synth_fill(snd_sample_t SND_PTR *buf, uint32_t frames, snd_wave_t wave,
                   int rate, int hz, int16_t gain_8_8);
int snd_synth_clip(snd_clip_t SND_PTR *out, snd_sample_t SND_PTR *buf,
                   uint32_t frames, int rate, snd_wave_t wave, int hz,
                   int16_t gain_8_8);

/* Equal-tempered note to Hz, A4 = note 69 = 440 Hz. Integer only: a lookup
   over one octave plus a shift, so a 386 pays a table read and not a pow(). */
int snd_note_hz(int midi_note);

#ifdef __cplusplus
}
#endif

#endif /* SND_SYNTH_H */
