#ifndef SND_GENMIDI_OPL_H
#define SND_GENMIDI_OPL_H

#include "snd.h"
#include "snd_midi.h"
#include "opl3.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Strict Doom/DMX register-native GENMIDI backend.
 *
 * snd_mus owns MUS parsing/timing. snd_midi owns normalized channel messages.
 * This layer deliberately models the historical DMX OPL driver rather than a
 * generic MIDI synthesizer:
 *
 *   MUS -> snd_midi -> DMX channel/voice rules -> OPL registers -> Nuked OPL3
 *
 * The default behavior targets the Doom 1.9 / Ultimate Doom OPL2 path:
 * nine voices, raw GENMIDI operators, DMX volume law, FIFO physical voice
 * reuse, DMX voice stealing, DMX pitch table/indexing, and DMX controller
 * limitations.
 *
 * No malloc, stdio, WAD access or platform clock is used here.
 */

#define SND_GENMIDI_OPL_PROGRAM_COUNT 128
#define SND_GENMIDI_OPL_PERCUSSION_FIRST_NOTE 35
#define SND_GENMIDI_OPL_PERCUSSION_COUNT 47
#define SND_GENMIDI_OPL_INSTRUMENT_COUNT                                  \
  (SND_GENMIDI_OPL_PROGRAM_COUNT + SND_GENMIDI_OPL_PERCUSSION_COUNT)

#define SND_GENMIDI_OPL_HEADER_BYTES 8u
#define SND_GENMIDI_OPL_RAW_INSTRUMENT_BYTES 36u
#define SND_GENMIDI_OPL_MIN_BYTES                                         \
  (SND_GENMIDI_OPL_HEADER_BYTES +                                         \
   SND_GENMIDI_OPL_INSTRUMENT_COUNT * SND_GENMIDI_OPL_RAW_INSTRUMENT_BYTES)

#define SND_GENMIDI_OPL_FLAG_FIXED 0x0001u
#define SND_GENMIDI_OPL_FLAG_2VOICE 0x0004u

#define SND_GENMIDI_OPL_VOICES 9
#define SND_GENMIDI_OPL_QUEUE_EVENTS 128

typedef enum snd_genmidi_opl_error {
  SND_GENMIDI_OPL_OK = 0,
  SND_GENMIDI_OPL_ERR_ARGUMENT = 1,
  SND_GENMIDI_OPL_ERR_HEADER = 2,
  SND_GENMIDI_OPL_ERR_TRUNCATED = 3
} snd_genmidi_opl_error_t;

/*
 * Chocolate Doom / DSDA / Woof use 284 as the transition from the low-note
 * prefix into the repeating 12*32 table. Newer libADLMIDI reverse-engineering
 * models a historical DMX off-by-one at 283. Keep both available for A/B
 * validation; Doom19_284 is the default compatibility target.
 */
typedef enum snd_genmidi_opl_freq_model {
  SND_GENMIDI_OPL_FREQ_DOOM19_284 = 0,
  SND_GENMIDI_OPL_FREQ_DMX_BUG_283 = 1
} snd_genmidi_opl_freq_model_t;

typedef void (*snd_genmidi_opl_trace_fn)(
    void SND_PTR *user, long frame, uint16_t reg, uint8_t value);

typedef struct snd_genmidi_opl_operator {
  uint8_t tremolo;
  uint8_t attack;
  uint8_t sustain;
  uint8_t waveform;
  uint8_t scale;
  uint8_t level;
} snd_genmidi_opl_operator_t;

typedef struct snd_genmidi_opl_patch_voice {
  snd_genmidi_opl_operator_t modulator;
  uint8_t feedback;
  snd_genmidi_opl_operator_t carrier;
  int16_t base_note_offset;
} snd_genmidi_opl_patch_voice_t;

typedef struct snd_genmidi_opl_instrument {
  uint16_t flags;
  uint8_t fine_tuning;
  uint8_t fixed_note;
  snd_genmidi_opl_patch_voice_t voice[2];
} snd_genmidi_opl_instrument_t;

typedef struct snd_genmidi_opl_bank {
  snd_genmidi_opl_instrument_t
      instrument[SND_GENMIDI_OPL_INSTRUMENT_COUNT];
  uint8_t valid;
} snd_genmidi_opl_bank_t;

/* Indexed by DMX channel number, not normalized MIDI channel number.
 * Chocolate Doom swaps MIDI 9 <-> DMX 15 so percussion retains DMX's lowest
 * channel priority during voice stealing. */
typedef struct snd_genmidi_opl_channel {
  int16_t bend;       /* -64..63, only MIDI pitch-bend MSB is used */
  uint8_t program;    /* 0..127 */
  uint8_t volume;     /* effective DMX channel volume */
  uint8_t volume_base;
  uint8_t pan;        /* OPL pan bits; 0x30 in OPL2 mode */
} snd_genmidi_opl_channel_t;

/* One physical OPL channel. Instrument/layer cache intentionally survives a
 * release and free-list round trip because DMX skips operator rewrites when a
 * physical voice is reused for the same GENMIDI voice. */
typedef struct snd_genmidi_opl_voice {
  const snd_genmidi_opl_instrument_t SND_PTR *current_instr;
  uint16_t freq;
  uint8_t current_instr_voice;
  uint8_t dmx_channel;
  uint8_t key;
  uint8_t note;
  uint8_t note_volume;
  uint8_t car_volume;
  uint8_t mod_volume;
  uint8_t reg_pan;
  uint8_t priority;
  uint8_t active;
} snd_genmidi_opl_voice_t;

typedef struct snd_genmidi_opl_event {
  long frame;
  snd_midi_message_t msg;
} snd_genmidi_opl_event_t;

typedef struct snd_genmidi_opl {
  const snd_genmidi_opl_bank_t SND_PTR *bank;
  opl3_chip chip;

  snd_genmidi_opl_channel_t channel[SND_MIDI_CHANNELS];
  snd_genmidi_opl_voice_t voice[SND_GENMIDI_OPL_VOICES];

  /* DMX uses FIFO free and allocated lists. List ordering is observable:
   * note-off, pitch bend, and steals alter which physical channel is reused. */
  uint8_t free_list[SND_GENMIDI_OPL_VOICES];
  uint8_t alloc_list[SND_GENMIDI_OPL_VOICES];
  uint8_t free_count;
  uint8_t alloc_count;

  snd_genmidi_opl_event_t events[SND_GENMIDI_OPL_QUEUE_EVENTS];
  int event_head;
  int event_count;

  int output_rate;
  int16_t output_gain; /* 8.8 post-chip bus gain; 256 = unity */
  uint8_t freq_model;

  long current_frame;
  snd_genmidi_opl_trace_fn trace;
  void SND_PTR *trace_user;

  unsigned long midi_notes_started;
  unsigned long opl_voices_started;
  unsigned long opl_voices_released;
  unsigned long voices_stolen;
  unsigned long secondary_voices_dropped;
  unsigned long dropped_events;
  unsigned long register_writes;
} snd_genmidi_opl_t;

int snd_genmidi_opl_load_bank(snd_genmidi_opl_bank_t SND_PTR *out,
                              const void SND_PTR *data, uint32_t bytes);
const char *snd_genmidi_opl_error_string(int error);

void snd_genmidi_opl_init(snd_genmidi_opl_t SND_PTR *opl, int output_rate);
void snd_genmidi_opl_reset(snd_genmidi_opl_t SND_PTR *opl);
void snd_genmidi_opl_set_bank(
    snd_genmidi_opl_t SND_PTR *opl,
    const snd_genmidi_opl_bank_t SND_PTR *bank);
void snd_genmidi_opl_set_output_gain(snd_genmidi_opl_t SND_PTR *opl,
                                     int16_t gain_8_8);
void snd_genmidi_opl_set_freq_model(
    snd_genmidi_opl_t SND_PTR *opl,
    snd_genmidi_opl_freq_model_t model);
void snd_genmidi_opl_set_trace(snd_genmidi_opl_t SND_PTR *opl,
                               snd_genmidi_opl_trace_fn trace,
                               void SND_PTR *user);

void snd_genmidi_opl_bind(snd_genmidi_opl_t SND_PTR *opl,
                          snd_midi_t SND_PTR *midi);
void snd_genmidi_opl_midi_emit(
    void SND_PTR *user, long frame,
    const snd_midi_message_t SND_PTR *msg,
    const snd_midi_channel_t SND_PTR *channel);

int snd_genmidi_opl_queue_message(
    snd_genmidi_opl_t SND_PTR *opl, long frame,
    const snd_midi_message_t SND_PTR *msg);

void snd_genmidi_opl_mix_block(snd_genmidi_opl_t SND_PTR *opl,
                               snd_mixer_t SND_PTR *m);

void snd_genmidi_opl_all_sound_off(snd_genmidi_opl_t SND_PTR *opl);
int snd_genmidi_opl_active_voices(
    const snd_genmidi_opl_t SND_PTR *opl);
int snd_genmidi_opl_pending_events(
    const snd_genmidi_opl_t SND_PTR *opl);

#ifdef __cplusplus
}
#endif

#endif /* SND_GENMIDI_OPL_H */
