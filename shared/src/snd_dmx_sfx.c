#include "snd_dmx_sfx.h"

#include <string.h>

static uint16_t snd_dmx_rd16le(const uint8_t SND_PTR *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t snd_dmx_rd32le(const uint8_t SND_PTR *p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int snd_dmx_sfx_load(snd_clip_t SND_PTR *clip,
                     const void SND_PTR *data,
                     uint32_t bytes) {
  const uint8_t SND_PTR *src;
  uint32_t declared_bytes;
  uint32_t pcm_bytes;
  uint16_t type;
  uint16_t rate;

  if (clip == NULL || data == NULL)
    return SND_DMX_SFX_ERR_ARGUMENT;

  memset(clip, 0, sizeof(*clip));

  if (bytes < SND_DMX_SFX_HEADER_BYTES)
    return SND_DMX_SFX_ERR_TRUNCATED;

  src = (const uint8_t SND_PTR *)data;
  type = snd_dmx_rd16le(src);
  if (type != (uint16_t)SND_DMX_SFX_TYPE_PCM)
    return SND_DMX_SFX_ERR_TYPE;

  rate = snd_dmx_rd16le(src + 2u);
  if (rate == 0u)
    return SND_DMX_SFX_ERR_RATE;

  declared_bytes = snd_dmx_rd32le(src + 4u);
  if (declared_bytes < SND_DMX_SFX_MIN_DECLARED_BYTES)
    return SND_DMX_SFX_ERR_LENGTH;

  if (declared_bytes > UINT32_MAX - SND_DMX_SFX_HEADER_BYTES)
    return SND_DMX_SFX_ERR_LENGTH;
  if (SND_DMX_SFX_HEADER_BYTES + declared_bytes > bytes)
    return SND_DMX_SFX_ERR_TRUNCATED;

  pcm_bytes = declared_bytes - SND_DMX_SFX_TOTAL_PAD_BYTES;

  clip->data = src + SND_DMX_SFX_PCM_OFFSET;
  clip->frames = pcm_bytes;
  clip->bytes = pcm_bytes;
  clip->loop_start = 0u;
  clip->loop_end = 0u;
  clip->rate = (int)rate;
  clip->channels = 1u;
  clip->flags = SND_CLIP_PCM8;

  if (!snd_clip_validate(clip)) {
    memset(clip, 0, sizeof(*clip));
    return SND_DMX_SFX_ERR_CLIP;
  }

  return SND_DMX_SFX_OK;
}

const char *snd_dmx_sfx_error_string(int error) {
  switch (error) {
  case SND_DMX_SFX_OK:
    return "ok";
  case SND_DMX_SFX_ERR_ARGUMENT:
    return "invalid argument";
  case SND_DMX_SFX_ERR_TRUNCATED:
    return "truncated DMX sound lump";
  case SND_DMX_SFX_ERR_TYPE:
    return "unsupported DMX sound type";
  case SND_DMX_SFX_ERR_LENGTH:
    return "invalid DMX sound length";
  case SND_DMX_SFX_ERR_RATE:
    return "invalid DMX sample rate";
  case SND_DMX_SFX_ERR_CLIP:
    return "decoded DMX clip failed validation";
  default:
    return "unknown DMX sound error";
  }
}
