#include "snd_midi.h"

#include <stdio.h>
#include <string.h>

typedef struct midi_log_entry {
  long frame;
  snd_midi_message_t msg;
  snd_midi_channel_t state;
} midi_log_entry_t;

typedef struct midi_log {
  midi_log_entry_t entry[64];
  int count;
} midi_log_t;

static int checks;
static int failures;

#define CHECK(cond, text)                                                       \
  do {                                                                          \
    ++checks;                                                                    \
    if (!(cond)) {                                                               \
      ++failures;                                                                \
      printf("FAIL: %s\n", (text));                                             \
    }                                                                            \
  } while (0)

static void log_emit(void *user, long frame, const snd_midi_message_t *msg,
                     const snd_midi_channel_t *channel) {
  midi_log_t *log = (midi_log_t *)user;
  midi_log_entry_t *e;

  if (!log || !msg || !channel || log->count >= 64)
    return;

  e = &log->entry[log->count++];
  e->frame = frame;
  e->msg = *msg;
  e->state = *channel;
}

static void clear_log(midi_log_t *log) {
  memset(log, 0, sizeof(*log));
}

static void test_defaults(void) {
  snd_midi_t m;

  printf("midi defaults\n");
  snd_midi_init(&m);

  CHECK(m.channel[0].program == 0u, "program starts at zero");
  CHECK(m.channel[0].volume == 127u, "volume starts neutral");
  CHECK(m.channel[0].expression == 127u, "expression starts neutral");
  CHECK(m.channel[0].pan == 64u, "pan starts centered");
  CHECK(m.channel[0].pitch_bend == SND_MIDI_PITCH_CENTER,
        "pitch bend starts centered");
  CHECK(m.running_status == 0u, "parser starts without running status");
}

static void test_complete_messages(void) {
  snd_midi_t m;
  midi_log_t log;

  printf("complete midi messages\n");
  clear_log(&log);
  snd_midi_init(&m);
  snd_midi_set_sink(&m, log_emit, &log);

  CHECK(snd_midi_message(&m, 100, 0xC2u, 41u, 0u) == 1,
        "program change accepted");
  CHECK(m.channel[2].program == 41u, "program state updated");

  CHECK(snd_midi_message(&m, 101, 0xB2u, SND_MIDI_CC_VOLUME, 93u) == 1,
        "volume controller accepted");
  CHECK(m.channel[2].volume == 93u, "volume state updated");

  CHECK(snd_midi_message(&m, 102, 0xB2u, SND_MIDI_CC_PAN, 17u) == 1,
        "pan controller accepted");
  CHECK(m.channel[2].pan == 17u, "pan state updated");

  CHECK(snd_midi_message(&m, 103, 0xE2u, 0u, 64u) == 1,
        "pitch bend accepted");
  CHECK(m.channel[2].pitch_bend == SND_MIDI_PITCH_CENTER,
        "pitch bend decodes 14-bit center");

  CHECK(log.count == 4, "all complete messages reached the sink");
  CHECK(log.entry[0].frame == 100, "absolute frame is preserved");
  CHECK(log.entry[0].msg.size == 2u, "program change is a two-byte message");
}

static void test_velocity_zero(void) {
  snd_midi_t m;
  midi_log_t log;

  printf("note-on velocity zero normalization\n");
  clear_log(&log);
  snd_midi_init(&m);
  snd_midi_set_sink(&m, log_emit, &log);

  snd_midi_message(&m, 12, 0x91u, 60u, 0u);
  CHECK(log.count == 1, "velocity-zero note reaches sink once");
  CHECK(log.entry[0].msg.status == 0x81u,
        "velocity-zero note-on becomes note-off");
  CHECK(log.entry[0].msg.data1 == 60u, "note number is preserved");
}

static void test_running_status(void) {
  static const uint8_t bytes[] = {0x90u, 60u, 100u, 62u, 80u, 64u, 0u};
  snd_midi_t m;
  midi_log_t log;
  int i;

  printf("running status\n");
  clear_log(&log);
  snd_midi_init(&m);
  snd_midi_set_sink(&m, log_emit, &log);

  for (i = 0; i < (int)sizeof(bytes); ++i)
    snd_midi_feed_byte(&m, 1000 + i, bytes[i]);

  CHECK(log.count == 3, "three running-status notes decoded");
  CHECK(log.entry[0].msg.status == 0x90u, "first note-on decoded");
  CHECK(log.entry[1].msg.data1 == 62u, "running-status second note decoded");
  CHECK(log.entry[2].msg.status == 0x80u,
        "running-status velocity zero normalized to note-off");
}

static void test_realtime_and_system(void) {
  snd_midi_t m;
  midi_log_t log;

  printf("real-time/system byte handling\n");
  clear_log(&log);
  snd_midi_init(&m);
  snd_midi_set_sink(&m, log_emit, &log);

  snd_midi_feed_byte(&m, 1, 0x90u);
  snd_midi_feed_byte(&m, 2, 60u);
  snd_midi_feed_byte(&m, 3, 0xF8u); /* clock: must not disturb pending note */
  snd_midi_feed_byte(&m, 4, 100u);
  CHECK(log.count == 1, "real-time byte may appear inside a message");

  snd_midi_feed_byte(&m, 5, 0xF0u);
  snd_midi_feed_byte(&m, 6, 0x7Du);
  snd_midi_feed_byte(&m, 7, 0x01u);
  snd_midi_feed_byte(&m, 8, 0xF7u);
  snd_midi_feed_byte(&m, 9, 62u);
  snd_midi_feed_byte(&m, 10, 90u);
  CHECK(log.count == 1, "SysEx cancels running status");

  snd_midi_feed_byte(&m, 11, 0x90u);
  snd_midi_feed_byte(&m, 12, 62u);
  snd_midi_feed_byte(&m, 13, 90u);
  CHECK(log.count == 2, "new status works after SysEx");
}

static void test_controller_reset(void) {
  snd_midi_t m;
  midi_log_t log;

  printf("controller state\n");
  clear_log(&log);
  snd_midi_init(&m);
  snd_midi_set_sink(&m, log_emit, &log);

  snd_midi_message(&m, 0, 0xB0u, SND_MIDI_CC_BANK_MSB, 2u);
  snd_midi_message(&m, 0, 0xB0u, SND_MIDI_CC_BANK_LSB, 3u);
  snd_midi_message(&m, 0, 0xC0u, 20u, 0u);
  snd_midi_message(&m, 0, 0xB0u, SND_MIDI_CC_EXPRESSION, 40u);
  snd_midi_message(&m, 0, 0xB0u, SND_MIDI_CC_PAN, 10u);
  snd_midi_message(&m, 0, 0xB0u, SND_MIDI_CC_SUSTAIN, 127u);
  snd_midi_message(&m, 0, 0xE0u, 0u, 127u);
  snd_midi_message(&m, 0, 0xB0u, SND_MIDI_CC_RESET_CONTROLLERS, 0u);

  CHECK(m.channel[0].program == 20u, "reset controllers preserves program");
  CHECK(m.channel[0].bank_msb == 2u && m.channel[0].bank_lsb == 3u,
        "reset controllers preserves bank");
  CHECK(m.channel[0].expression == 127u, "expression reset to neutral");
  CHECK(m.channel[0].pan == 64u, "pan reset to centre");
  CHECK(m.channel[0].sustain == 0u, "sustain released by controller reset");
  CHECK(m.channel[0].pitch_bend == SND_MIDI_PITCH_CENTER,
        "pitch bend reset to centre");
}

int main(void) {
  test_defaults();
  test_complete_messages();
  test_velocity_zero();
  test_running_status();
  test_realtime_and_system();
  test_controller_reset();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
