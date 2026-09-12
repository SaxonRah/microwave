#include "snd_dmx_mix.h"
#include "snd_dmx_sfx.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define EXPECT(expr)                                                           \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);         \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

static void wr16le(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)(value & 0xffu);
  p[1] = (uint8_t)(value >> 8u);
}

static void wr32le(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value & 0xffu);
  p[1] = (uint8_t)((value >> 8u) & 0xffu);
  p[2] = (uint8_t)((value >> 16u) & 0xffu);
  p[3] = (uint8_t)((value >> 24u) & 0xffu);
}

static void make_valid_lump(uint8_t *raw, uint32_t bytes) {
  unsigned int i;

  memset(raw, 0, bytes);
  wr16le(raw + 0, SND_DMX_SFX_TYPE_PCM);
  wr16le(raw + 2, 11025u);
  wr32le(raw + 4, 49u);

  for (i = 0u; i < 17u; ++i)
    raw[SND_DMX_SFX_PCM_OFFSET + i] = (uint8_t)(0x70u + i);
}

static void test_decoder(void) {
  uint8_t raw[SND_DMX_SFX_HEADER_BYTES + 49u];
  snd_clip_t clip;
  int error;

  make_valid_lump(raw, (uint32_t)sizeof(raw));
  memset(&clip, 0xcc, sizeof(clip));

  error = snd_dmx_sfx_load(&clip, raw, (uint32_t)sizeof(raw));
  EXPECT(error == SND_DMX_SFX_OK);
  EXPECT(clip.data == raw + SND_DMX_SFX_PCM_OFFSET);
  EXPECT(clip.bytes == 17u);
  EXPECT(clip.frames == 17u);
  EXPECT(clip.rate == 11025);
  EXPECT(clip.channels == 1u);
  EXPECT(clip.flags == SND_CLIP_PCM8);
  EXPECT(snd_clip_validate(&clip) == 1);

  EXPECT(snd_dmx_sfx_load(NULL, raw, (uint32_t)sizeof(raw)) ==
         SND_DMX_SFX_ERR_ARGUMENT);
  EXPECT(snd_dmx_sfx_load(&clip, NULL, (uint32_t)sizeof(raw)) ==
         SND_DMX_SFX_ERR_ARGUMENT);
  EXPECT(snd_dmx_sfx_load(&clip, raw, 7u) == SND_DMX_SFX_ERR_TRUNCATED);

  make_valid_lump(raw, (uint32_t)sizeof(raw));
  wr16le(raw + 0, 2u);
  EXPECT(snd_dmx_sfx_load(&clip, raw, (uint32_t)sizeof(raw)) ==
         SND_DMX_SFX_ERR_TYPE);

  make_valid_lump(raw, (uint32_t)sizeof(raw));
  wr16le(raw + 2, 0u);
  EXPECT(snd_dmx_sfx_load(&clip, raw, (uint32_t)sizeof(raw)) ==
         SND_DMX_SFX_ERR_RATE);

  make_valid_lump(raw, (uint32_t)sizeof(raw));
  wr32le(raw + 4, 48u);
  EXPECT(snd_dmx_sfx_load(&clip, raw, (uint32_t)sizeof(raw)) ==
         SND_DMX_SFX_ERR_LENGTH);

  make_valid_lump(raw, (uint32_t)sizeof(raw));
  EXPECT(snd_dmx_sfx_load(&clip, raw, (uint32_t)sizeof(raw) - 1u) ==
         SND_DMX_SFX_ERR_TRUNCATED);

  EXPECT(strcmp(snd_dmx_sfx_error_string(SND_DMX_SFX_OK), "ok") == 0);
  EXPECT(strcmp(snd_dmx_sfx_error_string(999), "unknown DMX sound error") == 0);
}

static void test_gain_pan(void) {
  int16_t left;
  int16_t right;

  EXPECT(snd_dmx_gain_8_8(-1) == 0);
  EXPECT(snd_dmx_gain_8_8(0) == 0);
  EXPECT(snd_dmx_gain_8_8(3) == 0);
  EXPECT(snd_dmx_gain_8_8(4) == 4);
  EXPECT(snd_dmx_gain_8_8(255) == 256);
  EXPECT(snd_dmx_gain_8_8(999) == 256);

  left = -1;
  right = -1;
  snd_dmx_pan_gains_8_8(63, 0, 0, &left, &right);
  EXPECT(left == 256);
  EXPECT(right == 0);

  snd_dmx_pan_gains_8_8(63, 0, 1, &left, &right);
  EXPECT(left == 0);
  EXPECT(right == 256);

  snd_dmx_pan_gains_8_8(63, 127, 0, &left, &right);
  EXPECT(left == 125);
  EXPECT(right == 125);

  snd_dmx_pan_gains_8_8(999, -20, 0, &left, &right);
  EXPECT(left == 256);
  EXPECT(right == 0);

  snd_dmx_pan_gains_8_8(0, 254, 0, NULL, &right);
  EXPECT(right == 0);
}

int main(void) {
  test_decoder();
  test_gain_pan();

  if (g_failures != 0) {
    fprintf(stderr, "DMX tests: %d failure(s)\n", g_failures);
    return 1;
  }

  printf("DMX tests: pass\n");
  return 0;
}
