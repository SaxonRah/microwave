#ifndef SND_SEQ_H
#define SND_SEQ_H

#include "snd.h"
#include "snd_synth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pattern sequencer.
 *
 * A song is an order list over patterns, a pattern is rows by channels, and a
 * row is a note event. That is the same shape as a tilemap: an index array
 * over a tileset, walked with a camera. gfx_draw_tilemap() walks the part of
 * the map the camera can see; snd_player_mix_block() walks the part of the
 * song the current block can hear.
 *
 * Row events are scheduled at absolute output frames, not "once per callback".
 * A block boundary that falls in the middle of a row does not move the note,
 * and a target that renders blocks slowly does not play the song slowly. This
 * is the same argument mr_timestep.h makes for the game simulation, and for
 * the same reason: determinism is what makes the tests worth anything.
 */

#ifndef SND_SEQ_MAX_CHANNELS
#define SND_SEQ_MAX_CHANNELS 8
#endif

/* note values */
#define SND_NOTE_NONE 0u
#define SND_NOTE_OFF 1u
/* Anything else is a MIDI note number plus 2, so that 0 and 1 stay free. */
#define SND_NOTE(n) ((uint8_t)((n) + 2))
#define SND_NOTE_VALUE(v) ((int)(v) - 2)

#define SND_VOL_KEEP 0xFFu

typedef struct snd_event {
  uint8_t note;
  uint8_t instrument;
  uint8_t volume; /* 0..64, or SND_VOL_KEEP */
  uint8_t pan;    /* 0..128, 64 centre */
} snd_event_t;

typedef struct snd_pattern {
  const snd_event_t SND_PTR *events; /* row_count * channel_count */
  int row_count;
  int channel_count;
} snd_pattern_t;

/* An instrument is either a clip or a synth waveform. A song that uses only
   waveforms needs no assets at all, which is what lets the shared demo run on
   a Pico with an empty flash. */
typedef struct snd_instrument {
  const snd_clip_t SND_PTR *clip; /* NULL selects the synth path */
  uint8_t wave;                   /* snd_wave_t, used when clip is NULL */
  int base_note;                  /* MIDI note the clip plays at native rate */
  int16_t gain;                   /* 8.8 */
  snd_env_t env;
} snd_instrument_t;

typedef struct snd_song {
  const snd_pattern_t SND_PTR *patterns;
  int pattern_count;
  const uint8_t SND_PTR *order;
  int order_length;
  const snd_instrument_t SND_PTR *instruments;
  int instrument_count;
  int bpm;
  int rows_per_beat;
} snd_song_t;

typedef struct snd_player {
  const snd_song_t SND_PTR *song;
  long frames_per_row;
  long next_row_frame; /* absolute output frame of the next row */
  int order_index;
  int row;
  int channel_count;
  int loop;
  int finished;
  unsigned long rows_played;
  snd_tone_t tones[SND_SEQ_MAX_CHANNELS];
  snd_voice_t voices[SND_SEQ_MAX_CHANNELS];
} snd_player_t;

/* `start_frame` is the absolute output frame row 0 should sound on. */
void snd_player_init(snd_player_t SND_PTR *p, const snd_song_t SND_PTR *song,
                     const snd_mixer_t SND_PTR *m, long start_frame, int loop);
void snd_player_stop(snd_player_t SND_PTR *p);
int snd_player_finished(const snd_player_t SND_PTR *p);

/* Trigger every row that lands inside the current block, then mix every
   channel. Call this from a mix_scene callback; it reads the mixer's current
   block window and nothing else. */
void snd_player_mix_block(snd_player_t SND_PTR *p, snd_mixer_t SND_PTR *m,
                          snd_mix_stats_t SND_PTR *stats);

/* Total output frames the song occupies, one pass, no loop. Useful for
   offline rendering and for the tests, which need to know how much to ask
   for. */
long snd_song_length_frames(const snd_song_t SND_PTR *song,
                            const snd_mixer_t SND_PTR *m);

/* Frames per row for a given tempo and mix rate. Exposed because the DOS
   frontend needs it to size its DMA buffer sensibly. */
long snd_seq_frames_per_row(const snd_song_t SND_PTR *song, int rate);

#ifdef __cplusplus
}
#endif

#endif /* SND_SEQ_H */
