#ifndef SND_MIDI_H
#define SND_MIDI_H

#include "snd_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MicroWave MIDI foundation.
 *
 * This layer deliberately implements MIDI *messages*, not a MIDI file format
 * and not a synthesizer.  It is the common event/state boundary that a MUS
 * decoder, a Standard MIDI File decoder, a live serial MIDI input, an OPL
 * backend, or a sample synth can all share without knowing about one another.
 *
 * In other words:
 *
 *      MUS / SMF / live bytes
 *              |
 *              v
 *          snd_midi
 *              |
 *              v
 *      OPL / wavetable / hardware MIDI / ...
 *
 * There is no malloc, stdio, clock, song storage or audio dependency here.
 */

#define SND_MIDI_CHANNELS 16
#define SND_MIDI_NOTES 128
#define SND_MIDI_PITCH_CENTER 8192u
#define SND_MIDI_PITCH_MAX 16383u
#define SND_MIDI_GM_PERCUSSION_CHANNEL 9u /* MIDI channel 10, zero based. */

/* Channel-voice status nibbles. */
#define SND_MIDI_NOTE_OFF 0x80u
#define SND_MIDI_NOTE_ON 0x90u
#define SND_MIDI_POLY_PRESSURE 0xA0u
#define SND_MIDI_CONTROL_CHANGE 0xB0u
#define SND_MIDI_PROGRAM_CHANGE 0xC0u
#define SND_MIDI_CHANNEL_PRESSURE 0xD0u
#define SND_MIDI_PITCH_BEND 0xE0u

/* Controllers the state tracker understands directly.  Unknown controllers
 * are still emitted to the sink unchanged. */
#define SND_MIDI_CC_BANK_MSB 0u
#define SND_MIDI_CC_VOLUME 7u
#define SND_MIDI_CC_PAN 10u
#define SND_MIDI_CC_EXPRESSION 11u
#define SND_MIDI_CC_BANK_LSB 32u
#define SND_MIDI_CC_SUSTAIN 64u
#define SND_MIDI_CC_ALL_SOUND_OFF 120u
#define SND_MIDI_CC_RESET_CONTROLLERS 121u
#define SND_MIDI_CC_ALL_NOTES_OFF 123u

typedef struct snd_midi_message {
  uint8_t status; /* full status byte, including the zero-based channel */
  uint8_t data1;
  uint8_t data2;
  uint8_t size;   /* 2 for Cn/Dn, otherwise 3 for channel messages */
} snd_midi_message_t;

/* Cached channel state.  This is intentionally small: note ownership belongs
 * to the sink/synth because a hardware MIDI sink does not need 16*128 bytes of
 * note state and a polyphonic synth needs more information than one bit. */
typedef struct snd_midi_channel {
  uint16_t pitch_bend; /* 0..16383, centre 8192 */
  uint8_t program;     /* 0..127 */
  uint8_t bank_msb;
  uint8_t bank_lsb;
  uint8_t volume;      /* CC7 */
  uint8_t expression;  /* CC11 */
  uint8_t pan;         /* CC10, 64 centre */
  uint8_t sustain;     /* 0 or 1 */
  uint8_t pressure;    /* channel pressure */
} snd_midi_channel_t;

/* One callback is enough.  The state tracker updates the channel first, then
 * emits the normalized message and a pointer to that channel's new state.
 * Note-On with velocity zero is normalized to Note-Off before it reaches the
 * sink.  A sink may ignore state entirely and forward msg bytes to hardware. */
typedef void (*snd_midi_emit_fn)(void SND_PTR *user, long frame,
                                 const snd_midi_message_t SND_PTR *msg,
                                 const snd_midi_channel_t SND_PTR *channel);

typedef struct snd_midi_sink {
  snd_midi_emit_fn emit;
  void SND_PTR *user;
} snd_midi_sink_t;

typedef struct snd_midi {
  snd_midi_channel_t channel[SND_MIDI_CHANNELS];
  snd_midi_sink_t sink;

  /* Raw-byte parser state.  Running status is retained across channel data
   * messages and real-time bytes, but cancelled by System Common/SysEx. */
  uint8_t running_status;
  uint8_t pending_status;
  uint8_t data[2];
  uint8_t data_count;
  uint8_t data_need;
  uint8_t system_data_left;
  uint8_t in_sysex;
} snd_midi_t;

/* Neutral MIDI state, with no sink attached. */
void snd_midi_init(snd_midi_t SND_PTR *m);
/* Reset parser + channel state while preserving the installed sink. */
void snd_midi_reset(snd_midi_t SND_PTR *m);
void snd_midi_set_sink(snd_midi_t SND_PTR *m, snd_midi_emit_fn emit,
                       void SND_PTR *user);

/* Number of data bytes for a channel status, or 0 when status is not a valid
 * channel-voice status. */
int snd_midi_data_bytes(uint8_t status);

/* Submit one complete channel message.  data2 is ignored for the two-byte
 * Program Change and Channel Pressure forms.  Returns 1 when accepted. */
int snd_midi_message(snd_midi_t SND_PTR *m, long frame, uint8_t status,
                     uint8_t data1, uint8_t data2);

/* Feed one raw MIDI byte.  This understands channel running status and safely
 * skips System Common, SysEx and real-time traffic.  It returns 1 only when a
 * complete channel message was dispatched to snd_midi_message(). */
int snd_midi_feed_byte(snd_midi_t SND_PTR *m, long frame, uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* SND_MIDI_H */
