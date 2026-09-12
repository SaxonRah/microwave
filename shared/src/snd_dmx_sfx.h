#ifndef SND_DMX_SFX_H
#define SND_DMX_SFX_H

#include "snd.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Doom/DMX type-3 SFX lump -> MicroWave clip adapter.
 *
 * This layer understands only the byte layout of a caller-owned DMX sound
 * lump. It owns no WAD API, allocator, file I/O, cache lifetime, channel
 * policy, spatialization, or playback handle:
 *
 *     caller-owned DMX bytes -> snd_dmx_sfx -> snd_clip_t
 *
 * The resulting clip points directly into the caller-owned input bytes, so the
 * caller must keep those bytes alive for as long as the clip can be played.
 */

#define SND_DMX_SFX_TYPE_PCM 3u
#define SND_DMX_SFX_HEADER_BYTES 8u
#define SND_DMX_SFX_PAD_BYTES 16u
#define SND_DMX_SFX_PCM_OFFSET                                             \
  (SND_DMX_SFX_HEADER_BYTES + SND_DMX_SFX_PAD_BYTES)
#define SND_DMX_SFX_TOTAL_PAD_BYTES (SND_DMX_SFX_PAD_BYTES * 2u)

/* FastDoom's proven type-3 path rejects declared payloads of 48 bytes or
 * fewer. That leaves at least 17 bytes of real PCM after removing the two
 * 16-byte DMX pad regions. */
#define SND_DMX_SFX_MIN_DECLARED_BYTES 49u

typedef enum snd_dmx_sfx_error {
  SND_DMX_SFX_OK = 0,
  SND_DMX_SFX_ERR_ARGUMENT = 1,
  SND_DMX_SFX_ERR_TRUNCATED = 2,
  SND_DMX_SFX_ERR_TYPE = 3,
  SND_DMX_SFX_ERR_LENGTH = 4,
  SND_DMX_SFX_ERR_RATE = 5,
  SND_DMX_SFX_ERR_CLIP = 6
} snd_dmx_sfx_error_t;

/* Decode a Doom/DMX type-3 sound lump without copying sample data.
 *
 * Layout:
 *   u16 type (= 3)
 *   u16 sample rate
 *   u32 declared payload bytes
 *   16 leading pad bytes
 *   unsigned 8-bit PCM
 *   16 trailing pad bytes
 *
 * `bytes` is the complete caller-owned lump size, including the 8-byte
 * header. Extra bytes after the declared payload are ignored. */
int snd_dmx_sfx_load(snd_clip_t SND_PTR *clip,
                     const void SND_PTR *data,
                     uint32_t bytes);

const char *snd_dmx_sfx_error_string(int error);

#ifdef __cplusplus
}
#endif

#endif /* SND_DMX_SFX_H */
