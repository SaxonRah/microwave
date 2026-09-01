#ifndef MW_MUSIC_DEMO_H
#define MW_MUSIC_DEMO_H

#include "snd.h"
#include "snd_seq.h"
#include "snd_synth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The demo every frontend plays.
 *
 * Deliberately built from synth instruments only, so it needs no pack file and
 * no flash contents: a Pico with an empty filesystem, a DOS box with no
 * GAME.MRP next to the executable, and a CI runner with no assets checked out
 * all play the identical thing. It is the audio counterpart of
 * mr_game_demo.c, and it exists for the same reason -- so that "does DOS sound
 * like the host?" is a question with a yes-or-no answer rather than an
 * opinion.
 *
 * Everything here is integer and seeded. Two builds at the same mix rate must
 * produce bit-identical output; tests/mw_test_unit.c relies on that.
 */

typedef struct mw_demo {
  snd_song_t song;
  snd_player_t player;
  snd_bank_t sfx; /* one-shot effects, mixed alongside the song */
  int initialized;
} mw_demo_t;

/* Set up the demo song and player. `start_frame` is the absolute output frame
   the first row sounds on, normally 0. */
void mw_demo_init(mw_demo_t SND_PTR *d, const snd_mixer_t SND_PTR *m,
                  long start_frame, int loop);

/* Mix one block. Call from a mix_scene callback. */
void mw_demo_mix(snd_mixer_t SND_PTR *m, void SND_PTR *user);

/* Total frames one pass of the song occupies. */
long mw_demo_length_frames(const mw_demo_t SND_PTR *d,
                           const snd_mixer_t SND_PTR *m);

/* Fire a one-shot sound effect at an absolute output frame. Passing a NULL
   clip uses a built-in synth blip, so a frontend with no assets can still
   demonstrate the sfx path. */
void mw_demo_trigger_sfx(mw_demo_t SND_PTR *d, const snd_mixer_t SND_PTR *m,
                         const snd_clip_t SND_PTR *clip, long at_frame);

int mw_demo_finished(const mw_demo_t SND_PTR *d);

#ifdef __cplusplus
}
#endif

#endif /* MW_MUSIC_DEMO_H */
