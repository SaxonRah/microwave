/* The shared demo song.
 *
 * Four channels: bass, lead, harmony, percussion. Sixteen rows per pattern,
 * four patterns in the order list. Written out longhand rather than generated,
 * because a table you can read is a table you can diff when a target sounds
 * wrong.
 */

#include "mw_music_demo.h"

/* MIDI notes. A minor pentatonic, which is hard to make sound bad on a square
   wave and is therefore the correct choice for a demo nobody will tune. */
#define A2 45
#define C3 48
#define D3 50
#define E3 52
#define G3 55
#define A3 57
#define C4 60
#define D4 62
#define E4 64
#define G4 67
#define A4 69
#define C5 72
#define D5 74
#define E5 76

#define __ SND_NOTE_NONE
#define OFF SND_NOTE_OFF
#define K SND_VOL_KEEP

/* channel 0 bass, 1 lead, 2 harmony, 3 percussion */
#define ROW(n0, i0, n1, i1, n2, i2, n3, i3)                                    \
  {n0, i0, K, K}, {n1, i1, K, K}, {n2, i2, K, K}, { n3, i3, K, K }

#define N(x) SND_NOTE(x)

static const snd_event_t mw_pattern_a[16 * 4] = {
    ROW(N(A2), 0, N(A4), 1, N(E4), 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, N(C5), 1, __, 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(N(A2), 0, N(D5), 1, N(A4), 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, N(C5), 1, __, 2, N(60), 3),
    ROW(__, 0, OFF, 1, __, 2, __, 3),
    ROW(N(C3), 0, N(E5), 1, N(C5), 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, N(D5), 1, __, 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(N(C3), 0, N(C5), 1, N(G4), 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, N(A4), 1, __, 2, N(60), 3),
    ROW(OFF, 0, OFF, 1, OFF, 2, __, 3)};

static const snd_event_t mw_pattern_b[16 * 4] = {
    ROW(N(D3), 0, N(D5), 1, N(A4), 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, N(E5), 1, __, 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(N(D3), 0, N(G4), 1, N(D4), 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, N(A4), 1, __, 2, N(60), 3),
    ROW(__, 0, OFF, 1, __, 2, __, 3),
    ROW(N(E3), 0, N(E5), 1, N(E4), 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, N(D5), 1, __, 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(N(G3), 0, N(C5), 1, N(G4), 2, N(60), 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, N(A4), 1, __, 2, N(60), 3),
    ROW(OFF, 0, OFF, 1, OFF, 2, __, 3)};

/* A quieter bridge, so the demo demonstrates that silence and low voice counts
   actually exercise the silent-block path rather than always being busy. */
static const snd_event_t mw_pattern_c[16 * 4] = {
    ROW(N(A2), 0, __, 1, N(A3), 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, N(E4), 1, __, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(N(G3), 0, N(G4), 1, N(D4), 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(OFF, 0, OFF, 1, OFF, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3),
    ROW(__, 0, __, 1, __, 2, __, 3)};

static const snd_pattern_t mw_patterns[3] = {
    {mw_pattern_a, 16, 4}, {mw_pattern_b, 16, 4}, {mw_pattern_c, 16, 4}};

static const uint8_t mw_order[6] = {0u, 0u, 1u, 0u, 2u, 1u};

static snd_instrument_t mw_instruments[4];

/* Gains are chosen so that all four channels sounding at once stays inside
   full scale. Four voices at unity would clip continuously, and a mix that
   clips continuously sounds the same on every target for the wrong reason. */
static void mw_build_instruments(void) {
  /* 0: bass, a triangle so it has body without eating the mix. */
  mw_instruments[0].clip = 0;
  mw_instruments[0].wave = (uint8_t)SND_WAVE_TRIANGLE;
  mw_instruments[0].base_note = 60;
  mw_instruments[0].gain = (int16_t)(SND_GAIN_UNITY / 2);
  snd_env_init(&mw_instruments[0].env, 40, 600,
               (int16_t)(SND_GAIN_UNITY / 3), 900);

  /* 1: lead square, short and bright. */
  mw_instruments[1].clip = 0;
  mw_instruments[1].wave = (uint8_t)SND_WAVE_SQUARE;
  mw_instruments[1].base_note = 60;
  mw_instruments[1].gain = (int16_t)(SND_GAIN_UNITY / 3);
  snd_env_init(&mw_instruments[1].env, 20, 900,
               (int16_t)(SND_GAIN_UNITY / 5), 1200);

  /* 2: harmony saw, deliberately quiet. */
  mw_instruments[2].clip = 0;
  mw_instruments[2].wave = (uint8_t)SND_WAVE_SAW;
  mw_instruments[2].base_note = 60;
  mw_instruments[2].gain = (int16_t)(SND_GAIN_UNITY / 7);
  snd_env_init(&mw_instruments[2].env, 300, 800,
               (int16_t)(SND_GAIN_UNITY / 6), 1500);

  /* 3: percussion, a noise burst with a very fast decay. Pitch is ignored for
     noise, so the note number in the pattern is arbitrary. */
  mw_instruments[3].clip = 0;
  mw_instruments[3].wave = (uint8_t)SND_WAVE_NOISE;
  mw_instruments[3].base_note = 60;
  mw_instruments[3].gain = (int16_t)(SND_GAIN_UNITY / 5);
  snd_env_init(&mw_instruments[3].env, 2, 1400, 0, 200);
}

void mw_demo_init(mw_demo_t SND_PTR *d, const snd_mixer_t SND_PTR *m,
                  long start_frame, int loop) {
  if (!d)
    return;

  mw_build_instruments();

  d->song.patterns = mw_patterns;
  d->song.pattern_count = 3;
  d->song.order = mw_order;
  d->song.order_length = 6;
  d->song.instruments = mw_instruments;
  d->song.instrument_count = 4;
  d->song.bpm = 132;
  d->song.rows_per_beat = 4;

  snd_player_init(&d->player, &d->song, m, start_frame, loop);
  snd_bank_init(&d->sfx);
  d->initialized = 1;
}

long mw_demo_length_frames(const mw_demo_t SND_PTR *d,
                           const snd_mixer_t SND_PTR *m) {
  if (!d || !d->initialized)
    return 0;
  return snd_song_length_frames(&d->song, m);
}

void mw_demo_mix(snd_mixer_t SND_PTR *m, void SND_PTR *user) {
  mw_demo_t SND_PTR *d = (mw_demo_t SND_PTR *)user;
  if (!d || !d->initialized)
    return;
  snd_player_mix_block(&d->player, m, 0);
  snd_bank_mix(m, &d->sfx, 0);
}

void mw_demo_trigger_sfx(mw_demo_t SND_PTR *d, const snd_mixer_t SND_PTR *m,
                         const snd_clip_t SND_PTR *clip, long at_frame) {
  snd_voice_t SND_PTR *v;

  if (!d || !d->initialized || !clip)
    return;

  /* Steal rather than drop: a game that fires more effects than it has voices
     should lose the quietest one, not the newest one. */
  v = snd_bank_alloc_or_steal(&d->sfx);
  if (!v)
    return;
  snd_voice_start(v, clip, m, at_frame, SND_GAIN_UNITY,
                  (int16_t)(SND_GAIN_UNITY * 3 / 4));
}

int mw_demo_finished(const mw_demo_t SND_PTR *d) {
  if (!d || !d->initialized)
    return 1;
  if (!snd_player_finished(&d->player))
    return 0;
  return snd_bank_active(&d->sfx) ? 0 : 1;
}
