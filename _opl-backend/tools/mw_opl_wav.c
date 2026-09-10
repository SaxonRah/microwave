#include "snd_genmidi_opl.h"
#include "snd_midi.h"
#include "snd_mus.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if SND_SAMPLE_FORMAT != SND_SAMPLE_FORMAT_S16
#error mw_opl_wav requires the normal MicroWave S16 logical sample format
#endif

#define MW_OPL_DEFAULT_SECONDS 60
#define MW_OPL_DEFAULT_RATE SND_DEFAULT_RATE
#define MW_OPL_DEFAULT_BLOCK SND_DEFAULT_BLOCK_FRAMES
#define MW_OPL_CHANNELS 2
#define MW_WAD_NAME_BYTES 8
#define MW_WAD_DIR_ENTRY_BYTES 16u

typedef struct mw_args {
  const char *wad_path;
  const char *music_name;
  const char *out_path;
  int seconds;
  int rate;
  int block;
  int gain;
  int loop;
  int wide_accum;
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

typedef struct mw_stats {
  uint64_t samples;
  uint64_t full_scale;
  uint64_t would_clip;
  uint64_t preclip_peak;
  int peak;
  int64_t sum_l;
  int64_t sum_r;
} mw_stats_t;

static uint32_t mw_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void mw_wr16(FILE *fp, uint16_t value) {
  uint8_t b[2];
  b[0] = (uint8_t)(value & 0xffu);
  b[1] = (uint8_t)((value >> 8) & 0xffu);
  (void)fwrite(b, 1u, sizeof(b), fp);
}

static void mw_wr32(FILE *fp, uint32_t value) {
  uint8_t b[4];
  b[0] = (uint8_t)(value & 0xffu);
  b[1] = (uint8_t)((value >> 8) & 0xffu);
  b[2] = (uint8_t)((value >> 16) & 0xffu);
  b[3] = (uint8_t)((value >> 24) & 0xffu);
  (void)fwrite(b, 1u, sizeof(b), fp);
}

static void mw_usage(const char *exe) {
  printf("MicroWave register-native Doom OPL WAV renderer\n\n");
  printf("usage:\n");
  printf("  %s --wad FILE --music LUMP --out FILE [options]\n\n", exe);
  printf("options:\n");
  printf("  --seconds N   render length, default %d\n",
         MW_OPL_DEFAULT_SECONDS);
  printf("  --rate N      output rate, default %d\n",
         MW_OPL_DEFAULT_RATE);
  printf("  --block N     mixer block frames, default %d\n",
         MW_OPL_DEFAULT_BLOCK);
  printf("  --gain N      OPL output gain, 0..256, default 256\n");
  printf("  --accum MODE  wide (default) or saturating\n");
  printf("  --no-loop     stop at the MUS end marker\n");
  printf("  -h, --help    show this help\n\n");
  printf("example:\n");
  printf("  %s --wad C:\\microconsole\\wads\\doom.wad ", exe);
  printf("--music D_E1M1 --out e1m1-nuked-opl.wav --seconds 60\n");
}

static int mw_parse_int(const char *text, int lo, int hi, int *out) {
  char *end = NULL;
  long value;
  if (!text || !out)
    return 0;
  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' ||
      value < (long)lo || value > (long)hi)
    return 0;
  *out = (int)value;
  return 1;
}

static int mw_parse_args(int argc, char **argv, mw_args_t *args) {
  int i;
  if (!args)
    return 0;
  memset(args, 0, sizeof(*args));
  args->seconds = MW_OPL_DEFAULT_SECONDS;
  args->rate = MW_OPL_DEFAULT_RATE;
  args->block = MW_OPL_DEFAULT_BLOCK;
  args->gain = 256;
  args->loop = 1;
  args->wide_accum = 1;

  for (i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value;

    if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      mw_usage(argv[0]);
      return -1;
    }
    if (!strcmp(arg, "--no-loop")) {
      args->loop = 0;
      continue;
    }

    if (strcmp(arg, "--wad") && strcmp(arg, "--music") &&
        strcmp(arg, "--out") && strcmp(arg, "--seconds") &&
        strcmp(arg, "--rate") && strcmp(arg, "--block") &&
        strcmp(arg, "--gain") && strcmp(arg, "--accum")) {
      fprintf(stderr, "ERROR: unknown argument: %s\n", arg);
      return 0;
    }

    if (i + 1 >= argc) {
      fprintf(stderr, "ERROR: %s requires a value.\n", arg);
      return 0;
    }
    value = argv[++i];

    if (!strcmp(arg, "--wad")) {
      args->wad_path = value;
    } else if (!strcmp(arg, "--music")) {
      args->music_name = value;
    } else if (!strcmp(arg, "--out")) {
      args->out_path = value;
    } else if (!strcmp(arg, "--seconds")) {
      if (!mw_parse_int(value, 1, 3600, &args->seconds)) {
        fprintf(stderr, "ERROR: --seconds must be 1..3600.\n");
        return 0;
      }
    } else if (!strcmp(arg, "--rate")) {
      if (!mw_parse_int(value, 8000, 192000, &args->rate)) {
        fprintf(stderr, "ERROR: --rate must be 8000..192000.\n");
        return 0;
      }
    } else if (!strcmp(arg, "--block")) {
      if (!mw_parse_int(value, 1, 8192, &args->block)) {
        fprintf(stderr, "ERROR: --block must be 1..8192.\n");
        return 0;
      }
    } else if (!strcmp(arg, "--gain")) {
      if (!mw_parse_int(value, 0, 256, &args->gain)) {
        fprintf(stderr, "ERROR: --gain must be 0..256.\n");
        return 0;
      }
    } else if (!strcmp(arg, "--accum")) {
      if (!strcmp(value, "wide")) {
        args->wide_accum = 1;
      } else if (!strcmp(value, "saturating") ||
                 !strcmp(value, "sat")) {
        args->wide_accum = 0;
      } else {
        fprintf(stderr,
                "ERROR: --accum must be wide or saturating.\n");
        return 0;
      }
    }
  }

  if (!args->wad_path || !args->music_name || !args->out_path) {
    fprintf(stderr,
            "ERROR: --wad, --music and --out are required.\n\n");
    mw_usage(argv[0]);
    return 0;
  }
  if (strlen(args->music_name) < 1u ||
      strlen(args->music_name) > MW_WAD_NAME_BYTES) {
    fprintf(stderr, "ERROR: WAD lump names are 1..8 characters.\n");
    return 0;
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

static void mw_copy_name(char out[MW_WAD_NAME_BYTES + 1],
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
  uint8_t header[12];
  long end;
  uint64_t directory_end;

  memset(wad, 0, sizeof(*wad));
  wad->fp = fopen(path, "rb");
  if (!wad->fp) {
    fprintf(stderr, "ERROR: cannot open WAD: %s\n", path);
    return 0;
  }

  if (fread(header, 1u, sizeof(header), wad->fp) != sizeof(header)) {
    fprintf(stderr, "ERROR: truncated WAD header.\n");
    goto fail;
  }
  if (memcmp(header, "IWAD", 4u) != 0 &&
      memcmp(header, "PWAD", 4u) != 0) {
    fprintf(stderr, "ERROR: not an IWAD/PWAD.\n");
    goto fail;
  }

  memcpy(wad->type, header, 4u);
  wad->type[4] = '\0';
  wad->lump_count = mw_rd32(header + 4);
  wad->directory_offset = mw_rd32(header + 8);

  if (fseek(wad->fp, 0L, SEEK_END) != 0)
    goto fail;
  end = ftell(wad->fp);
  if (end < 0L || (uint64_t)(unsigned long)end > UINT32_MAX) {
    fprintf(stderr, "ERROR: unsupported WAD size.\n");
    goto fail;
  }
  wad->bytes = (uint32_t)end;

  directory_end =
      (uint64_t)wad->directory_offset +
      (uint64_t)wad->lump_count * MW_WAD_DIR_ENTRY_BYTES;
  if (directory_end > (uint64_t)wad->bytes) {
    fprintf(stderr, "ERROR: WAD directory extends past EOF.\n");
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

static int mw_wad_find(const mw_wad_t *wad, const char *name,
                       mw_lump_t *out) {
  uint32_t i;
  uint8_t entry[MW_WAD_DIR_ENTRY_BYTES];

  for (i = wad->lump_count; i > 0u; --i) {
    uint32_t directory_pos =
        wad->directory_offset + (i - 1u) * MW_WAD_DIR_ENTRY_BYTES;
    uint32_t offset;
    uint32_t bytes;

    if (fseek(wad->fp, (long)directory_pos, SEEK_SET) != 0 ||
        fread(entry, 1u, sizeof(entry), wad->fp) != sizeof(entry))
      return 0;
    if (!mw_name_matches(entry + 8, name))
      continue;

    offset = mw_rd32(entry);
    bytes = mw_rd32(entry + 4);
    if ((uint64_t)offset + (uint64_t)bytes >
        (uint64_t)wad->bytes) {
      fprintf(stderr, "ERROR: lump %s points outside the WAD.\n",
              name);
      return 0;
    }

    memset(out, 0, sizeof(*out));
    out->index = i - 1u;
    out->offset = offset;
    out->bytes = bytes;
    mw_copy_name(out->name, entry + 8);
    return 1;
  }
  return 0;
}

static void *mw_wad_load(const mw_wad_t *wad,
                         const mw_lump_t *lump) {
  void *data;
  size_t bytes;

  if (!wad || !wad->fp || !lump || lump->bytes == 0u)
    return NULL;
  bytes = (size_t)lump->bytes;
  if ((uint32_t)bytes != lump->bytes)
    return NULL;

  data = malloc(bytes);
  if (!data)
    return NULL;
  if (fseek(wad->fp, (long)lump->offset, SEEK_SET) != 0 ||
      fread(data, 1u, bytes, wad->fp) != bytes) {
    free(data);
    return NULL;
  }
  return data;
}

static int mw_wav_header(FILE *fp, int rate, uint32_t data_bytes) {
  uint32_t byte_rate = (uint32_t)rate * MW_OPL_CHANNELS * 2u;
  uint16_t block_align = (uint16_t)(MW_OPL_CHANNELS * 2);

  if (fwrite("RIFF", 1u, 4u, fp) != 4u)
    return 0;
  mw_wr32(fp, 36u + data_bytes);
  if (fwrite("WAVEfmt ", 1u, 8u, fp) != 8u)
    return 0;
  mw_wr32(fp, 16u);
  mw_wr16(fp, 1u);
  mw_wr16(fp, MW_OPL_CHANNELS);
  mw_wr32(fp, (uint32_t)rate);
  mw_wr32(fp, byte_rate);
  mw_wr16(fp, block_align);
  mw_wr16(fp, 16u);
  if (fwrite("data", 1u, 4u, fp) != 4u)
    return 0;
  mw_wr32(fp, data_bytes);
  return !ferror(fp);
}

static int mw_write_block(FILE *fp, const snd_sample_t *samples,
                          int frames, mw_stats_t *stats) {
  int frame;
  for (frame = 0; frame < frames; ++frame) {
    int channel;
    for (channel = 0; channel < MW_OPL_CHANNELS; ++channel) {
      int value = SND_SAMPLE_TO_MIX(
          samples[frame * MW_OPL_CHANNELS + channel]);
      int magnitude = (value < 0) ? -value : value;
      uint16_t raw = (uint16_t)(int16_t)value;
      uint8_t bytes[2];

      if (magnitude > stats->peak)
        stats->peak = magnitude;
      if (value == SND_MIX_MIN || value == SND_MIX_MAX)
        ++stats->full_scale;
      if (channel == 0)
        stats->sum_l += value;
      else
        stats->sum_r += value;

      bytes[0] = (uint8_t)(raw & 0xffu);
      bytes[1] = (uint8_t)((raw >> 8) & 0xffu);
      if (fwrite(bytes, 1u, sizeof(bytes), fp) != sizeof(bytes))
        return 0;
      ++stats->samples;
    }
  }
  return 1;
}

static long mw_probe_song_frames(const snd_mus_song_t *song,
                                 int rate) {
  snd_mus_player_t player;
  snd_midi_t midi;
  long step;
  long end_frame;

  snd_midi_init(&midi);
  if (!snd_mus_player_init(&player, song, rate,
                           SND_MUS_DOOM_TICK_HZ, 0L, 0))
    return -1L;

  step = (long)rate * 60L;
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
  return (snd_mus_player_error(&player) == SND_MUS_OK)
             ? player.next_frame
             : -1L;
}

int main(int argc, char **argv) {
  mw_args_t args;
  mw_wad_t wad;
  mw_lump_t gen_lump;
  mw_lump_t music_lump;
  void *gen_data = NULL;
  void *music_data = NULL;
  snd_genmidi_opl_bank_t bank;
  snd_genmidi_opl_t opl;
  snd_midi_t midi;
  snd_mus_song_t song;
  snd_mus_player_t player;
  snd_mixer_t mixer;
  snd_sample_t *block = NULL;
#if SND_WIDE_ACCUM
  int32_t *accum = NULL;
#endif
  FILE *wav = NULL;
  mw_stats_t stats;
  uint64_t total_frames_64;
  uint64_t data_bytes_64;
  uint32_t data_bytes;
  long total_frames;
  long song_frames;
  long frame = 0L;
  int parse_result;
  int error;
  int rc = 1;

  memset(&wad, 0, sizeof(wad));
  memset(&stats, 0, sizeof(stats));

  parse_result = mw_parse_args(argc, argv, &args);
  if (parse_result < 0)
    return 0;
  if (parse_result == 0)
    return 2;

  if (!mw_wad_open(&wad, args.wad_path))
    goto done;
  if (!mw_wad_find(&wad, "GENMIDI", &gen_lump)) {
    fprintf(stderr, "ERROR: GENMIDI lump not found.\n");
    goto done;
  }
  if (!mw_wad_find(&wad, args.music_name, &music_lump)) {
    fprintf(stderr, "ERROR: music lump %s not found.\n",
            args.music_name);
    goto done;
  }

  gen_data = mw_wad_load(&wad, &gen_lump);
  music_data = mw_wad_load(&wad, &music_lump);
  if (!gen_data || !music_data) {
    fprintf(stderr, "ERROR: failed to load WAD lump data.\n");
    goto done;
  }

  error = snd_genmidi_opl_load_bank(&bank, gen_data,
                                    gen_lump.bytes);
  if (error != SND_GENMIDI_OPL_OK) {
    fprintf(stderr, "ERROR: GENMIDI: %s\n",
            snd_genmidi_opl_error_string(error));
    goto done;
  }
  error = snd_mus_open(&song, music_data, music_lump.bytes);
  if (error != SND_MUS_OK) {
    fprintf(stderr, "ERROR: MUS: %s\n",
            snd_mus_error_string(error));
    goto done;
  }

  total_frames_64 =
      (uint64_t)(unsigned int)args.rate *
      (uint64_t)(unsigned int)args.seconds;
  data_bytes_64 = total_frames_64 * MW_OPL_CHANNELS * 2u;
  if (total_frames_64 > (uint64_t)LONG_MAX ||
      data_bytes_64 > UINT32_MAX - 36u) {
    fprintf(stderr, "ERROR: requested render is too long.\n");
    goto done;
  }
  total_frames = (long)total_frames_64;
  data_bytes = (uint32_t)data_bytes_64;

  block = (snd_sample_t *)malloc(
      (size_t)args.block * MW_OPL_CHANNELS * sizeof(*block));
  if (!block) {
    fprintf(stderr, "ERROR: could not allocate mix block.\n");
    goto done;
  }

  snd_init(&mixer, args.rate, MW_OPL_CHANNELS,
           block, args.block, NULL, NULL);
#if SND_WIDE_ACCUM
  if (args.wide_accum) {
    accum = (int32_t *)malloc(
        (size_t)args.block * MW_OPL_CHANNELS * sizeof(*accum));
    if (!accum) {
      fprintf(stderr, "ERROR: could not allocate accumulator.\n");
      goto done;
    }
    snd_set_accumulator(&mixer, accum);
  }
#else
  if (args.wide_accum) {
    fprintf(stderr,
            "ERROR: this build has SND_WIDE_ACCUM=0; "
            "use --accum saturating.\n");
    goto done;
  }
#endif

  snd_midi_init(&midi);
  snd_genmidi_opl_init(&opl, args.rate);
  snd_genmidi_opl_set_bank(&opl, &bank);
  snd_genmidi_opl_set_output_gain(&opl, (int16_t)args.gain);
  snd_genmidi_opl_bind(&opl, &midi);
  if (!snd_mus_player_init(&player, &song, args.rate,
                           SND_MUS_DOOM_TICK_HZ, 0L,
                           args.loop)) {
    fprintf(stderr, "ERROR: could not initialize MUS player.\n");
    goto done;
  }

  wav = fopen(args.out_path, "wb");
  if (!wav) {
    fprintf(stderr, "ERROR: cannot create %s\n", args.out_path);
    goto done;
  }
  if (!mw_wav_header(wav, args.rate, data_bytes)) {
    fprintf(stderr, "ERROR: could not write WAV header.\n");
    goto done;
  }

  song_frames = mw_probe_song_frames(&song, args.rate);

  printf("MicroWave register-native Doom OPL renderer\n");
  printf("Backend:                  Nuked OPL3, OPL2 compatibility\n");
  printf("WAD:                      %s (%s, %lu lumps)\n",
         args.wad_path, wad.type, (unsigned long)wad.lump_count);
  printf("GENMIDI:                  %lu bytes\n",
         (unsigned long)gen_lump.bytes);
  printf("Music:                    %s, %lu bytes\n",
         music_lump.name, (unsigned long)music_lump.bytes);
  if (song_frames >= 0L)
    printf("MUS first-pass duration:  %.3f s\n",
           (double)song_frames / (double)args.rate);
  printf("Render:                   %d Hz stereo, block=%d, %d s, "
         "loop=%s, gain=%d/256, accum=%s\n",
         args.rate, args.block, args.seconds,
         args.loop ? "yes" : "no", args.gain,
         args.wide_accum ? "wide" : "saturating");
  printf("Output:                   %s\n\n", args.out_path);

  while (frame < total_frames) {
    long remaining = total_frames - frame;
    int count = (remaining > (long)args.block)
                    ? args.block
                    : (int)remaining;
    int emitted;

    snd_begin_block(&mixer, frame, count);
    emitted = snd_mus_process_until(&player, &midi,
                                    frame + (long)count);
    if (emitted < 0) {
      fprintf(stderr, "ERROR: MUS playback failed at frame %ld: %s\n",
              frame,
              snd_mus_error_string(
                  snd_mus_player_error(&player)));
      goto done;
    }
    if (opl.dropped_events != 0uL) {
      fprintf(stderr,
              "ERROR: OPL event queue dropped %lu events. "
              "Try a smaller --block.\n",
              opl.dropped_events);
      goto done;
    }

    snd_genmidi_opl_mix_block(&opl, &mixer);

#if SND_WIDE_ACCUM
    if (args.wide_accum && mixer.block_touched) {
      long sample_count =
          (long)mixer.block_frames * mixer.channels;
      long i;
      for (i = 0; i < sample_count; ++i) {
        int64_t value = (int64_t)accum[i];
        uint64_t magnitude =
            (value < 0) ? (uint64_t)(-value)
                        : (uint64_t)value;
        if (magnitude > stats.preclip_peak)
          stats.preclip_peak = magnitude;
        if (value < (int64_t)SND_MIX_MIN ||
            value > (int64_t)SND_MIX_MAX)
          ++stats.would_clip;
      }
    }
#endif

    snd_flush_block(&mixer);
    if (!mw_write_block(wav, mixer.block,
                        mixer.block_frames, &stats)) {
      fprintf(stderr, "ERROR: WAV write failed.\n");
      goto done;
    }
    frame += mixer.block_frames;
  }

  if (fflush(wav) != 0 || ferror(wav)) {
    fprintf(stderr, "ERROR: final WAV flush failed.\n");
    goto done;
  }

  printf("MUS loops completed:      %lu\n",
         player.loops_completed);
  printf("MIDI notes started:       %lu\n",
         opl.midi_notes_started);
  printf("OPL voices started:       %lu\n",
         opl.opl_voices_started);
  printf("OPL voices released:      %lu\n",
         opl.opl_voices_released);
  printf("OPL voices stolen:        %lu\n",
         opl.voices_stolen);
  printf("MIDI events dropped:      %lu\n",
         opl.dropped_events);
#if SND_WIDE_ACCUM
  if (args.wide_accum) {
    printf("Pre-clamp peak:            %llu / 32768 (%.2f%%)\n",
           (unsigned long long)stats.preclip_peak,
           (100.0 * (double)stats.preclip_peak) / 32768.0);
    printf("Would-clip samples:        %llu\n",
           (unsigned long long)stats.would_clip);
  }
#endif
  printf("Peak absolute sample:     %d / 32768 (%.2f%%)\n",
         stats.peak,
         (100.0 * (double)stats.peak) / 32768.0);
  printf("Full-scale samples:       %llu\n",
         (unsigned long long)stats.full_scale);
  if (stats.samples >= 2u) {
    double frames_written =
        (double)(stats.samples / MW_OPL_CHANNELS);
    printf("Mean sample L/R:          %.3f / %.3f\n",
           (double)stats.sum_l / frames_written,
           (double)stats.sum_r / frames_written);
  }
  printf("\nWrote %s\n", args.out_path);
  rc = 0;

done:
  if (wav)
    fclose(wav);
#if SND_WIDE_ACCUM
  free(accum);
#endif
  free(block);
  free(music_data);
  free(gen_data);
  mw_wad_close(&wad);
  return rc;
}
