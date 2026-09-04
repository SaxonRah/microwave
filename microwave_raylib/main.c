/* MicroWave desktop frontend.
 *
 * Raylib owns the device; MicroWave owns the mix. The only thing this file
 * does that the DOS and Pico frontends do not is convert the finished S16
 * block into the float buffer Raylib's audio stream wants, and it does that in
 * the drain callback -- the same place DOS quantizes to 8-bit unsigned. The
 * mixing code above it is byte-identical on all three.
 *
 * Build with the repository's mw.bat:  .\mw.bat build raylib
 *
 * A headless mode is included so CI can exercise this file without an audio
 * device. That is the whole reason the frontend is split from the mixer: the
 * part that can be tested is tested, and the part that cannot is small enough
 * to read.
 */

#include "mw_music_demo.h"
#include "snd.h"
#include "snd_pack.h"
#include "snd_seq.h"
#include "snd_synth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MW_HEADLESS
#define MW_HEADLESS 0
#endif

#if !MW_HEADLESS
#include "raylib.h"
#endif

/* Overridable from CMake, so a deliberately odd block size can be used to
   shake out block-boundary bugs without editing this file. */
#ifndef MW_RATE
#define MW_RATE 22050
#endif
#ifndef MW_CHANNELS
#define MW_CHANNELS 1
#endif
/* 1024 frames is 46 ms, which is comfortable for a desktop audio callback and
   deliberately different from the DOS and Pico buffer sizes. If the three
   targets ever stop agreeing, the block size is the first thing to suspect,
   and the unit tests already rule it out. */
#ifndef MW_BLOCK
#define MW_BLOCK 1024
#endif

static snd_sample_t g_block[MW_BLOCK * MW_CHANNELS];
static snd_sample_t g_block_b[MW_BLOCK * MW_CHANNELS];
#if SND_WIDE_ACCUM
static int32_t g_accum[MW_BLOCK * MW_CHANNELS];
#endif
static float g_float[MW_BLOCK * MW_CHANNELS];

/* Optional: audio pulled straight out of a MicroRender GAME.MRP. */
static unsigned char g_clip_bytes[192 * 1024];
static snd_clip_t g_sfx_clip;
static int g_have_sfx = 0;

typedef struct {
  mw_demo_t demo;
  long frames_produced;
#if !MW_HEADLESS
  AudioStream stream;
#endif
} app_t;

/* ------------------------------------------------------------------ */
/* the drain: the only target-specific code in this file               */
/* ------------------------------------------------------------------ */

static void mw_drain_float(snd_mixer_t *m, long frame, int frames,
                           const snd_sample_t *samples, void *user) {
  app_t *app = (app_t *)user;
  long n = (long)frames * (long)m->channels;

  (void)frame;

  if (!samples) {
    /* SND_RENDER_SKIP_SILENT handed us a block nothing touched. */
    long i;
    for (i = 0; i < n; ++i)
      g_float[i] = 0.0f;
  } else {
    snd_pack_float(samples, g_float, n);
  }

#if !MW_HEADLESS
  UpdateAudioStream(app->stream, g_float, frames);
#else
  (void)app;
#endif
}

/* ------------------------------------------------------------------ */
/* optional pack loading                                               */
/* ------------------------------------------------------------------ */

/* Look for a pack next to the executable and pull one sound effect out of it.
   Note that this opens GAME.MRP, MicroRender's own pack, not a MicroWave one:
   the audio entries in it are already in a format this library reads, so a
   project that has a renderer pack does not need a second asset pipeline. */
static void try_load_pack(const char *path, const char *entry) {
  snd_pack_t pack;

  if (!snd_pack_open(&pack, path)) {
    printf("no pack at %s (that is fine; the demo needs no assets)\n", path);
    return;
  }

  printf("opened %s (%s container, %u entries)\n", path,
         snd_pack_is_microrender(&pack) ? "MicroRender MRP1" : "MicroWave MWP1",
         (unsigned)pack.count);

  if (snd_pack_load_clip(&pack, entry, g_clip_bytes, sizeof(g_clip_bytes),
                         &g_sfx_clip)) {
    g_have_sfx = 1;
    printf("  loaded '%s': %lu frames at %d Hz\n", entry,
           (unsigned long)g_sfx_clip.frames, g_sfx_clip.rate);
  } else {
    printf("  entry '%s' not present or not usable audio\n", entry);
  }

  snd_pack_close(&pack);
}

/* ------------------------------------------------------------------ */

static void usage(const char *argv0) {
  printf("usage: %s [options]\n", argv0);
  printf("  --seconds N     how long to play (default: one pass of the song)\n");
  printf("  --pack PATH     load sound effects from a MicroWave or "
         "MicroRender pack\n");
  printf("  --entry NAME    pack entry to use as the effect (default "
         "\"pickup\")\n");
  printf("  --wav PATH      also write the mix to a RIFF WAV file\n");
  printf("  --volume N      initial volume, 0..100 (default 100)\n");
  printf("  --help          this text\n");
}

/* Writing a WAV is not part of the library. It lives here because a frontend
   is exactly where "turn the canonical format into something this platform
   understands" belongs. */
static FILE *wav_open(const char *path, int rate, int channels) {
  FILE *f = fopen(path, "wb");
  unsigned char h[44];
  if (!f)
    return NULL;
  memset(h, 0, sizeof(h));
  memcpy(h, "RIFF", 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  h[16] = 16;
  h[20] = 1;
  h[22] = (unsigned char)channels;
  h[24] = (unsigned char)(rate & 0xFF);
  h[25] = (unsigned char)((rate >> 8) & 0xFF);
  h[26] = (unsigned char)((rate >> 16) & 0xFF);
  h[32] = (unsigned char)(channels * 2);
  h[34] = 16;
  memcpy(h + 36, "data", 4);
  fwrite(h, 1, sizeof(h), f);
  return f;
}

static void wav_close(FILE *f, long samples) {
  unsigned long data = (unsigned long)samples * 2uL;
  unsigned long riff = data + 36uL;
  unsigned char v[4];
  if (!f)
    return;
  fseek(f, 4, SEEK_SET);
  v[0] = (unsigned char)(riff & 0xFF);
  v[1] = (unsigned char)((riff >> 8) & 0xFF);
  v[2] = (unsigned char)((riff >> 16) & 0xFF);
  v[3] = (unsigned char)((riff >> 24) & 0xFF);
  fwrite(v, 1, 4, f);
  fseek(f, 40, SEEK_SET);
  v[0] = (unsigned char)(data & 0xFF);
  v[1] = (unsigned char)((data >> 8) & 0xFF);
  v[2] = (unsigned char)((data >> 16) & 0xFF);
  v[3] = (unsigned char)((data >> 24) & 0xFF);
  fwrite(v, 1, 4, f);
  fclose(f);
}

static FILE *g_wav = NULL;
static long g_wav_samples = 0;

static void mw_drain_tee(snd_mixer_t *m, long frame, int frames,
                         const snd_sample_t *samples, void *user) {
  mw_drain_float(m, frame, frames, samples, user);

  if (g_wav) {
    long n = (long)frames * (long)m->channels;
    long i;
    for (i = 0; i < n; ++i) {
      int v = samples ? SND_SAMPLE_TO_MIX(samples[i]) : 0;
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
      v *= 256;
#endif
      fputc(v & 0xFF, g_wav);
      fputc((v >> 8) & 0xFF, g_wav);
    }
    g_wav_samples += n;
  }
}

int main(int argc, char **argv) {
  snd_mixer_t mixer;
  app_t app;
  const char *pack_path = NULL;
  const char *entry = "pickup";
  const char *wav_path = NULL;
  double seconds = -1.0;
  int volume_pct = 100;
#if !MW_HEADLESS
  int muted = 0; /* only the interactive build has a key to toggle it */
#endif
  long total_frames;
  int i;

  memset(&app, 0, sizeof(app));

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = atof(argv[++i]);
    } else if (strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
      pack_path = argv[++i];
    } else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) {
      entry = argv[++i];
    } else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
      wav_path = argv[++i];
    } else if (strcmp(argv[i], "--volume") == 0 && i + 1 < argc) {
      volume_pct = atoi(argv[++i]);
      if (volume_pct < 0)
        volume_pct = 0;
      if (volume_pct > 100)
        volume_pct = 100;
    } else {
      printf("unknown argument: %s\n\n", argv[i]);
      usage(argv[0]);
      return 1;
    }
  }

  if (pack_path)
    try_load_pack(pack_path, entry);

  snd_init(&mixer, MW_RATE, MW_CHANNELS, g_block, MW_BLOCK, mw_drain_tee, &app);
#if SND_WIDE_ACCUM
  snd_set_accumulator(&mixer, g_accum);
#endif
  /* The volume applies to the WAV as well as to the device, because it is an
     output-stage gain in the mixer rather than something the audio backend
     does on the way out. Rendering headless at --volume 50 gives a file that
     is about 12 dB down, which is the point of putting it in the DSP path. */
  snd_set_master_volume_now(&mixer, snd_vol_from_percent(volume_pct));
  snd_set_volume_ramp(&mixer, MW_RATE / 50); /* 20 ms, no clicks while dragging */

  mw_demo_init(&app.demo, &mixer, 0, 0);

  if (g_have_sfx) {
    /* Drop the pack effect in a few times so it is audible against the song. */
    mw_demo_trigger_sfx(&app.demo, &mixer, &g_sfx_clip, MW_RATE / 2);
    mw_demo_trigger_sfx(&app.demo, &mixer, &g_sfx_clip, MW_RATE * 2);
    mw_demo_trigger_sfx(&app.demo, &mixer, &g_sfx_clip, MW_RATE * 7 / 2);
  }

  total_frames = (seconds > 0.0) ? (long)(seconds * (double)MW_RATE)
                                 : mw_demo_length_frames(&app.demo, &mixer);
  if (total_frames <= 0)
    total_frames = MW_RATE * 10;

  if (wav_path) {
    g_wav = wav_open(wav_path, MW_RATE, MW_CHANNELS);
    if (!g_wav)
      printf("could not open %s for writing\n", wav_path);
  }

#if MW_HEADLESS
  printf("MicroWave headless: %ld frames (%.2f s) at %d Hz\n", total_frames,
         (double)total_frames / (double)MW_RATE, MW_RATE);
  snd_render_blocked_ex(&mixer, total_frames, mw_demo_mix, &app.demo,
                        SND_RENDER_SKIP_SILENT);
  printf("done\n");
#else
  InitWindow(480, 200, "MicroWave");
  InitAudioDevice();

  /* Raylib treats this value as the size of ONE streaming sub-buffer and
     internally allocates two of them. UpdateAudioStream() fills one sub-buffer
     at a time, so the requested sub-buffer size must match one MicroWave block. */
  SetAudioStreamBufferSizeDefault(MW_BLOCK);
  app.stream = LoadAudioStream(MW_RATE, 32, MW_CHANNELS);
  PlayAudioStream(app.stream);
  SetTargetFPS(60);

  while (!WindowShouldClose() && app.frames_produced < total_frames) {
    /* Push exactly as many blocks as the device has room for. The mixer is
       driven by the device's appetite, not by a frame timer -- the audio
       equivalent of never rendering a tile the display is not ready for. */
    while (IsAudioStreamProcessed(app.stream) &&
           app.frames_produced < total_frames) {
      long left = total_frames - app.frames_produced;
      int n = (left > MW_BLOCK) ? MW_BLOCK : (int)left;
      snd_render_one_block(&mixer, app.frames_produced, n, mw_demo_mix,
                           &app.demo, SND_RENDER_SKIP_SILENT);
      app.frames_produced += n;
    }

    if (IsKeyPressed(KEY_SPACE) && g_have_sfx)
      mw_demo_trigger_sfx(&app.demo, &mixer, &g_sfx_clip,
                          app.frames_produced + MW_BLOCK);

    /* Volume. Held keys repeat, and snd_set_master_volume() ramps, so holding
       a key sweeps smoothly instead of stepping. The mixer owns the target, so
       there is no shadow copy here that can drift out of sync with it. */
    if (IsKeyDown(KEY_MINUS) || IsKeyDown(KEY_KP_SUBTRACT)) {
      volume_pct = snd_vol_to_percent(snd_master_volume(&mixer)) - 1;
      if (volume_pct < 0)
        volume_pct = 0;
      snd_set_master_volume(&mixer, snd_vol_from_percent(volume_pct));
      muted = 0;
    }
    if (IsKeyDown(KEY_EQUAL) || IsKeyDown(KEY_KP_ADD)) {
      volume_pct = snd_vol_to_percent(snd_master_volume(&mixer)) + 1;
      if (volume_pct > 100)
        volume_pct = 100;
      snd_set_master_volume(&mixer, snd_vol_from_percent(volume_pct));
      muted = 0;
    }
    if (IsKeyPressed(KEY_M)) {
      muted = !muted;
      snd_set_master_volume(
          &mixer, muted ? SND_VOL_SILENT : snd_vol_from_percent(volume_pct));
    }

    BeginDrawing();
    ClearBackground((Color){18, 18, 24, 255});
    DrawText("MicroWave", 20, 24, 40, (Color){235, 235, 245, 255});
    DrawText(g_have_sfx ? "SPACE: effect   -/+: volume   M: mute"
                        : "no pack loaded   -/+: volume   M: mute",
             20, 80, 20, (Color){150, 150, 170, 255});
    DrawText(TextFormat("%.1f / %.1f s",
                        (double)app.frames_produced / (double)MW_RATE,
                        (double)total_frames / (double)MW_RATE),
             20, 120, 20, (Color){150, 150, 170, 255});
    {
      /* Show the target, not the ramping value: a readout that chases the ramp
         looks broken even though it is telling the truth. */
      int shown = snd_vol_to_percent(snd_master_volume(&mixer));
      DrawText(TextFormat(muted ? "volume %d%% (muted)" : "volume %d%%", shown),
               20, 150, 20, (Color){150, 150, 170, 255});
      DrawRectangle(200, 156, 200, 8, (Color){40, 40, 52, 255});
      DrawRectangle(200, 156, 2 * shown, 8,
                    muted ? (Color){90, 90, 110, 255}
                          : (Color){120, 200, 160, 255});
    }
    EndDrawing();
  }

  UnloadAudioStream(app.stream);
  CloseAudioDevice();
  CloseWindow();
#endif

  if (g_wav) {
    wav_close(g_wav, g_wav_samples);
    printf("wrote %s (%ld samples)\n", wav_path, g_wav_samples);
  }
  (void)g_block_b;
  return 0;
}
