#ifndef SND_DMX_MIX_H
#define SND_DMX_MIX_H

#include "snd_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional compatibility helpers for the classic Doom/DMX/Multivoc volume
 * path. These are deliberately separate from snd_dmx_sfx: decoding bytes and
 * reproducing a historical mixer gain law are independent concerns. */

/* Convert a 0..255 DMX/Multivoc channel volume to MicroWave 8.8 gain using
 * the historical volume >> 2 quantization (0..63). Values are clamped. */
int16_t snd_dmx_gain_8_8(int volume);

/* Convert Doom-style volume/separation into quantized left/right 8.8 gains.
 * `volume` is clamped to 0..255 and `separation` to 0..254. `reverse_stereo`
 * swaps the final gains. This helper owns only the gain/pan law; callers still
 * own distance attenuation, source tracking, priorities, and channel policy. */
void snd_dmx_pan_gains_8_8(int volume,
                           int separation,
                           int reverse_stereo,
                           int16_t SND_PTR *gain_l,
                           int16_t SND_PTR *gain_r);

#ifdef __cplusplus
}
#endif

#endif /* SND_DMX_MIX_H */
