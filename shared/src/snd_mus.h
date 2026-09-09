#ifndef SND_MUS_H
#define SND_MUS_H

#include "snd_midi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Doom/DMX MUS source adapter.
 *
 * This layer decodes an in-memory MUS score into normalized snd_midi messages.
 * It is deliberately only a source-format adapter:
 *
 *     caller-owned MUS bytes -> snd_mus -> snd_midi -> backend
 *
 * No malloc, stdio, WAD access, audio device, synth, or mixer dependency.
 * The player carries a cursor into the caller-owned bytes and schedules events
 * on absolute output frames.  Processing in 1-frame, 37-frame, or 1024-frame
 * chunks therefore produces the same MIDI event stream at the same frames.
 */

#define SND_MUS_DOOM_TICK_HZ 140
#define SND_MUS_RAPTOR_TICK_HZ 70
#define SND_MUS_CHANNELS 16
#define SND_MUS_PERCUSSION_CHANNEL 15

/* Errors are stable API values so a frontend can log a useful reason without
 * pulling strings or stdio into the decoder itself. */
typedef enum snd_mus_error {
  SND_MUS_OK = 0,
  SND_MUS_ERR_ARGUMENT = 1,
  SND_MUS_ERR_HEADER = 2,
  SND_MUS_ERR_MAGIC = 3,
  SND_MUS_ERR_RANGE = 4,
  SND_MUS_ERR_TRUNCATED = 5,
  SND_MUS_ERR_EVENT = 6,
  SND_MUS_ERR_CONTROLLER = 7,
  SND_MUS_ERR_DELAY = 8,
  SND_MUS_ERR_NO_END = 9,
  SND_MUS_ERR_CHANNELS = 10,
  SND_MUS_ERR_TIME = 11,
  SND_MUS_ERR_ZERO_LOOP = 12
} snd_mus_error_t;

typedef struct snd_mus_song {
  const uint8_t SND_PTR *data;
  uint32_t bytes;
  uint32_t score_offset;
  uint32_t score_length;
  uint16_t primary_channels;
  uint16_t secondary_channels;
  uint16_t instrument_count;
  uint8_t valid;
} snd_mus_song_t;

/* Runtime state is public so callers can place it statically on DOS/Pico. */
typedef struct snd_mus_player {
  const snd_mus_song_t SND_PTR *song;
  uint32_t cursor;
  uint32_t score_end;
  uint32_t tick_remainder;
  uint32_t ticks_in_loop;
  long next_frame;
  long start_frame;
  int output_rate;
  int tick_hz;
  int loop;
  int finished;
  int error;
  unsigned long loops_completed;

  /* MUS channels are mapped lazily to MIDI channels in first-use order,
   * skipping MIDI channel 10 (index 9), exactly as classic MUS converters do.
   * MUS channel 15 always maps to MIDI percussion channel 9. */
  int16_t channel_map[SND_MUS_CHANNELS];
  int16_t next_midi_channel;
  uint8_t velocity[SND_MUS_CHANNELS];
} snd_mus_player_t;

/* Validate and describe caller-owned MUS bytes.  The data is never copied and
 * must outlive the song/player.  Validation scans the score once so malformed
 * event payloads cannot fail later inside an audio callback. */
int snd_mus_open(snd_mus_song_t SND_PTR *song, const void SND_PTR *data,
                 uint32_t bytes);

/* Instrument list metadata from the MUS header.  Returns 0xffff for an invalid
 * index.  This does not load or interpret a patch bank. */
uint16_t snd_mus_instrument(const snd_mus_song_t SND_PTR *song, int index);

const char *snd_mus_error_string(int error);

/* `tick_hz` is 140 for Doom/Heretic/Hexen/Strife MUS and 70 for Raptor.
 * `start_frame` is the absolute output frame at which the first MUS event is
 * scheduled.  Returns 1 on success. */
int snd_mus_player_init(snd_mus_player_t SND_PTR *player,
                        const snd_mus_song_t SND_PTR *song, int output_rate,
                        int tick_hz, long start_frame, int loop);

/* Restart the same song at a new absolute frame.  Timing fractional remainder
 * is reset because this is an explicit new playback, unlike an internal loop
 * where the remainder is carried to avoid long-term drift. */
void snd_mus_player_restart(snd_mus_player_t SND_PTR *player,
                            long start_frame);
void snd_mus_player_stop(snd_mus_player_t SND_PTR *player);
int snd_mus_player_finished(const snd_mus_player_t SND_PTR *player);
int snd_mus_player_error(const snd_mus_player_t SND_PTR *player);

/* Decode and emit every event whose timestamp is strictly before `end_frame`.
 * The half-open boundary matches MicroWave mix blocks.  Returns the number of
 * MIDI messages emitted, or -1 if the player enters an error state. */
int snd_mus_process_until(snd_mus_player_t SND_PTR *player,
                          snd_midi_t SND_PTR *midi, long end_frame);

#ifdef __cplusplus
}
#endif

#endif /* SND_MUS_H */
