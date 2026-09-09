#ifndef SND_MIDI_FM_H
#define SND_MIDI_FM_H

#include "snd.h"
#include "snd_midi.h"
#include "snd_synth.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic two-operator FM backend for snd_midi.
 *
 * This is deliberately OPL-shaped, not a cycle/register-accurate YM3812
 * emulator.  It gives MicroWave a small deterministic FM synthesis target for
 * MIDI while keeping hardware-accurate OPL and DMX GENMIDI parsing as separate
 * optional layers:
 *
 *      MUS / SMF / live MIDI -> snd_midi -> snd_midi_fm -> snd_mixer
 *
 * Patches and player state are caller-owned.  No malloc, stdio, clock, WAD or
 * platform API is used here.
 */

#ifndef SND_MIDI_FM_MAX_VOICES
#define SND_MIDI_FM_MAX_VOICES 18
#endif

#ifndef SND_MIDI_FM_QUEUE_EVENTS
#define SND_MIDI_FM_QUEUE_EVENTS 128
#endif

#define SND_FM_FIXED_NOTE_NONE 0xFFu
#define SND_FM_ALGORITHM_FM 0u
#define SND_FM_ALGORITHM_ADDITIVE 1u

/* Envelope times are milliseconds so one patch bank is independent of the
 * mix rate. Sustain is 8.8 gain, matching the rest of MicroWave. */
typedef struct snd_fm_env {
  uint16_t attack_ms;
  uint16_t decay_ms;
  uint16_t release_ms;
  int16_t sustain;
} snd_fm_env_t;

typedef struct snd_fm_operator {
  uint16_t multiple; /* 8.8 frequency ratio; 256 = 1x */
  int16_t level;     /* 8.8 amplitude; 256 = unity */
  uint8_t wave;      /* snd_wave_t */
  snd_fm_env_t env;
} snd_fm_operator_t;

typedef struct snd_fm_patch {
  snd_fm_operator_t modulator;
  snd_fm_operator_t carrier;

  /* Peak phase displacement, in 8.8 cycles. 256 therefore means a full-cycle
   * displacement at full modulator amplitude; useful musical patches are
   * normally much smaller (roughly 8..96). */
  uint16_t modulation;
  uint16_t feedback;

  uint8_t algorithm; /* SND_FM_ALGORITHM_* */
  int8_t transpose;  /* whole semitones */
  int16_t detune_128; /* signed 1/128-semitone fine tuning */
} snd_fm_patch_t;

#define SND_FM_MAX_LAYERS 2

typedef struct snd_fm_instrument {
  snd_fm_patch_t layer[SND_FM_MAX_LAYERS];
  uint8_t layer_count; /* 1 or 2; two layers map naturally to DMX double voice */
  uint8_t fixed_note;  /* MIDI note or SND_FM_FIXED_NOTE_NONE */
  uint8_t reserved[2];
} snd_fm_instrument_t;

typedef struct snd_midi_fm_bank {
  const snd_fm_instrument_t SND_PTR *programs;
  int program_count; /* normally 128; may be smaller */

  /* Percussion instruments are indexed by MIDI key - percussion_first_note.
   * This matches Doom's eventual GENMIDI use (notes 35..81) without baking
   * Doom or GENMIDI into the synth itself. */
  const snd_fm_instrument_t SND_PTR *percussion;
  int percussion_first_note;
  int percussion_count;

  const snd_fm_instrument_t SND_PTR *fallback;
  const snd_fm_instrument_t SND_PTR *percussion_fallback;
} snd_midi_fm_bank_t;

typedef struct snd_midi_fm_channel {
  uint16_t pitch_bend;
  uint8_t program;
  uint8_t volume;
  uint8_t expression;
  uint8_t pan;
  uint8_t sustain;
} snd_midi_fm_channel_t;

typedef struct snd_midi_fm_voice {
  const snd_fm_patch_t SND_PTR *patch;
  snd_fixed_t mod_phase;
  snd_fixed_t car_phase;
  snd_fixed_t mod_step;
  snd_fixed_t car_step;
  snd_env_t mod_env;
  snd_env_t car_env;
  long start_frame;
  long release_frame;
  unsigned long serial;
  uint32_t mod_noise;
  uint32_t car_noise;
  int16_t last_mod;
  uint8_t channel;
  uint8_t key;
  uint8_t velocity;
  int16_t pitch_128; /* base pitch before channel bend, in 1/128 semitone */
  uint8_t active;
  uint8_t key_released;
  uint8_t sustained;
} snd_midi_fm_voice_t;

typedef struct snd_midi_fm_event {
  long frame;
  snd_midi_message_t msg;
} snd_midi_fm_event_t;

typedef struct snd_midi_fm {
  snd_midi_fm_bank_t bank;
  snd_midi_fm_channel_t channel[SND_MIDI_CHANNELS];
  snd_midi_fm_voice_t voices[SND_MIDI_FM_MAX_VOICES];
  snd_midi_fm_event_t events[SND_MIDI_FM_QUEUE_EVENTS];
  int event_head;
  int event_count;
  int voice_limit;
  unsigned long next_serial;
  unsigned long midi_notes_started;
  unsigned long fm_voices_started;
  unsigned long fm_voices_released;
  unsigned long voices_stolen;
  unsigned long dropped_events;
} snd_midi_fm_t;

/* Safe built-in patches used only when a bank does not provide one. */
extern const snd_fm_instrument_t snd_fm_default_instrument;
extern const snd_fm_instrument_t snd_fm_default_percussion_instrument;

void snd_midi_fm_init(snd_midi_fm_t SND_PTR *fm);
/* Reset playback/channel/queue state while preserving the bank and voice limit. */
void snd_midi_fm_reset(snd_midi_fm_t SND_PTR *fm);
void snd_midi_fm_set_bank(snd_midi_fm_t SND_PTR *fm,
                          const snd_midi_fm_bank_t SND_PTR *bank);
void snd_midi_fm_set_voice_limit(snd_midi_fm_t SND_PTR *fm, int voices);

/* Install this synth as snd_midi's sink.  snd_midi currently owns one sink;
 * callers that need fan-out can invoke snd_midi_fm_midi_emit() themselves. */
void snd_midi_fm_bind(snd_midi_fm_t SND_PTR *fm, snd_midi_t SND_PTR *midi);
void snd_midi_fm_midi_emit(void SND_PTR *user, long frame,
                           const snd_midi_message_t SND_PTR *msg,
                           const snd_midi_channel_t SND_PTR *channel);

/* Queue a normalized MIDI message directly. Events must be supplied in
 * nondecreasing frame order. Returns 0 if the bounded queue rejects it. */
int snd_midi_fm_queue_message(snd_midi_fm_t SND_PTR *fm, long frame,
                              const snd_midi_message_t SND_PTR *msg);

/* Mix the current block. The event queue is consumed at exact frame boundaries
 * by subdividing m->span, so MIDI arriving in the middle of a 1024-frame block
 * sounds at the same sample as it does with 37-frame blocks. */
void snd_midi_fm_mix_block(snd_midi_fm_t SND_PTR *fm,
                           snd_mixer_t SND_PTR *m);

void snd_midi_fm_all_sound_off(snd_midi_fm_t SND_PTR *fm);
int snd_midi_fm_active_voices(const snd_midi_fm_t SND_PTR *fm);
int snd_midi_fm_pending_events(const snd_midi_fm_t SND_PTR *fm);

#ifdef __cplusplus
}
#endif

#endif /* SND_MIDI_FM_H */
