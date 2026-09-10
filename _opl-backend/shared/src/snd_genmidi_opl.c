#include "snd_genmidi_opl.h"

#include <limits.h>
#include <string.h>

#define GM_FLAG_OFFSET 0u
#define GM_FINE_OFFSET 2u
#define GM_FIXED_OFFSET 3u
#define GM_VOICE0_OFFSET 4u
#define GM_VOICE1_OFFSET 20u
#define GM_MOD_OFFSET 0u
#define GM_FEEDBACK_OFFSET 6u
#define GM_CARRIER_OFFSET 7u
#define GM_BASE_NOTE_OFFSET 14u

/* YM3812 operator register offsets for channels 0..8. */
static const uint8_t gm_mod_operator[SND_GENMIDI_OPL_VOICES] = {
    0x00u, 0x01u, 0x02u, 0x08u, 0x09u, 0x0au, 0x10u, 0x11u, 0x12u
};

static const uint8_t gm_car_operator[SND_GENMIDI_OPL_VOICES] = {
    0x03u, 0x04u, 0x05u, 0x0bu, 0x0cu, 0x0du, 0x13u, 0x14u, 0x15u
};

/* DMX/Chocolate Doom MIDI-volume mapping. The register-native path uses
 * this before converting the carrier's GENMIDI output level into OPL total
 * level. It is intentionally not a generic PCM/logarithmic gain curve. */
static const uint8_t gm_volume_mapping[128] = {
      0,   1,   3,   5,   6,   8,  10,  11,
     13,  14,  16,  17,  19,  20,  22,  23,
     25,  26,  27,  29,  30,  32,  33,  34,
     36,  37,  39,  41,  43,  45,  47,  49,
     50,  52,  54,  55,  57,  59,  60,  61,
     63,  64,  66,  67,  68,  69,  71,  72,
     73,  74,  75,  76,  77,  79,  80,  81,
     82,  83,  84,  84,  85,  86,  87,  88,
     89,  90,  91,  92,  92,  93,  94,  95,
     96,  96,  97,  98,  99,  99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127
};

/* One octave of OPL f-numbers at 1/32-semitone resolution, normalized to
 * block 4. Generated from the YM3812 frequency relationship using the
 * 49.716 kHz native output clock. */
static const uint16_t gm_fnum_32[12 * 32] = {
    345, 345, 346, 347, 347, 348, 349, 349, 350, 351, 351, 352,
    352, 353, 354, 354, 355, 356, 356, 357, 358, 358, 359, 359,
    360, 361, 361, 362, 363, 363, 364, 365, 365, 366, 367, 367,
    368, 369, 369, 370, 371, 371, 372, 373, 373, 374, 375, 375,
    376, 377, 377, 378, 379, 380, 380, 381, 382, 382, 383, 384,
    384, 385, 386, 386, 387, 388, 389, 389, 390, 391, 391, 392,
    393, 393, 394, 395, 396, 396, 397, 398, 398, 399, 400, 401,
    401, 402, 403, 404, 404, 405, 406, 406, 407, 408, 409, 409,
    410, 411, 412, 412, 413, 414, 415, 415, 416, 417, 418, 418,
    419, 420, 421, 421, 422, 423, 424, 424, 425, 426, 427, 428,
    428, 429, 430, 431, 431, 432, 433, 434, 435, 435, 436, 437,
    438, 438, 439, 440, 441, 442, 442, 443, 444, 445, 446, 446,
    447, 448, 449, 450, 450, 451, 452, 453, 454, 455, 455, 456,
    457, 458, 459, 460, 460, 461, 462, 463, 464, 465, 465, 466,
    467, 468, 469, 470, 470, 471, 472, 473, 474, 475, 476, 476,
    477, 478, 479, 480, 481, 482, 482, 483, 484, 485, 486, 487,
    488, 489, 489, 490, 491, 492, 493, 494, 495, 496, 497, 498,
    498, 499, 500, 501, 502, 503, 504, 505, 506, 507, 507, 508,
    509, 510, 511, 512, 513, 514, 515, 516, 517, 518, 519, 520,
    520, 521, 522, 523, 524, 525, 526, 527, 528, 529, 530, 531,
    532, 533, 534, 535, 536, 537, 538, 539, 540, 541, 542, 543,
    544, 545, 545, 546, 547, 548, 549, 550, 551, 552, 553, 554,
    555, 556, 557, 558, 559, 560, 561, 562, 563, 565, 566, 567,
    568, 569, 570, 571, 572, 573, 574, 575, 576, 577, 578, 579,
    580, 581, 582, 583, 584, 585, 586, 587, 588, 590, 591, 592,
    593, 594, 595, 596, 597, 598, 599, 600, 601, 602, 604, 605,
    606, 607, 608, 609, 610, 611, 612, 613, 615, 616, 617, 618,
    619, 620, 621, 622, 623, 625, 626, 627, 628, 629, 630, 631,
    633, 634, 635, 636, 637, 638, 639, 641, 642, 643, 644, 645,
    646, 648, 649, 650, 651, 652, 653, 655, 656, 657, 658, 659,
    661, 662, 663, 664, 665, 666, 668, 669, 670, 671, 673, 674,
    675, 676, 677, 679, 680, 681, 682, 684, 685, 686, 687, 689
};

static uint16_t gm_rd16(const uint8_t SND_PTR *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t gm_rds16(const uint8_t SND_PTR *p) {
  return (int16_t)gm_rd16(p);
}

static void gm_decode_operator(
    snd_genmidi_opl_operator_t SND_PTR *out,
    const uint8_t SND_PTR *raw) {
  out->tremolo = raw[0];
  out->attack = raw[1];
  out->sustain = raw[2];
  out->waveform = raw[3];
  out->scale = raw[4];
  out->level = raw[5];
}

static void gm_decode_voice(
    snd_genmidi_opl_patch_voice_t SND_PTR *out,
    const uint8_t SND_PTR *raw) {
  gm_decode_operator(&out->modulator, raw + GM_MOD_OFFSET);
  out->feedback = raw[GM_FEEDBACK_OFFSET];
  gm_decode_operator(&out->carrier, raw + GM_CARRIER_OFFSET);
  out->base_note_offset = gm_rds16(raw + GM_BASE_NOTE_OFFSET);
}

static void gm_decode_instrument(
    snd_genmidi_opl_instrument_t SND_PTR *out,
    const uint8_t SND_PTR *raw) {
  out->flags = gm_rd16(raw + GM_FLAG_OFFSET);
  out->fine_tuning = raw[GM_FINE_OFFSET];
  out->fixed_note = raw[GM_FIXED_OFFSET];
  gm_decode_voice(&out->voice[0], raw + GM_VOICE0_OFFSET);
  gm_decode_voice(&out->voice[1], raw + GM_VOICE1_OFFSET);
}

int snd_genmidi_opl_load_bank(snd_genmidi_opl_bank_t SND_PTR *out,
                              const void SND_PTR *data, uint32_t bytes) {
  static const uint8_t magic[SND_GENMIDI_OPL_HEADER_BYTES] = {
      '#', 'O', 'P', 'L', '_', 'I', 'I', '#'
  };
  const uint8_t SND_PTR *src;
  uint32_t off;
  int i;

  if (!out || !data)
    return SND_GENMIDI_OPL_ERR_ARGUMENT;

  memset(out, 0, sizeof(*out));
  src = (const uint8_t SND_PTR *)data;

  if (bytes < SND_GENMIDI_OPL_HEADER_BYTES)
    return SND_GENMIDI_OPL_ERR_TRUNCATED;
  if (memcmp(src, magic, sizeof(magic)) != 0)
    return SND_GENMIDI_OPL_ERR_HEADER;
  if (bytes < SND_GENMIDI_OPL_MIN_BYTES)
    return SND_GENMIDI_OPL_ERR_TRUNCATED;

  off = SND_GENMIDI_OPL_HEADER_BYTES;
  for (i = 0; i < SND_GENMIDI_OPL_INSTRUMENT_COUNT; ++i) {
    gm_decode_instrument(&out->instrument[i], src + off);
    off += SND_GENMIDI_OPL_RAW_INSTRUMENT_BYTES;
  }
  out->valid = 1u;
  return SND_GENMIDI_OPL_OK;
}

const char *snd_genmidi_opl_error_string(int error) {
  switch (error) {
  case SND_GENMIDI_OPL_OK:
    return "ok";
  case SND_GENMIDI_OPL_ERR_ARGUMENT:
    return "invalid argument";
  case SND_GENMIDI_OPL_ERR_HEADER:
    return "invalid GENMIDI header";
  case SND_GENMIDI_OPL_ERR_TRUNCATED:
    return "truncated GENMIDI bank";
  default:
    return "unknown GENMIDI OPL error";
  }
}

static void gm_channel_default(snd_genmidi_opl_channel_t SND_PTR *c) {
  c->pitch_bend = (uint16_t)SND_MIDI_PITCH_CENTER;
  c->program = 0u;
  c->volume = 127u;
  c->expression = 127u;
  c->pan = 64u;
  c->sustain = 0u;
}

static void gm_write(snd_genmidi_opl_t SND_PTR *opl,
                     uint16_t reg, uint8_t value) {
  /* The buffered API models the short hardware register-write delay and is
   * the path used by mature Nuked OPL integrations. */
  OPL3_WriteRegBuffered(&opl->chip, reg, value);
}

static void gm_chip_reset(snd_genmidi_opl_t SND_PTR *opl) {
  int i;
  OPL3_Reset(&opl->chip, (uint32_t)opl->output_rate);

  /* OPL2 compatibility mode. Enable waveform select and disable rhythm. */
  gm_write(opl, 0x105u, 0x00u);
  gm_write(opl, 0x01u, 0x20u);
  gm_write(opl, 0xbdu, 0x00u);

  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i) {
    gm_write(opl, (uint16_t)(0xb0u + (uint16_t)i), 0x00u);
    gm_write(opl, (uint16_t)(0xc0u + (uint16_t)i), 0x00u);
  }
}

static void gm_clear_runtime(snd_genmidi_opl_t SND_PTR *opl) {
  int i;
  for (i = 0; i < SND_MIDI_CHANNELS; ++i)
    gm_channel_default(&opl->channel[i]);
  memset(opl->voice, 0, sizeof(opl->voice));
  opl->event_head = 0;
  opl->event_count = 0;
  opl->next_serial = 0u;
  opl->midi_notes_started = 0uL;
  opl->opl_voices_started = 0uL;
  opl->opl_voices_released = 0uL;
  opl->voices_stolen = 0uL;
  opl->dropped_events = 0uL;
}

void snd_genmidi_opl_init(snd_genmidi_opl_t SND_PTR *opl,
                          int output_rate) {
  if (!opl)
    return;
  memset(opl, 0, sizeof(*opl));
  if (output_rate < 8000)
    output_rate = 8000;
  opl->output_rate = output_rate;
  opl->output_gain = 256;
  gm_clear_runtime(opl);
  gm_chip_reset(opl);
}

void snd_genmidi_opl_reset(snd_genmidi_opl_t SND_PTR *opl) {
  const snd_genmidi_opl_bank_t SND_PTR *bank;
  int rate;
  int16_t gain;
  if (!opl)
    return;
  bank = opl->bank;
  rate = opl->output_rate;
  gain = opl->output_gain;
  gm_clear_runtime(opl);
  opl->bank = bank;
  opl->output_rate = rate;
  opl->output_gain = gain;
  gm_chip_reset(opl);
}

void snd_genmidi_opl_set_bank(
    snd_genmidi_opl_t SND_PTR *opl,
    const snd_genmidi_opl_bank_t SND_PTR *bank) {
  if (opl)
    opl->bank = (bank && bank->valid) ? bank : 0;
}

void snd_genmidi_opl_set_output_gain(snd_genmidi_opl_t SND_PTR *opl,
                                     int16_t gain_8_8) {
  if (!opl)
    return;
  if (gain_8_8 < 0)
    gain_8_8 = 0;
  if (gain_8_8 > 256)
    gain_8_8 = 256;
  opl->output_gain = gain_8_8;
}

void snd_genmidi_opl_bind(snd_genmidi_opl_t SND_PTR *opl,
                          snd_midi_t SND_PTR *midi) {
  int i;
  if (!opl || !midi)
    return;

  for (i = 0; i < SND_MIDI_CHANNELS; ++i) {
    opl->channel[i].pitch_bend = midi->channel[i].pitch_bend;
    opl->channel[i].program = midi->channel[i].program;
    opl->channel[i].volume = midi->channel[i].volume;
    opl->channel[i].expression = midi->channel[i].expression;
    opl->channel[i].pan = midi->channel[i].pan;
    opl->channel[i].sustain = midi->channel[i].sustain;
  }
  snd_midi_set_sink(midi, snd_genmidi_opl_midi_emit, opl);
}

static int gm_event_index(const snd_genmidi_opl_t SND_PTR *opl, int n) {
  return (opl->event_head + n) % SND_GENMIDI_OPL_QUEUE_EVENTS;
}

int snd_genmidi_opl_queue_message(
    snd_genmidi_opl_t SND_PTR *opl, long frame,
    const snd_midi_message_t SND_PTR *msg) {
  int idx;
  if (!opl || !msg)
    return 0;

  if (opl->event_count >= SND_GENMIDI_OPL_QUEUE_EVENTS) {
    ++opl->dropped_events;
    return 0;
  }
  if (opl->event_count > 0) {
    int last = gm_event_index(opl, opl->event_count - 1);
    if (frame < opl->events[last].frame) {
      ++opl->dropped_events;
      return 0;
    }
  }

  idx = gm_event_index(opl, opl->event_count);
  opl->events[idx].frame = frame;
  opl->events[idx].msg = *msg;
  ++opl->event_count;
  return 1;
}

void snd_genmidi_opl_midi_emit(
    void SND_PTR *user, long frame,
    const snd_midi_message_t SND_PTR *msg,
    const snd_midi_channel_t SND_PTR *channel) {
  snd_genmidi_opl_t SND_PTR *opl =
      (snd_genmidi_opl_t SND_PTR *)user;
  (void)channel;
  if (opl && msg)
    (void)snd_genmidi_opl_queue_message(opl, frame, msg);
}

static snd_genmidi_opl_event_t SND_PTR *gm_front_event(
    snd_genmidi_opl_t SND_PTR *opl) {
  if (!opl || opl->event_count <= 0)
    return 0;
  return &opl->events[opl->event_head];
}

static void gm_pop_event(snd_genmidi_opl_t SND_PTR *opl) {
  if (!opl || opl->event_count <= 0)
    return;
  opl->event_head =
      (opl->event_head + 1) % SND_GENMIDI_OPL_QUEUE_EVENTS;
  --opl->event_count;
  if (opl->event_count == 0)
    opl->event_head = 0;
}

static const snd_genmidi_opl_instrument_t SND_PTR *
gm_instrument_for(const snd_genmidi_opl_t SND_PTR *opl,
                  int channel, int key) {
  int index;
  if (!opl || !opl->bank || !opl->bank->valid)
    return 0;

  if (channel == (int)SND_MIDI_GM_PERCUSSION_CHANNEL) {
    index = key - SND_GENMIDI_OPL_PERCUSSION_FIRST_NOTE;
    if (index < 0 || index >= SND_GENMIDI_OPL_PERCUSSION_COUNT)
      return 0;
    return &opl->bank->instrument[
        SND_GENMIDI_OPL_PROGRAM_COUNT + index];
  }

  index = (int)opl->channel[channel].program;
  if (index < 0 || index >= SND_GENMIDI_OPL_PROGRAM_COUNT)
    index = 0;
  return &opl->bank->instrument[index];
}

static uint8_t gm_dmx_full_volume(
    const snd_genmidi_opl_t SND_PTR *opl,
    const snd_genmidi_opl_voice_t SND_PTR *voice) {
  uint32_t volume;
  const snd_genmidi_opl_channel_t SND_PTR *channel =
      &opl->channel[voice->midi_channel];

  /* Chocolate Doom/DMX:
   *
   *   map[note] * map[channel] * map[music] / (127 * 127)
   *
   * MicroWave has no separate OPL music-volume state here, so CC11 expression
   * occupies that third 0..127 multiplier. Its default is 127, making ordinary
   * Doom MUS playback identical to the classic three-term expression with
   * music volume at maximum. */
  volume =
      (uint32_t)gm_volume_mapping[voice->velocity] *
      (uint32_t)gm_volume_mapping[channel->volume] *
      (uint32_t)gm_volume_mapping[channel->expression];
  volume /= (127u * 127u);
  if (volume > 127u)
    volume = 127u;
  return (uint8_t)volume;
}

static uint8_t gm_dmx_register_volume(
    const snd_genmidi_opl_patch_voice_t SND_PTR *patch,
    uint8_t full_volume) {
  uint32_t operator_volume;
  uint32_t register_volume;

  operator_volume = 0x3fu - (uint32_t)(patch->carrier.level & 0x3fu);
  register_volume =
      (operator_volume * (uint32_t)full_volume) / 128u;
  register_volume = 0x3fu - register_volume;

  return (uint8_t)((register_volume & 0x3fu) |
                   (patch->carrier.scale & 0xc0u));
}

static void gm_write_operator(
    snd_genmidi_opl_t SND_PTR *opl, uint8_t op,
    const snd_genmidi_opl_operator_t SND_PTR *src,
    int force_silent) {
  uint8_t level =
      (uint8_t)((src->scale & 0xc0u) | (src->level & 0x3fu));
  if (force_silent)
    level = (uint8_t)((src->scale & 0xc0u) | 0x3fu);

  /* Match classic OPL drivers' register grouping. */
  gm_write(opl, (uint16_t)(0x40u + op), level);
  gm_write(opl, (uint16_t)(0x20u + op), src->tremolo);
  gm_write(opl, (uint16_t)(0x60u + op), src->attack);
  gm_write(opl, (uint16_t)(0x80u + op), src->sustain);
  gm_write(opl, (uint16_t)(0xe0u + op),
           (uint8_t)(src->waveform & 0x03u));
}

static void gm_update_voice_volume(
    snd_genmidi_opl_t SND_PTR *opl, int index) {
  snd_genmidi_opl_voice_t SND_PTR *voice;
  const snd_genmidi_opl_patch_voice_t SND_PTR *patch;
  uint8_t full_volume;
  uint8_t register_volume;

  if (!opl || index < 0 || index >= SND_GENMIDI_OPL_VOICES)
    return;
  voice = &opl->voice[index];
  if (!voice->active || !voice->instrument)
    return;

  patch = &voice->instrument->voice[voice->layer];
  full_volume = gm_dmx_full_volume(opl, voice);
  register_volume = gm_dmx_register_volume(patch, full_volume);

  gm_write(opl, (uint16_t)(0x40u + gm_car_operator[index]),
           register_volume);

  /* In additive mode DMX writes the exact same calculated register byte to
   * both operators, including the carrier's KSL bits. */
  if ((patch->feedback & 0x01u) != 0u) {
    gm_write(opl, (uint16_t)(0x40u + gm_mod_operator[index]),
             register_volume);
  }
}

static int32_t gm_bend_32(uint16_t bend) {
  /* Vanilla Doom/DMX considers only the pitch-bend MSB. Its signed distance
   * from 64 is already in the driver's 1/32-semitone frequency-index domain. */
  return (int32_t)((int)(bend >> 7) - 64);
}

static uint16_t gm_frequency_for_pitch(int32_t pitch_32,
                                       uint8_t SND_PTR *b0_out) {
  int32_t note;
  int32_t frac;
  int32_t octave;
  int32_t semi;
  int index;
  uint16_t fnum;
  int block;

  if (pitch_32 < 0)
    pitch_32 = 0;
  if (pitch_32 > 127 * 32)
    pitch_32 = 127 * 32;

  note = pitch_32 / 32;
  frac = pitch_32 % 32;

  /* Keep the note in the practical OPL2 range while preserving pitch class,
   * matching the wrap-by-octaves behavior of classic Doom OPL drivers. */
  while (note < 12)
    note += 12;
  while (note > 95)
    note -= 12;

  octave = note / 12;
  semi = note % 12;
  block = (int)octave - 1;
  if (block < 0)
    block = 0;
  if (block > 7)
    block = 7;

  index = (int)(semi * 32 + frac);
  if (index < 0)
    index = 0;
  if (index >= 12 * 32)
    index = 12 * 32 - 1;
  fnum = gm_fnum_32[index];

  *b0_out = (uint8_t)(((unsigned)block << 2) |
                      ((unsigned)fnum >> 8));
  return (uint16_t)(fnum & 0x03ffu);
}

static void gm_update_voice_frequency(
    snd_genmidi_opl_t SND_PTR *opl, int index, int key_on) {
  snd_genmidi_opl_voice_t SND_PTR *voice;
  int32_t pitch;
  uint16_t fnum;
  uint8_t b0;

  voice = &opl->voice[index];
  if (!voice->instrument)
    return;

  pitch = (int32_t)voice->base_pitch_32 +
          gm_bend_32(opl->channel[voice->midi_channel].pitch_bend);
  fnum = gm_frequency_for_pitch(pitch, &b0);
  voice->b0 = b0;

  gm_write(opl, (uint16_t)(0xa0u + (uint16_t)index),
           (uint8_t)(fnum & 0xffu));
  gm_write(opl, (uint16_t)(0xb0u + (uint16_t)index),
           (uint8_t)(b0 | (key_on ? 0x20u : 0x00u)));
}

static void gm_key_off_voice(snd_genmidi_opl_t SND_PTR *opl,
                             int index, int stolen) {
  snd_genmidi_opl_voice_t SND_PTR *voice;
  if (!opl || index < 0 || index >= SND_GENMIDI_OPL_VOICES)
    return;

  voice = &opl->voice[index];
  if (!voice->active)
    return;

  gm_write(opl, (uint16_t)(0xb0u + (uint16_t)index),
           (uint8_t)(voice->b0 & (uint8_t)~0x20u));
  voice->active = 0u;
  voice->sustained = 0u;
  voice->key_released = 0u;
  ++opl->opl_voices_released;
  if (stolen)
    ++opl->voices_stolen;
}

static int gm_find_voice(snd_genmidi_opl_t SND_PTR *opl) {
  int i;
  int best = -1;

  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i)
    if (!opl->voice[i].active)
      return i;

  /* Doom/DMX-shaped policy: second layers are expendable first. Within the
   * same class, higher MIDI channels are lower priority; ties choose oldest. */
  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i) {
    const snd_genmidi_opl_voice_t SND_PTR *v = &opl->voice[i];
    if (best < 0) {
      best = i;
      continue;
    }
    if (v->layer > opl->voice[best].layer ||
        (v->layer == opl->voice[best].layer &&
         (v->midi_channel > opl->voice[best].midi_channel ||
          (v->midi_channel == opl->voice[best].midi_channel &&
           v->serial < opl->voice[best].serial)))) {
      best = i;
    }
  }
  return (best >= 0) ? best : 0;
}

static int16_t gm_base_pitch_32(
    const snd_genmidi_opl_instrument_t SND_PTR *instrument,
    const snd_genmidi_opl_patch_voice_t SND_PTR *patch,
    int channel, int key, int layer) {
  int32_t note;
  int32_t pitch;

  if ((instrument->flags & SND_GENMIDI_OPL_FLAG_FIXED) != 0u)
    note = (int32_t)(instrument->fixed_note & 0x7fu);
  else if (channel == (int)SND_MIDI_GM_PERCUSSION_CHANNEL)
    note = 60;
  else
    note = key;

  pitch = note * 32;
  if ((instrument->flags & SND_GENMIDI_OPL_FLAG_FIXED) == 0u)
    pitch += (int32_t)patch->base_note_offset * 32;

  if (layer != 0)
    pitch += (int32_t)(instrument->fine_tuning / 2u) - 64;

  if (pitch < INT16_MIN)
    pitch = INT16_MIN;
  if (pitch > INT16_MAX)
    pitch = INT16_MAX;
  return (int16_t)pitch;
}

static void gm_start_layer(
    snd_genmidi_opl_t SND_PTR *opl, int channel, int key,
    int velocity,
    const snd_genmidi_opl_instrument_t SND_PTR *instrument,
    int layer) {
  const snd_genmidi_opl_patch_voice_t SND_PTR *patch;
  snd_genmidi_opl_voice_t SND_PTR *voice;
  int modulating;
  int index;

  index = gm_find_voice(opl);
  if (opl->voice[index].active)
    gm_key_off_voice(opl, index, 1);

  patch = &instrument->voice[layer];
  voice = &opl->voice[index];
  memset(voice, 0, sizeof(*voice));
  voice->instrument = instrument;
  voice->serial = ++opl->next_serial;
  voice->base_pitch_32 =
      gm_base_pitch_32(instrument, patch, channel, key, layer);
  voice->midi_channel = (uint8_t)channel;
  voice->key = (uint8_t)key;
  voice->velocity = (uint8_t)velocity;
  voice->layer = (uint8_t)layer;
  voice->active = 1u;

  modulating = ((patch->feedback & 0x01u) == 0u);

  /* Silence/key-off before replacing all channel registers. */
  gm_write(opl, (uint16_t)(0xb0u + (uint16_t)index), 0x00u);

  /* DMX loads the carrier first at minimum output. In additive mode the
   * modulator is also audible, so it also starts at minimum output. */
  gm_write_operator(opl, gm_car_operator[index], &patch->carrier, 1);
  gm_write_operator(opl, gm_mod_operator[index], &patch->modulator,
                    modulating ? 0 : 1);

  /* 0x30 is ignored in OPL2 compatibility mode but is part of the original
   * driver register stream and keeps future OPL3 mode behavior well-defined. */
  gm_write(opl, (uint16_t)(0xc0u + (uint16_t)index),
           (uint8_t)((patch->feedback & 0x0fu) | 0x30u));

  gm_update_voice_volume(opl, index);
  gm_update_voice_frequency(opl, index, 1);
  ++opl->opl_voices_started;
}

static void gm_note_on(snd_genmidi_opl_t SND_PTR *opl,
                       int channel, int key, int velocity) {
  const snd_genmidi_opl_instrument_t SND_PTR *instrument;
  int layers;
  int layer;

  if (!opl || velocity <= 0)
    return;
  instrument = gm_instrument_for(opl, channel, key);
  if (!instrument)
    return;

  layers =
      ((instrument->flags & SND_GENMIDI_OPL_FLAG_2VOICE) != 0u) ? 2 : 1;
  ++opl->midi_notes_started;
  for (layer = 0; layer < layers; ++layer)
    gm_start_layer(opl, channel, key, velocity, instrument, layer);
}

static void gm_note_off(snd_genmidi_opl_t SND_PTR *opl,
                        int channel, int key) {
  int i;
  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i) {
    snd_genmidi_opl_voice_t SND_PTR *voice = &opl->voice[i];
    if (!voice->active ||
        voice->midi_channel != (uint8_t)channel ||
        voice->key != (uint8_t)key)
      continue;

    voice->key_released = 1u;
    if (opl->channel[channel].sustain)
      voice->sustained = 1u;
    else
      gm_key_off_voice(opl, i, 0);
  }
}

static void gm_release_sustained(snd_genmidi_opl_t SND_PTR *opl,
                                 int channel) {
  int i;
  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i)
    if (opl->voice[i].active &&
        opl->voice[i].midi_channel == (uint8_t)channel &&
        opl->voice[i].sustained)
      gm_key_off_voice(opl, i, 0);
}

static void gm_all_notes_off_channel(
    snd_genmidi_opl_t SND_PTR *opl, int channel) {
  int i;
  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i) {
    snd_genmidi_opl_voice_t SND_PTR *voice = &opl->voice[i];
    if (!voice->active ||
        voice->midi_channel != (uint8_t)channel)
      continue;
    voice->key_released = 1u;
    if (opl->channel[channel].sustain)
      voice->sustained = 1u;
    else
      gm_key_off_voice(opl, i, 0);
  }
}

static void gm_kill_channel(snd_genmidi_opl_t SND_PTR *opl,
                            int channel) {
  int i;
  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i)
    if (opl->voice[i].active &&
        opl->voice[i].midi_channel == (uint8_t)channel)
      gm_key_off_voice(opl, i, 0);
}

static void gm_update_channel_volume(
    snd_genmidi_opl_t SND_PTR *opl, int channel) {
  int i;
  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i)
    if (opl->voice[i].active &&
        opl->voice[i].midi_channel == (uint8_t)channel)
      gm_update_voice_volume(opl, i);
}

static void gm_update_channel_pitch(
    snd_genmidi_opl_t SND_PTR *opl, int channel) {
  int i;
  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i)
    if (opl->voice[i].active &&
        opl->voice[i].midi_channel == (uint8_t)channel)
      gm_update_voice_frequency(opl, i, 1);
}

static void gm_apply_event(snd_genmidi_opl_t SND_PTR *opl,
                           const snd_genmidi_opl_event_t SND_PTR *event) {
  uint8_t type;
  int channel;
  snd_genmidi_opl_channel_t SND_PTR *c;

  type = (uint8_t)(event->msg.status & 0xf0u);
  channel = (int)(event->msg.status & 0x0fu);
  c = &opl->channel[channel];

  switch (type) {
  case SND_MIDI_NOTE_ON:
    gm_note_on(opl, channel, (int)event->msg.data1,
               (int)event->msg.data2);
    break;

  case SND_MIDI_NOTE_OFF:
    gm_note_off(opl, channel, (int)event->msg.data1);
    break;

  case SND_MIDI_PROGRAM_CHANGE:
    c->program = event->msg.data1;
    break;

  case SND_MIDI_PITCH_BEND:
    c->pitch_bend =
        (uint16_t)(((uint16_t)event->msg.data2 << 7) |
                   (uint16_t)event->msg.data1);
    gm_update_channel_pitch(opl, channel);
    break;

  case SND_MIDI_CONTROL_CHANGE:
    switch (event->msg.data1) {
    case SND_MIDI_CC_VOLUME:
      c->volume = event->msg.data2;
      gm_update_channel_volume(opl, channel);
      break;
    case SND_MIDI_CC_EXPRESSION:
      c->expression = event->msg.data2;
      gm_update_channel_volume(opl, channel);
      break;
    case SND_MIDI_CC_PAN:
      c->pan = event->msg.data2;
      break;
    case SND_MIDI_CC_SUSTAIN: {
      uint8_t old = c->sustain;
      c->sustain = (uint8_t)(event->msg.data2 >= 64u);
      if (old && !c->sustain)
        gm_release_sustained(opl, channel);
      break;
    }
    case SND_MIDI_CC_ALL_SOUND_OFF:
      gm_kill_channel(opl, channel);
      break;
    case SND_MIDI_CC_ALL_NOTES_OFF:
      gm_all_notes_off_channel(opl, channel);
      break;
    case SND_MIDI_CC_RESET_CONTROLLERS: {
      uint8_t old_sustain = c->sustain;
      c->pitch_bend = (uint16_t)SND_MIDI_PITCH_CENTER;
      c->expression = 127u;
      c->pan = 64u;
      c->sustain = 0u;
      if (old_sustain)
        gm_release_sustained(opl, channel);
      gm_update_channel_pitch(opl, channel);
      gm_update_channel_volume(opl, channel);
      break;
    }
    default:
      break;
    }
    break;

  default:
    break;
  }
}

void snd_genmidi_opl_mix_block(snd_genmidi_opl_t SND_PTR *opl,
                               snd_mixer_t SND_PTR *m) {
  long block_start;
  int f;

  if (!opl || !m || m->block_frames <= 0)
    return;

  block_start = m->block_frame;
  if (m->span_1 <= m->span_0)
    return;

  snd_touch_block(m);

  for (f = m->span_0; f < m->span_1; ++f) {
    long absolute_frame = block_start + (long)f;
    snd_genmidi_opl_event_t SND_PTR *event = gm_front_event(opl);
    int16_t chip_sample[2];
    long left;
    long right;

    while (event && event->frame <= absolute_frame) {
      gm_apply_event(opl, event);
      gm_pop_event(opl);
      event = gm_front_event(opl);
    }

    OPL3_GenerateResampled(&opl->chip, chip_sample);
    left = ((long)chip_sample[0] * (long)opl->output_gain) >> 8;
    right = ((long)chip_sample[1] * (long)opl->output_gain) >> 8;

    if (m->channels == 2) {
      long index = (long)f * 2L;
      snd_block_add(m, index, left);
      snd_block_add(m, index + 1L, right);
    } else {
      snd_block_add(m, (long)f, (left + right) / 2L);
    }
  }
}

void snd_genmidi_opl_all_sound_off(snd_genmidi_opl_t SND_PTR *opl) {
  int i;
  if (!opl)
    return;
  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i)
    if (opl->voice[i].active)
      gm_key_off_voice(opl, i, 0);
  opl->event_head = 0;
  opl->event_count = 0;
}

int snd_genmidi_opl_active_voices(
    const snd_genmidi_opl_t SND_PTR *opl) {
  int i;
  int count = 0;
  if (!opl)
    return 0;
  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i)
    if (opl->voice[i].active)
      ++count;
  return count;
}

int snd_genmidi_opl_pending_events(
    const snd_genmidi_opl_t SND_PTR *opl) {
  return opl ? opl->event_count : 0;
}
