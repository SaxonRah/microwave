#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "snd_pack.h"
#include <string.h>

static uint16_t snd_rd16(const unsigned char *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t snd_rd32(const unsigned char *p) {
  return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int snd_pack_open(snd_pack_t SND_PTR *pack, const char *path) {
  unsigned char header[SND_PACK_HEADER_SIZE];
  uint32_t magic;
  uint16_t count;
  uint32_t data_offset;
  uint16_t i;

  if (!pack || !path)
    return 0;
  memset(pack, 0, sizeof(*pack));
  pack->fp = fopen(path, "rb");
  if (!pack->fp)
    return 0;

  if (fread(header, 1, sizeof(header), pack->fp) != sizeof(header)) {
    snd_pack_close(pack);
    return 0;
  }

  magic = snd_rd32(header);
  if (magic != SND_PACK_MAGIC_MW && magic != SND_PACK_MAGIC_MR) {
    snd_pack_close(pack);
    return 0;
  }

  count = snd_rd16(header + 4);
  data_offset = snd_rd32(header + 8);
  if (count > SND_PACK_MAX_ENTRIES) {
    snd_pack_close(pack);
    return 0;
  }
  if (data_offset <
      (uint32_t)(SND_PACK_HEADER_SIZE +
                 ((uint32_t)count * (uint32_t)SND_PACK_DIR_ENTRY_SIZE))) {
    snd_pack_close(pack);
    return 0;
  }

  pack->magic = magic;
  pack->count = count;
  pack->data_offset = data_offset;

  for (i = 0; i < count; ++i) {
    unsigned char dir[SND_PACK_DIR_ENTRY_SIZE];
    unsigned int n;
    if (fread(dir, 1, sizeof(dir), pack->fp) != sizeof(dir)) {
      snd_pack_close(pack);
      return 0;
    }
    n = dir[0];
    if (n > 31u)
      n = 31u;
    memset(pack->entries[i].name, 0, sizeof(pack->entries[i].name));
    memcpy(pack->entries[i].name, dir + 1, n);
    pack->entries[i].kind = snd_rd16(dir + 32);
    pack->entries[i].offset = snd_rd32(dir + 36);
    pack->entries[i].size = snd_rd32(dir + 40);
  }

  return 1;
}

void snd_pack_close(snd_pack_t SND_PTR *pack) {
  if (!pack)
    return;
  if (pack->fp)
    fclose(pack->fp);
  memset(pack, 0, sizeof(*pack));
}

int snd_pack_is_microrender(const snd_pack_t SND_PTR *pack) {
  return (pack && pack->magic == SND_PACK_MAGIC_MR) ? 1 : 0;
}

const snd_pack_entry_t SND_PTR *snd_pack_find(const snd_pack_t SND_PTR *pack,
                                              const char *name) {
  uint16_t i;
  if (!pack || !name)
    return 0;
  for (i = 0; i < pack->count; ++i) {
    if (strcmp(pack->entries[i].name, name) == 0)
      return &pack->entries[i];
  }
  return 0;
}

const char *snd_pack_kind_name(uint16_t kind) {
  switch (kind) {
  case SND_PACK_ENTRY_AUDIO_U8:
    return "audio_u8";
  case SND_PACK_ENTRY_PROJECT_INFO:
    return "project_info";
  case SND_PACK_ENTRY_AUDIO_S16:
    return "audio_s16";
  case SND_PACK_ENTRY_AUDIO_ADPCM4:
    return "audio_adpcm4";
  case SND_PACK_ENTRY_SONG:
    return "song";
  case SND_PACK_ENTRY_INSTRUMENT:
    return "instrument";
  default:
    /* Sprite, tilemap and palette entries in a shared MRP1 belong to the
       renderer. Naming them here would imply this library can use them. */
    return "other";
  }
}

int snd_pack_read(const snd_pack_t SND_PTR *pack,
                  const snd_pack_entry_t SND_PTR *entry, void SND_PTR *dst,
                  uint32_t max_bytes, uint32_t SND_PTR *out_bytes) {
  long where;
  size_t got;

  if (out_bytes)
    *out_bytes = 0u;
  if (!pack || !pack->fp || !entry || !dst)
    return 0;
  if (entry->size > max_bytes)
    return 0;
  if (entry->size == 0u)
    return 0;

  where = (long)pack->data_offset + (long)entry->offset;
  if (fseek(pack->fp, where, SEEK_SET) != 0)
    return 0;

  got = fread(dst, 1, (size_t)entry->size, pack->fp);
  if (got != (size_t)entry->size)
    return 0;

  if (out_bytes)
    *out_bytes = entry->size;
  return 1;
}

/* Every audio payload starts with the descriptor MicroRender's packer emits:
   u16 rate, u16 bits, u32 byte count. */
#define SND_AUDIO_HDR 8u

uint32_t snd_pack_clip_bytes(const snd_pack_t SND_PTR *pack, const char *name) {
  const snd_pack_entry_t SND_PTR *e = snd_pack_find(pack, name);
  if (!e)
    return 0u;
  switch (e->kind) {
  case SND_PACK_ENTRY_AUDIO_U8:
  case SND_PACK_ENTRY_AUDIO_S16:
  case SND_PACK_ENTRY_AUDIO_ADPCM4:
    return e->size;
  default:
    return 0u;
  }
}

int snd_pack_load_clip(const snd_pack_t SND_PTR *pack, const char *name,
                       void SND_PTR *dst, uint32_t max_bytes,
                       snd_clip_t SND_PTR *out) {
  const snd_pack_entry_t SND_PTR *e;
  unsigned char SND_PTR *bytes = (unsigned char SND_PTR *)dst;
  uint32_t got = 0u;
  uint32_t rate, bits, payload;

  if (!pack || !name || !dst || !out)
    return 0;

  e = snd_pack_find(pack, name);
  if (!e)
    return 0;
  if (e->kind != SND_PACK_ENTRY_AUDIO_U8 && e->kind != SND_PACK_ENTRY_AUDIO_S16 &&
      e->kind != SND_PACK_ENTRY_AUDIO_ADPCM4)
    return 0;
  if (e->size <= SND_AUDIO_HDR)
    return 0;

  if (!snd_pack_read(pack, e, dst, max_bytes, &got))
    return 0;
  if (got <= SND_AUDIO_HDR)
    return 0;

  rate = snd_rd16(bytes);
  bits = snd_rd16(bytes + 2);
  payload = snd_rd32(bytes + 4);

  /* Never trust the declared length over the length actually read. */
  if (payload > got - SND_AUDIO_HDR)
    payload = got - SND_AUDIO_HDR;
  if (payload == 0u || rate == 0u)
    return 0;

  memset(out, 0, sizeof(*out));
  out->data = bytes + SND_AUDIO_HDR;
  out->bytes = payload;
  out->rate = (int)rate;
  out->channels = 1u;

  switch (e->kind) {
  case SND_PACK_ENTRY_AUDIO_U8:
    if (bits != 8u)
      return 0;
    out->flags = (uint8_t)SND_CLIP_PCM8;
    break;
  case SND_PACK_ENTRY_AUDIO_S16:
    if (bits != 16u)
      return 0;
    out->flags = (uint8_t)SND_CLIP_PCM16;
    break;
  case SND_PACK_ENTRY_AUDIO_ADPCM4:
    if (bits != 4u)
      return 0;
    out->flags = (uint8_t)SND_CLIP_ADPCM4;
    break;
  default:
    return 0;
  }

  out->frames = snd_clip_frames_for_bytes(out->flags, out->channels, payload);
  out->loop_start = 0u;
  out->loop_end = 0u;

  /* The validate call is the whole point of routing pack loads through here:
     anything that came off disk is untrusted, and the mix loops do not
     re-check bounds per block. */
  return snd_clip_validate(out);
}
