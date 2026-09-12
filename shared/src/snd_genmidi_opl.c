#include "snd_genmidi_opl.h"

#if defined(MC_OPL3_TIME_CRITICAL) && (defined(__GNUC__) || defined(__clang__))
#define MW_OPL_TIME_CRITICAL(name) \
    __attribute__((noinline, section(".time_critical.mw_opl." #name))) name
#else
#define MW_OPL_TIME_CRITICAL(name) name
#endif


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

#define GM_OPL_REG_WAVEFORM_ENABLE 0x01u
#define GM_OPL_REG_TIMER_CONTROL 0x04u
#define GM_OPL_REG_FM_MODE 0x08u
#define GM_OPL_REG_TREMOLO 0x20u
#define GM_OPL_REG_LEVEL 0x40u
#define GM_OPL_REG_ATTACK 0x60u
#define GM_OPL_REG_SUSTAIN 0x80u
#define GM_OPL_REG_FREQ_1 0xa0u
#define GM_OPL_REG_FREQ_2 0xb0u
#define GM_OPL_REG_FEEDBACK 0xc0u
#define GM_OPL_REG_WAVEFORM 0xe0u
#define GM_OPL_NUM_OPERATORS 21u

/* YM3812 operator register offsets for channels 0..8. */
static const uint8_t gm_mod_operator[SND_GENMIDI_OPL_VOICES] = {
    0x00u, 0x01u, 0x02u, 0x08u, 0x09u, 0x0au, 0x10u, 0x11u, 0x12u
};

static const uint8_t gm_car_operator[SND_GENMIDI_OPL_VOICES] = {
    0x03u, 0x04u, 0x05u, 0x0bu, 0x0cu, 0x0du, 0x13u, 0x14u, 0x15u
};

/* Vanilla DMX MIDI-volume map. Independently present in Chocolate Doom,
 * DSDA-Doom, Woof, and libADLMIDI's DMX model. */
static const uint8_t gm_volume_mapping[128] = {
      0u,   1u,   3u,   5u,   6u,   8u,  10u,  11u,
     13u,  14u,  16u,  17u,  19u,  20u,  22u,  23u,
     25u,  26u,  27u,  29u,  30u,  32u,  33u,  34u,
     36u,  37u,  39u,  41u,  43u,  45u,  47u,  49u,
     50u,  52u,  54u,  55u,  57u,  59u,  60u,  61u,
     63u,  64u,  66u,  67u,  68u,  69u,  71u,  72u,
     73u,  74u,  75u,  76u,  77u,  79u,  80u,  81u,
     82u,  83u,  84u,  84u,  85u,  86u,  87u,  88u,
     89u,  90u,  91u,  92u,  92u,  93u,  94u,  95u,
     96u,  96u,  97u,  98u,  99u,  99u, 100u, 101u,
    101u, 102u, 103u, 103u, 104u, 105u, 105u, 106u,
    107u, 107u, 108u, 109u, 109u, 110u, 110u, 111u,
    112u, 112u, 113u, 113u, 114u, 114u, 115u, 115u,
    116u, 117u, 117u, 118u, 118u, 119u, 119u, 120u,
    120u, 121u, 121u, 122u, 122u, 123u, 123u, 123u,
    124u, 124u, 125u, 125u, 126u, 126u, 127u, 127u
};

/*
 * DMX frequency table.
 *
 * The values and historical model were cross-checked against libADLMIDI
 * src/models/model_dmx.c, revision
 * d114c313c9f6a54b6a93adef2b077810136cf508.
 *
 * libADLMIDI's model_dmx.c is MIT licensed:
 *
 * Copyright (c) 2025-2026 Vitaly Novichkov <admin@wohlnet.ru>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Chocolate Doom / DSDA / Woof use split index 284. libADLMIDI's newer
 * reverse-engineered model can reproduce a DMX off-by-one with split 283.
 */
static const uint16_t gm_frequency_curve[668] = {
    0x0133u, 0x0133u, 0x0134u, 0x0134u, 0x0135u, 0x0136u, 0x0136u, 0x0137u,
    0x0137u, 0x0138u, 0x0138u, 0x0139u, 0x0139u, 0x013au, 0x013bu, 0x013bu,
    0x013cu, 0x013cu, 0x013du, 0x013du, 0x013eu, 0x013fu, 0x013fu, 0x0140u,
    0x0140u, 0x0141u, 0x0142u, 0x0142u, 0x0143u, 0x0143u, 0x0144u, 0x0144u,
    0x0145u, 0x0146u, 0x0146u, 0x0147u, 0x0147u, 0x0148u, 0x0149u, 0x0149u,
    0x014au, 0x014au, 0x014bu, 0x014cu, 0x014cu, 0x014du, 0x014du, 0x014eu,
    0x014fu, 0x014fu, 0x0150u, 0x0150u, 0x0151u, 0x0152u, 0x0152u, 0x0153u,
    0x0153u, 0x0154u, 0x0155u, 0x0155u, 0x0156u, 0x0157u, 0x0157u, 0x0158u,
    0x0158u, 0x0159u, 0x015au, 0x015au, 0x015bu, 0x015bu, 0x015cu, 0x015du,
    0x015du, 0x015eu, 0x015fu, 0x015fu, 0x0160u, 0x0161u, 0x0161u, 0x0162u,
    0x0162u, 0x0163u, 0x0164u, 0x0164u, 0x0165u, 0x0166u, 0x0166u, 0x0167u,
    0x0168u, 0x0168u, 0x0169u, 0x016au, 0x016au, 0x016bu, 0x016cu, 0x016cu,
    0x016du, 0x016eu, 0x016eu, 0x016fu, 0x0170u, 0x0170u, 0x0171u, 0x0172u,
    0x0172u, 0x0173u, 0x0174u, 0x0174u, 0x0175u, 0x0176u, 0x0176u, 0x0177u,
    0x0178u, 0x0178u, 0x0179u, 0x017au, 0x017au, 0x017bu, 0x017cu, 0x017cu,
    0x017du, 0x017eu, 0x017eu, 0x017fu, 0x0180u, 0x0181u, 0x0181u, 0x0182u,
    0x0183u, 0x0183u, 0x0184u, 0x0185u, 0x0185u, 0x0186u, 0x0187u, 0x0188u,
    0x0188u, 0x0189u, 0x018au, 0x018au, 0x018bu, 0x018cu, 0x018du, 0x018du,
    0x018eu, 0x018fu, 0x018fu, 0x0190u, 0x0191u, 0x0192u, 0x0192u, 0x0193u,
    0x0194u, 0x0194u, 0x0195u, 0x0196u, 0x0197u, 0x0197u, 0x0198u, 0x0199u,
    0x019au, 0x019au, 0x019bu, 0x019cu, 0x019du, 0x019du, 0x019eu, 0x019fu,
    0x01a0u, 0x01a0u, 0x01a1u, 0x01a2u, 0x01a3u, 0x01a3u, 0x01a4u, 0x01a5u,
    0x01a6u, 0x01a6u, 0x01a7u, 0x01a8u, 0x01a9u, 0x01a9u, 0x01aau, 0x01abu,
    0x01acu, 0x01adu, 0x01adu, 0x01aeu, 0x01afu, 0x01b0u, 0x01b0u, 0x01b1u,
    0x01b2u, 0x01b3u, 0x01b4u, 0x01b4u, 0x01b5u, 0x01b6u, 0x01b7u, 0x01b8u,
    0x01b8u, 0x01b9u, 0x01bau, 0x01bbu, 0x01bcu, 0x01bcu, 0x01bdu, 0x01beu,
    0x01bfu, 0x01c0u, 0x01c0u, 0x01c1u, 0x01c2u, 0x01c3u, 0x01c4u, 0x01c4u,
    0x01c5u, 0x01c6u, 0x01c7u, 0x01c8u, 0x01c9u, 0x01c9u, 0x01cau, 0x01cbu,
    0x01ccu, 0x01cdu, 0x01ceu, 0x01ceu, 0x01cfu, 0x01d0u, 0x01d1u, 0x01d2u,
    0x01d3u, 0x01d3u, 0x01d4u, 0x01d5u, 0x01d6u, 0x01d7u, 0x01d8u, 0x01d8u,
    0x01d9u, 0x01dau, 0x01dbu, 0x01dcu, 0x01ddu, 0x01deu, 0x01deu, 0x01dfu,
    0x01e0u, 0x01e1u, 0x01e2u, 0x01e3u, 0x01e4u, 0x01e5u, 0x01e5u, 0x01e6u,
    0x01e7u, 0x01e8u, 0x01e9u, 0x01eau, 0x01ebu, 0x01ecu, 0x01edu, 0x01edu,
    0x01eeu, 0x01efu, 0x01f0u, 0x01f1u, 0x01f2u, 0x01f3u, 0x01f4u, 0x01f5u,
    0x01f6u, 0x01f6u, 0x01f7u, 0x01f8u, 0x01f9u, 0x01fau, 0x01fbu, 0x01fcu,
    0x01fdu, 0x01feu, 0x01ffu, 0x0200u, 0x0201u, 0x0201u, 0x0202u, 0x0203u,
    0x0204u, 0x0205u, 0x0206u, 0x0207u, 0x0208u, 0x0209u, 0x020au, 0x020bu,
    0x020cu, 0x020du, 0x020eu, 0x020fu, 0x0210u, 0x0210u, 0x0211u, 0x0212u,
    0x0213u, 0x0214u, 0x0215u, 0x0216u, 0x0217u, 0x0218u, 0x0219u, 0x021au,
    0x021bu, 0x021cu, 0x021du, 0x021eu, 0x021fu, 0x0220u, 0x0221u, 0x0222u,
    0x0223u, 0x0224u, 0x0225u, 0x0226u, 0x0227u, 0x0228u, 0x0229u, 0x022au,
    0x022bu, 0x022cu, 0x022du, 0x022eu, 0x022fu, 0x0230u, 0x0231u, 0x0232u,
    0x0233u, 0x0234u, 0x0235u, 0x0236u, 0x0237u, 0x0238u, 0x0239u, 0x023au,
    0x023bu, 0x023cu, 0x023du, 0x023eu, 0x023fu, 0x0240u, 0x0241u, 0x0242u,
    0x0244u, 0x0245u, 0x0246u, 0x0247u, 0x0248u, 0x0249u, 0x024au, 0x024bu,
    0x024cu, 0x024du, 0x024eu, 0x024fu, 0x0250u, 0x0251u, 0x0252u, 0x0253u,
    0x0254u, 0x0256u, 0x0257u, 0x0258u, 0x0259u, 0x025au, 0x025bu, 0x025cu,
    0x025du, 0x025eu, 0x025fu, 0x0260u, 0x0262u, 0x0263u, 0x0264u, 0x0265u,
    0x0266u, 0x0267u, 0x0268u, 0x0269u, 0x026au, 0x026cu, 0x026du, 0x026eu,
    0x026fu, 0x0270u, 0x0271u, 0x0272u, 0x0273u, 0x0275u, 0x0276u, 0x0277u,
    0x0278u, 0x0279u, 0x027au, 0x027bu, 0x027du, 0x027eu, 0x027fu, 0x0280u,
    0x0281u, 0x0282u, 0x0284u, 0x0285u, 0x0286u, 0x0287u, 0x0288u, 0x0289u,
    0x028bu, 0x028cu, 0x028du, 0x028eu, 0x028fu, 0x0290u, 0x0292u, 0x0293u,
    0x0294u, 0x0295u, 0x0296u, 0x0298u, 0x0299u, 0x029au, 0x029bu, 0x029cu,
    0x029eu, 0x029fu, 0x02a0u, 0x02a1u, 0x02a2u, 0x02a4u, 0x02a5u, 0x02a6u,
    0x02a7u, 0x02a9u, 0x02aau, 0x02abu, 0x02acu, 0x02aeu, 0x02afu, 0x02b0u,
    0x02b1u, 0x02b2u, 0x02b4u, 0x02b5u, 0x02b6u, 0x02b7u, 0x02b9u, 0x02bau,
    0x02bbu, 0x02bdu, 0x02beu, 0x02bfu, 0x02c0u, 0x02c2u, 0x02c3u, 0x02c4u,
    0x02c5u, 0x02c7u, 0x02c8u, 0x02c9u, 0x02cbu, 0x02ccu, 0x02cdu, 0x02ceu,
    0x02d0u, 0x02d1u, 0x02d2u, 0x02d4u, 0x02d5u, 0x02d6u, 0x02d8u, 0x02d9u,
    0x02dau, 0x02dcu, 0x02ddu, 0x02deu, 0x02e0u, 0x02e1u, 0x02e2u, 0x02e4u,
    0x02e5u, 0x02e6u, 0x02e8u, 0x02e9u, 0x02eau, 0x02ecu, 0x02edu, 0x02eeu,
    0x02f0u, 0x02f1u, 0x02f2u, 0x02f4u, 0x02f5u, 0x02f6u, 0x02f8u, 0x02f9u,
    0x02fbu, 0x02fcu, 0x02fdu, 0x02ffu, 0x0300u, 0x0302u, 0x0303u, 0x0304u,
    0x0306u, 0x0307u, 0x0309u, 0x030au, 0x030bu, 0x030du, 0x030eu, 0x0310u,
    0x0311u, 0x0312u, 0x0314u, 0x0315u, 0x0317u, 0x0318u, 0x031au, 0x031bu,
    0x031cu, 0x031eu, 0x031fu, 0x0321u, 0x0322u, 0x0324u, 0x0325u, 0x0327u,
    0x0328u, 0x0329u, 0x032bu, 0x032cu, 0x032eu, 0x032fu, 0x0331u, 0x0332u,
    0x0334u, 0x0335u, 0x0337u, 0x0338u, 0x033au, 0x033bu, 0x033du, 0x033eu,
    0x0340u, 0x0341u, 0x0343u, 0x0344u, 0x0346u, 0x0347u, 0x0349u, 0x034au,
    0x034cu, 0x034du, 0x034fu, 0x0350u, 0x0352u, 0x0353u, 0x0355u, 0x0357u,
    0x0358u, 0x035au, 0x035bu, 0x035du, 0x035eu, 0x0360u, 0x0361u, 0x0363u,
    0x0365u, 0x0366u, 0x0368u, 0x0369u, 0x036bu, 0x036cu, 0x036eu, 0x0370u,
    0x0371u, 0x0373u, 0x0374u, 0x0376u, 0x0378u, 0x0379u, 0x037bu, 0x037cu,
    0x037eu, 0x0380u, 0x0381u, 0x0383u, 0x0384u, 0x0386u, 0x0388u, 0x0389u,
    0x038bu, 0x038du, 0x038eu, 0x0390u, 0x0392u, 0x0393u, 0x0395u, 0x0397u,
    0x0398u, 0x039au, 0x039cu, 0x039du, 0x039fu, 0x03a1u, 0x03a2u, 0x03a4u,
    0x03a6u, 0x03a7u, 0x03a9u, 0x03abu, 0x03acu, 0x03aeu, 0x03b0u, 0x03b1u,
    0x03b3u, 0x03b5u, 0x03b7u, 0x03b8u, 0x03bau, 0x03bcu, 0x03bdu, 0x03bfu,
    0x03c1u, 0x03c3u, 0x03c4u, 0x03c6u, 0x03c8u, 0x03cau, 0x03cbu, 0x03cdu,
    0x03cfu, 0x03d1u, 0x03d2u, 0x03d4u, 0x03d6u, 0x03d8u, 0x03dau, 0x03dbu,
    0x03ddu, 0x03dfu, 0x03e1u, 0x03e3u, 0x03e4u, 0x03e6u, 0x03e8u, 0x03eau,
    0x03ecu, 0x03edu, 0x03efu, 0x03f1u, 0x03f3u, 0x03f5u, 0x03f6u, 0x03f8u,
    0x03fau, 0x03fcu, 0x03feu, 0x036cu
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

/* mus2mid maps MUS percussion 15 -> MIDI 9. Chocolate Doom then swaps MIDI
 * channel 9 back to DMX channel 15 before the OPL driver sees it. Preserve
 * that internal channel numbering because channel number participates in the
 * Doom 1.9 voice-stealing policy. */
static int gm_dmx_channel_for_midi(int midi_channel) {
  if (midi_channel == 9)
    return 15;
  if (midi_channel == 15)
    return 9;
  return midi_channel;
}

static void gm_channel_default(snd_genmidi_opl_channel_t SND_PTR *channel) {
  channel->bend = 0;
  channel->program = 0u;
  channel->volume_base = 100u;
  channel->volume = 100u;
  channel->pan = 0x30u;
}

static void gm_reset_channels(snd_genmidi_opl_t SND_PTR *opl) {
  int i;
  for (i = 0; i < SND_MIDI_CHANNELS; ++i)
    gm_channel_default(&opl->channel[i]);
}

static void gm_write_direct(snd_genmidi_opl_t SND_PTR *opl,
                            uint16_t reg, uint8_t value) {
  if (opl->trace)
    opl->trace(opl->trace_user, opl->current_frame, reg, value);
  ++opl->register_writes;
  OPL3_WriteReg(&opl->chip, reg, value);
}

static void gm_write(snd_genmidi_opl_t SND_PTR *opl,
                     uint16_t reg, uint8_t value) {
  if (opl->trace)
    opl->trace(opl->trace_user, opl->current_frame, reg, value);
  ++opl->register_writes;
  OPL3_WriteRegBuffered(&opl->chip, reg, value);
}

/* Doom's OPL_InitRegisters() writes far more than the minimum necessary,
 * including nonexistent registers. At startup we apply the same final register
 * state directly so the chip is settled before frame zero; runtime writes use
 * Nuked's buffered register path, matching Chocolate Doom's SDL backend. */
static void gm_chip_reset(snd_genmidi_opl_t SND_PTR *opl) {
  uint16_t reg;

  OPL3_Reset(&opl->chip, (uint32_t)opl->output_rate);
  opl->current_frame = -1L;

  for (reg = GM_OPL_REG_LEVEL;
       reg <= GM_OPL_REG_LEVEL + GM_OPL_NUM_OPERATORS; ++reg)
    gm_write_direct(opl, reg, 0x3fu);

  for (reg = GM_OPL_REG_ATTACK;
       reg <= GM_OPL_REG_WAVEFORM + GM_OPL_NUM_OPERATORS; ++reg)
    gm_write_direct(opl, reg, 0x00u);

  for (reg = 1u; reg < GM_OPL_REG_LEVEL; ++reg)
    gm_write_direct(opl, reg, 0x00u);

  gm_write_direct(opl, GM_OPL_REG_TIMER_CONTROL, 0x60u);
  gm_write_direct(opl, GM_OPL_REG_TIMER_CONTROL, 0x80u);
  gm_write_direct(opl, GM_OPL_REG_WAVEFORM_ENABLE, 0x20u);
  gm_write_direct(opl, GM_OPL_REG_FM_MODE, 0x40u);
}

static void gm_init_voice_lists(snd_genmidi_opl_t SND_PTR *opl,
                                int clear_instrument_cache) {
  int i;

  opl->free_count = SND_GENMIDI_OPL_VOICES;
  opl->alloc_count = 0u;

  for (i = 0; i < SND_GENMIDI_OPL_VOICES; ++i) {
    snd_genmidi_opl_voice_t SND_PTR *voice = &opl->voice[i];

    if (clear_instrument_cache) {
      memset(voice, 0, sizeof(*voice));
      voice->current_instr = 0;
      voice->current_instr_voice = 0u;
      voice->car_volume = 0x3fu;
      voice->mod_volume = 0x3fu;
    } else {
      voice->active = 0u;
      voice->dmx_channel = 0xffu;
      voice->key = 0u;
      voice->note = 0u;
      voice->note_volume = 0u;
      voice->freq = 0u;
    }

    opl->free_list[i] = (uint8_t)i;
    opl->alloc_list[i] = 0u;
  }
}

static void gm_clear_runtime(snd_genmidi_opl_t SND_PTR *opl,
                             int clear_instrument_cache) {
  gm_reset_channels(opl);
  gm_init_voice_lists(opl, clear_instrument_cache);
  opl->event_head = 0;
  opl->event_count = 0;
  opl->current_frame = -1L;

  opl->midi_notes_started = 0uL;
  opl->opl_voices_started = 0uL;
  opl->opl_voices_released = 0uL;
  opl->voices_stolen = 0uL;
  opl->secondary_voices_dropped = 0uL;
  opl->dropped_events = 0uL;
  opl->register_writes = 0uL;
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
  opl->freq_model = (uint8_t)SND_GENMIDI_OPL_FREQ_DOOM19_284;

  gm_clear_runtime(opl, 1);
  gm_chip_reset(opl);
}

void snd_genmidi_opl_reset(snd_genmidi_opl_t SND_PTR *opl) {
  const snd_genmidi_opl_bank_t SND_PTR *bank;
  snd_genmidi_opl_trace_fn trace;
  void SND_PTR *trace_user;
  int rate;
  int16_t gain;
  uint8_t freq_model;

  if (!opl)
    return;

  bank = opl->bank;
  trace = opl->trace;
  trace_user = opl->trace_user;
  rate = opl->output_rate;
  gain = opl->output_gain;
  freq_model = opl->freq_model;

  gm_clear_runtime(opl, 1);

  opl->bank = bank;
  opl->trace = trace;
  opl->trace_user = trace_user;
  opl->output_rate = rate;
  opl->output_gain = gain;
  opl->freq_model = freq_model;

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

void snd_genmidi_opl_set_freq_model(
    snd_genmidi_opl_t SND_PTR *opl,
    snd_genmidi_opl_freq_model_t model) {
  if (!opl)
    return;
  if (model != SND_GENMIDI_OPL_FREQ_DMX_BUG_283)
    model = SND_GENMIDI_OPL_FREQ_DOOM19_284;
  opl->freq_model = (uint8_t)model;
}

void snd_genmidi_opl_set_trace(snd_genmidi_opl_t SND_PTR *opl,
                               snd_genmidi_opl_trace_fn trace,
                               void SND_PTR *user) {
  if (!opl)
    return;
  opl->trace = trace;
  opl->trace_user = user;
}

void snd_genmidi_opl_bind(snd_genmidi_opl_t SND_PTR *opl,
                          snd_midi_t SND_PTR *midi) {
  if (!opl || !midi)
    return;

  /* Deliberately do not inherit snd_midi's generic defaults. DMX starts every
   * channel at program 0, volume 100, bend 0, pan 0x30. */
  gm_reset_channels(opl);
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
                  int midi_channel, int dmx_channel, int key) {
  int index;

  if (!opl || !opl->bank || !opl->bank->valid)
    return 0;

  if (midi_channel == (int)SND_MIDI_GM_PERCUSSION_CHANNEL) {
    index = key - SND_GENMIDI_OPL_PERCUSSION_FIRST_NOTE;
    if (index < 0 || index >= SND_GENMIDI_OPL_PERCUSSION_COUNT)
      return 0;
    return &opl->bank->instrument[
        SND_GENMIDI_OPL_PROGRAM_COUNT + index];
  }

  index = (int)opl->channel[dmx_channel].program;
  if (index < 0 || index >= SND_GENMIDI_OPL_PROGRAM_COUNT)
    index = 0;
  return &opl->bank->instrument[index];
}

static uint8_t gm_get_free_voice(snd_genmidi_opl_t SND_PTR *opl) {
  uint8_t result;
  int i;

  if (!opl || opl->free_count == 0u)
    return 0xffu;

  result = opl->free_list[0];
  --opl->free_count;

  for (i = 0; i < (int)opl->free_count; ++i)
    opl->free_list[i] = opl->free_list[i + 1];

  opl->alloc_list[opl->alloc_count++] = result;
  opl->voice[result].active = 1u;
  return result;
}

static void gm_voice_key_off(snd_genmidi_opl_t SND_PTR *opl,
                             snd_genmidi_opl_voice_t SND_PTR *voice,
                             int physical_index) {
  if (!voice || !voice->active)
    return;

  gm_write(opl,
           (uint16_t)(GM_OPL_REG_FREQ_2 + (uint16_t)physical_index),
           (uint8_t)(voice->freq >> 8));
}

static void gm_release_voice(snd_genmidi_opl_t SND_PTR *opl,
                             int alloc_index, int stolen) {
  uint8_t physical;
  snd_genmidi_opl_voice_t SND_PTR *voice;
  int i;

  if (!opl || alloc_index < 0 || alloc_index >= (int)opl->alloc_count)
    return;

  physical = opl->alloc_list[alloc_index];
  voice = &opl->voice[physical];

  gm_voice_key_off(opl, voice, (int)physical);

  voice->active = 0u;
  voice->dmx_channel = 0xffu;
  voice->note = 0u;

  --opl->alloc_count;
  for (i = alloc_index; i < (int)opl->alloc_count; ++i)
    opl->alloc_list[i] = opl->alloc_list[i + 1];

  opl->free_list[opl->free_count++] = physical;

  ++opl->opl_voices_released;
  if (stolen)
    ++opl->voices_stolen;
}

static void gm_replace_existing_voice(snd_genmidi_opl_t SND_PTR *opl) {
  int i;
  int result = 0;

  if (!opl || opl->alloc_count == 0u)
    return;

  /* Doom 1.9 DMX policy reconstructed by Chocolate/DSDA:
   * second GENMIDI voices are expendable, otherwise higher-numbered DMX
   * channels lose to lower-numbered channels. The >= tie makes allocation
   * list order observable. */
  for (i = 0; i < (int)opl->alloc_count; ++i) {
    const snd_genmidi_opl_voice_t SND_PTR *candidate =
        &opl->voice[opl->alloc_list[i]];
    const snd_genmidi_opl_voice_t SND_PTR *selected =
        &opl->voice[opl->alloc_list[result]];

    if (candidate->current_instr_voice != 0u ||
        candidate->dmx_channel >= selected->dmx_channel)
      result = i;
  }

  gm_release_voice(opl, result, 1);
}

static void gm_load_operator(
    snd_genmidi_opl_t SND_PTR *opl, uint8_t operator_offset,
    const snd_genmidi_opl_operator_t SND_PTR *data,
    int max_level, uint8_t SND_PTR *cached_volume) {
  uint8_t level = data->scale;

  if (max_level)
    level = (uint8_t)(level | 0x3fu);
  else
    level = (uint8_t)(level | data->level);

  *cached_volume = level;

  gm_write(opl, (uint16_t)(GM_OPL_REG_LEVEL + operator_offset), level);
  gm_write(opl, (uint16_t)(GM_OPL_REG_TREMOLO + operator_offset),
           data->tremolo);
  gm_write(opl, (uint16_t)(GM_OPL_REG_ATTACK + operator_offset),
           data->attack);
  gm_write(opl, (uint16_t)(GM_OPL_REG_SUSTAIN + operator_offset),
           data->sustain);
  gm_write(opl, (uint16_t)(GM_OPL_REG_WAVEFORM + operator_offset),
           data->waveform);
}

static void gm_set_voice_instrument(
    snd_genmidi_opl_t SND_PTR *opl, int physical,
    const snd_genmidi_opl_instrument_t SND_PTR *instrument,
    unsigned int instrument_voice) {
  snd_genmidi_opl_voice_t SND_PTR *voice = &opl->voice[physical];
  const snd_genmidi_opl_patch_voice_t SND_PTR *data;
  int modulating;

  if (voice->current_instr == instrument &&
      voice->current_instr_voice == (uint8_t)instrument_voice)
    return;

  voice->current_instr = instrument;
  voice->current_instr_voice = (uint8_t)instrument_voice;

  data = &instrument->voice[instrument_voice];
  modulating = ((data->feedback & 0x01u) == 0u);

  /* Doom loads operator 2 (carrier) first and keeps it at minimum output until
   * SetVoiceVolume. In additive mode operator 1 is also audible, so it too is
   * initially silenced. */
  gm_load_operator(opl, gm_car_operator[physical], &data->carrier, 1,
                   &voice->car_volume);
  gm_load_operator(opl, gm_mod_operator[physical], &data->modulator,
                   !modulating, &voice->mod_volume);

  gm_write(opl,
           (uint16_t)(GM_OPL_REG_FEEDBACK + (uint16_t)physical),
           (uint8_t)(data->feedback | voice->reg_pan));

  voice->priority =
      (uint8_t)((0x0fu - (data->carrier.attack >> 4)) +
                (0x0fu - (data->carrier.sustain & 0x0fu)));
}

static void gm_set_voice_volume(snd_genmidi_opl_t SND_PTR *opl,
                                int physical, unsigned int volume) {
  snd_genmidi_opl_voice_t SND_PTR *voice = &opl->voice[physical];
  const snd_genmidi_opl_patch_voice_t SND_PTR *opl_voice;
  unsigned int midi_volume;
  unsigned int full_volume;
  unsigned int car_volume;
  unsigned int mod_volume;

  if (!voice->current_instr)
    return;

  if (volume > 127u)
    volume = 127u;
  voice->note_volume = (uint8_t)volume;

  opl_voice =
      &voice->current_instr->voice[voice->current_instr_voice];

  midi_volume =
      2u * ((unsigned int)gm_volume_mapping[
                opl->channel[voice->dmx_channel].volume] + 1u);

  full_volume =
      ((unsigned int)gm_volume_mapping[voice->note_volume] *
       midi_volume) >> 9;

  if (full_volume > 63u)
    full_volume = 63u;

  car_volume = 0x3fu - full_volume;

  if (car_volume != (unsigned int)(voice->car_volume & 0x3fu)) {
    voice->car_volume =
        (uint8_t)(car_volume | (voice->car_volume & 0xc0u));

    gm_write(opl,
             (uint16_t)(GM_OPL_REG_LEVEL +
                        gm_car_operator[physical]),
             voice->car_volume);

    /* Preserve DMX's AM/additive quirk: the modulator starts from its original
     * GENMIDI TL and is only raised to the carrier attenuation floor. */
    if ((opl_voice->feedback & 0x01u) != 0u &&
        opl_voice->modulator.level != 0x3fu) {
      mod_volume = opl_voice->modulator.level;
      if (mod_volume < car_volume)
        mod_volume = car_volume;

      mod_volume |= voice->mod_volume & 0xc0u;

      if (mod_volume != (unsigned int)voice->mod_volume) {
        voice->mod_volume = (uint8_t)mod_volume;
        gm_write(opl,
                 (uint16_t)(GM_OPL_REG_LEVEL +
                            gm_mod_operator[physical]),
                 (uint8_t)(mod_volume |
                           (opl_voice->modulator.scale & 0xc0u)));
      }
    }
  }
}

static uint16_t gm_frequency_for_voice(
    const snd_genmidi_opl_t SND_PTR *opl,
    const snd_genmidi_opl_voice_t SND_PTR *voice) {
  const snd_genmidi_opl_patch_voice_t SND_PTR *gm_voice;
  int freq_index;
  unsigned int octave;
  unsigned int sub_index;
  int note;
  int split;

  note = (int)voice->note;
  gm_voice =
      &voice->current_instr->voice[voice->current_instr_voice];

  if ((voice->current_instr->flags &
       SND_GENMIDI_OPL_FLAG_FIXED) == 0u)
    note += (int)gm_voice->base_note_offset;

  while (note < 0)
    note += 12;
  while (note > 95)
    note -= 12;

  freq_index =
      64 + 32 * note +
      (int)opl->channel[voice->dmx_channel].bend;

  if (voice->current_instr_voice != 0u)
    freq_index +=
        (int)(voice->current_instr->fine_tuning / 2u) - 64;

  if (freq_index < 0)
    freq_index = 0;

  split = (opl->freq_model ==
           (uint8_t)SND_GENMIDI_OPL_FREQ_DMX_BUG_283)
              ? 283
              : 284;

  if (freq_index < split)
    return gm_frequency_curve[freq_index];

  sub_index =
      (unsigned int)(freq_index - split) % (12u * 32u);
  octave =
      (unsigned int)(freq_index - split) / (12u * 32u);

  if (octave >= 7u)
    octave = 7u;

  return (uint16_t)(
      gm_frequency_curve[sub_index + (unsigned int)split] |
      (uint16_t)(octave << 10));
}

static void gm_update_voice_frequency(
    snd_genmidi_opl_t SND_PTR *opl, int physical) {
  snd_genmidi_opl_voice_t SND_PTR *voice = &opl->voice[physical];
  unsigned int freq;

  if (!voice->active || !voice->current_instr)
    return;

  freq = gm_frequency_for_voice(opl, voice);

  if ((unsigned int)voice->freq != freq) {
    gm_write(opl,
             (uint16_t)(GM_OPL_REG_FREQ_1 +
                        (uint16_t)physical),
             (uint8_t)(freq & 0xffu));
    gm_write(opl,
             (uint16_t)(GM_OPL_REG_FREQ_2 +
                        (uint16_t)physical),
             (uint8_t)((freq >> 8) | 0x20u));

    voice->freq = (uint16_t)freq;
  }
}

static int gm_voice_key_on(
    snd_genmidi_opl_t SND_PTR *opl, int dmx_channel,
    const snd_genmidi_opl_instrument_t SND_PTR *instrument,
    unsigned int instrument_voice, unsigned int note,
    unsigned int key, unsigned int volume) {
  uint8_t physical;
  snd_genmidi_opl_voice_t SND_PTR *voice;

  physical = gm_get_free_voice(opl);
  if (physical == 0xffu)
    return 0;

  voice = &opl->voice[physical];
  voice->dmx_channel = (uint8_t)dmx_channel;
  voice->key = (uint8_t)key;

  if ((instrument->flags & SND_GENMIDI_OPL_FLAG_FIXED) != 0u)
    voice->note = instrument->fixed_note;
  else
    voice->note = (uint8_t)note;

  voice->reg_pan = opl->channel[dmx_channel].pan;

  gm_set_voice_instrument(opl, (int)physical, instrument,
                          instrument_voice);
  gm_set_voice_volume(opl, (int)physical, volume);

  voice->freq = 0u;
  gm_update_voice_frequency(opl, (int)physical);

  ++opl->opl_voices_started;
  return 1;
}

static void gm_key_on_event(snd_genmidi_opl_t SND_PTR *opl,
                            int midi_channel, int key,
                            int volume) {
  const snd_genmidi_opl_instrument_t SND_PTR *instrument;
  int dmx_channel;
  unsigned int note;
  int double_voice;

  if (!opl || volume <= 0)
    return;

  dmx_channel = gm_dmx_channel_for_midi(midi_channel);
  note = (unsigned int)key;

  instrument =
      gm_instrument_for(opl, midi_channel, dmx_channel, key);
  if (!instrument)
    return;

  if (midi_channel == (int)SND_MIDI_GM_PERCUSSION_CHANNEL)
    note = 60u;

  double_voice =
      (instrument->flags & SND_GENMIDI_OPL_FLAG_2VOICE) != 0u;

  ++opl->midi_notes_started;

  /* Doom 1.9 frees at most one existing voice for a whole key-on event.
   * Primary gets the newly freed slot. A double-voice secondary is simply
   * dropped if no second free physical channel remains. */
  if (opl->free_count == 0u)
    gm_replace_existing_voice(opl);

  (void)gm_voice_key_on(opl, dmx_channel, instrument, 0u, note,
                        (unsigned int)key, (unsigned int)volume);

  if (double_voice) {
    if (!gm_voice_key_on(opl, dmx_channel, instrument, 1u, note,
                         (unsigned int)key, (unsigned int)volume))
      ++opl->secondary_voices_dropped;
  }
}

static void gm_key_off_event(snd_genmidi_opl_t SND_PTR *opl,
                             int midi_channel, int key) {
  int dmx_channel = gm_dmx_channel_for_midi(midi_channel);
  int i;

  for (i = 0; i < (int)opl->alloc_count; ++i) {
    snd_genmidi_opl_voice_t SND_PTR *voice =
        &opl->voice[opl->alloc_list[i]];

    if (voice->dmx_channel == (uint8_t)dmx_channel &&
        voice->key == (uint8_t)key) {
      gm_release_voice(opl, i, 0);
      --i;
    }
  }
}

static void gm_all_notes_off(snd_genmidi_opl_t SND_PTR *opl,
                             int dmx_channel) {
  int i;

  for (i = 0; i < (int)opl->alloc_count; ++i) {
    const snd_genmidi_opl_voice_t SND_PTR *voice =
        &opl->voice[opl->alloc_list[i]];

    if (voice->dmx_channel == (uint8_t)dmx_channel) {
      gm_release_voice(opl, i, 0);
      --i;
    }
  }
}

static void gm_set_channel_volume(snd_genmidi_opl_t SND_PTR *opl,
                                  int dmx_channel,
                                  unsigned int volume) {
  int i;

  if (volume > 127u)
    volume = 127u;

  opl->channel[dmx_channel].volume_base = (uint8_t)volume;
  opl->channel[dmx_channel].volume = (uint8_t)volume;

  for (i = 0; i < (int)opl->alloc_count; ++i) {
    int physical = (int)opl->alloc_list[i];
    snd_genmidi_opl_voice_t SND_PTR *voice =
        &opl->voice[physical];

    if (voice->dmx_channel == (uint8_t)dmx_channel)
      gm_set_voice_volume(opl, physical, voice->note_volume);
  }
}

static void gm_pitch_bend_event(snd_genmidi_opl_t SND_PTR *opl,
                                int dmx_channel,
                                uint8_t bend_msb) {
  uint8_t not_updated[SND_GENMIDI_OPL_VOICES];
  uint8_t updated[SND_GENMIDI_OPL_VOICES];
  int not_count = 0;
  int updated_count = 0;
  int i;

  opl->channel[dmx_channel].bend = (int16_t)((int)bend_msb - 64);

  /* Doom updates matching voices and moves them to the end of the allocated
   * list. This has no immediate sonic purpose, but it changes the next voice
   * chosen by the >= tie in ReplaceExistingVoice. */
  for (i = 0; i < (int)opl->alloc_count; ++i) {
    uint8_t physical = opl->alloc_list[i];

    if (opl->voice[physical].dmx_channel ==
        (uint8_t)dmx_channel) {
      gm_update_voice_frequency(opl, (int)physical);
      updated[updated_count++] = physical;
    } else {
      not_updated[not_count++] = physical;
    }
  }

  for (i = 0; i < not_count; ++i)
    opl->alloc_list[i] = not_updated[i];
  for (i = 0; i < updated_count; ++i)
    opl->alloc_list[not_count + i] = updated[i];
}

static void gm_apply_event(snd_genmidi_opl_t SND_PTR *opl,
                           const snd_genmidi_opl_event_t SND_PTR *event) {
  uint8_t type;
  int midi_channel;
  int dmx_channel;

  type = (uint8_t)(event->msg.status & 0xf0u);
  midi_channel = (int)(event->msg.status & 0x0fu);
  dmx_channel = gm_dmx_channel_for_midi(midi_channel);

  switch (type) {
  case SND_MIDI_NOTE_ON:
    gm_key_on_event(opl, midi_channel, (int)event->msg.data1,
                    (int)event->msg.data2);
    break;

  case SND_MIDI_NOTE_OFF:
    gm_key_off_event(opl, midi_channel, (int)event->msg.data1);
    break;

  case SND_MIDI_PROGRAM_CHANGE:
    opl->channel[dmx_channel].program = event->msg.data1;
    break;

  case SND_MIDI_PITCH_BEND:
    /* Only the MSB is considered by Doom. */
    gm_pitch_bend_event(opl, dmx_channel, event->msg.data2);
    break;

  case SND_MIDI_CONTROL_CHANGE:
    switch (event->msg.data1) {
    case SND_MIDI_CC_VOLUME:
      gm_set_channel_volume(opl, dmx_channel,
                            (unsigned int)event->msg.data2);
      break;

    case SND_MIDI_CC_PAN:
      /* In the strict nine-channel OPL2 path, DMX's pan handler does nothing.
       * OPL3 uses C0 stereo bits, but OPL3 mode is intentionally not enabled
       * by this backend. */
      break;

    case SND_MIDI_CC_ALL_NOTES_OFF:
      gm_all_notes_off(opl, dmx_channel);
      break;

    default:
      /* Vanilla OPL playback ignores expression, sustain, all-sound-off,
       * reset-controllers, mono/poly mode, reverb, chorus, etc. */
      break;
    }
    break;

  default:
    break;
  }
}

void MW_OPL_TIME_CRITICAL(snd_genmidi_opl_mix_block)(
    snd_genmidi_opl_t SND_PTR *opl,
    snd_mixer_t SND_PTR *m) {
  long block_start;
  int frame;

  if (!opl || !m || m->block_frames <= 0)
    return;

  block_start = m->block_frame;
  if (m->span_1 <= m->span_0)
    return;

  snd_touch_block(m);

  for (frame = m->span_0; frame < m->span_1; ++frame) {
    long absolute_frame = block_start + (long)frame;
    snd_genmidi_opl_event_t SND_PTR *event;
    int16_t chip_sample[2];
    long left;
    long right;

    opl->current_frame = absolute_frame;
    event = gm_front_event(opl);

    while (event && event->frame <= absolute_frame) {
      gm_apply_event(opl, event);
      gm_pop_event(opl);
      event = gm_front_event(opl);
    }

    OPL3_GenerateResampled(&opl->chip, chip_sample);
    left =
        ((long)chip_sample[0] * (long)opl->output_gain) >> 8;
    right =
        ((long)chip_sample[1] * (long)opl->output_gain) >> 8;

    if (m->channels == 2) {
      long index = (long)frame * 2L;
      snd_block_add(m, index, left);
      snd_block_add(m, index + 1L, right);
    } else {
      snd_block_add(m, (long)frame, (left + right) / 2L);
    }
  }
}

void snd_genmidi_opl_all_sound_off(snd_genmidi_opl_t SND_PTR *opl) {
  if (!opl)
    return;

  while (opl->alloc_count > 0u)
    gm_release_voice(opl, 0, 0);

  opl->event_head = 0;
  opl->event_count = 0;
}

int snd_genmidi_opl_active_voices(
    const snd_genmidi_opl_t SND_PTR *opl) {
  return opl ? (int)opl->alloc_count : 0;
}

int snd_genmidi_opl_pending_events(
    const snd_genmidi_opl_t SND_PTR *opl) {
  return opl ? opl->event_count : 0;
}
