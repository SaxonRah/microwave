/* MicroWave RP2350 frontend: PIO + DMA three-wire I2S audio.
 *
 * The shared mixer still owns every sample. This file is only the presentation
 * layer, exactly as the Pico frontend in MicroRender only presents finished
 * RGB565 tiles. Two S16 mix buffers are alternated while DMA drains the other.
 *
 * Supported sinks share the same standard I2S transport:
 *   MAX98357A  - mono Class-D I2S amplifier
 *   PCM5102A   - stereo DAC, used in 3-wire/BCK-PLL mode (no MCLK required)
 *   NS4168     - mono Class-D I2S amplifier
 *
 * The mono MicroWave mix is duplicated into both left and right I2S slots.
 * That deliberately makes the channel-select pins on MAX98357A and NS4168
 * irrelevant to the demo audio: either selected channel contains the same
 * signal. PCM5102A produces the same signal on both analog outputs.
 */

#include "mw_music_demo.h"
#include "snd.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "mw_i2s.pio.h"
#include "pico/stdlib.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MW_PICO_DEVICE
#define MW_PICO_DEVICE 1
#endif
#ifndef MW_PICO_RATE
#define MW_PICO_RATE 32000
#endif
#ifndef MW_PICO_BLOCK
#define MW_PICO_BLOCK 256
#endif
#ifndef MW_PICO_SYS_KHZ
#define MW_PICO_SYS_KHZ 0
#endif
#ifndef MW_I2S_PIN_BCLK
#define MW_I2S_PIN_BCLK 10
#endif
#ifndef MW_I2S_PIN_LRCLK
#define MW_I2S_PIN_LRCLK 11
#endif
#ifndef MW_I2S_PIN_DATA
#define MW_I2S_PIN_DATA 12
#endif
#ifndef MW_I2S_PIO
#define MW_I2S_PIO 0
#endif
#ifndef MW_I2S_SM
#define MW_I2S_SM 0
#endif
#ifndef MW_I2S_DMA
#define MW_I2S_DMA 0
#endif
#ifndef MW_PICO_SERIAL
#define MW_PICO_SERIAL 1
#endif

#if MW_I2S_PIN_LRCLK != (MW_I2S_PIN_BCLK + 1)
#error "MW_I2S_PIN_LRCLK must equal MW_I2S_PIN_BCLK + 1"
#endif

static snd_sample_t g_block_a[MW_PICO_BLOCK];
static snd_sample_t g_block_b[MW_PICO_BLOCK];
static uint32_t g_i2s_a[MW_PICO_BLOCK];
static uint32_t g_i2s_b[MW_PICO_BLOCK];

#if MW_I2S_PIO == 1
#define MW_AUDIO_PIO pio1
#define MW_AUDIO_GPIO_FUNC GPIO_FUNC_PIO1
#define MW_AUDIO_DREQ_BASE DREQ_PIO1_TX0
#else
#define MW_AUDIO_PIO pio0
#define MW_AUDIO_GPIO_FUNC GPIO_FUNC_PIO0
#define MW_AUDIO_DREQ_BASE DREQ_PIO0_TX0
#endif

static PIO g_pio = MW_AUDIO_PIO;
static uint g_sm = MW_I2S_SM;
static int g_dma_chan = MW_I2S_DMA;
static mw_demo_t g_demo;

static const char *mw_device_name(void) {
#if MW_PICO_DEVICE == 1
  return "MAX98357A";
#elif MW_PICO_DEVICE == 2
  return "PCM5102A";
#elif MW_PICO_DEVICE == 3
  return "NS4168";
#else
  return "UNKNOWN";
#endif
}

/* The serial command handler adjusts volume, so it needs the mixer. It is a
   pointer rather than a copy because mw_drain_wait() runs between blocks, and
   a volume set there must land on the mixer the render loop is using. */
static snd_mixer_t *g_mixer = NULL;

#if MW_PICO_SERIAL
static char g_cmd[32];
static unsigned g_cmd_n = 0;

/* "VOL 60" -> 60. Returns -1 when the line is not a volume command. */
static int mw_parse_vol(const char *cmd) {
  int v = 0;
  const char *p = cmd;

  if (strncmp(p, "VOL", 3) != 0)
    return -1;
  p += 3;
  while (*p == ' ')
    ++p;
  if (*p < '0' || *p > '9')
    return -1;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    if (v > 100)
      return 100;
    ++p;
  }
  return v;
}

static void mw_service_stdio(void) {
  int ch;
  while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
    if (ch == '\r' || ch == '\n') {
      if (g_cmd_n != 0) {
        g_cmd[g_cmd_n] = '\0';
        if (strcmp(g_cmd, "PING") == 0) {
          printf("MWPICO1 device=%s rate=%d bclk=%d lrclk=%d data=%d vol=%d\n",
                 mw_device_name(), MW_PICO_RATE, MW_I2S_PIN_BCLK,
                 MW_I2S_PIN_LRCLK, MW_I2S_PIN_DATA,
                 snd_vol_to_percent(snd_master_volume(g_mixer)));
          fflush(stdout);
        } else {
          /* Volume is the one control this board has, since there are no
             buttons wired. It is serviced from mw_drain_wait(), so it takes
             effect on the next block rather than at the end of the song --
             about 6 ms at the default block size. */
          int v = mw_parse_vol(g_cmd);
          if (v >= 0 && g_mixer) {
            snd_set_master_volume(g_mixer, snd_vol_from_percent(v));
            printf("MWPICO1 vol=%d\n", v);
            fflush(stdout);
          }
        }
        g_cmd_n = 0;
      }
    } else if (g_cmd_n + 1u < sizeof(g_cmd)) {
      g_cmd[g_cmd_n++] = (char)ch;
    } else {
      g_cmd_n = 0;
    }
  }
}
#else
static void mw_service_stdio(void) {}
#endif

static int16_t mw_sample_to_s16(snd_sample_t s) {
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
  return (int16_t)(((int)s - 128) << 8);
#else
  return (int16_t)s;
#endif
}

static void audio_hw_init(void) {
  uint offset;
  uint32_t sys_hz;
  uint32_t div256;
  dma_channel_config dc;

#if MW_PICO_SYS_KHZ > 0
  if (!set_sys_clock_khz(MW_PICO_SYS_KHZ, false))
    panic("MicroWave: could not set requested system clock");
#endif

  pio_sm_claim(g_pio, g_sm);
  dma_channel_claim((uint)g_dma_chan);

  gpio_set_function(MW_I2S_PIN_DATA, MW_AUDIO_GPIO_FUNC);
  gpio_set_function(MW_I2S_PIN_BCLK, MW_AUDIO_GPIO_FUNC);
  gpio_set_function(MW_I2S_PIN_LRCLK, MW_AUDIO_GPIO_FUNC);

  if (!pio_can_add_program(g_pio, &mw_i2s_program))
    panic("MicroWave: no room for I2S PIO program");
  offset = pio_add_program(g_pio, &mw_i2s_program);
  mw_i2s_program_init(g_pio, g_sm, offset,
                      MW_I2S_PIN_DATA, MW_I2S_PIN_BCLK);

  /* The PIO program executes 64 instructions per stereo frame: two PIO
     instructions for each of 32 transmitted bits. The SDK divider register is
     16.8 fixed point, so divider*256 = clk_sys*4/sample_rate. This is the same
     calculation used by Raspberry Pi's pico_audio_i2s backend. */
  sys_hz = clock_get_hz(clk_sys);
  div256 = (uint32_t)((((uint64_t)sys_hz * 4u) + MW_PICO_RATE / 2u) /
                      (uint32_t)MW_PICO_RATE);
  if ((div256 >> 8u) == 0u || (div256 >> 8u) > 0xFFFFu)
    panic("MicroWave: I2S PIO clock divider is out of range");
  pio_sm_set_clkdiv_int_frac(g_pio, g_sm,
                             (uint16_t)(div256 >> 8u),
                             (uint8_t)(div256 & 0xFFu));

  dc = dma_channel_get_default_config((uint)g_dma_chan);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, MW_AUDIO_DREQ_BASE + g_sm);
  dma_channel_configure((uint)g_dma_chan, &dc,
                        &g_pio->txf[g_sm], NULL, 0, false);

  pio_sm_set_enabled(g_pio, g_sm, true);
}

static void mw_drain_begin(snd_mixer_t *m, long frame, int frames,
                           const snd_sample_t *samples, void *user) {
  uint32_t *dst = (m->block == g_block_a) ? g_i2s_a : g_i2s_b;
  int i;

  (void)frame;
  (void)user;

  for (i = 0; i < frames; ++i) {
    int16_t left = 0;
    int16_t right = 0;
    if (samples) {
      if (m->channels == 2) {
        left = mw_sample_to_s16(samples[i * 2]);
        right = mw_sample_to_s16(samples[i * 2 + 1]);
      } else {
        left = mw_sample_to_s16(samples[i]);
        right = left;
      }
    }
    dst[i] = ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
  }

  dma_channel_set_read_addr((uint)g_dma_chan, dst, false);
  dma_channel_set_trans_count((uint)g_dma_chan, (uint32_t)frames, true);
}

static void mw_drain_wait(snd_mixer_t *m, void *user) {
  (void)m;
  (void)user;
  while (dma_channel_is_busy((uint)g_dma_chan)) {
    mw_service_stdio();
    tight_loop_contents();
  }
  mw_service_stdio();
}

int main(void) {
  snd_mixer_t mixer;

  audio_hw_init();
#if MW_PICO_SERIAL
  stdio_init_all();
  printf("MicroWave Pico I2S: %s, %d Hz, BCLK GP%d, LRCLK GP%d, DATA GP%d\n",
         mw_device_name(), MW_PICO_RATE, MW_I2S_PIN_BCLK,
         MW_I2S_PIN_LRCLK, MW_I2S_PIN_DATA);
  printf("MWPICO1 ready\n");
#endif

  snd_init(&mixer, MW_PICO_RATE, 1, g_block_a, MW_PICO_BLOCK, NULL, NULL);
  snd_set_async_drain(&mixer, mw_drain_begin, mw_drain_wait);
  g_mixer = &mixer;
  /* Ramped, because a VOL command arriving between two blocks would otherwise
     be a step discontinuity straight into the DAC. 512 frames is about 23 ms
     at 22050 Hz. */
  snd_set_master_volume_now(&mixer, SND_VOL_UNITY);
  snd_set_volume_ramp(&mixer, 512);

  for (;;) {
    mw_demo_init(&g_demo, &mixer, 0, 0);
    snd_render_blocked_pipelined(&mixer, g_block_b,
                                 mw_demo_length_frames(&g_demo, &mixer),
                                 mw_demo_mix, &g_demo,
                                 SND_RENDER_SKIP_SILENT);
    /* Restart immediately. Keeping BCLK/LRCLK gaps short matters for 3-wire
       DACs such as PCM5102A, whose internal PLL derives its system clock from
       the incoming bit clock. */
    mw_service_stdio();
  }
}
