#include "snd_genmidi.h"
#include "snd_midi.h"
#include "snd_midi_fm.h"

#include <stdio.h>
#include <string.h>

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

static uint8_t *raw_instr(uint8_t *bank, int index) {
  return bank + SND_GENMIDI_HEADER_BYTES +
         (uint32_t)index * SND_GENMIDI_RAW_INSTRUMENT_BYTES;
}

static void set_op(uint8_t *p, uint8_t tremolo, uint8_t attack,
                   uint8_t sustain, uint8_t waveform, uint8_t scale,
                   uint8_t level) {
  p[0] = tremolo;
  p[1] = attack;
  p[2] = sustain;
  p[3] = waveform;
  p[4] = scale;
  p[5] = level;
}

static void set_voice(uint8_t *p, uint8_t feedback, int16_t base_note,
                      uint8_t mod_level, uint8_t car_level) {
  set_op(p + 0, 0x21u, 0xf4u, 0x35u, 0u, 0u, mod_level);
  p[6] = feedback;
  set_op(p + 7, 0x21u, 0xf4u, 0x35u, 0u, 0u, car_level);
  p[13] = 0u;
  wr16(p + 14, (uint16_t)base_note);
}

static void make_bank(uint8_t *data, uint32_t bytes) {
  uint8_t *p;
  memset(data, 0, bytes);
  if (bytes < SND_GENMIDI_MIN_BYTES)
    return;
  memcpy(data, "#OPL_II#", 8u);

  p = raw_instr(data, 0);
  wr16(p + 0, SND_GENMIDI_FLAG_2VOICE);
  p[2] = 160u;
  p[3] = 64u;
  set_voice(p + 4, 0x06u, -12, 8u, 37u);  /* FM, feedback 3 */
  set_voice(p + 20, 0x05u, 7, 16u, 21u);  /* additive, feedback 2 */

  p = raw_instr(data, 1);
  wr16(p + 0, SND_GENMIDI_FLAG_FIXED);
  p[2] = 128u;
  p[3] = 72u;
  set_voice(p + 4, 0x02u, 24, 0u, 0u);

  p = raw_instr(data, SND_GENMIDI_PROGRAM_COUNT);
  wr16(p + 0, SND_GENMIDI_FLAG_2VOICE);
  p[2] = 128u;
  set_voice(p + 4, 0x04u, -2, 4u, 0u);
  set_voice(p + 20, 0x04u, 3, 4u, 0u);
}

static void test_validation(void) {
  uint8_t data[SND_GENMIDI_MIN_BYTES];
  snd_genmidi_bank_t bank;

  printf("genmidi validation\n");
  make_bank(data, sizeof(data));
  CHECK(snd_genmidi_load(&bank, data, sizeof(data)) == SND_GENMIDI_OK,
        "minimum header+instrument table accepted");
  CHECK(bank.has_names == 0u, "minimum bank reports no names");
  CHECK(snd_genmidi_load(&bank, data, SND_GENMIDI_MIN_BYTES - 1u) ==
            SND_GENMIDI_ERR_TRUNCATED,
        "truncated bank rejected");
  data[0] = 'X';
  CHECK(snd_genmidi_load(&bank, data, sizeof(data)) == SND_GENMIDI_ERR_HEADER,
        "bad magic rejected");
  CHECK(snd_genmidi_load(NULL, data, sizeof(data)) == SND_GENMIDI_ERR_ARGUMENT,
        "null output rejected");
}

static void test_translation(void) {
  uint8_t data[SND_GENMIDI_CANONICAL_BYTES];
  snd_genmidi_bank_t bank;
  const snd_fm_instrument_t *p0;
  const snd_fm_instrument_t *p1;
  const snd_fm_instrument_t *drum;

  printf("genmidi instrument translation\n");
  make_bank(data, sizeof(data));
  CHECK(snd_genmidi_load(&bank, data, sizeof(data)) == SND_GENMIDI_OK,
        "canonical bank loads");
  CHECK(bank.has_names == 1u, "canonical-size bank reports names present");
  CHECK(bank.bank.program_count == 128, "128 melodic programs exposed");
  CHECK(bank.bank.percussion_first_note == 35,
        "percussion starts on MIDI note 35");
  CHECK(bank.bank.percussion_count == 47, "47 percussion entries exposed");

  p0 = &bank.programs[0];
  CHECK(p0->layer_count == 2u, "double-voice flag becomes two FM layers");
  CHECK(p0->fixed_note == SND_FM_FIXED_NOTE_NONE,
        "melodic non-fixed program stays key tracked");
  CHECK(p0->layer[0].algorithm == SND_FM_ALGORITHM_FM,
        "connection bit zero becomes FM algorithm");
  CHECK(p0->layer[1].algorithm == SND_FM_ALGORITHM_ADDITIVE,
        "connection bit one becomes additive algorithm");
  CHECK(p0->layer[0].feedback == 36u, "three-bit feedback translated");
  CHECK(p0->layer[0].detune_128 == -1536,
        "voice 0 base-note offset becomes pitch offset");
  CHECK(p0->layer[1].detune_128 == 960,
        "voice 1 base-note plus fine tuning translated");
  CHECK(p0->layer[0].carrier.level == SND_GAIN_UNITY,
        "DMX carrier level is driven from MIDI gain");
  CHECK(p0->layer[0].modulator.multiple == 256u,
        "OPL multiple 1 becomes 1.0 in 8.8");

  p1 = &bank.programs[1];
  CHECK(p1->fixed_note == 72u, "fixed-note flag preserved");
  CHECK(p1->layer[0].detune_128 == 0,
        "fixed-note instrument ignores base-note offset");

  drum = &bank.percussion[0];
  CHECK(drum->fixed_note == 60u,
        "non-fixed DMX percussion uses MIDI note 60 as its base");
  CHECK(drum->layer_count == 2u, "percussion can preserve double voice");
  CHECK(drum->layer[0].detune_128 == -256,
        "percussion voice 0 base offset preserved");
  CHECK(drum->layer[1].detune_128 == 384,
        "percussion voice 1 base offset preserved");
}

static void test_end_to_end_audio(void) {
  uint8_t data[SND_GENMIDI_MIN_BYTES];
  snd_genmidi_bank_t bank;
  snd_midi_t midi;
  snd_midi_fm_t fm;
  snd_mixer_t mix;
  snd_sample_t block[256];
  long energy = 0;
  int i;

  printf("genmidi -> midi fm audio\n");
  make_bank(data, sizeof(data));
  CHECK(snd_genmidi_load(&bank, data, sizeof(data)) == SND_GENMIDI_OK,
        "synthetic bank loads for rendering");

  snd_midi_init(&midi);
  snd_midi_fm_init(&fm);
  snd_midi_fm_set_bank(&fm, snd_genmidi_midi_fm_bank(&bank));
  snd_midi_fm_bind(&fm, &midi);
  snd_init(&mix, 22050, 1, block, 256, NULL, NULL);
  snd_begin_block(&mix, 0, 256);
  snd_clear_block(&mix);

  (void)snd_midi_message(&midi, 0, 0xc0u, 0u, 0u);
  (void)snd_midi_message(&midi, 0, 0x90u, 60u, 110u);
  snd_midi_fm_mix_block(&fm, &mix);

  for (i = 0; i < 256; ++i) {
    long v = (long)SND_SAMPLE_TO_MIX(block[i]);
    energy += (v < 0) ? -v : v;
  }
  CHECK(energy > 0, "translated GENMIDI patch produces audio");
  CHECK(fm.fm_voices_started == 2u,
        "double-voice program starts two software FM voices");
}

int main(void) {
  test_validation();
  test_translation();
  test_end_to_end_audio();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
