#ifndef SND_PACK_H
#define SND_PACK_H

#include "snd.h"
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pack reader.
 *
 * This deliberately opens two magics:
 *
 *   MWP1  MicroWave's own pack, produced by shared/tools/mw_pack.py
 *   MRP1  MicroRender's pack, produced by shared/tools/mr_pack.py
 *
 * The container layout is byte-identical -- 12-byte header, 44-byte directory
 * entries, kind-tagged payloads -- because MicroRender already reserved
 * entry kind 10 for 8-bit audio and there was no reason to invent a second
 * format that says the same thing.
 *
 * The practical consequence is the one that matters: if you already build a
 * GAME.MRP for MicroRender, MicroWave reads its audio entries out of that
 * file with no second asset pipeline and no duplicated bytes. If you are using
 * MicroWave on its own, mw_pack.py writes an MWP1 that also carries ADPCM and
 * song entries, which mr_pack.py has no reason to know about.
 */

#define SND_PACK_MAGIC_MW 0x3150574DuL /* 'M','W','P','1' little-endian */
#define SND_PACK_MAGIC_MR 0x3150524DuL /* 'M','R','P','1' little-endian */

#define SND_PACK_HEADER_SIZE 12u
#define SND_PACK_DIR_ENTRY_SIZE 44u

/* Kinds 1..11 are MicroRender's and are passed through unchanged so a shared
   pack stays readable by both libraries. Only 10 is audio. */
#define SND_PACK_ENTRY_AUDIO_U8 10u
#define SND_PACK_ENTRY_PROJECT_INFO 11u
/* MicroWave extensions. Numbered above MicroRender's range on purpose: an
   MRP1 pack will never contain them, and MicroRender's reader reports them as
   unknown rather than misreading them. */
#define SND_PACK_ENTRY_AUDIO_S16 32u
#define SND_PACK_ENTRY_AUDIO_ADPCM4 33u
#define SND_PACK_ENTRY_SONG 34u
#define SND_PACK_ENTRY_INSTRUMENT 35u

#ifndef SND_PACK_MAX_ENTRIES
#define SND_PACK_MAX_ENTRIES 96
#endif

typedef struct snd_pack_entry {
  char name[32];
  uint16_t kind;
  uint32_t offset;
  uint32_t size;
} snd_pack_entry_t;

typedef struct snd_pack {
  FILE *fp;
  uint16_t count;
  uint32_t data_offset;
  uint32_t magic; /* which of the two containers this turned out to be */
  snd_pack_entry_t entries[SND_PACK_MAX_ENTRIES];
} snd_pack_t;

int snd_pack_open(snd_pack_t SND_PTR *pack, const char *path);
void snd_pack_close(snd_pack_t SND_PTR *pack);
const snd_pack_entry_t SND_PTR *snd_pack_find(const snd_pack_t SND_PTR *pack,
                                              const char *name);
const char *snd_pack_kind_name(uint16_t kind);
int snd_pack_is_microrender(const snd_pack_t SND_PTR *pack);
int snd_pack_read(const snd_pack_t SND_PTR *pack,
                  const snd_pack_entry_t SND_PTR *entry, void SND_PTR *dst,
                  uint32_t max_bytes, uint32_t SND_PTR *out_bytes);

/* Load a named audio entry into a caller-owned buffer and describe it as a
   clip. Allocates nothing, and the clip is passed through snd_clip_validate()
   before it is returned, so a truncated or hostile pack cannot produce a
   mixable clip. Returns 1 on success.
 *
 * Every audio payload begins with the same six-byte descriptor MicroRender
 * writes: u16 rate, u16 bits, u32 byte count. */
int snd_pack_load_clip(const snd_pack_t SND_PTR *pack, const char *name,
                       void SND_PTR *dst, uint32_t max_bytes,
                       snd_clip_t SND_PTR *out);

/* Bytes the payload needs, so a caller can size a static buffer before
   loading. Returns 0 if the entry is missing or is not audio. */
uint32_t snd_pack_clip_bytes(const snd_pack_t SND_PTR *pack, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SND_PACK_H */
