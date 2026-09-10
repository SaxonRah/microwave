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
 * Register-native Doom GENMIDI OPL backend.
 *
 * snd_mus remains the source-format/timing layer. snd_midi remains the
 * normalized event boundary. This backend owns the part the old
 * snd_genmidi -> snd_midi_fm conversion could not represent:
 *
 *   raw GENMIDI operator registers
 *   -> nine-channel DMX-shaped allocation
 *   -> YM3812 register writes
 *   -> Nuked OPL3 in OPL2 compatibility mode
 *   -> MicroWave mix block
 *
 * No allocator, WAD API, file I/O or clock is used here. All state is
 * caller-owned. The third-party chip core is kept as a separate target.
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

typedef struct snd_genmidi_opl_channel {
  uint16_t pitch_bend;
  uint8_t program;
  uint8_t volume;
  uint8_t expression;
  uint8_t pan;
  uint8_t sustain;
} snd_genmidi_opl_channel_t;

typedef struct snd_genmidi_opl_voice {
  const snd_genmidi_opl_instrument_t SND_PTR *instrument;
  uint32_t serial;
  int16_t base_pitch_32;
  uint8_t midi_channel;
  uint8_t key;
  uint8_t velocity;
  uint8_t layer;
  uint8_t b0;
  uint8_t active;
  uint8_t key_released;
  uint8_t sustained;
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
  snd_genmidi_opl_event_t events[SND_GENMIDI_OPL_QUEUE_EVENTS];

  int event_head;
  int event_count;
  int output_rate;
  int16_t output_gain; /* 8.8; 256 = chip output at unity */

  uint32_t next_serial;
  unsigned long midi_notes_started;
  unsigned long opl_voices_started;
  unsigned long opl_voices_released;
  unsigned long voices_stolen;
  unsigned long dropped_events;
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

void snd_genmidi_opl_bind(snd_genmidi_opl_t SND_PTR *opl,
                          snd_midi_t SND_PTR *midi);
void snd_genmidi_opl_midi_emit(
    void SND_PTR *user, long frame,
    const snd_midi_message_t SND_PTR *msg,
    const snd_midi_channel_t SND_PTR *channel);

int snd_genmidi_opl_queue_message(
    snd_genmidi_opl_t SND_PTR *opl, long frame,
    const snd_midi_message_t SND_PTR *msg);

/* Generate the current mixer span. MIDI events are applied at exact absolute
 * output frames before that frame's OPL sample is generated. */
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
