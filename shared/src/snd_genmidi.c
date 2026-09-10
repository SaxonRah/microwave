#include "snd_genmidi.h"

#include <limits.h>
#include <string.h>

#define GM_VOICE_BYTES 16u
#define GM_OP_BYTES 6u
#define GM_FLAG_OFFSET 0u
#define GM_FINE_OFFSET 2u
#define GM_FIXED_OFFSET 3u
#define GM_VOICE0_OFFSET 4u
#define GM_VOICE1_OFFSET 20u
#define GM_MOD_OFFSET 0u
#define GM_FEEDBACK_OFFSET 6u
#define GM_CARRIER_OFFSET 7u
#define GM_BASE_NOTE_OFFSET 14u

static const uint16_t gm_multiple_8_8[16] = {
    128u, 256u, 512u, 768u, 1024u, 1280u, 1536u, 1792u,
    2048u, 2304u, 2560u, 2560u, 3072u, 3072u, 3840u, 3840u};

/* 0.75 dB OPL total-level steps, converted once to 8.8 linear gain. */
static const int16_t gm_level_gain[64] = {
    256, 235, 215, 198, 181, 166, 152, 140, 128, 118, 108, 99, 91,
    83, 76, 70, 64, 59, 54, 50, 45, 42, 38, 35, 32, 29, 27, 25, 23,
    21, 19, 18, 16, 15, 14, 13, 12, 11, 10, 9, 8, 8, 7, 6, 6, 5, 5,
    4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1};

/* OPL sustain attenuation is 3 dB/step, with level 15 effectively -93 dB. */
static const int16_t gm_sustain_gain[16] = {
    256, 181, 128, 91, 64, 46, 32, 23, 16, 11, 8, 6, 4, 3, 2, 0};

/*
 * YM3812 envelope rates are not one shared millisecond scale. Attack has a
 * much steeper curve than decay/release. These are the application-manual
 * RM=n, RL=0 timings rounded to whole milliseconds:
 *
 *   attack: 0 dB -> -36 dB-equivalent rise metric
 *   decay:  0 dB -> -36 dB attenuation
 *
 * Rate zero means "no envelope change". uint16_t cannot encode infinity, so
 * 65535 ms is used as a practical sentinel for this generic FM adapter.
 *
 * This is still an approximation because the generic FM backend does not yet
 * model KSR's note-dependent RL component or the chip's logarithmic envelope
 * counter. It is intentionally much closer to YM3812 behavior than the old
 * table that used the same 60000..2 ms curve for attack, decay and release.
 */
static const uint16_t gm_attack_ms[16] = {
    65535u, 2826u, 1413u, 707u, 353u, 177u, 88u, 50u,
    25u, 12u, 6u, 3u, 1u, 1u, 1u, 0u};

static const uint16_t gm_decay_release_ms[16] = {
    65535u, 39281u, 19640u, 9820u, 4910u, 2455u, 1228u, 614u,
    307u, 153u, 77u, 38u, 19u, 10u, 5u, 2u};

/* GENMIDI is an OPL2 bank. OPL2 waveform numbers are not generic
 * sine/triangle/square/saw selections. Preserve the actual four YM3812
 * wave-select shapes. */
static const uint8_t gm_wave_map[4] = {
    SND_WAVE_OPL_SINE,
    SND_WAVE_OPL_HALF_SINE,
    SND_WAVE_OPL_ABS_SINE,
    SND_WAVE_OPL_QUARTER_SINE};

static uint16_t gm_rd16(const uint8_t SND_PTR *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t gm_rds16(const uint8_t SND_PTR *p) {
  uint16_t u = gm_rd16(p);
  return (int16_t)u;
}

static int16_t gm_clamp_i16(int32_t v) {
  if (v < INT16_MIN)
    return INT16_MIN;
  if (v > INT16_MAX)
    return INT16_MAX;
  return (int16_t)v;
}

static uint16_t gm_clamp_u16_u32(uint32_t v) {
  return (v > 65535u) ? 65535u : (uint16_t)v;
}

/* The manual's decay table measures the time to 36 dB attenuation. Scale that
 * interval to the programmed sustain level so an SL of, say, 12 dB does not
 * incorrectly consume the whole 36 dB interval. SL=15 is the OPL special
 * -93 dB value. */
static uint16_t gm_decay_to_sustain_ms(uint16_t decay_36_ms, uint8_t sl) {
  uint32_t attenuation_db;
  uint32_t ms;

  sl &= 0x0fu;
  if (sl == 0u)
    return 0u;
  attenuation_db = (sl == 15u) ? 93u : (uint32_t)sl * 3u;
  ms = ((uint32_t)decay_36_ms * attenuation_db + 18u) / 36u;
  return gm_clamp_u16_u32(ms);
}

static void gm_convert_operator(const uint8_t SND_PTR *raw,
                                snd_fm_operator_t SND_PTR *out) {
  uint8_t tremolo;
  uint8_t attack;
  uint8_t sustain;
  uint8_t ar;
  uint8_t dr;
  uint8_t sl;
  uint8_t rr;
  uint16_t decay_to_sl;
  uint16_t release_ms;

  tremolo = raw[0];
  attack = raw[1];
  sustain = raw[2];

  ar = (uint8_t)((attack >> 4) & 0x0fu);
  dr = (uint8_t)(attack & 0x0fu);
  sl = (uint8_t)((sustain >> 4) & 0x0fu);
  rr = (uint8_t)(sustain & 0x0fu);

  out->multiple = gm_multiple_8_8[tremolo & 0x0fu];
  out->level = gm_level_gain[raw[5] & 0x3fu];

  /* #OPL_II# data is OPL2: only wave-select bits 0..1 are meaningful. */
  out->wave = gm_wave_map[raw[3] & 0x03u];

  out->env.attack_ms = gm_attack_ms[ar];
  decay_to_sl = gm_decay_to_sustain_ms(gm_decay_release_ms[dr], sl);
  release_ms = gm_decay_release_ms[rr];
  out->env.release_ms = release_ms;

  /*
   * EGT=1 ("continuing"): attack -> decay -> hold SL -> release at key-off.
   *
   * EGT=0 ("diminishing"): the OPL envelope enters release immediately after
   * it reaches SL, even while the key remains on. snd_midi_fm's generic ADSR
   * has no automatic post-decay release state, so approximate the two serial
   * segments as one decay-to-zero interval. This preserves the important
   * duration/order instead of the old behavior, which simply decayed to zero
   * using DR and ignored RR while the key was held.
   */
  if ((tremolo & 0x20u) != 0u) {
    out->env.decay_ms = decay_to_sl;
    out->env.sustain = gm_sustain_gain[sl];
  } else {
    uint32_t total = (uint32_t)decay_to_sl + (uint32_t)release_ms;
    out->env.decay_ms = gm_clamp_u16_u32(total);
    out->env.sustain = 0;
  }
}

static int16_t gm_pitch_offset_128(int16_t base_note, uint8_t fine,
                                   int second_voice, int apply_base) {
  int32_t v = 0;
  if (apply_base)
    v += (int32_t)base_note * 128;

  /* DMX applies the second voice fine tuning as
   *     (fine / 2) - 64
   * in a 32-steps-per-semitone frequency index. In this backend's
   * 1/128-semitone domain that is exactly 2*fine - 256. */
  if (second_voice)
    v += (int32_t)fine * 2 - 256;

  return gm_clamp_i16(v);
}

static void gm_convert_voice(const uint8_t SND_PTR *raw,
                             snd_fm_patch_t SND_PTR *out,
                             uint16_t flags, uint8_t fine,
                             int second_voice, int percussion) {
  uint8_t feedback;
  int modulating;
  int apply_base;
  int16_t base_note;

  memset(out, 0, sizeof(*out));
  gm_convert_operator(raw + GM_MOD_OFFSET, &out->modulator);
  gm_convert_operator(raw + GM_CARRIER_OFFSET, &out->carrier);

  feedback = raw[GM_FEEDBACK_OFFSET];
  modulating = (feedback & 0x01u) == 0u;
  out->algorithm = (uint8_t)(modulating ? SND_FM_ALGORITHM_FM
                                       : SND_FM_ALGORITHM_ADDITIVE);

  /* The generic FM renderer uses explicit phase-displacement values. GENMIDI
   * only has the OPL connection bit and 3-bit feedback field, so these remain
   * approximations until GENMIDI has a native OPL register backend. */
  out->modulation = modulating ? 64u : 0u;
  out->feedback = (uint16_t)(((feedback >> 1) & 0x07u) * 12u);

  /* DMX ignores the stored carrier total level and drives it from MIDI note /
   * channel volume. The generic backend already applies MIDI gain outside the
   * operator, so carrier unity is the closest equivalent here. */
  out->carrier.level = SND_GAIN_UNITY;

  out->transpose = 0;
  base_note = gm_rds16(raw + GM_BASE_NOTE_OFFSET);

  /* Fixed-pitch GENMIDI instruments ignore base_note_offset. Percussion that
   * is not explicitly fixed is played by DMX from note 60, then receives each
   * voice's base offset, so keep that offset here in the layer detune. */
  apply_base = ((flags & SND_GENMIDI_FLAG_FIXED) == 0u) ? 1 : 0;
  (void)percussion;
  out->detune_128 =
      gm_pitch_offset_128(base_note, fine, second_voice, apply_base);
}

static void gm_convert_instrument(const uint8_t SND_PTR *raw,
                                  snd_fm_instrument_t SND_PTR *out,
                                  int percussion) {
  uint16_t flags;
  uint8_t fine;
  uint8_t fixed_note;

  memset(out, 0, sizeof(*out));
  flags = gm_rd16(raw + GM_FLAG_OFFSET);
  fine = raw[GM_FINE_OFFSET];
  fixed_note = raw[GM_FIXED_OFFSET];

  out->layer_count =
      (uint8_t)(((flags & SND_GENMIDI_FLAG_2VOICE) != 0u) ? 2u : 1u);
  if ((flags & SND_GENMIDI_FLAG_FIXED) != 0u)
    out->fixed_note = (uint8_t)(fixed_note & 0x7fu);
  else if (percussion)
    out->fixed_note = 60u;
  else
    out->fixed_note = SND_FM_FIXED_NOTE_NONE;

  gm_convert_voice(raw + GM_VOICE0_OFFSET, &out->layer[0], flags, fine, 0,
                   percussion);
  if (out->layer_count > 1u)
    gm_convert_voice(raw + GM_VOICE1_OFFSET, &out->layer[1], flags, fine, 1,
                     percussion);
}

int snd_genmidi_load(snd_genmidi_bank_t SND_PTR *out,
                     const void SND_PTR *data, uint32_t bytes) {
  static const uint8_t magic[SND_GENMIDI_HEADER_BYTES] = {
      '#', 'O', 'P', 'L', '_', 'I', 'I', '#'};
  const uint8_t SND_PTR *src;
  uint32_t off;
  int i;

  if (!out || !data)
    return SND_GENMIDI_ERR_ARGUMENT;

  memset(out, 0, sizeof(*out));
  src = (const uint8_t SND_PTR *)data;

  if (bytes < SND_GENMIDI_HEADER_BYTES)
    return SND_GENMIDI_ERR_TRUNCATED;
  if (memcmp(src, magic, sizeof(magic)) != 0)
    return SND_GENMIDI_ERR_HEADER;
  if (bytes < SND_GENMIDI_MIN_BYTES)
    return SND_GENMIDI_ERR_TRUNCATED;

  off = SND_GENMIDI_HEADER_BYTES;
  for (i = 0; i < SND_GENMIDI_PROGRAM_COUNT; ++i) {
    gm_convert_instrument(src + off, &out->programs[i], 0);
    off += SND_GENMIDI_RAW_INSTRUMENT_BYTES;
  }
  for (i = 0; i < SND_GENMIDI_PERCUSSION_COUNT; ++i) {
    gm_convert_instrument(src + off, &out->percussion[i], 1);
    off += SND_GENMIDI_RAW_INSTRUMENT_BYTES;
  }

  out->bank.programs = out->programs;
  out->bank.program_count = SND_GENMIDI_PROGRAM_COUNT;
  out->bank.percussion = out->percussion;
  out->bank.percussion_first_note = SND_GENMIDI_PERCUSSION_FIRST_NOTE;
  out->bank.percussion_count = SND_GENMIDI_PERCUSSION_COUNT;
  out->bank.fallback = &snd_fm_default_instrument;
  out->bank.percussion_fallback = &snd_fm_default_percussion_instrument;
  out->has_names = (uint8_t)(bytes >= SND_GENMIDI_CANONICAL_BYTES);

  return SND_GENMIDI_OK;
}

const snd_midi_fm_bank_t SND_PTR *
snd_genmidi_midi_fm_bank(const snd_genmidi_bank_t SND_PTR *bank) {
  return bank ? &bank->bank : 0;
}

const char *snd_genmidi_error_string(int error) {
  switch (error) {
  case SND_GENMIDI_OK:
    return "ok";
  case SND_GENMIDI_ERR_ARGUMENT:
    return "invalid argument";
  case SND_GENMIDI_ERR_HEADER:
    return "invalid GENMIDI header";
  case SND_GENMIDI_ERR_TRUNCATED:
    return "truncated GENMIDI bank";
  default:
    return "unknown GENMIDI error";
  }
}
