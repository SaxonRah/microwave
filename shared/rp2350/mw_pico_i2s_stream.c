#include "mw_pico_i2s_stream.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "mw_i2s.pio.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if MW_PICO_I2S_PERIOD_FRAMES <= 0
#error "MW_PICO_I2S_PERIOD_FRAMES must be positive"
#endif
#if MW_PICO_I2S_RING_COUNT < 2
#error "MW_PICO_I2S_RING_COUNT must be at least 2"
#endif
#if MW_PICO_I2S_RING_TARGET < 1 || \
    MW_PICO_I2S_RING_TARGET >= MW_PICO_I2S_RING_COUNT
#error "MW_PICO_I2S_RING_TARGET must be 1..MW_PICO_I2S_RING_COUNT-1"
#endif
#if MW_PICO_I2S_PRODUCER_FRAMES <= 0
#error "MW_PICO_I2S_PRODUCER_FRAMES must be positive"
#endif
#if MW_PICO_I2S_CORE1_STACK_BYTES < 2048
#error "MW_PICO_I2S_CORE1_STACK_BYTES must be at least 2048"
#endif
#if (MW_PICO_I2S_CORE1_STACK_BYTES % 4) != 0
#error "MW_PICO_I2S_CORE1_STACK_BYTES must be a multiple of 4"
#endif
#if MW_PICO_I2S_TIMING_WINDOW <= 0
#error "MW_PICO_I2S_TIMING_WINDOW must be positive"
#endif

enum {
  MW_PICO_RING_FREE = 0,
  MW_PICO_RING_READY = 1,
  MW_PICO_RING_ACTIVE = 2
};

static mw_pico_i2s_stream_config_t g_cfg;
static uint32_t g_i2s[MW_PICO_I2S_RING_COUNT][MW_PICO_I2S_PERIOD_FRAMES];
static uint32_t g_silence[MW_PICO_I2S_PERIOD_FRAMES];
static snd_sample_t g_producer_block[MW_PICO_I2S_PRODUCER_FRAMES * 2];
static volatile uint8_t g_ring_state[MW_PICO_I2S_RING_COUNT];
static volatile unsigned int g_ring_write;
static volatile unsigned int g_ring_read;
static volatile int g_active = -1;

static int g_dma = -1;
static int g_dma_claimed;
static PIO g_pio;
static uint g_pio_offset;
static int g_pio_claimed;
static int g_irq_installed;

static volatile int g_ready;
static int g_failed;
static volatile int g_core1_started;
static volatile int g_core1_running;

static volatile unsigned long g_underruns;
static volatile unsigned long g_refills;
static volatile uint32_t g_last_refill_us;
static volatile uint32_t g_min_refill_us;
static volatile uint32_t g_avg_refill_us;
static volatile uint32_t g_max_refill_us;
static volatile unsigned int g_ring_low_water = MW_PICO_I2S_RING_TARGET;
static volatile unsigned int g_ring_high_water;
static uint32_t g_timing_samples[MW_PICO_I2S_TIMING_WINDOW];
static uint32_t g_timing_sum;
static unsigned int g_timing_count;
static unsigned int g_timing_pos;

static mw_pico_i2s_control_fn volatile g_control_fn;
static void *volatile g_control_user;
static volatile uint32_t g_control_request_seq;
static volatile uint32_t g_control_done_seq;
static volatile unsigned long g_control_timeouts;

static uint32_t __attribute__((aligned(8)))
    g_core1_stack[MW_PICO_I2S_CORE1_STACK_BYTES / sizeof(uint32_t)];

#if defined(__GNUC__)
#define MW_PICO_I2S_HOT(name) \
  __attribute__((noinline, section(".time_critical." #name))) name
#else
#define MW_PICO_I2S_HOT(name) name
#endif

static PIO mw_pico_stream_pio(unsigned int index) {
  return index == 0u ? pio0 : pio1;
}

static gpio_function_t mw_pico_stream_gpio_func(unsigned int index) {
  return index == 0u ? GPIO_FUNC_PIO0 : GPIO_FUNC_PIO1;
}

static uint mw_pico_stream_dreq_base(unsigned int index) {
  return index == 0u ? DREQ_PIO0_TX0 : DREQ_PIO1_TX0;
}

static int16_t mw_pico_sample_s16(snd_sample_t sample) {
#if SND_SAMPLE_FORMAT == SND_SAMPLE_FORMAT_U8
  return (int16_t)(((int)sample - 128) << 8);
#else
  return (int16_t)sample;
#endif
}

static uint32_t mw_pico_pack_frame(int16_t left, int16_t right) {
  if (g_cfg.output_mode == MW_PICO_I2S_OUTPUT_MONO_MIXDOWN) {
    int32_t mono = ((int32_t)left + (int32_t)right) / 2;
    uint16_t sample = (uint16_t)(int16_t)mono;
    return ((uint32_t)sample << 16) | (uint32_t)sample;
  }

  return ((uint32_t)(uint16_t)left << 16) | (uint32_t)(uint16_t)right;
}

static unsigned int mw_pico_ring_ready_count_snapshot(void) {
  unsigned int i;
  unsigned int count = 0u;

  for (i = 0u; i < (unsigned int)MW_PICO_I2S_RING_COUNT; ++i) {
    if (g_ring_state[i] == MW_PICO_RING_READY)
      ++count;
  }

  return count;
}

static void mw_pico_control_checkpoint(void);

static void MW_PICO_I2S_HOT(mw_pico_fill)(unsigned int index) {
  unsigned int offset = 0u;

  while (offset < (unsigned int)MW_PICO_I2S_PERIOD_FRAMES) {
    unsigned int remaining = (unsigned int)MW_PICO_I2S_PERIOD_FRAMES - offset;
    unsigned int chunk = remaining > (unsigned int)MW_PICO_I2S_PRODUCER_FRAMES
                             ? (unsigned int)MW_PICO_I2S_PRODUCER_FRAMES
                             : remaining;
    unsigned int sample_count = chunk * g_cfg.channels;
    unsigned int produced;
    unsigned int i;

    for (i = 0u; i < sample_count; ++i)
      g_producer_block[i] = SND_SAMPLE_SILENCE;

    produced = g_cfg.producer(g_producer_block, chunk, g_cfg.channels,
                              g_cfg.producer_user);
    if (produced > chunk)
      produced = chunk;

    for (i = 0u; i < chunk; ++i) {
      int16_t left = 0;
      int16_t right = 0;

      if (i < produced) {
        if (g_cfg.channels == 2u) {
          left = mw_pico_sample_s16(g_producer_block[i * 2u]);
          right = mw_pico_sample_s16(g_producer_block[i * 2u + 1u]);
        } else {
          left = mw_pico_sample_s16(g_producer_block[i]);
          right = left;
        }
      }

      g_i2s[index][offset + i] = mw_pico_pack_frame(left, right);
    }

    offset += chunk;
    mw_pico_control_checkpoint();
  }
}

static void mw_pico_start_dma_buffer(const uint32_t *buffer) {
  dma_channel_set_read_addr((uint)g_dma, buffer, false);
  dma_channel_set_trans_count((uint)g_dma,
                              (uint32_t)MW_PICO_I2S_PERIOD_FRAMES,
                              true);
}

static void mw_pico_dma_irq(void) {
  uint32_t mask;
  unsigned int ready_after;

  if (g_dma < 0)
    return;

  mask = 1u << (unsigned int)g_dma;
  if ((dma_hw->ints0 & mask) == 0u)
    return;

  dma_hw->ints0 = mask;

  if (g_active >= 0) {
    g_ring_state[g_active] = MW_PICO_RING_FREE;
    g_active = -1;
    __dmb();
  }

  if (g_ready && g_ring_state[g_ring_read] == MW_PICO_RING_READY) {
    unsigned int index = g_ring_read;

    g_ring_state[index] = MW_PICO_RING_ACTIVE;
    g_active = (int)index;
    g_ring_read = (index + 1u) % (unsigned int)MW_PICO_I2S_RING_COUNT;
    __dmb();

    mw_pico_start_dma_buffer(g_i2s[index]);
  } else if (g_ready) {
    ++g_underruns;
    mw_pico_start_dma_buffer(g_silence);
  }

  ready_after = mw_pico_ring_ready_count_snapshot();
  if (ready_after < g_ring_low_water)
    g_ring_low_water = ready_after;
}

static void mw_pico_update_timing(uint32_t elapsed_us) {
  uint32_t old = g_timing_samples[g_timing_pos];

  g_timing_sum -= old;
  g_timing_samples[g_timing_pos] = elapsed_us;
  g_timing_sum += elapsed_us;

  g_timing_pos =
      (g_timing_pos + 1u) % (unsigned int)MW_PICO_I2S_TIMING_WINDOW;
  if (g_timing_count < (unsigned int)MW_PICO_I2S_TIMING_WINDOW)
    ++g_timing_count;

  if (g_timing_count != 0u)
    g_avg_refill_us = g_timing_sum / g_timing_count;
}

static void MW_PICO_I2S_HOT(mw_pico_service_once)(void) {
  unsigned int index;
  unsigned int ready;
  uint32_t begin_us;
  uint32_t elapsed_us;

  if (!g_ready)
    return;

  ready = mw_pico_ring_ready_count_snapshot();
  if (ready >= (unsigned int)MW_PICO_I2S_RING_TARGET)
    return;

  index = g_ring_write;
  if (g_ring_state[index] != MW_PICO_RING_FREE)
    return;

  begin_us = time_us_32();
  mw_pico_fill(index);
  elapsed_us = time_us_32() - begin_us;

  __dmb();
  g_ring_state[index] = MW_PICO_RING_READY;
  g_ring_write = (index + 1u) % (unsigned int)MW_PICO_I2S_RING_COUNT;
  __dmb();

  g_last_refill_us = elapsed_us;
  if (g_min_refill_us == 0u || elapsed_us < g_min_refill_us)
    g_min_refill_us = elapsed_us;
  if (elapsed_us > g_max_refill_us)
    g_max_refill_us = elapsed_us;
  mw_pico_update_timing(elapsed_us);
  ++g_refills;

  ready = mw_pico_ring_ready_count_snapshot();
  if (ready > g_ring_high_water)
    g_ring_high_water = ready;
}

static void mw_pico_control_checkpoint(void) {
  uint32_t request = g_control_request_seq;
  mw_pico_i2s_control_fn fn;
  void *user;

  if (request == g_control_done_seq)
    return;

  __dmb();
  fn = g_control_fn;
  user = g_control_user;

  if (fn != NULL)
    fn(user);

  __dmb();
  g_control_done_seq = request;
  __dmb();
}

static void mw_pico_core1_main(void) {
  g_core1_running = 1;
  __dmb();

  for (;;) {
    mw_pico_control_checkpoint();

    if (g_ready &&
        mw_pico_ring_ready_count_snapshot() <
            (unsigned int)MW_PICO_I2S_RING_TARGET)
      mw_pico_service_once();
    else
      tight_loop_contents();
  }
}

static int mw_pico_config_valid(const mw_pico_i2s_stream_config_t *config) {
  if (config == NULL || config->producer == NULL)
    return 0;
  if (config->sample_rate == 0u)
    return 0;
  if (config->channels != 1u && config->channels != 2u)
    return 0;
  if (config->pin_lrclk != config->pin_bclk + 1u)
    return 0;
  if (config->pio_index > 1u || config->state_machine > 3u)
    return 0;
  if (config->output_mode != MW_PICO_I2S_OUTPUT_STEREO &&
      config->output_mode != MW_PICO_I2S_OUTPUT_MONO_MIXDOWN)
    return 0;
  if (config->dma_channel < -1 ||
      config->dma_channel >= (int)NUM_DMA_CHANNELS)
    return 0;
  return 1;
}

static void mw_pico_reset_runtime_state(void) {
  memset(g_i2s, 0, sizeof(g_i2s));
  memset(g_silence, 0, sizeof(g_silence));
  memset(g_producer_block, 0, sizeof(g_producer_block));
  {
    unsigned int i;
    for (i = 0u; i < (unsigned int)MW_PICO_I2S_RING_COUNT; ++i)
      g_ring_state[i] = MW_PICO_RING_FREE;
  }
  memset(g_timing_samples, 0, sizeof(g_timing_samples));

  g_ring_write = 0u;
  g_ring_read = 0u;
  g_active = -1;
  g_underruns = 0ul;
  g_refills = 0ul;
  g_last_refill_us = 0u;
  g_min_refill_us = 0u;
  g_avg_refill_us = 0u;
  g_max_refill_us = 0u;
  g_ring_low_water = MW_PICO_I2S_RING_TARGET;
  g_ring_high_water = 0u;
  g_timing_sum = 0u;
  g_timing_count = 0u;
  g_timing_pos = 0u;
  g_control_fn = NULL;
  g_control_user = NULL;
  g_control_request_seq = 0u;
  g_control_done_seq = 0u;
  g_control_timeouts = 0ul;
}

/*
 * Whole-platform warm-boot recovery, matching the ordering used by the
 * proven FastDoom Pico target.  This is deliberately NOT hidden inside
 * mw_pico_i2s_stream_start(): it resets Core 1 and all DMA IRQ/channel state,
 * so an application should call it once, very early in platform boot, before
 * stdio or other subsystems claim multicore/DMA resources.
 */
int mw_pico_i2s_platform_recover(void) {
  if (g_core1_started || g_ready)
    return 0;

  g_core1_running = 0;
  __dmb();
  multicore_reset_core1();

  irq_set_enabled(DMA_IRQ_0, false);
  irq_set_enabled(DMA_IRQ_1, false);

  dma_hw->inte0 = 0u;
  dma_hw->inte1 = 0u;
  dma_hw->abort = (uint32_t)~0u;
  while (dma_hw->abort)
    tight_loop_contents();

  dma_hw->ints0 = (uint32_t)~0u;
  dma_hw->ints1 = (uint32_t)~0u;
  __dmb();
  return 1;
}

int mw_pico_i2s_stream_start(const mw_pico_i2s_stream_config_t *config) {
  uint32_t sys_hz;
  uint32_t div256;
  dma_channel_config dc;
  gpio_function_t gpio_func;
  uint dreq_base;

  if (g_ready)
    return 1;
  if (g_failed || !mw_pico_config_valid(config))
    return 0;

  g_cfg = *config;
  g_pio = mw_pico_stream_pio(g_cfg.pio_index);
  gpio_func = mw_pico_stream_gpio_func(g_cfg.pio_index);
  dreq_base = mw_pico_stream_dreq_base(g_cfg.pio_index);

  if (!pio_can_add_program(g_pio, &mw_i2s_program)) {
    g_failed = 1;
    return 0;
  }

  pio_sm_claim(g_pio, (uint)g_cfg.state_machine);
  g_pio_claimed = 1;

  gpio_set_function(g_cfg.pin_data, gpio_func);
  gpio_set_function(g_cfg.pin_bclk, gpio_func);
  gpio_set_function(g_cfg.pin_lrclk, gpio_func);

  g_pio_offset = pio_add_program(g_pio, &mw_i2s_program);
  mw_i2s_program_init(g_pio, (uint)g_cfg.state_machine, g_pio_offset,
                      g_cfg.pin_data, g_cfg.pin_bclk);

  sys_hz = clock_get_hz(clk_sys);
  div256 = (uint32_t)((((uint64_t)sys_hz * 4u) +
                       (uint64_t)g_cfg.sample_rate / 2u) /
                      (uint64_t)g_cfg.sample_rate);
  if ((div256 >> 8u) == 0u || (div256 >> 8u) > 0xffffu) {
    mw_pico_i2s_stream_stop();
    g_failed = 1;
    return 0;
  }

  pio_sm_set_clkdiv_int_frac(g_pio, (uint)g_cfg.state_machine,
                             (uint16_t)(div256 >> 8u),
                             (uint8_t)(div256 & 0xffu));

  if (g_cfg.dma_channel < 0)
    g_dma = dma_claim_unused_channel(false);
  else {
    g_dma = g_cfg.dma_channel;
    dma_channel_claim((uint)g_dma);
  }

  if (g_dma < 0) {
    mw_pico_i2s_stream_stop();
    g_failed = 1;
    return 0;
  }
  g_dma_claimed = 1;

  dc = dma_channel_get_default_config((uint)g_dma);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, dreq_base + (uint)g_cfg.state_machine);
  dma_channel_configure((uint)g_dma, &dc,
                        &g_pio->txf[g_cfg.state_machine], NULL, 0, false);

  mw_pico_reset_runtime_state();

  if (!g_core1_started) {
    memset(g_core1_stack, 0xa5, sizeof(g_core1_stack));
    multicore_launch_core1_with_stack(mw_pico_core1_main,
                                      g_core1_stack,
                                      sizeof(g_core1_stack));
    g_core1_started = 1;
    __dmb();
    while (!g_core1_running)
      tight_loop_contents();
  }

  /* Match the proven FastDoom transport: this firmware owns the
   * DMA IRQ line for its one audio DMA channel, so use the direct
   * exclusive vector rather than the shared-handler trampoline. */
  irq_set_exclusive_handler(DMA_IRQ_0, mw_pico_dma_irq);
  g_irq_installed = 1;
  dma_channel_set_irq0_enabled((uint)g_dma, true);
  irq_set_enabled(DMA_IRQ_0, true);

  pio_sm_set_enabled(g_pio, (uint)g_cfg.state_machine, true);

  g_ready = 1;
  __dmb();
  mw_pico_start_dma_buffer(g_silence);
  return 1;
}

void mw_pico_i2s_stream_stop(void) {
  g_ready = 0;
  __dmb();

  if (g_dma >= 0 && g_dma_claimed) {
    uint32_t mask = 1u << (unsigned int)g_dma;
    dma_channel_set_irq0_enabled((uint)g_dma, false);
    dma_channel_abort((uint)g_dma);
    dma_hw->ints0 = mask;
  }

  if (g_irq_installed) {
    irq_remove_handler(DMA_IRQ_0, mw_pico_dma_irq);
    g_irq_installed = 0;
  }

  if (g_dma >= 0 && g_dma_claimed) {
    dma_channel_unclaim((uint)g_dma);
    g_dma_claimed = 0;
  }
  g_dma = -1;

  if (g_pio_claimed) {
    pio_sm_set_enabled(g_pio, (uint)g_cfg.state_machine, false);
    pio_sm_clear_fifos(g_pio, (uint)g_cfg.state_machine);
    pio_sm_unclaim(g_pio, (uint)g_cfg.state_machine);
    pio_remove_program(g_pio, &mw_i2s_program, g_pio_offset);
    g_pio_claimed = 0;
  }

  g_active = -1;
  {
    unsigned int i;
    for (i = 0u; i < (unsigned int)MW_PICO_I2S_RING_COUNT; ++i)
      g_ring_state[i] = MW_PICO_RING_FREE;
  }
  g_ring_write = 0u;
  g_ring_read = 0u;
  g_failed = 0;
}

int mw_pico_i2s_stream_ready(void) {
  return g_ready ? 1 : 0;
}

void mw_pico_i2s_stream_service(void) {
  if (g_core1_started && get_core_num() != 1u)
    return;
  mw_pico_service_once();
}

int mw_pico_i2s_stream_run_control(mw_pico_i2s_control_fn fn,
                                   void *user,
                                   unsigned int timeout_us) {
  uint64_t deadline;
  uint32_t request;

  if (fn == NULL)
    return 0;

  if (!g_core1_started || !g_core1_running || get_core_num() == 1u) {
    fn(user);
    return 1;
  }

  if (timeout_us == 0u)
    timeout_us = 250000u;
  deadline = time_us_64() + (uint64_t)timeout_us;

  while (g_control_request_seq != g_control_done_seq) {
    if (time_us_64() >= deadline) {
      ++g_control_timeouts;
      return 0;
    }
    tight_loop_contents();
  }

  request = g_control_request_seq + 1u;
  if (request == 0u)
    request = 1u;

  g_control_fn = fn;
  g_control_user = user;
  __dmb();
  g_control_request_seq = request;
  __dmb();

  while (g_control_done_seq != request) {
    if (time_us_64() >= deadline) {
      ++g_control_timeouts;
      return 0;
    }
    tight_loop_contents();
  }

  __dmb();
  g_control_fn = NULL;
  g_control_user = NULL;
  return 1;
}

int mw_pico_i2s_stream_control_pending(void) {
  return g_control_request_seq != g_control_done_seq ? 1 : 0;
}

static unsigned int mw_pico_core1_stack_used_bytes(void) {
  const uint32_t fill = 0xa5a5a5a5u;
  unsigned int words =
      (unsigned int)(sizeof(g_core1_stack) / sizeof(g_core1_stack[0]));
  unsigned int i;

  for (i = 0u; i < words; ++i) {
    if (g_core1_stack[i] != fill)
      break;
  }

  return (words - i) * (unsigned int)sizeof(uint32_t);
}

void mw_pico_i2s_stream_get_diag(mw_pico_i2s_stream_diag_t *diag) {
  if (diag == NULL)
    return;

  __dmb();
  memset(diag, 0, sizeof(*diag));
  diag->underruns = g_underruns;
  diag->refills = g_refills;
  diag->control_timeouts = g_control_timeouts;
  diag->last_refill_us = g_last_refill_us;
  diag->min_refill_us = g_min_refill_us;
  diag->avg_refill_us = g_avg_refill_us;
  diag->max_refill_us = g_max_refill_us;
  diag->ring_ready = mw_pico_ring_ready_count_snapshot();
  diag->ring_count = (unsigned int)MW_PICO_I2S_RING_COUNT;
  diag->ring_target = (unsigned int)MW_PICO_I2S_RING_TARGET;
  diag->ring_low_water = g_ring_low_water;
  diag->ring_high_water = g_ring_high_water;
  diag->pio_index = g_cfg.pio_index;
  diag->state_machine = g_cfg.state_machine;
  diag->dma_channel = g_dma;
  diag->period_frames = (unsigned int)MW_PICO_I2S_PERIOD_FRAMES;
  diag->producer_frames = (unsigned int)MW_PICO_I2S_PRODUCER_FRAMES;
  if (g_cfg.sample_rate != 0u) {
    diag->period_us = (unsigned int)(
        (((uint64_t)MW_PICO_I2S_PERIOD_FRAMES * 1000000ull) +
         (uint64_t)g_cfg.sample_rate / 2ull) /
        (uint64_t)g_cfg.sample_rate);
  }
  diag->core1_stack_bytes = (unsigned int)sizeof(g_core1_stack);
  diag->core1_stack_used_bytes = mw_pico_core1_stack_used_bytes();
  diag->core1_running = g_core1_running ? 1 : 0;
  diag->control_pending = mw_pico_i2s_stream_control_pending();
  diag->ready = g_ready ? 1 : 0;
  __dmb();
}
