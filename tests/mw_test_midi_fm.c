#include "snd_midi.h"
#include "snd_midi_fm.h"
#include "snd_mus.h"

#include <stdio.h>
#include <string.h>

#define RATE 22050
#define TOTAL_FRAMES 2048
#define MAX_BLOCK 256

static int checks;
static int failures;

#define CHECK(cond, text)                                                       \
  do {                                                                          \
    ++checks;                                                                    \
    if (!(cond)) {                                                               \
      ++failures;                                                                \
      printf("FAIL: %s\n", (text));                                           \
    }                                                                            \
  } while (0)

typedef struct capture {
  snd_sample_t sample[TOTAL_FRAMES];
  long frames;
} capture_t;

static snd_sample_t g_block[MAX_BLOCK];
#if SND_WIDE_ACCUM
static int32_t g_accum[MAX_BLOCK];
#endif

static const snd_fm_instrument_t test_programs[2] = {
    {
        {
            {{256u, 128, SND_WAVE_SINE, {0u, 0u, 40u, 256}},
             {256u, 256, SND_WAVE_SINE, {0u, 0u, 40u, 256}},
             24u, 0u, SND_FM_ALGORITHM_FM, 0, 0},
            {{256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
             {256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
             0u, 0u, SND_FM_ALGORITHM_FM, 0, 0}
        },
        1u, SND_FM_FIXED_NOTE_NONE, {0u, 0u}
    },
    {
        {
            {{512u, 112, SND_WAVE_SINE, {0u, 0u, 30u, 220}},
             {256u, 240, SND_WAVE_TRIANGLE, {0u, 0u, 30u, 220}},
             12u, 8u, SND_FM_ALGORITHM_ADDITIVE, 0, 0},
            {{256u, 72, SND_WAVE_SINE, {0u, 0u, 20u, 180}},
             {256u, 128, SND_WAVE_SINE, {0u, 0u, 20u, 180}},
             18u, 0u, SND_FM_ALGORITHM_FM, 0, 8}
        },
        2u, SND_FM_FIXED_NOTE_NONE, {0u, 0u}
    }
};

static const snd_fm_instrument_t test_percussion[2] = {
    {
        {
            {{256u, 128, SND_WAVE_NOISE, {0u, 0u, 20u, 128}},
             {256u, 256, SND_WAVE_NOISE, {0u, 0u, 35u, 96}},
             4u, 0u, SND_FM_ALGORITHM_ADDITIVE, 0, 0},
            {{256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
             {256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
             0u, 0u, SND_FM_ALGORITHM_FM, 0, 0}
        },
        1u, 48u, {0u, 0u}
    },
    {
        {
            {{512u, 96, SND_WAVE_NOISE, {0u, 0u, 15u, 96}},
             {256u, 256, SND_WAVE_SQUARE, {0u, 0u, 25u, 72}},
             8u, 0u, SND_FM_ALGORITHM_FM, 0, 0},
            {{256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
             {256u, 0, SND_WAVE_SINE, {0u, 0u, 0u, 0}},
             0u, 0u, SND_FM_ALGORITHM_FM, 0, 0}
        },
        1u, 60u, {0u, 0u}
    }
};

static const snd_midi_fm_bank_t test_bank = {
    test_programs, 2, test_percussion, 35, 2,
    &test_programs[0], &test_percussion[0]};

static void capture_drain(snd_mixer_t *m, long frame, int frames,
                          const snd_sample_t *samples, void *user) {
  capture_t *c = (capture_t *)user;
  int i;
  (void)m;
  if (!c || frame < 0 || frames < 0)
    return;
  for (i = 0; i < frames; ++i) {
    long at = frame + (long)i;
    if (at >= 0 && at < TOTAL_FRAMES)
      c->sample[at] = samples ? samples[i] : SND_SAMPLE_SILENCE;
  }
  if (frame + (long)frames > c->frames)
    c->frames = frame + (long)frames;
}

static void fm_mix_scene(snd_mixer_t *m, void *user) {
  snd_midi_fm_mix_block((snd_midi_fm_t *)user, m);
}

static void setup_mixer(snd_mixer_t *m, capture_t *c, int block_frames) {
  memset(c, 0, sizeof(*c));
  memset(g_block, 0, sizeof(g_block));
#if SND_WIDE_ACCUM
  memset(g_accum, 0, sizeof(g_accum));
#endif
  snd_init(m, RATE, 1, g_block, block_frames, capture_drain, c);
#if SND_WIDE_ACCUM
  snd_set_accumulator(m, g_accum);
#endif
}

static void render_remaining(snd_mixer_t *m, snd_midi_fm_t *fm,
                             long first, int block_frames) {
  long frame = first;
  while (frame < TOTAL_FRAMES) {
    long left = TOTAL_FRAMES - frame;
    int n = (left < (long)block_frames) ? (int)left : block_frames;
    snd_render_one_block(m, frame, n, fm_mix_scene, fm, SND_RENDER_CLEAR);
    frame += (long)n;
  }
}

static void queue_demo_midi(snd_midi_t *midi) {
  (void)snd_midi_message(midi, 0, 0x90u, 60u, 100u);
  (void)snd_midi_message(midi, 300, 0xB0u, SND_MIDI_CC_VOLUME, 90u);
  (void)snd_midi_message(midi, 500, 0xE0u, 0u, 80u);
  (void)snd_midi_message(midi, 700, 0x80u, 60u, 0u);
  (void)snd_midi_message(midi, 900, 0xC0u, 1u, 0u);
  (void)snd_midi_message(midi, 1000, 0x90u, 67u, 110u);
  (void)snd_midi_message(midi, 1300, 0x80u, 67u, 0u);
}

static void render_midi_case(capture_t *out, int block_frames) {
  snd_mixer_t mixer;
  snd_midi_t midi;
  snd_midi_fm_t fm;

  setup_mixer(&mixer, out, block_frames);
  snd_midi_init(&midi);
  snd_midi_fm_init(&fm);
  snd_midi_fm_set_bank(&fm, &test_bank);
  snd_midi_fm_bind(&fm, &midi);
  queue_demo_midi(&midi);
  render_remaining(&mixer, &fm, 0, block_frames);
}

static int capture_equal(const capture_t *a, const capture_t *b, long *diff) {
  long i;
  for (i = 0; i < TOTAL_FRAMES; ++i) {
    if (a->sample[i] != b->sample[i]) {
      if (diff)
        *diff = i;
      return 0;
    }
  }
  if (diff)
    *diff = -1;
  return 1;
}

static long capture_peak(const capture_t *c) {
  long i;
  long peak = 0;
  for (i = 0; i < TOTAL_FRAMES; ++i) {
    long v = (long)SND_SAMPLE_TO_MIX(c->sample[i]);
    if (v < 0)
      v = -v;
    if (v > peak)
      peak = v;
  }
  return peak;
}

static void test_defaults_and_binding(void) {
  snd_midi_t midi;
  snd_midi_fm_t fm;

  printf("midi fm defaults and binding\n");
  snd_midi_init(&midi);
  (void)snd_midi_message(&midi, 0, 0xC2u, 1u, 0u);
  (void)snd_midi_message(&midi, 0, 0xB2u, SND_MIDI_CC_VOLUME, 73u);
  snd_midi_fm_init(&fm);
  snd_midi_fm_bind(&fm, &midi);

  CHECK(fm.voice_limit == 9, "FM defaults to OPL2-shaped nine-voice polyphony");
  CHECK(SND_MIDI_FM_DEFAULT_OUTPUT_GAIN == 40,
        "FM default output gain is 40/256");
  CHECK(fm.output_gain == SND_MIDI_FM_DEFAULT_OUTPUT_GAIN,
        "FM starts with the configured pre-sum headroom");
  CHECK(fm.channel[2].program == 1u, "binding inherits existing MIDI program");
  CHECK(fm.channel[2].volume == 73u, "binding inherits existing MIDI volume");
  CHECK(fm.bank.fallback != NULL, "a deterministic fallback patch exists");

  snd_midi_fm_set_voice_limit(&fm, 99);
  CHECK(fm.voice_limit == SND_MIDI_FM_MAX_VOICES,
        "voice limit clamps to compiled maximum");
  snd_midi_fm_set_voice_limit(&fm, 0);
  CHECK(fm.voice_limit == 1, "voice limit never falls below one");

  snd_midi_fm_set_output_gain(&fm, (int16_t)SND_GAIN_UNITY);
  CHECK(fm.output_gain == SND_GAIN_UNITY,
        "FM output gain can be restored to unity explicitly");
  snd_midi_fm_set_output_gain(&fm, -1);
  CHECK(fm.output_gain == 0, "negative FM output gain clamps to silence");
  snd_midi_fm_set_output_gain(&fm, 999);
  CHECK(fm.output_gain == SND_GAIN_UNITY,
        "FM output gain clamps at unity");

  snd_midi_fm_set_output_gain(&fm, 37);
  snd_midi_fm_reset(&fm);
  CHECK(fm.output_gain == 37,
        "FM reset preserves caller-selected output gain");
}

static void test_queue_order(void) {
  snd_midi_fm_t fm;
  snd_midi_message_t msg;

  printf("midi fm bounded event queue\n");
  snd_midi_fm_init(&fm);
  msg.status = 0x90u;
  msg.data1 = 60u;
  msg.data2 = 100u;
  msg.size = 3u;

  CHECK(snd_midi_fm_queue_message(&fm, 10, &msg) == 1,
        "first queued event accepted");
  CHECK(snd_midi_fm_queue_message(&fm, 10, &msg) == 1,
        "same-frame event accepted");
  CHECK(snd_midi_fm_queue_message(&fm, 9, &msg) == 0,
        "out-of-order event rejected rather than reordering audio");
  CHECK(fm.dropped_events == 1uL, "queue rejection is observable");
  CHECK(snd_midi_fm_pending_events(&fm) == 2, "accepted events remain queued");
}

static void test_half_open_scheduling(void) {
  snd_mixer_t mixer;
  snd_midi_t midi;
  snd_midi_fm_t fm;
  capture_t cap;
  long i;
  int silent = 1;

  printf("midi fm half-open scheduling\n");
  setup_mixer(&mixer, &cap, 100);
  snd_midi_init(&midi);
  snd_midi_fm_init(&fm);
  snd_midi_fm_set_bank(&fm, &test_bank);
  snd_midi_fm_bind(&fm, &midi);
  (void)snd_midi_message(&midi, 100, 0x90u, 60u, 100u);

  snd_render_one_block(&mixer, 0, 100, fm_mix_scene, &fm, SND_RENDER_CLEAR);
  CHECK(snd_midi_fm_active_voices(&fm) == 0,
        "event exactly at block end waits for next block");
  CHECK(snd_midi_fm_pending_events(&fm) == 1,
        "block-end event remains queued");
  for (i = 0; i < 100; ++i)
    if (SND_SAMPLE_TO_MIX(cap.sample[i]) != 0)
      silent = 0;
  CHECK(silent, "audio before the scheduled note is silent");

  snd_render_one_block(&mixer, 100, 100, fm_mix_scene, &fm, SND_RENDER_CLEAR);
  CHECK(snd_midi_fm_active_voices(&fm) == 1,
        "event becomes active at next half-open block");
  CHECK(snd_midi_fm_pending_events(&fm) == 0,
        "scheduled event consumed once");
}

static void test_block_invariance(void) {
  capture_t a, b;
  long diff = -1;

  printf("midi fm block-size invariance\n");
  render_midi_case(&a, 37);
  render_midi_case(&b, 256);

  CHECK(capture_peak(&a) > (SND_FULL_SCALE / 32),
        "FM backend produces audible output");
  CHECK(capture_equal(&a, &b, &diff),
        "37-frame and 256-frame MIDI renders are sample-identical");
  CHECK(diff == -1, "no hidden block-boundary difference remains");
}

static void test_sustain_and_voice_steal(void) {
  snd_mixer_t mixer;
  snd_midi_t midi;
  snd_midi_fm_t fm;
  capture_t cap;

  printf("midi fm sustain and voice stealing\n");
  setup_mixer(&mixer, &cap, 128);
  snd_midi_init(&midi);
  snd_midi_fm_init(&fm);
  snd_midi_fm_set_bank(&fm, &test_bank);
  snd_midi_fm_set_voice_limit(&fm, 2);
  snd_midi_fm_bind(&fm, &midi);

  (void)snd_midi_message(&midi, 0, 0xB0u, SND_MIDI_CC_SUSTAIN, 127u);
  (void)snd_midi_message(&midi, 0, 0x90u, 60u, 100u);
  (void)snd_midi_message(&midi, 0, 0x90u, 64u, 100u);
  (void)snd_midi_message(&midi, 0, 0x90u, 67u, 100u);
  (void)snd_midi_message(&midi, 50, 0x80u, 64u, 0u);
  (void)snd_midi_message(&midi, 100, 0xB0u, SND_MIDI_CC_SUSTAIN, 0u);

  snd_render_one_block(&mixer, 0, 128, fm_mix_scene, &fm, SND_RENDER_CLEAR);
  CHECK(fm.voices_stolen == 1uL, "third note deterministically steals one voice");
  CHECK(snd_midi_fm_active_voices(&fm) <= 2, "voice limit is enforced");
  CHECK(fm.channel[0].sustain == 0u, "sustain release reached backend state");
  CHECK(fm.midi_notes_started == 3uL, "all note-on events were handled");
}

static void test_percussion_and_controls(void) {
  snd_mixer_t mixer;
  snd_midi_t midi;
  snd_midi_fm_t fm;
  capture_t cap;
  int i;
  int found_percussion = 0;

  printf("midi fm bank, percussion and controllers\n");
  setup_mixer(&mixer, &cap, 128);
  snd_midi_init(&midi);
  snd_midi_fm_init(&fm);
  snd_midi_fm_set_bank(&fm, &test_bank);
  snd_midi_fm_bind(&fm, &midi);

  (void)snd_midi_message(&midi, 0, 0xB9u, SND_MIDI_CC_PAN, 20u);
  (void)snd_midi_message(&midi, 0, 0x99u, 35u, 120u);
  (void)snd_midi_message(&midi, 10, 0xE9u, 0u, 64u);
  snd_render_one_block(&mixer, 0, 64, fm_mix_scene, &fm, SND_RENDER_CLEAR);

  for (i = 0; i < fm.voice_limit; ++i) {
    if (fm.voices[i].active && fm.voices[i].channel == 9u) {
      found_percussion = (fm.voices[i].patch == &test_percussion[0].layer[0]);
      break;
    }
  }
  CHECK(found_percussion, "MIDI channel 10 selects note-indexed percussion bank");
  CHECK(fm.channel[9].pan == 20u, "pan controller applied at its event frame");
  CHECK(fm.channel[9].pitch_bend == SND_MIDI_PITCH_CENTER,
        "pitch bend center is preserved");

  (void)snd_midi_message(&midi, 64, 0xB9u, SND_MIDI_CC_ALL_SOUND_OFF, 0u);
  snd_render_one_block(&mixer, 64, 64, fm_mix_scene, &fm, SND_RENDER_CLEAR);
  CHECK(snd_midi_fm_active_voices(&fm) == 0,
        "All Sound Off kills channel voices immediately");
}

/* ------------------------------------------------------------------ */
/* End-to-end synthetic MUS -> MIDI -> FM check.                       */
/* ------------------------------------------------------------------ */

typedef struct mus_builder {
  uint8_t data[128];
  uint32_t pos;
  uint32_t score_start;
} mus_builder_t;

static void wr16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void mus_begin(mus_builder_t *b) {
  memset(b, 0, sizeof(*b));
  b->data[0] = 'M';
  b->data[1] = 'U';
  b->data[2] = 'S';
  b->data[3] = 0x1au;
  wr16(b->data + 8, 1u);
  b->score_start = 16u;
  wr16(b->data + 6, (uint16_t)b->score_start);
  b->pos = b->score_start;
}

static void mus_u8(mus_builder_t *b, uint8_t v) {
  if (b->pos < (uint32_t)sizeof(b->data))
    b->data[b->pos++] = v;
}

static uint32_t mus_done(mus_builder_t *b) {
  wr16(b->data + 4, (uint16_t)(b->pos - b->score_start));
  return b->pos;
}

static uint32_t make_test_mus(mus_builder_t *b) {
  mus_begin(b);
  /* Note 60, velocity 100, then wait 2 MUS ticks. */
  mus_u8(b, 0x90u);
  mus_u8(b, (uint8_t)(0x80u | 60u));
  mus_u8(b, 100u);
  mus_u8(b, 2u);
  /* Release note 60, wait one tick. */
  mus_u8(b, 0x80u);
  mus_u8(b, 60u);
  mus_u8(b, 1u);
  mus_u8(b, 0x60u);
  return mus_done(b);
}

static void render_mus_case(capture_t *out, int block_frames) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_mixer_t mixer;
  snd_midi_t midi;
  snd_midi_fm_t fm;
  uint32_t bytes = make_test_mus(&b);
  long frame = 0;

  setup_mixer(&mixer, out, block_frames);
  snd_midi_init(&midi);
  snd_midi_fm_init(&fm);
  snd_midi_fm_set_bank(&fm, &test_bank);
  snd_midi_fm_bind(&fm, &midi);

  if (snd_mus_open(&song, b.data, bytes) != SND_MUS_OK)
    return;
  if (!snd_mus_player_init(&player, &song, RATE, SND_MUS_DOOM_TICK_HZ, 0, 0))
    return;

  while (frame < TOTAL_FRAMES) {
    long left = TOTAL_FRAMES - frame;
    int n = (left < (long)block_frames) ? (int)left : block_frames;
    (void)snd_mus_process_until(&player, &midi, frame + (long)n);
    snd_render_one_block(&mixer, frame, n, fm_mix_scene, &fm,
                         SND_RENDER_CLEAR);
    frame += (long)n;
  }
}

static void test_mus_to_fm(void) {
  capture_t a, b;
  long diff = -1;

  printf("mus -> midi -> fm integration\n");
  render_mus_case(&a, 37);
  render_mus_case(&b, 256);
  CHECK(capture_peak(&a) > (SND_FULL_SCALE / 32),
        "synthetic MUS reaches the FM renderer and makes sound");
  CHECK(capture_equal(&a, &b, &diff),
        "MUS->MIDI->FM remains sample-identical across block sizes");
}

int main(void) {
  printf("MicroWave MIDI FM tests (format=%s, wide_accum=%d)\n",
         (SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_S16) ? "S16" : "U8",
         SND_WIDE_ACCUM);

  test_defaults_and_binding();
  test_queue_order();
  test_half_open_scheduling();
  test_block_invariance();
  test_sustain_and_voice_steal();
  test_percussion_and_controls();
  test_mus_to_fm();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
