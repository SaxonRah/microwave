#ifndef SND_GENMIDI_H
#define SND_GENMIDI_H

#include "snd_midi_fm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DMX GENMIDI -> generic MicroWave FM-bank adapter.
 *
 * This layer understands the packed instrument-bank format used by Doom-family
 * GENMIDI lumps and nothing else.  It owns no WAD API, allocator, file I/O,
 * clock, MIDI player, or audio device:
 *
 *     caller-owned GENMIDI bytes -> snd_genmidi -> snd_midi_fm_bank_t
 *
 * The converted bank lives entirely in caller-owned snd_genmidi_bank_t.
 */

#define SND_GENMIDI_PROGRAM_COUNT 128
#define SND_GENMIDI_PERCUSSION_FIRST_NOTE 35
#define SND_GENMIDI_PERCUSSION_COUNT 47
#define SND_GENMIDI_INSTRUMENT_COUNT \
  (SND_GENMIDI_PROGRAM_COUNT + SND_GENMIDI_PERCUSSION_COUNT)
#define SND_GENMIDI_HEADER_BYTES 8u
#define SND_GENMIDI_RAW_INSTRUMENT_BYTES 36u
#define SND_GENMIDI_NAME_BYTES 32u
#define SND_GENMIDI_MIN_BYTES                                                \
  (SND_GENMIDI_HEADER_BYTES +                                               \
   SND_GENMIDI_INSTRUMENT_COUNT * SND_GENMIDI_RAW_INSTRUMENT_BYTES)
#define SND_GENMIDI_CANONICAL_BYTES                                          \
  (SND_GENMIDI_MIN_BYTES +                                                   \
   SND_GENMIDI_INSTRUMENT_COUNT * SND_GENMIDI_NAME_BYTES)

#define SND_GENMIDI_FLAG_FIXED 0x0001u
#define SND_GENMIDI_FLAG_2VOICE 0x0004u

typedef enum snd_genmidi_error {
  SND_GENMIDI_OK = 0,
  SND_GENMIDI_ERR_ARGUMENT = 1,
  SND_GENMIDI_ERR_HEADER = 2,
  SND_GENMIDI_ERR_TRUNCATED = 3
} snd_genmidi_error_t;

typedef struct snd_genmidi_bank {
  snd_fm_instrument_t programs[SND_GENMIDI_PROGRAM_COUNT];
  snd_fm_instrument_t percussion[SND_GENMIDI_PERCUSSION_COUNT];
  snd_midi_fm_bank_t bank;
  uint8_t has_names;
  uint8_t reserved[3];
} snd_genmidi_bank_t;

/* Decode a complete DMX instrument table.  The canonical Doom lump is 11908
 * bytes including 32-byte debug names, but only the 6308-byte header+instrument
 * table is required for synthesis.  Names are deliberately not copied into
 * the runtime bank. */
int snd_genmidi_load(snd_genmidi_bank_t SND_PTR *out,
                     const void SND_PTR *data, uint32_t bytes);

const snd_midi_fm_bank_t SND_PTR *
snd_genmidi_midi_fm_bank(const snd_genmidi_bank_t SND_PTR *bank);

const char *snd_genmidi_error_string(int error);

#ifdef __cplusplus
}
#endif

#endif /* SND_GENMIDI_H */
