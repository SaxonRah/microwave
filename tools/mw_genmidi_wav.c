#include "snd_genmidi.h"
#include "snd_midi.h"
#include "snd_midi_fm.h"
#include "snd_mus.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if SND_SAMPLE_FORMAT != SND_SAMPLE_FORMAT_S16
#error mw_genmidi_wav requires the normal MicroWave S16 logical sample format
#endif

#define MW_TOOL_DEFAULT_SECONDS 30
#define MW_TOOL_DEFAULT_RATE SND_DEFAULT_RATE
#define MW_TOOL_DEFAULT_BLOCK SND_DEFAULT_BLOCK_FRAMES
#define MW_TOOL_DEFAULT_VOICES 9
#define MW_TOOL_CHANNELS 2
#define MW_WAD_NAME_BYTES 8
#define MW_WAD_DIR_ENTRY_BYTES 16u

typedef struct mw_args {
  const char *wad_path;
  const char *music_name;
  const char *out_path;
  int seconds;
  int rate;
  int block;
  int voices;
  int fm_gain;
  int loop;
  int wide_accum;
  int scan_music;
} mw_args_t;

typedef struct mw_wad {
  FILE *fp;
  uint32_t bytes;
  uint32_t directory_offset;
  uint32_t lump_count;
  char type[5];
} mw_wad_t;

typedef struct mw_lump {
  uint32_t index;
  uint32_t offset;
  uint32_t bytes;
  char name[MW_WAD_NAME_BYTES + 1];
} mw_lump_t;

typedef struct mw_render_stats {
  uint64_t samples_written;
  uint64_t full_scale_samples;
  uint64_t preclip_samples;
  uint64_t preclip_peak_abs;
  int peak_abs;
  unsigned long midi_messages;
  unsigned long mus_loops;
  unsigned long midi_notes_started;
  unsigned long fm_voices_started;
  unsigned long fm_voices_released;
  unsigned long fm_voices_stolen;
  unsigned long midi_events_dropped;
} mw_render_stats_t;

static uint32_t mw_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void mw_wr16(FILE *fp, uint16_t v) {
  uint8_t b[2];
  b[0] = (uint8_t)(v & 0xffu);
  b[1] = (uint8_t)((v >> 8) & 0xffu);
  (void)fwrite(b, 1u, sizeof(b), fp);
}

static void mw_wr32(FILE *fp, uint32_t v) {
  uint8_t b[4];
  b[0] = (uint8_t)(v & 0xffu);
  b[1] = (uint8_t)((v >> 8) & 0xffu);
  b[2] = (uint8_t)((v >> 16) & 0xffu);
  b[3] = (uint8_t)((v >> 24) & 0xffu);
  (void)fwrite(b, 1u, sizeof(b), fp);
}

static void mw_usage(const char *exe) {
  printf("MicroWave real GENMIDI + MUS -> WAV validator\n\n");
  printf("usage:\n");
  printf("  %s --wad FILE --music LUMP --out FILE [options]\n", exe);
  printf("  %s --wad FILE --scan-music [options]\n\n", exe);
  printf("options:\n");
  printf("  --seconds N   render length, default %d\n", MW_TOOL_DEFAULT_SECONDS);
  printf("  --rate N      sample rate, default %d\n", MW_TOOL_DEFAULT_RATE);
  printf("  --block N     mixer block frames, default %d\n", MW_TOOL_DEFAULT_BLOCK);
  printf("  --voices N    FM voices, default %d, max %d\n",
         MW_TOOL_DEFAULT_VOICES, SND_MIDI_FM_MAX_VOICES);
  printf("  --fm-gain N   FM pre-sum gain, 0..256, default %d\n",
         (int)SND_MIDI_FM_DEFAULT_OUTPUT_GAIN);
  printf("  --accum MODE  host accumulation: wide (default) or saturating\n");
  printf("  --scan-music  scan every effective D_* MUS lump for full-song peak\n");
  printf("  --no-loop     stop at the MUS end marker instead of looping\n");
  printf("  -h, --help    show this help\n\n");
  printf("Examples:\n");
  printf("  %s --wad C:\\microconsole\\wads\\doom.wad --music D_E1M1 ", exe);
  printf("--out e1m1-microwave.wav --seconds 30\n");
  printf("  %s --wad C:\\microconsole\\wads\\doom.wad --scan-music\n", exe);
}

static int mw_parse_positive(const char *text, int lo, int hi, int *out) {
  char *end = NULL;
  long v;
  if (!text || !out)
    return 0;
  errno = 0;
  v = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || v < (long)lo ||
      v > (long)hi)
    return 0;
  *out = (int)v;
  return 1;
}

static int mw_parse_args(int argc, char **argv, mw_args_t *a) {
  int i;
  if (!a)
    return 0;
  memset(a, 0, sizeof(*a));
  a->seconds = MW_TOOL_DEFAULT_SECONDS;
  a->rate = MW_TOOL_DEFAULT_RATE;
  a->block = MW_TOOL_DEFAULT_BLOCK;
  a->voices = MW_TOOL_DEFAULT_VOICES;
  a->fm_gain = (int)SND_MIDI_FM_DEFAULT_OUTPUT_GAIN;
  a->loop = 1;
  a->wide_accum = 1;

  for (i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      mw_usage(argv[0]);
      return -1;
    }
    if (!strcmp(arg, "--no-loop")) {
      a->loop = 0;
      continue;
    }
    if (!strcmp(arg, "--scan-music")) {
      a->scan_music = 1;
      continue;
    }
    if (!strcmp(arg, "--wad") || !strcmp(arg, "--music") ||
        !strcmp(arg, "--out") || !strcmp(arg, "--seconds") ||
        !strcmp(arg, "--rate") || !strcmp(arg, "--block") ||
        !strcmp(arg, "--voices") || !strcmp(arg, "--fm-gain") ||
        !strcmp(arg, "--accum")) {
      const char *value;
      if (i + 1 >= argc) {
        fprintf(stderr, "ERROR: %s requires a value.\n", arg);
        return 0;
      }
      value = argv[++i];
      if (!strcmp(arg, "--wad"))
        a->wad_path = value;
      else if (!strcmp(arg, "--music"))
        a->music_name = value;
      else if (!strcmp(arg, "--out"))
        a->out_path = value;
      else if (!strcmp(arg, "--seconds")) {
        if (!mw_parse_positive(value, 1, 3600, &a->seconds)) {
          fprintf(stderr, "ERROR: --seconds must be 1..3600.\n");
          return 0;
        }
      } else if (!strcmp(arg, "--rate")) {
        if (!mw_parse_positive(value, 8000, 192000, &a->rate)) {
          fprintf(stderr, "ERROR: --rate must be 8000..192000.\n");
          return 0;
        }
      } else if (!strcmp(arg, "--block")) {
        if (!mw_parse_positive(value, 1, 8192, &a->block)) {
          fprintf(stderr, "ERROR: --block must be 1..8192.\n");
          return 0;
        }
      } else if (!strcmp(arg, "--voices")) {
        if (!mw_parse_positive(value, 1, SND_MIDI_FM_MAX_VOICES,
                               &a->voices)) {
          fprintf(stderr, "ERROR: --voices must be 1..%d.\n",
                  SND_MIDI_FM_MAX_VOICES);
          return 0;
        }
      } else if (!strcmp(arg, "--fm-gain")) {
        if (!mw_parse_positive(value, 0, SND_GAIN_UNITY, &a->fm_gain)) {
          fprintf(stderr, "ERROR: --fm-gain must be 0..%d.\n",
                  SND_GAIN_UNITY);
          return 0;
        }
      } else if (!strcmp(arg, "--accum")) {
        if (!strcmp(value, "wide"))
          a->wide_accum = 1;
        else if (!strcmp(value, "saturating") || !strcmp(value, "sat"))
          a->wide_accum = 0;
        else {
          fprintf(stderr, "ERROR: --accum must be wide or saturating.\n");
          return 0;
        }
      }
      continue;
    }
    fprintf(stderr, "ERROR: unknown argument: %s\n", arg);
    return 0;
  }

  if (!a->wad_path) {
    fprintf(stderr, "ERROR: --wad is required.\n\n");
    mw_usage(argv[0]);
    return 0;
  }
  if (a->scan_music) {
    if (a->music_name || a->out_path) {
      fprintf(stderr,
              "ERROR: --scan-music is a report mode; do not combine it with "
              "--music or --out.\n");
      return 0;
    }
  } else {
    if (!a->music_name || !a->out_path) {
      fprintf(stderr,
              "ERROR: --music and --out are required unless --scan-music "
              "is used.\n\n");
      mw_usage(argv[0]);
      return 0;
    }
    if (strlen(a->music_name) < 1u ||
        strlen(a->music_name) > MW_WAD_NAME_BYTES) {
      fprintf(stderr, "ERROR: WAD lump names are 1..8 characters.\n");
      return 0;
    }
  }
  return 1;
}

static int mw_ascii_upper(int c) {
  if (c >= 'a' && c <= 'z')
    return c - ('a' - 'A');
  return c;
}

static int mw_name_matches(const uint8_t raw[MW_WAD_NAME_BYTES],
                           const char *wanted) {
  size_t len;
  int i;
  if (!wanted)
    return 0;
  len = strlen(wanted);
  if (len > MW_WAD_NAME_BYTES)
    return 0;
  for (i = 0; i < MW_WAD_NAME_BYTES; ++i) {
    int a = raw[i];
    int b = ((size_t)i < len) ? (unsigned char)wanted[i] : 0;
    if (mw_ascii_upper(a) != mw_ascii_upper(b))
      return 0;
  }
  return 1;
}

static void mw_copy_lump_name(char out[MW_WAD_NAME_BYTES + 1],
                              const uint8_t raw[MW_WAD_NAME_BYTES]) {
  int i;
  for (i = 0; i < MW_WAD_NAME_BYTES; ++i) {
    if (raw[i] == 0u)
      break;
    out[i] = (char)raw[i];
  }
  while (i < MW_WAD_NAME_BYTES)
    out[i++] = '\0';
  out[MW_WAD_NAME_BYTES] = '\0';
}

static int mw_wad_open(mw_wad_t *wad, const char *path) {
  uint8_t hdr[12];
  long end;
  uint64_t dir_end;
  if (!wad || !path)
    return 0;
  memset(wad, 0, sizeof(*wad));
  wad->fp = fopen(path, "rb");
  if (!wad->fp) {
    fprintf(stderr, "ERROR: cannot open WAD: %s\n", path);
    return 0;
  }
  if (fread(hdr, 1u, sizeof(hdr), wad->fp) != sizeof(hdr)) {
    fprintf(stderr, "ERROR: WAD header is truncated.\n");
    goto fail;
  }
  if (memcmp(hdr, "IWAD", 4u) != 0 && memcmp(hdr, "PWAD", 4u) != 0) {
    fprintf(stderr, "ERROR: not an IWAD/PWAD file.\n");
    goto fail;
  }
  memcpy(wad->type, hdr, 4u);
  wad->type[4] = '\0';
  wad->lump_count = mw_rd32(hdr + 4);
  wad->directory_offset = mw_rd32(hdr + 8);
  if (wad->lump_count == 0u || wad->lump_count > 1000000u) {
    fprintf(stderr, "ERROR: unreasonable WAD lump count: %lu\n",
            (unsigned long)wad->lump_count);
    goto fail;
  }
  if (fseek(wad->fp, 0L, SEEK_END) != 0)
    goto fail;
  end = ftell(wad->fp);
  if (end < 0L) {
    fprintf(stderr, "ERROR: could not determine WAD size.\n");
    goto fail;
  }
  wad->bytes = (uint32_t)end;
  if ((uint64_t)(unsigned long)end != (uint64_t)wad->bytes) {
    fprintf(stderr, "ERROR: WAD is larger than this host tool supports.\n");
    goto fail;
  }
  dir_end = (uint64_t)wad->directory_offset +
            (uint64_t)wad->lump_count * MW_WAD_DIR_ENTRY_BYTES;
  if (dir_end > (uint64_t)wad->bytes) {
    fprintf(stderr, "ERROR: WAD directory extends past end of file.\n");
    goto fail;
  }
  return 1;

fail:
  fclose(wad->fp);
  memset(wad, 0, sizeof(*wad));
  return 0;
}

static void mw_wad_close(mw_wad_t *wad) {
  if (wad && wad->fp)
    fclose(wad->fp);
  if (wad)
    memset(wad, 0, sizeof(*wad));
}

static int mw_wad_find(const mw_wad_t *wad, const char *name, mw_lump_t *out) {
  uint32_t i;
  uint8_t ent[MW_WAD_DIR_ENTRY_BYTES];
  if (!wad || !wad->fp || !name || !out)
    return 0;

  /* Doom resolves duplicate lump names from the end of the directory.  Do the
   * same here so a hosted validation tool has the same override semantics as
   * the game even when pointed at a PWAD-like container. */
  for (i = wad->lump_count; i > 0u; --i) {
    uint32_t off;
    uint32_t bytes;
    uint32_t dir_pos =
        wad->directory_offset + (i - 1u) * MW_WAD_DIR_ENTRY_BYTES;
    if (fseek(wad->fp, (long)dir_pos, SEEK_SET) != 0 ||
        fread(ent, 1u, sizeof(ent), wad->fp) != sizeof(ent))
      return 0;
    if (!mw_name_matches(ent + 8, name))
      continue;
    off = mw_rd32(ent + 0);
    bytes = mw_rd32(ent + 4);
    if ((uint64_t)off + (uint64_t)bytes > (uint64_t)wad->bytes) {
      fprintf(stderr, "ERROR: lump %s points outside the WAD.\n", name);
      return 0;
    }
    memset(out, 0, sizeof(*out));
    out->index = i - 1u;
    out->offset = off;
    out->bytes = bytes;
    mw_copy_lump_name(out->name, ent + 8);
    return 1;
  }
  return 0;
}

static int mw_wad_get(const mw_wad_t *wad, uint32_t index, mw_lump_t *out) {
  uint8_t ent[MW_WAD_DIR_ENTRY_BYTES];
  uint32_t dir_pos;
  uint32_t off;
  uint32_t bytes;
  if (!wad || !wad->fp || !out || index >= wad->lump_count)
    return 0;
  dir_pos = wad->directory_offset + index * MW_WAD_DIR_ENTRY_BYTES;
  if (fseek(wad->fp, (long)dir_pos, SEEK_SET) != 0 ||
      fread(ent, 1u, sizeof(ent), wad->fp) != sizeof(ent))
    return 0;
  off = mw_rd32(ent + 0);
  bytes = mw_rd32(ent + 4);
  if ((uint64_t)off + (uint64_t)bytes > (uint64_t)wad->bytes)
    return 0;
  memset(out, 0, sizeof(*out));
  out->index = index;
  out->offset = off;
  out->bytes = bytes;
  mw_copy_lump_name(out->name, ent + 8);
  return 1;
}

static void *mw_wad_load(const mw_wad_t *wad, const mw_lump_t *lump) {
  void *data;
  size_t want;
  if (!wad || !wad->fp || !lump || lump->bytes == 0u)
    return NULL;
  want = (size_t)lump->bytes;
  if ((uint32_t)want != lump->bytes)
    return NULL;
  data = malloc(want);
  if (!data)
    return NULL;
  if (fseek(wad->fp, (long)lump->offset, SEEK_SET) != 0 ||
      fread(data, 1u, want, wad->fp) != want) {
    free(data);
    return NULL;
  }
  return data;
}

static int mw_wav_header(FILE *fp, int rate, int channels,
                         uint32_t data_bytes) {
  uint32_t byte_rate;
  uint16_t block_align;
  if (!fp || rate <= 0 || channels <= 0)
    return 0;
  byte_rate = (uint32_t)rate * (uint32_t)channels * 2u;
  block_align = (uint16_t)(channels * 2);
  if (fwrite("RIFF", 1u, 4u, fp) != 4u)
    return 0;
  mw_wr32(fp, 36u + data_bytes);
  if (fwrite("WAVEfmt ", 1u, 8u, fp) != 8u)
    return 0;
  mw_wr32(fp, 16u);
  mw_wr16(fp, 1u);
  mw_wr16(fp, (uint16_t)channels);
  mw_wr32(fp, (uint32_t)rate);
  mw_wr32(fp, byte_rate);
  mw_wr16(fp, block_align);
  mw_wr16(fp, 16u);
  if (fwrite("data", 1u, 4u, fp) != 4u)
    return 0;
  mw_wr32(fp, data_bytes);
  return ferror(fp) ? 0 : 1;
}

static int mw_wav_write_samples(FILE *fp, const snd_sample_t *samples,
                                long count, mw_render_stats_t *stats) {
  long i;
  if (!samples || count < 0 || !stats)
    return 0;
  for (i = 0; i < count; ++i) {
    int v = (int)SND_SAMPLE_TO_MIX(samples[i]);
    int a = (v < 0) ? -v : v;
    uint16_t u = (uint16_t)(int16_t)v;
    uint8_t b[2];
    if (a > stats->peak_abs)
      stats->peak_abs = a;
    if (v == SND_MIX_MIN || v == SND_MIX_MAX)
      ++stats->full_scale_samples;
    b[0] = (uint8_t)(u & 0xffu);
    b[1] = (uint8_t)((u >> 8) & 0xffu);
    if (fp && fwrite(b, 1u, sizeof(b), fp) != sizeof(b))
      return 0;
  }
  stats->samples_written += (uint64_t)(unsigned long)count;
  return 1;
}

static long mw_probe_mus_frames(const snd_mus_song_t *song, int rate) {
  snd_mus_player_t player;
  snd_midi_t midi;
  long end_frame;
  long step;
  if (!song || rate <= 0)
    return -1L;
  snd_midi_init(&midi);
  if (!snd_mus_player_init(&player, song, rate, SND_MUS_DOOM_TICK_HZ, 0L, 0))
    return -1L;
  step = (long)rate * 60L;
  if (step <= 0L)
    return -1L;
  end_frame = step;
  while (!snd_mus_player_finished(&player)) {
    if (snd_mus_process_until(&player, &midi, end_frame) < 0)
      return -1L;
    if (snd_mus_player_finished(&player))
      break;
    if (end_frame > LONG_MAX - step)
      return -1L;
    end_frame += step;
  }
  if (snd_mus_player_error(&player) != SND_MUS_OK)
    return -1L;
  return player.next_frame;
}

static int mw_render(const mw_args_t *a, const snd_mus_song_t *song,
                     const snd_genmidi_bank_t *genmidi, FILE *wav,
                     long total_frames, int loop, mw_render_stats_t *stats) {
  snd_sample_t *block = NULL;
#if SND_WIDE_ACCUM
  int32_t *accum = NULL;
#endif
  snd_mixer_t mixer;
  snd_midi_t midi;
  snd_midi_fm_t fm;
  snd_mus_player_t player;
  long frame = 0L;
  unsigned long last_dropped = 0uL;
  int ok = 0;

  if (!a || !song || !genmidi || !stats || total_frames < 0L)
    return 0;

  block = (snd_sample_t *)malloc((size_t)a->block * MW_TOOL_CHANNELS *
                                 sizeof(*block));
  if (!block) {
    fprintf(stderr, "ERROR: could not allocate mixer block.\n");
    return 0;
  }

  snd_init(&mixer, a->rate, MW_TOOL_CHANNELS, block, a->block, NULL, NULL);
#if SND_WIDE_ACCUM
  if (a->wide_accum) {
    accum = (int32_t *)malloc((size_t)a->block * MW_TOOL_CHANNELS *
                              sizeof(*accum));
    if (!accum) {
      fprintf(stderr, "ERROR: could not allocate wide accumulator.\n");
      goto done;
    }
    snd_set_accumulator(&mixer, accum);
  }
#else
  if (a->wide_accum) {
    fprintf(stderr,
            "ERROR: this build compiled out SND_WIDE_ACCUM; use "
            "--accum saturating.\n");
    goto done;
  }
#endif
  snd_midi_init(&midi);
  snd_midi_fm_init(&fm);
  snd_midi_fm_set_output_gain(&fm, (int16_t)a->fm_gain);
  snd_midi_fm_set_bank(&fm, snd_genmidi_midi_fm_bank(genmidi));
  snd_midi_fm_set_voice_limit(&fm, a->voices);
  snd_midi_fm_bind(&fm, &midi);
  if (!snd_mus_player_init(&player, song, a->rate, SND_MUS_DOOM_TICK_HZ, 0L,
                           loop)) {
    fprintf(stderr, "ERROR: could not initialize MUS player: %s\n",
            snd_mus_error_string(snd_mus_player_error(&player)));
    goto done;
  }

  while (frame < total_frames) {
    long left = total_frames - frame;
    int n = (left > (long)a->block) ? a->block : (int)left;
    int emitted;

    snd_begin_block(&mixer, frame, n);
    emitted = snd_mus_process_until(&player, &midi, frame + (long)n);
    if (emitted < 0) {
      fprintf(stderr, "ERROR: MUS playback failed at frame %ld: %s\n", frame,
              snd_mus_error_string(snd_mus_player_error(&player)));
      goto done;
    }
    stats->midi_messages += (unsigned long)emitted;

    if (fm.dropped_events != last_dropped) {
      fprintf(stderr,
              "ERROR: MIDI-FM event queue overflowed/out-of-order at frame %ld "
              "(dropped=%lu). Try a smaller --block; if it persists the score "
              "has too many simultaneous events for the current queue.\n",
              frame, fm.dropped_events);
      goto done;
    }

    snd_midi_fm_mix_block(&fm, &mixer);
#if SND_WIDE_ACCUM
    if (accum && mixer.block_touched) {
      long si;
      long sample_count = (long)mixer.block_frames * mixer.channels;
      for (si = 0; si < sample_count; ++si) {
        int64_t raw = (int64_t)accum[si];
        uint64_t mag = (raw < 0) ? (uint64_t)(-raw) : (uint64_t)raw;
        if (mag > stats->preclip_peak_abs)
          stats->preclip_peak_abs = mag;
        if (raw < (int64_t)SND_MIX_MIN || raw > (int64_t)SND_MIX_MAX)
          ++stats->preclip_samples;
      }
    }
#endif
    snd_flush_block(&mixer);
    if (!mw_wav_write_samples(wav, mixer.block,
                              (long)mixer.block_frames * mixer.channels,
                              stats)) {
      fprintf(stderr, "ERROR: failed while writing WAV data.\n");
      goto done;
    }
    frame += (long)mixer.block_frames;
  }

  stats->mus_loops = player.loops_completed;
  stats->midi_notes_started = fm.midi_notes_started;
  stats->fm_voices_started = fm.fm_voices_started;
  stats->fm_voices_released = fm.fm_voices_released;
  stats->fm_voices_stolen = fm.voices_stolen;
  stats->midi_events_dropped = fm.dropped_events;
  ok = 1;

done:
#if SND_WIDE_ACCUM
  free(accum);
#endif
  free(block);
  return ok;
}

static int mw_is_music_name(const char *name) {
  if (!name)
    return 0;
  return mw_ascii_upper((unsigned char)name[0]) == 'D' && name[1] == '_';
}

static int mw_scan_music(const mw_args_t *a, const mw_wad_t *wad,
                         const snd_genmidi_bank_t *genmidi) {
  uint32_t i;
  unsigned int scanned = 0u;
  unsigned int skipped = 0u;
  unsigned int clipped_tracks = 0u;
  uint64_t worst_peak = 0u;
  uint64_t total_clip_samples = 0u;
  char worst_name[MW_WAD_NAME_BYTES + 1];

  if (!a || !wad || !genmidi)
    return 0;
  memset(worst_name, 0, sizeof(worst_name));

  printf("MicroWave Doom music headroom scan\n");
  printf("WAD: %s (%s, %lu lumps)\n", a->wad_path, wad->type,
         (unsigned long)wad->lump_count);
  printf("Render: %d Hz stereo, block=%d, voices=%d, first pass, "
         "fm-gain=%d/256 (%.2f%%), accum=%s\n\n",
         a->rate, a->block, a->voices, a->fm_gain,
         (100.0 * (double)a->fm_gain) / 256.0,
         a->wide_accum ? "wide" : "saturating");
  printf("%-8s %9s %10s %8s %12s %8s\n",
         "Lump", "Seconds", "Peak", "% FS", "Clip samples", "Steals");
  printf("-------- --------- ---------- -------- ------------ --------\n");

  for (i = 0u; i < wad->lump_count; ++i) {
    mw_lump_t lump;
    mw_lump_t effective;
    void *music_data = NULL;
    snd_mus_song_t song;
    mw_render_stats_t stats;
    long song_frames;
    int err;
    uint64_t peak;
    uint64_t clips;

    if (!mw_wad_get(wad, i, &lump)) {
      fprintf(stderr, "ERROR: could not read WAD directory entry %lu.\n",
              (unsigned long)i);
      return 0;
    }
    if (!mw_is_music_name(lump.name))
      continue;

    /* Only scan the effective definition of a duplicate name, matching Doom's
     * reverse-directory override semantics. */
    if (!mw_wad_find(wad, lump.name, &effective))
      return 0;
    if (effective.index != i)
      continue;

    music_data = mw_wad_load(wad, &lump);
    if (!music_data) {
      fprintf(stderr, "ERROR: could not load %s.\n", lump.name);
      return 0;
    }
    err = snd_mus_open(&song, music_data, lump.bytes);
    if (err != SND_MUS_OK) {
      ++skipped;
      free(music_data);
      continue;
    }
    song_frames = mw_probe_mus_frames(&song, a->rate);
    if (song_frames < 0L) {
      fprintf(stderr, "ERROR: could not determine duration of %s.\n", lump.name);
      free(music_data);
      return 0;
    }

    memset(&stats, 0, sizeof(stats));
    if (!mw_render(a, &song, genmidi, NULL, song_frames, 0, &stats)) {
      fprintf(stderr, "ERROR: render failed while scanning %s.\n", lump.name);
      free(music_data);
      return 0;
    }
    free(music_data);

    if (a->wide_accum) {
      peak = stats.preclip_peak_abs;
      clips = stats.preclip_samples;
    } else {
      peak = (uint64_t)(unsigned int)stats.peak_abs;
      clips = stats.full_scale_samples;
    }
    ++scanned;
    if (clips != 0u) {
      ++clipped_tracks;
      total_clip_samples += clips;
    }
    if (peak > worst_peak) {
      worst_peak = peak;
      memcpy(worst_name, lump.name, sizeof(worst_name));
    }

    printf("%-8s %9.3f %10llu %7.2f%% %12llu %8lu\n",
           lump.name, (double)song_frames / (double)a->rate,
           (unsigned long long)peak,
           (100.0 * (double)peak) / 32768.0,
           (unsigned long long)clips, stats.fm_voices_stolen);
  }

  printf("\nScanned: %u valid effective D_* MUS lumps", scanned);
  if (skipped != 0u)
    printf(" (%u D_* lumps skipped as non-MUS)", skipped);
  printf("\n");
  if (scanned == 0u) {
    printf("No valid D_* MUS lumps found.\n");
    return 0;
  }
  printf("Worst peak: %s = %llu / 32768 (%.2f%%)\n",
         worst_name, (unsigned long long)worst_peak,
         (100.0 * (double)worst_peak) / 32768.0);
  printf("Tracks with clipping: %u\n", clipped_tracks);
  printf("Total clipping samples: %llu\n",
         (unsigned long long)total_clip_samples);
  return 1;
}

int main(int argc, char **argv) {
  mw_args_t args;
  mw_wad_t wad;
  mw_lump_t gen_lump;
  mw_lump_t music_lump;
  void *gen_data = NULL;
  void *music_data = NULL;
  snd_genmidi_bank_t genmidi;
  snd_mus_song_t song;
  mw_render_stats_t stats;
  FILE *wav = NULL;
  uint64_t data_bytes64;
  uint32_t data_bytes;
  uint64_t total_frames64;
  long total_frames;
  long song_frames;
  int parse;
  int err;
  int rc = 1;

  parse = mw_parse_args(argc, argv, &args);
  if (parse < 0)
    return 0;
  if (parse == 0)
    return 2;

  memset(&stats, 0, sizeof(stats));
  memset(&wad, 0, sizeof(wad));
  if (!mw_wad_open(&wad, args.wad_path))
    goto done;

  if (!mw_wad_find(&wad, "GENMIDI", &gen_lump)) {
    fprintf(stderr, "ERROR: GENMIDI lump not found.\n");
    goto done;
  }
  gen_data = mw_wad_load(&wad, &gen_lump);
  if (!gen_data) {
    fprintf(stderr, "ERROR: failed to load GENMIDI lump data.\n");
    goto done;
  }
  err = snd_genmidi_load(&genmidi, gen_data, gen_lump.bytes);
  if (err != SND_GENMIDI_OK) {
    fprintf(stderr, "ERROR: GENMIDI decode failed: %s\n",
            snd_genmidi_error_string(err));
    goto done;
  }

  if (args.scan_music) {
    rc = mw_scan_music(&args, &wad, &genmidi) ? 0 : 1;
    goto done;
  }

  if (!mw_wad_find(&wad, args.music_name, &music_lump)) {
    fprintf(stderr, "ERROR: music lump %s not found.\n", args.music_name);
    goto done;
  }
  music_data = mw_wad_load(&wad, &music_lump);
  if (!music_data) {
    fprintf(stderr, "ERROR: failed to load requested music lump data.\n");
    goto done;
  }
  err = snd_mus_open(&song, music_data, music_lump.bytes);
  if (err != SND_MUS_OK) {
    fprintf(stderr, "ERROR: %s is not a supported MUS score: %s\n",
            music_lump.name, snd_mus_error_string(err));
    goto done;
  }

  song_frames = mw_probe_mus_frames(&song, args.rate);

  total_frames64 = (uint64_t)(unsigned int)args.rate *
                   (uint64_t)(unsigned int)args.seconds;
  if (total_frames64 > (uint64_t)LONG_MAX) {
    fprintf(stderr, "ERROR: requested render is too long for this build.\n");
    goto done;
  }
  total_frames = (long)total_frames64;
  data_bytes64 = total_frames64 * MW_TOOL_CHANNELS * 2u;
  if (data_bytes64 > UINT32_MAX - 36u) {
    fprintf(stderr,
            "ERROR: requested WAV exceeds RIFF/WAVE 32-bit size limit.\n");
    goto done;
  }
  data_bytes = (uint32_t)data_bytes64;

  wav = fopen(args.out_path, "wb");
  if (!wav) {
    fprintf(stderr, "ERROR: cannot create output WAV: %s\n", args.out_path);
    goto done;
  }
  if (!mw_wav_header(wav, args.rate, MW_TOOL_CHANNELS, data_bytes)) {
    fprintf(stderr, "ERROR: could not write WAV header.\n");
    goto done;
  }

  printf("MicroWave real Doom music validator\n");
  printf("WAD:                      %s\n", args.wad_path);
  printf("WAD type / lumps:         %s / %lu\n", wad.type,
         (unsigned long)wad.lump_count);
  printf("GENMIDI:                  offset %lu, %lu bytes, names=%s\n",
         (unsigned long)gen_lump.offset, (unsigned long)gen_lump.bytes,
         genmidi.has_names ? "yes" : "no");
  printf("Music lump:               %s, offset %lu, %lu bytes\n",
         music_lump.name, (unsigned long)music_lump.offset,
         (unsigned long)music_lump.bytes);
  printf("MUS score bytes:          %lu\n", (unsigned long)song.score_length);
  printf("MUS channels:             primary=%u secondary=%u\n",
         (unsigned int)song.primary_channels,
         (unsigned int)song.secondary_channels);
  printf("MUS instruments listed:   %u\n", (unsigned int)song.instrument_count);
  if (song_frames >= 0L)
    printf("MUS first-pass duration:  %.3f s (%ld frames @ %d Hz)\n",
           (double)song_frames / (double)args.rate, song_frames, args.rate);
  else
    printf("MUS first-pass duration:  unavailable\n");
  printf("Render:                   %d Hz stereo, block=%d, voices=%d, %d s, "
         "loop=%s, fm-gain=%d/256 (%.2f%%), accum=%s\n",
         args.rate, args.block, args.voices, args.seconds,
         args.loop ? "yes" : "no", args.fm_gain,
         (100.0 * (double)args.fm_gain) / 256.0,
         args.wide_accum ? "wide" : "saturating");
  printf("Output:                   %s\n\n", args.out_path);

  if (!mw_render(&args, &song, &genmidi, wav, total_frames, args.loop, &stats))
    goto done;

  if (fflush(wav) != 0 || ferror(wav)) {
    fprintf(stderr, "ERROR: final WAV flush failed.\n");
    goto done;
  }

  printf("MUS messages emitted:     %lu\n", stats.midi_messages);
  printf("MUS loops completed:      %lu\n", stats.mus_loops);
  printf("MIDI notes started:       %lu\n", stats.midi_notes_started);
  printf("FM voices started:        %lu\n", stats.fm_voices_started);
  printf("FM voices released:       %lu\n", stats.fm_voices_released);
  printf("FM voices stolen:         %lu\n", stats.fm_voices_stolen);
  printf("MIDI events dropped:      %lu\n", stats.midi_events_dropped);
  if (args.wide_accum) {
    printf("Pre-clamp peak:            %llu / 32768 (%.2f%%)\n",
           (unsigned long long)stats.preclip_peak_abs,
           (100.0 * (double)stats.preclip_peak_abs) / 32768.0);
    printf("Would-clip samples:        %llu\n",
           (unsigned long long)stats.preclip_samples);
  }
  printf("Peak absolute sample:     %d / 32768 (%.2f%%)\n", stats.peak_abs,
         (100.0 * (double)stats.peak_abs) / 32768.0);
  printf("Full-scale samples:       %llu\n",
         (unsigned long long)stats.full_scale_samples);
  printf("PCM samples written:      %llu\n",
         (unsigned long long)stats.samples_written);
  printf("WAV data bytes:           %lu\n", (unsigned long)data_bytes);
  printf("\nWrote %s\n", args.out_path);
  rc = 0;

done:
  if (wav)
    fclose(wav);
  free(music_data);
  free(gen_data);
  mw_wad_close(&wad);
  return rc;
}
