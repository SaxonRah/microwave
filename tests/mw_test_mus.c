#include "snd_midi.h"
#include "snd_mus.h"

#include <stdio.h>
#include <string.h>

typedef struct mus_log_entry {
  long frame;
  snd_midi_message_t msg;
  snd_midi_channel_t state;
} mus_log_entry_t;

typedef struct mus_log {
  mus_log_entry_t entry[256];
  int count;
} mus_log_t;

typedef struct mus_builder {
  uint8_t data[512];
  uint32_t pos;
  uint32_t score_start;
} mus_builder_t;

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

static void wr16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xffu);
  p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static void mb_begin(mus_builder_t *b, const uint16_t *instruments,
                     int instrument_count) {
  int i;
  memset(b, 0, sizeof(*b));
  b->data[0] = 'M';
  b->data[1] = 'U';
  b->data[2] = 'S';
  b->data[3] = 0x1au;
  wr16(b->data + 8, 4u);  /* primary channels */
  wr16(b->data + 10, 0u); /* secondary channels */
  wr16(b->data + 12, (uint16_t)instrument_count);
  wr16(b->data + 14, 0u);
  b->score_start = 16u + (uint32_t)instrument_count * 2u;
  wr16(b->data + 6, (uint16_t)b->score_start);
  for (i = 0; i < instrument_count; ++i)
    wr16(b->data + 16u + (uint32_t)i * 2u, instruments[i]);
  b->pos = b->score_start;
}

static void mb_u8(mus_builder_t *b, uint8_t v) {
  if (b->pos < (uint32_t)sizeof(b->data))
    b->data[b->pos++] = v;
}

static uint32_t mb_done(mus_builder_t *b) {
  uint32_t score_len = b->pos - b->score_start;
  wr16(b->data + 4, (uint16_t)score_len);
  return b->pos;
}

static void log_emit(void *user, long frame, const snd_midi_message_t *msg,
                     const snd_midi_channel_t *channel) {
  mus_log_t *log = (mus_log_t *)user;
  mus_log_entry_t *e;
  if (!log || !msg || !channel || log->count >= 256)
    return;
  e = &log->entry[log->count++];
  e->frame = frame;
  e->msg = *msg;
  e->state = *channel;
}

static void setup_midi(snd_midi_t *midi, mus_log_t *log) {
  memset(log, 0, sizeof(*log));
  snd_midi_init(midi);
  snd_midi_set_sink(midi, log_emit, log);
}

static int logs_equal(const mus_log_t *a, const mus_log_t *b) {
  int i;
  if (a->count != b->count)
    return 0;
  for (i = 0; i < a->count; ++i) {
    if (a->entry[i].frame != b->entry[i].frame)
      return 0;
    if (memcmp(&a->entry[i].msg, &b->entry[i].msg,
               sizeof(a->entry[i].msg)) != 0)
      return 0;
  }
  return 1;
}

static void test_header(void) {
  const uint16_t instruments[3] = {0u, 40u, 135u};
  mus_builder_t b;
  snd_mus_song_t song;
  uint32_t bytes;

  printf("mus header and metadata\n");
  mb_begin(&b, instruments, 3);
  mb_u8(&b, 0x60u); /* score end */
  bytes = mb_done(&b);

  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "valid MUS header opens");
  CHECK(song.valid == 1u, "opened song marked valid");
  CHECK(song.instrument_count == 3u, "instrument count preserved");
  CHECK(snd_mus_instrument(&song, 0) == 0u, "instrument zero readable");
  CHECK(snd_mus_instrument(&song, 1) == 40u, "instrument 40 readable");
  CHECK(snd_mus_instrument(&song, 2) == 135u,
        "percussion instrument number readable");
  CHECK(snd_mus_instrument(&song, 3) == 0xffffu,
        "out-of-range instrument rejected");
}

static void test_validation(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  uint32_t bytes;

  printf("mus validation\n");

  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);
  b.data[0] = 'X';
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_ERR_MAGIC,
        "bad signature rejected");

  CHECK(snd_mus_open(&song, b.data, 8u) == SND_MUS_ERR_HEADER,
        "short header rejected before signature");

  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x90u); /* note-on last */
  mb_u8(&b, 0xbcu); /* note 60 + velocity follows, but velocity is missing */
  bytes = mb_done(&b);
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_ERR_TRUNCATED,
        "truncated event payload rejected");

  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0xc0u); /* controller event, last */
  mb_u8(&b, 10u);   /* controller 10 is invalid for valued event */
  mb_u8(&b, 0u);
  mb_u8(&b, 0u); /* delay */
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_ERR_CONTROLLER,
        "invalid valued controller rejected");

  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0xb0u); /* system event, last */
  mb_u8(&b, 9u);
  mb_u8(&b, 0u);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_ERR_CONTROLLER,
        "invalid system controller rejected");

  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x80u); /* release, last */
  mb_u8(&b, 60u);
  mb_u8(&b, 0x90u);
  mb_u8(&b, 0x80u);
  mb_u8(&b, 0x80u);
  mb_u8(&b, 0x80u);
  mb_u8(&b, 0x00u); /* 16 * 128^4 overflows uint32 */
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_ERR_DELAY,
        "overflowing delay rejected");

  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x80u);
  mb_u8(&b, 60u);
  mb_u8(&b, 1u);
  bytes = mb_done(&b);
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_ERR_NO_END,
        "score without finish event rejected");
}

static void test_channel_mapping_velocity_and_percussion(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_midi_t midi;
  mus_log_t log;
  uint32_t bytes;

  printf("mus channel mapping, velocity and percussion\n");
  mb_begin(&b, NULL, 0);

  /* First-used MUS channel 2 -> MIDI channel 0. */
  mb_u8(&b, 0x12u); /* note on ch2, same group */
  mb_u8(&b, (uint8_t)(0x80u | 60u));
  mb_u8(&b, 100u);
  mb_u8(&b, 0x12u); /* second note inherits velocity */
  mb_u8(&b, 62u);

  /* Next first-used non-percussion channel maps to MIDI 1. */
  mb_u8(&b, 0x40u); /* program change on MUS ch0 */
  mb_u8(&b, 0u);
  mb_u8(&b, 41u);

  /* MUS channel 15 maps directly to MIDI percussion channel 9 and does not
     receive the synthetic all-notes-off allocation event. */
  mb_u8(&b, 0x9fu); /* note on ch15, last */
  mb_u8(&b, (uint8_t)(0x80u | 35u));
  mb_u8(&b, 110u);
  mb_u8(&b, 1u); /* delay */
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);

  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "mapping fixture opens");
  setup_midi(&midi, &log);
  CHECK(snd_mus_player_init(&player, &song, 14000,
                            SND_MUS_DOOM_TICK_HZ, 1000, 0) == 1,
        "player initializes");
  CHECK(snd_mus_process_until(&player, &midi, 2000) == 6,
        "expected translated MIDI message count");

  CHECK(log.count == 6, "six MIDI messages logged");
  CHECK(log.entry[0].msg.status == 0xb0u &&
            log.entry[0].msg.data1 == SND_MIDI_CC_ALL_NOTES_OFF,
        "new melodic channel starts with all-notes-off");
  CHECK(log.entry[1].msg.status == 0x90u && log.entry[1].msg.data1 == 60u &&
            log.entry[1].msg.data2 == 100u,
        "first note translated with explicit velocity");
  CHECK(log.entry[2].msg.status == 0x90u && log.entry[2].msg.data1 == 62u &&
            log.entry[2].msg.data2 == 100u,
        "next note inherits channel velocity");
  CHECK(log.entry[3].msg.status == 0xb1u &&
            log.entry[3].msg.data1 == SND_MIDI_CC_ALL_NOTES_OFF,
        "second first-used MUS channel allocates MIDI channel 1");
  CHECK(log.entry[4].msg.status == 0xc1u && log.entry[4].msg.data1 == 41u,
        "MUS instrument change becomes MIDI program change");
  CHECK(log.entry[5].msg.status == 0x99u && log.entry[5].msg.data1 == 35u &&
            log.entry[5].msg.data2 == 110u,
        "MUS channel 15 becomes MIDI percussion channel 9");
  CHECK(snd_mus_player_finished(&player),
        "score end emits no extra MIDI message");
}

/* The mapping fixture above ends at score-end and should emit no finish MIDI
 * event.  Keep its exact count test focused by using a separate release after
 * the delay. */
static void test_timing_and_release(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_midi_t midi;
  mus_log_t log;
  uint32_t bytes;

  printf("mus 140 Hz timing\n");
  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x90u); /* note on, last */
  mb_u8(&b, (uint8_t)(0x80u | 60u));
  mb_u8(&b, 100u);
  mb_u8(&b, 1u);
  mb_u8(&b, 0x80u); /* release, last */
  mb_u8(&b, 60u);
  mb_u8(&b, 1u);
  mb_u8(&b, 0x90u); /* another note, last */
  mb_u8(&b, 62u);
  mb_u8(&b, 1u);
  mb_u8(&b, 0x80u); /* release, last */
  mb_u8(&b, 62u);
  mb_u8(&b, 1u);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);

  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "timing fixture opens");
  setup_midi(&midi, &log);
  CHECK(snd_mus_player_init(&player, &song, 22050,
                            SND_MUS_DOOM_TICK_HZ, 0, 0) == 1,
        "22050 Hz player initializes");
  CHECK(snd_mus_process_until(&player, &midi, 1000) >= 0,
        "timing fixture processes");

  /* Entry 0 is synthetic All Notes Off.  Four musical events then occur at
     floor(cumulative_ticks * 22050 / 140): 0, 157, 315, 472. */
  CHECK(log.count == 5, "allocation plus four musical events emitted");
  CHECK(log.entry[1].frame == 0, "tick 0 maps to frame 0");
  CHECK(log.entry[2].frame == 157, "tick 1 maps to frame 157");
  CHECK(log.entry[3].frame == 315, "tick 2 carries half-frame remainder");
  CHECK(log.entry[4].frame == 472, "tick 3 maps without cumulative drift");
  CHECK(snd_mus_player_finished(&player), "non-looping score finishes");
}

static void test_controllers(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_midi_t midi;
  mus_log_t log;
  uint32_t bytes;
  static const uint8_t expected_cc[9] = {32u, 1u, 7u, 10u, 11u,
                                         91u, 93u, 64u, 67u};
  static const uint8_t expected_system[5] = {120u, 123u, 126u, 127u, 121u};
  int i;

  printf("mus controller translation\n");
  mb_begin(&b, NULL, 0);

  mb_u8(&b, 0x40u); /* program change */
  mb_u8(&b, 0u);
  mb_u8(&b, 12u);

  for (i = 1; i <= 9; ++i) {
    mb_u8(&b, 0x40u);
    mb_u8(&b, (uint8_t)i);
    mb_u8(&b, (uint8_t)(20 + i));
  }

  for (i = 10; i <= 14; ++i) {
    mb_u8(&b, 0x30u);
    mb_u8(&b, (uint8_t)i);
  }

  mb_u8(&b, 0xa0u); /* pitch, last */
  mb_u8(&b, 128u);  /* center */
  mb_u8(&b, 1u);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);

  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "controller fixture opens");
  setup_midi(&midi, &log);
  snd_mus_player_init(&player, &song, 14000, 140, 0, 0);
  CHECK(snd_mus_process_until(&player, &midi, 1000) >= 0,
        "controller fixture processes");

  CHECK(log.entry[1].msg.status == 0xc0u && log.entry[1].msg.data1 == 12u,
        "MUS controller zero maps to Program Change");
  for (i = 0; i < 9; ++i) {
    CHECK(log.entry[2 + i].msg.status == 0xb0u &&
              log.entry[2 + i].msg.data1 == expected_cc[i],
          "valued MUS controller maps to expected MIDI CC");
  }
  for (i = 0; i < 5; ++i) {
    CHECK(log.entry[11 + i].msg.status == 0xb0u &&
              log.entry[11 + i].msg.data1 == expected_system[i] &&
              log.entry[11 + i].msg.data2 == 0u,
          "MUS system event maps to valueless MIDI CC");
  }
  CHECK(log.entry[16].msg.status == 0xe0u && log.entry[16].msg.data1 == 0u &&
            log.entry[16].msg.data2 == 64u,
        "MUS pitch 128 maps to MIDI pitch center");
  CHECK(midi.channel[0].program == 12u, "MIDI program state updated");
  CHECK(midi.channel[0].volume == 23u, "MUS volume updates MIDI CC7 state");
  CHECK(log.entry[5].state.pan == 24u,
        "MUS pan updates MIDI CC10 state when emitted");
  CHECK(midi.channel[0].pan == 64u,
        "later Reset All Controllers returns pan to center");
}

static void build_invariance_song(mus_builder_t *b, uint32_t *bytes) {
  mb_begin(b, NULL, 0);
  mb_u8(b, 0x90u);
  mb_u8(b, (uint8_t)(0x80u | 60u));
  mb_u8(b, 90u);
  mb_u8(b, 1u);
  mb_u8(b, 0x80u);
  mb_u8(b, 60u);
  mb_u8(b, 2u);
  mb_u8(b, 0x90u);
  mb_u8(b, (uint8_t)(0x80u | 64u));
  mb_u8(b, 80u);
  mb_u8(b, 3u);
  mb_u8(b, 0x80u);
  mb_u8(b, 64u);
  mb_u8(b, 4u);
  mb_u8(b, 0x60u);
  *bytes = mb_done(b);
}

static void test_process_window_invariance(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t pa, pb;
  snd_midi_t ma, mb;
  mus_log_t la, lb;
  uint32_t bytes;
  long end;

  printf("mus process-window invariance\n");
  build_invariance_song(&b, &bytes);
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "invariance fixture opens");

  setup_midi(&ma, &la);
  snd_mus_player_init(&pa, &song, 22050, 140, 50, 0);
  snd_mus_process_until(&pa, &ma, 100000);

  setup_midi(&mb, &lb);
  snd_mus_player_init(&pb, &song, 22050, 140, 50, 0);
  for (end = 37; end < 4000 && !snd_mus_player_finished(&pb); end += 37)
    snd_mus_process_until(&pb, &mb, end);

  CHECK(logs_equal(&la, &lb),
        "37-frame processing emits identical event stream to one-shot");
}

static void test_looping(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_midi_t midi;
  mus_log_t log;
  uint32_t bytes;

  printf("mus looping\n");
  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x90u);
  mb_u8(&b, (uint8_t)(0x80u | 60u));
  mb_u8(&b, 100u);
  mb_u8(&b, 2u);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);

  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "loop fixture opens");
  setup_midi(&midi, &log);
  snd_mus_player_init(&player, &song, 22050, 140, 0, 1);
  CHECK(snd_mus_process_until(&player, &midi, 700) >= 0,
        "looping song processes");
  CHECK(player.loops_completed == 2uL,
        "two loop boundaries crossed before frame 700");
  CHECK(log.count == 6,
        "each of three loop starts emits all-notes-off plus note-on");
  CHECK(log.entry[1].frame == 0 && log.entry[3].frame == 315 &&
            log.entry[5].frame == 630,
        "loop starts remain on exact cumulative 140-Hz timeline");

  /* A score with no positive delay is valid for one-shot playback but must not
     spin forever when asked to loop. */
  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "zero-duration score is valid as data");
  setup_midi(&midi, &log);
  snd_mus_player_init(&player, &song, 22050, 140, 0, 1);
  CHECK(snd_mus_process_until(&player, &midi, 1) == -1,
        "zero-duration looping score is refused");
  CHECK(snd_mus_player_error(&player) == SND_MUS_ERR_ZERO_LOOP,
        "zero-duration loop reports precise error");
}

static void test_measure_and_unused_events(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_midi_t midi;
  mus_log_t log;
  uint32_t bytes;

  printf("mus no-op event compatibility\n");
  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x50u); /* end-of-measure, no payload */
  mb_u8(&b, 0xf0u); /* unused event 7, last, one ignored payload byte */
  mb_u8(&b, 0xa5u);
  mb_u8(&b, 1u);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);

  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "documented no-op/unused MUS events validate safely");
  setup_midi(&midi, &log);
  snd_mus_player_init(&player, &song, 14000, 140, 0, 0);
  CHECK(snd_mus_process_until(&player, &midi, 1000) == 0,
        "no-op MUS events emit no MIDI traffic");
  CHECK(log.count == 0, "no-op events do not allocate MIDI channels");
}


static void test_all_truncations(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  uint32_t bytes;
  uint32_t n;

  printf("mus truncation sweep\n");
  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x90u);
  mb_u8(&b, (uint8_t)(0x80u | 60u));
  mb_u8(&b, 100u);
  mb_u8(&b, 0x81u);
  mb_u8(&b, 0x01u);
  mb_u8(&b, 0x80u);
  mb_u8(&b, 60u);
  mb_u8(&b, 1u);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);

  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "truncation reference fixture opens whole");
  for (n = 0u; n < bytes; ++n) {
    CHECK(snd_mus_open(&song, b.data, n) != SND_MUS_OK,
          "every truncated prefix is rejected");
  }
}

static void test_all_channels(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_midi_t midi;
  mus_log_t log;
  uint32_t bytes;
  int ch;
  int li;

  printf("mus all-channel allocation\n");
  mb_begin(&b, NULL, 0);
  for (ch = 0; ch < 16; ++ch) {
    uint8_t desc = (uint8_t)(0x10u | (uint8_t)ch);
    if (ch == 15)
      desc |= 0x80u;
    mb_u8(&b, desc);
    mb_u8(&b, (uint8_t)(0x80u | (uint8_t)(40 + ch)));
    mb_u8(&b, 64u);
  }
  mb_u8(&b, 1u);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);

  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "16-channel fixture opens");
  setup_midi(&midi, &log);
  snd_mus_player_init(&player, &song, 14000, 140, 0, 0);
  CHECK(snd_mus_process_until(&player, &midi, 1000) == 31,
        "15 melodic allocations plus 16 notes emitted");

  li = 0;
  for (ch = 0; ch < 15; ++ch) {
    int midi_ch = (ch < 9) ? ch : ch + 1;
    CHECK(log.entry[li].msg.status == (uint8_t)(0xb0u | (uint8_t)midi_ch) &&
              log.entry[li].msg.data1 == SND_MIDI_CC_ALL_NOTES_OFF,
          "melodic MUS channel gets deterministic MIDI allocation");
    ++li;
    CHECK(log.entry[li].msg.status == (uint8_t)(0x90u | (uint8_t)midi_ch),
          "melodic note uses allocated MIDI channel");
    ++li;
  }
  CHECK(log.entry[li].msg.status == 0x99u,
        "MUS percussion remains MIDI channel 10 after all melodic channels");
}

static void test_half_open_and_tick_rates(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_midi_t midi;
  mus_log_t log;
  uint32_t bytes;

  printf("mus half-open scheduling and tick rates\n");
  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0x90u);
  mb_u8(&b, (uint8_t)(0x80u | 60u));
  mb_u8(&b, 100u);
  mb_u8(&b, 1u);
  mb_u8(&b, 0x80u);
  mb_u8(&b, 60u);
  mb_u8(&b, 1u);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "boundary fixture opens");

  setup_midi(&midi, &log);
  snd_mus_player_init(&player, &song, 22050, 140, 0, 0);
  snd_mus_process_until(&player, &midi, 157);
  CHECK(log.count == 2, "event exactly at end_frame is deferred");
  snd_mus_process_until(&player, &midi, 158);
  CHECK(log.count == 3 && log.entry[2].frame == 157,
        "deferred event appears in next half-open window");

  setup_midi(&midi, &log);
  snd_mus_player_init(&player, &song, 22050, SND_MUS_RAPTOR_TICK_HZ, 0, 0);
  snd_mus_process_until(&player, &midi, 1000);
  CHECK(log.entry[2].frame == 315,
        "70 Hz MUS tick rate maps one tick to 315 frames at 22050 Hz");
}

static void test_value_clamp_and_restart(void) {
  mus_builder_t b;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_midi_t midi;
  mus_log_t log;
  uint32_t bytes;

  printf("mus value quirk and restart\n");
  mb_begin(&b, NULL, 0);
  mb_u8(&b, 0xc0u); /* controller last */
  mb_u8(&b, 3u);    /* volume */
  mb_u8(&b, 0xffu); /* vanilla-compatible clamp to 127 */
  mb_u8(&b, 1u);
  mb_u8(&b, 0x60u);
  bytes = mb_done(&b);
  CHECK(snd_mus_open(&song, b.data, bytes) == SND_MUS_OK,
        "high-bit controller value fixture opens");

  setup_midi(&midi, &log);
  snd_mus_player_init(&player, &song, 22050, 140, 10, 0);
  snd_mus_process_until(&player, &midi, 1000);
  CHECK(log.entry[1].msg.data1 == SND_MIDI_CC_VOLUME &&
            log.entry[1].msg.data2 == 127u,
        "MUS valued controller with bit 7 set clamps to 127");

  snd_mus_player_restart(&player, 1000);
  CHECK(player.next_frame == 1000 && player.tick_remainder == 0u &&
            player.loops_completed == 0uL && !player.finished,
        "explicit restart resets timeline fraction and loop count");
}

int main(void) {
  test_header();
  test_validation();
  test_all_truncations();
  test_channel_mapping_velocity_and_percussion();
  test_all_channels();
  test_timing_and_release();
  test_controllers();
  test_process_window_invariance();
  test_half_open_and_tick_rates();
  test_looping();
  test_value_clamp_and_restart();
  test_measure_and_unused_events();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
