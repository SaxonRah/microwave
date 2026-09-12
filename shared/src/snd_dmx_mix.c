#include "snd_dmx_mix.h"

#include <stddef.h>

static int snd_dmx_clampi(int value, int lo, int hi) {
  if (value < lo)
    return lo;
  if (value > hi)
    return hi;
  return value;
}

int16_t snd_dmx_gain_8_8(int volume) {
  int index;

  volume = snd_dmx_clampi(volume, 0, 255);
  index = volume >> 2;
  return (int16_t)((index * 256) / 63);
}

void snd_dmx_pan_gains_8_8(int volume,
                           int separation,
                           int reverse_stereo,
                           int16_t SND_PTR *gain_l,
                           int16_t SND_PTR *gain_r) {
  int left_volume;
  int right_volume;
  int16_t left;
  int16_t right;

  volume = snd_dmx_clampi(volume, 0, 255);
  separation = snd_dmx_clampi(separation, 0, 254);

  left_volume = ((254 - separation) * volume) / 63;
  right_volume = (separation * volume) / 63;

  left = snd_dmx_gain_8_8(left_volume);
  right = snd_dmx_gain_8_8(right_volume);

  if (reverse_stereo) {
    int16_t tmp = left;
    left = right;
    right = tmp;
  }

  if (gain_l != NULL)
    *gain_l = left;
  if (gain_r != NULL)
    *gain_r = right;
}
