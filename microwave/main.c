/* MicroWave RP2350 frontend: DMA-fed audio out.
 *
 * Build via the repository's mw.bat:  .\mw.bat build pico
 *
 * This is the target the pipelined render path was designed for. The pattern
 * is exactly the one MicroRender uses to feed an ILI9341 over SPI:
 *
 *   - two buffers
 *   - hand one to DMA, mix into the other
 *   - block only when the DMA has not finished and there is nothing else to do
 *
 * snd_render_blocked_pipelined() expresses that directly, given a drain_begin
 * that starts a transfer and a drain_wait that blocks until it completes. The
 * mixer never allocates, never blocks on its own behalf, and never knows what
 * a DMA channel is.
 *
 * HONESTY NOTE
 *
 * As with the DOS frontend, the mixer here is the tested one and the hardware
 * bringup is not. This compiles against the Pico SDK and follows the documented
 * PWM and DMA sequences, but it has not been run on silicon in this
 * repository's CI, which has no silicon. The audio path it feeds is verified;
 * the pins are your problem.
 *
 * Two output modes:
 *   MW_PICO_PWM  (default) 8-bit PWM on a single GPIO, one RC filter away from
 *                a speaker. No extra hardware.
 *   MW_PICO_I2S            16-bit stereo I2S via PIO, for an external DAC.
 */

#include "mw_music_demo.h"
#include "snd.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"

#include <string.h>

#ifndef MW_PICO_PWM
#define MW_PICO_PWM 1
#endif

#define MW_RATE 22050
#define MW_BLOCK 256
#define MW_AUDIO_PIN 18

/* Two mix buffers, so the mixer can work on one while DMA drains the other.
   These are the only audio buffers in the program; the mixer has none. */
static snd_sample_t g_block_a[MW_BLOCK];
static snd_sample_t g_block_b[MW_BLOCK];

/* PWM wants unsigned 8-bit levels, so each mix buffer needs a converted twin.
   The conversion happens in drain_begin, which is this target's equivalent of
   MicroRender's "convert during presentation" rule. */
static uint8_t g_pwm_a[MW_BLOCK];
static uint8_t g_pwm_b[MW_BLOCK];

static int g_dma_chan = -1;
static volatile int g_dma_busy = 0;
static uint g_pwm_slice = 0;
static mw_demo_t g_demo;

static void dma_done_isr(void) {
  dma_hw->ints0 = 1u << (unsigned)g_dma_chan;
  g_dma_busy = 0;
}

static void audio_hw_init(void) {
  gpio_set_function(MW_AUDIO_PIN, GPIO_FUNC_PWM);
  g_pwm_slice = pwm_gpio_to_slice_num(MW_AUDIO_PIN);

  /* 8-bit PWM carrier well above the audio band. */
  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_wrap(&cfg, 255);
  pwm_config_set_clkdiv(&cfg, 1.0f);
  pwm_init(g_pwm_slice, &cfg, true);

  g_dma_chan = dma_claim_unused_channel(true);

  irq_set_exclusive_handler(DMA_IRQ_0, dma_done_isr);
  dma_channel_set_irq0_enabled((uint)g_dma_chan, true);
  irq_set_enabled(DMA_IRQ_0, true);
}

/* Start a transfer and return immediately. The mixer will call drain_wait
   before it reuses this buffer, and not before, which is what makes the
   overlap safe. */
static void mw_drain_begin(snd_mixer_t *m, long frame, int frames,
                           const snd_sample_t *samples, void *user) {
  uint8_t *dst;

  (void)frame;
  (void)user;

  /* Pick the converted buffer that belongs to whichever mix buffer this is. */
  dst = (samples == g_block_a) ? g_pwm_a : g_pwm_b;

  if (!samples) {
    /* Nothing touched this block. Emitting midscale is both correct and the
       cheapest thing available. */
    memset(dst, 0x80, (size_t)frames);
  } else {
    snd_pack_u8(samples, dst, (long)frames * (long)m->channels);
  }

  g_dma_busy = 1;
  dma_channel_config c = dma_channel_get_default_config((uint)g_dma_chan);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
  channel_config_set_read_increment(&c, true);
  channel_config_set_write_increment(&c, false);
  channel_config_set_dreq(&c, DREQ_PWM_WRAP0 + g_pwm_slice);

  dma_channel_configure((uint)g_dma_chan, &c,
                        &pwm_hw->slice[g_pwm_slice].cc, /* write address */
                        dst,                            /* read address  */
                        (uint)frames, true);
}

/* Block until the previous transfer has finished. Called by the pipelined
   render loop only after the next block has already been mixed, so on a fast
   enough core this returns immediately and the mixer never stalls. */
static void mw_drain_wait(snd_mixer_t *m, void *user) {
  (void)m;
  (void)user;
  while (g_dma_busy)
    tight_loop_contents();
}

int main(void) {
  snd_mixer_t mixer;

  stdio_init_all();
  audio_hw_init();

  snd_init(&mixer, MW_RATE, 1, g_block_a, MW_BLOCK, NULL, NULL);
  snd_set_async_drain(&mixer, mw_drain_begin, mw_drain_wait);
  snd_set_master_gain(&mixer, SND_GAIN_UNITY);

  for (;;) {
    mw_demo_init(&g_demo, &mixer, 0, 0);

    /* One call renders the whole song, overlapping every DMA transfer with the
       mix of the next block. There is no audio thread, no ring buffer and no
       lock: the device's appetite drives the loop. */
    snd_render_blocked_pipelined(&mixer, g_block_b,
                                 mw_demo_length_frames(&g_demo, &mixer),
                                 mw_demo_mix, &g_demo, SND_RENDER_SKIP_SILENT);

    sleep_ms(1000);
  }
}
