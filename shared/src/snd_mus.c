#include "snd_mus.h"

#include <limits.h>

#define MUS_HEADER_BYTES 16u
#define MUS_EVENT_RELEASE 0u
#define MUS_EVENT_PRESS 1u
#define MUS_EVENT_PITCH 2u
#define MUS_EVENT_SYSTEM 3u
#define MUS_EVENT_CONTROLLER 4u
#define MUS_EVENT_MEASURE 5u
#define MUS_EVENT_END 6u
#define MUS_EVENT_UNUSED 7u

/* MUS controller numbers 1..9.  Zero is Program Change rather than CC. */
static const uint8_t snd_mus_controller_map[10] = {
    0u, 32u, 1u, 7u, 10u, 11u, 91u, 93u, 64u, 67u};

/* MUS system events 10..14. */
static const uint8_t snd_mus_system_map[5] = {120u, 123u, 126u, 127u, 121u};

typedef struct snd_mus_cursor {
  const uint8_t SND_PTR *data;
  uint32_t pos;
  uint32_t end;
} snd_mus_cursor_t;

typedef struct snd_mus_event {
  uint8_t type;
  uint8_t channel;
  uint8_t last;
  uint8_t a;
  uint8_t b;
  uint8_t has_b;
} snd_mus_event_t;

static uint16_t snd_mus_le16(const uint8_t SND_PTR *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int snd_mus_read_u8(snd_mus_cursor_t SND_PTR *c,
                           uint8_t SND_PTR *out) {
  if (!c || !out || c->pos >= c->end)
    return 0;
  *out = c->data[c->pos++];
  return 1;
}

/* MUS delays are base-128, most-significant group first.  Bound the encoding
 * to five bytes and the value to uint32 so corrupted input can never spin or
 * wrap the song clock. */
static int snd_mus_read_delay(snd_mus_cursor_t SND_PTR *c,
                              uint32_t SND_PTR *out) {
  uint32_t value = 0u;
  int i;

  if (!c || !out)
    return SND_MUS_ERR_ARGUMENT;

  for (i = 0; i < 5; ++i) {
    uint8_t b;
    uint32_t low;
    if (!snd_mus_read_u8(c, &b))
      return SND_MUS_ERR_TRUNCATED;
    low = (uint32_t)(b & 0x7fu);
    if (value > (UINT32_MAX - low) / 128u)
      return SND_MUS_ERR_DELAY;
    value = value * 128u + low;
    if ((b & 0x80u) == 0u) {
      *out = value;
      return SND_MUS_OK;
    }
  }

  return SND_MUS_ERR_DELAY;
}

/* Decode one MUS event and consume only its event payload, not the group delay
 * that may follow when `last` is set. */
static int snd_mus_read_event(snd_mus_cursor_t SND_PTR *c,
                              snd_mus_event_t SND_PTR *ev) {
  uint8_t desc;
  uint8_t v;

  if (!c || !ev)
    return SND_MUS_ERR_ARGUMENT;
  if (!snd_mus_read_u8(c, &desc))
    return SND_MUS_ERR_TRUNCATED;

  ev->type = (uint8_t)((desc >> 4) & 7u);
  ev->channel = (uint8_t)(desc & 0x0fu);
  ev->last = (uint8_t)((desc & 0x80u) != 0u);
  ev->a = 0u;
  ev->b = 0u;
  ev->has_b = 0u;

  switch (ev->type) {
  case MUS_EVENT_RELEASE:
    if (!snd_mus_read_u8(c, &ev->a))
      return SND_MUS_ERR_TRUNCATED;
    ev->a &= 0x7fu;
    break;

  case MUS_EVENT_PRESS:
    if (!snd_mus_read_u8(c, &v))
      return SND_MUS_ERR_TRUNCATED;
    ev->a = (uint8_t)(v & 0x7fu);
    if (v & 0x80u) {
      if (!snd_mus_read_u8(c, &ev->b))
        return SND_MUS_ERR_TRUNCATED;
      ev->b &= 0x7fu;
      ev->has_b = 1u;
    }
    break;

  case MUS_EVENT_PITCH:
    if (!snd_mus_read_u8(c, &ev->a))
      return SND_MUS_ERR_TRUNCATED;
    break;

  case MUS_EVENT_SYSTEM:
    if (!snd_mus_read_u8(c, &ev->a))
      return SND_MUS_ERR_TRUNCATED;
    if (ev->a < 10u || ev->a > 14u)
      return SND_MUS_ERR_CONTROLLER;
    break;

  case MUS_EVENT_CONTROLLER:
    if (!snd_mus_read_u8(c, &ev->a) || !snd_mus_read_u8(c, &ev->b))
      return SND_MUS_ERR_TRUNCATED;
    if (ev->a > 9u)
      return SND_MUS_ERR_CONTROLLER;
    /* Vanilla-compatible quirk: values with bit 7 set become 127 rather than
     * wrapping through the 7-bit MIDI mask. */
    if (ev->b & 0x80u)
      ev->b = 127u;
    break;

  case MUS_EVENT_MEASURE:
    /* DMX documents this as a measure boundary.  It has no playback effect. */
    break;

  case MUS_EVENT_END:
    break;

  case MUS_EVENT_UNUSED:
    /* DMX left event type 7 unused but consumed one byte for forward
     * compatibility.  Skipping it makes the reader tolerant of such files. */
    if (!snd_mus_read_u8(c, &ev->a))
      return SND_MUS_ERR_TRUNCATED;
    break;

  default:
    return SND_MUS_ERR_EVENT;
  }

  return SND_MUS_OK;
}

static int snd_mus_validate_score(const snd_mus_song_t SND_PTR *song) {
  snd_mus_cursor_t c;
  int guard = 0;

  if (!song || !song->data)
    return SND_MUS_ERR_ARGUMENT;

  c.data = song->data;
  c.pos = song->score_offset;
  c.end = song->score_offset + song->score_length;

  while (c.pos < c.end) {
    snd_mus_event_t ev;
    int err = snd_mus_read_event(&c, &ev);
    if (err != SND_MUS_OK)
      return err;
    if (++guard > 65535)
      return SND_MUS_ERR_EVENT;

    if (ev.type == MUS_EVENT_END)
      return SND_MUS_OK;

    if (ev.last) {
      uint32_t delay;
      err = snd_mus_read_delay(&c, &delay);
      if (err != SND_MUS_OK)
        return err;
      (void)delay;
    }
  }

  return SND_MUS_ERR_NO_END;
}

int snd_mus_open(snd_mus_song_t SND_PTR *song, const void SND_PTR *data,
                 uint32_t bytes) {
  const uint8_t SND_PTR *p = (const uint8_t SND_PTR *)data;
  uint32_t instrument_end;
  int err;

  if (!song)
    return SND_MUS_ERR_ARGUMENT;

  song->data = p;
  song->bytes = bytes;
  song->score_offset = 0u;
  song->score_length = 0u;
  song->primary_channels = 0u;
  song->secondary_channels = 0u;
  song->instrument_count = 0u;
  song->valid = 0u;

  if (!p)
    return SND_MUS_ERR_ARGUMENT;
  if (bytes < MUS_HEADER_BYTES)
    return SND_MUS_ERR_HEADER;
  if (p[0] != 'M' || p[1] != 'U' || p[2] != 'S' || p[3] != 0x1au)
    return SND_MUS_ERR_MAGIC;

  song->score_length = (uint32_t)snd_mus_le16(p + 4);
  song->score_offset = (uint32_t)snd_mus_le16(p + 6);
  song->primary_channels = snd_mus_le16(p + 8);
  song->secondary_channels = snd_mus_le16(p + 10);
  song->instrument_count = snd_mus_le16(p + 12);

  /* 16-byte header includes the reserved word at bytes 14..15. */
  instrument_end = MUS_HEADER_BYTES + (uint32_t)song->instrument_count * 2u;
  if (instrument_end > song->score_offset || instrument_end > bytes)
    return SND_MUS_ERR_RANGE;
  if (song->score_offset > bytes ||
      song->score_length > bytes - song->score_offset)
    return SND_MUS_ERR_RANGE;
  if (song->score_length == 0u)
    return SND_MUS_ERR_NO_END;
  if (song->primary_channels > 15u || song->secondary_channels > 5u)
    return SND_MUS_ERR_CHANNELS;

  err = snd_mus_validate_score(song);
  if (err != SND_MUS_OK)
    return err;

  song->valid = 1u;
  return SND_MUS_OK;
}

uint16_t snd_mus_instrument(const snd_mus_song_t SND_PTR *song, int index) {
  uint32_t off;
  if (!song || !song->valid || !song->data || index < 0 ||
      index >= (int)song->instrument_count)
    return 0xffffu;
  off = MUS_HEADER_BYTES + (uint32_t)index * 2u;
  return snd_mus_le16(song->data + off);
}

const char *snd_mus_error_string(int error) {
  switch (error) {
  case SND_MUS_OK:
    return "ok";
  case SND_MUS_ERR_ARGUMENT:
    return "invalid argument";
  case SND_MUS_ERR_HEADER:
    return "truncated MUS header";
  case SND_MUS_ERR_MAGIC:
    return "invalid MUS signature";
  case SND_MUS_ERR_RANGE:
    return "MUS header range is outside the buffer";
  case SND_MUS_ERR_TRUNCATED:
    return "truncated MUS event";
  case SND_MUS_ERR_EVENT:
    return "invalid MUS event";
  case SND_MUS_ERR_CONTROLLER:
    return "invalid MUS controller";
  case SND_MUS_ERR_DELAY:
    return "invalid or overflowing MUS delay";
  case SND_MUS_ERR_NO_END:
    return "MUS score has no end event";
  case SND_MUS_ERR_CHANNELS:
    return "invalid MUS channel counts";
  case SND_MUS_ERR_TIME:
    return "MUS timeline exceeds output frame range";
  case SND_MUS_ERR_ZERO_LOOP:
    return "zero-duration MUS score cannot loop";
  default:
    return "unknown MUS error";
  }
}

static void snd_mus_player_state_reset(snd_mus_player_t SND_PTR *p,
                                       long start_frame,
                                       int reset_fraction) {
  int i;
  if (!p || !p->song)
    return;

  p->cursor = p->song->score_offset;
  p->score_end = p->song->score_offset + p->song->score_length;
  p->next_frame = start_frame;
  p->start_frame = start_frame;
  p->ticks_in_loop = 0u;
  p->finished = 0;
  p->error = SND_MUS_OK;
  p->next_midi_channel = 0;
  if (reset_fraction)
    p->tick_remainder = 0u;

  for (i = 0; i < SND_MUS_CHANNELS; ++i) {
    p->channel_map[i] = -1;
    p->velocity[i] = 127u;
  }
}

int snd_mus_player_init(snd_mus_player_t SND_PTR *player,
                        const snd_mus_song_t SND_PTR *song, int output_rate,
                        int tick_hz, long start_frame, int loop) {
  if (!player)
    return 0;

  player->song = song;
  player->output_rate = output_rate;
  player->tick_hz = tick_hz;
  player->loop = loop ? 1 : 0;
  player->loops_completed = 0uL;
  player->tick_remainder = 0u;

  if (!song || !song->valid || !song->data || output_rate <= 0 || tick_hz <= 0) {
    player->finished = 1;
    player->error = SND_MUS_ERR_ARGUMENT;
    return 0;
  }

  snd_mus_player_state_reset(player, start_frame, 1);
  return 1;
}

void snd_mus_player_restart(snd_mus_player_t SND_PTR *player,
                            long start_frame) {
  if (!player || !player->song || !player->song->valid)
    return;
  player->loops_completed = 0uL;
  snd_mus_player_state_reset(player, start_frame, 1);
}

void snd_mus_player_stop(snd_mus_player_t SND_PTR *player) {
  if (!player)
    return;
  player->finished = 1;
}

int snd_mus_player_finished(const snd_mus_player_t SND_PTR *player) {
  return (!player || player->finished) ? 1 : 0;
}

int snd_mus_player_error(const snd_mus_player_t SND_PTR *player) {
  return player ? player->error : SND_MUS_ERR_ARGUMENT;
}

/* Internal loop restart keeps tick_remainder: if a song is not an integer
 * number of output frames long, carrying the fraction makes repeated loops
 * converge on the exact 140-Hz timeline instead of drifting by a fraction of
 * a frame every pass. */
static void snd_mus_loop_restart(snd_mus_player_t SND_PTR *p) {
  int i;
  p->cursor = p->song->score_offset;
  p->ticks_in_loop = 0u;
  p->next_midi_channel = 0;
  for (i = 0; i < SND_MUS_CHANNELS; ++i) {
    p->channel_map[i] = -1;
    p->velocity[i] = 127u;
  }
}

static int snd_mus_add_delay(snd_mus_player_t SND_PTR *p, uint32_t ticks) {
  uint64_t numer;
  uint64_t delta;

  if (!p || p->output_rate <= 0 || p->tick_hz <= 0)
    return SND_MUS_ERR_ARGUMENT;

  numer = (uint64_t)ticks * (uint64_t)(unsigned int)p->output_rate +
          (uint64_t)p->tick_remainder;
  delta = numer / (uint64_t)(unsigned int)p->tick_hz;
  p->tick_remainder =
      (uint32_t)(numer % (uint64_t)(unsigned int)p->tick_hz);

  if (delta > (uint64_t)LONG_MAX)
    return SND_MUS_ERR_TIME;
  if (p->next_frame > LONG_MAX - (long)delta)
    return SND_MUS_ERR_TIME;
  p->next_frame += (long)delta;

  if (UINT32_MAX - p->ticks_in_loop < ticks)
    return SND_MUS_ERR_TIME;
  p->ticks_in_loop += ticks;
  return SND_MUS_OK;
}

static int snd_mus_get_midi_channel(snd_mus_player_t SND_PTR *p,
                                    snd_midi_t SND_PTR *midi,
                                    uint8_t mus_channel, long frame,
                                    int SND_PTR *emitted) {
  int ch;

  if (mus_channel == SND_MUS_PERCUSSION_CHANNEL)
    return (int)SND_MIDI_GM_PERCUSSION_CHANNEL;

  ch = p->channel_map[mus_channel];
  if (ch >= 0)
    return ch;

  ch = p->next_midi_channel;
  if (ch == (int)SND_MIDI_GM_PERCUSSION_CHANNEL)
    ++ch;
  if (ch >= SND_MIDI_CHANNELS) {
    p->error = SND_MUS_ERR_CHANNELS;
    p->finished = 1;
    return -1;
  }

  p->channel_map[mus_channel] = (int16_t)ch;
  p->next_midi_channel = (int16_t)(ch + 1);

  /* Classic mus2mid emits All Notes Off the first time each non-percussion
   * channel is allocated.  Besides preserving that behaviour, it prevents a
   * note left by the previous loop from bleeding into the next one. */
  if (snd_midi_message(midi, frame,
                       (uint8_t)(SND_MIDI_CONTROL_CHANGE | (uint8_t)ch),
                       SND_MIDI_CC_ALL_NOTES_OFF, 0u)) {
    if (emitted)
      ++*emitted;
  }

  return ch;
}

static int snd_mus_emit_event(snd_mus_player_t SND_PTR *p,
                              snd_midi_t SND_PTR *midi,
                              const snd_mus_event_t SND_PTR *ev, long frame,
                              int SND_PTR *emitted) {
  int ch;
  uint16_t wheel;
  uint8_t status;
  uint8_t cc;

  if (ev->type == MUS_EVENT_MEASURE || ev->type == MUS_EVENT_UNUSED)
    return SND_MUS_OK;
  if (ev->type == MUS_EVENT_END)
    return SND_MUS_OK;

  ch = snd_mus_get_midi_channel(p, midi, ev->channel, frame, emitted);
  if (ch < 0)
    return p->error;

  switch (ev->type) {
  case MUS_EVENT_RELEASE:
    status = (uint8_t)(SND_MIDI_NOTE_OFF | (uint8_t)ch);
    if (snd_midi_message(midi, frame, status, ev->a, 0u) && emitted)
      ++*emitted;
    break;

  case MUS_EVENT_PRESS:
    if (ev->has_b)
      p->velocity[ev->channel] = ev->b;
    status = (uint8_t)(SND_MIDI_NOTE_ON | (uint8_t)ch);
    if (snd_midi_message(midi, frame, status, ev->a,
                         p->velocity[ev->channel]) && emitted)
      ++*emitted;
    break;

  case MUS_EVENT_PITCH:
    wheel = (uint16_t)((uint16_t)ev->a * 64u);
    status = (uint8_t)(SND_MIDI_PITCH_BEND | (uint8_t)ch);
    if (snd_midi_message(midi, frame, status, (uint8_t)(wheel & 0x7fu),
                         (uint8_t)((wheel >> 7) & 0x7fu)) && emitted)
      ++*emitted;
    break;

  case MUS_EVENT_SYSTEM:
    cc = snd_mus_system_map[ev->a - 10u];
    status = (uint8_t)(SND_MIDI_CONTROL_CHANGE | (uint8_t)ch);
    if (snd_midi_message(midi, frame, status, cc, 0u) && emitted)
      ++*emitted;
    break;

  case MUS_EVENT_CONTROLLER:
    if (ev->a == 0u) {
      status = (uint8_t)(SND_MIDI_PROGRAM_CHANGE | (uint8_t)ch);
      if (snd_midi_message(midi, frame, status, (uint8_t)(ev->b & 0x7fu),
                           0u) && emitted)
        ++*emitted;
    } else {
      cc = snd_mus_controller_map[ev->a];
      status = (uint8_t)(SND_MIDI_CONTROL_CHANGE | (uint8_t)ch);
      if (snd_midi_message(midi, frame, status, cc, ev->b) && emitted)
        ++*emitted;
    }
    break;

  default:
    return SND_MUS_ERR_EVENT;
  }

  return SND_MUS_OK;
}

int snd_mus_process_until(snd_mus_player_t SND_PTR *p,
                          snd_midi_t SND_PTR *midi, long end_frame) {
  int emitted = 0;
  int group_guard = 0;

  if (!p || !midi || !p->song || !p->song->valid)
    return -1;
  if (p->error != SND_MUS_OK)
    return -1;
  if (p->finished)
    return 0;

  while (!p->finished && p->next_frame < end_frame) {
    snd_mus_cursor_t c;
    int last = 0;

    if (++group_guard > 65535) {
      p->error = SND_MUS_ERR_EVENT;
      p->finished = 1;
      return -1;
    }

    c.data = p->song->data;
    c.pos = p->cursor;
    c.end = p->score_end;

    while (!last) {
      snd_mus_event_t ev;
      int err = snd_mus_read_event(&c, &ev);
      if (err != SND_MUS_OK) {
        p->error = err;
        p->finished = 1;
        return -1;
      }

      if (ev.type == MUS_EVENT_END) {
        p->cursor = c.pos;
        ++p->loops_completed;
        if (!p->loop) {
          p->finished = 1;
          return emitted;
        }
        if (p->ticks_in_loop == 0u) {
          p->error = SND_MUS_ERR_ZERO_LOOP;
          p->finished = 1;
          return -1;
        }
        snd_mus_loop_restart(p);
        /* The first group of the next loop occurs at this same absolute frame.
         * Re-enter the outer loop so its half-open end_frame check still
         * applies consistently. */
        last = 1;
        break;
      }

      err = snd_mus_emit_event(p, midi, &ev, p->next_frame, &emitted);
      if (err != SND_MUS_OK) {
        p->error = err;
        p->finished = 1;
        return -1;
      }

      last = ev.last ? 1 : 0;
      p->cursor = c.pos;
    }

    if (p->finished)
      break;

    /* A loop restart sets cursor to score start and ticks_in_loop to zero. */
    if (p->cursor == p->song->score_offset && p->ticks_in_loop == 0u)
      continue;

    if (last) {
      uint32_t delay;
      int err;
      c.pos = p->cursor;
      err = snd_mus_read_delay(&c, &delay);
      if (err != SND_MUS_OK) {
        p->error = err;
        p->finished = 1;
        return -1;
      }
      p->cursor = c.pos;
      err = snd_mus_add_delay(p, delay);
      if (err != SND_MUS_OK) {
        p->error = err;
        p->finished = 1;
        return -1;
      }
    }
  }

  return emitted;
}
