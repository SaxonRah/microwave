#ifndef MW_PICO_I2S_STREAM_H
#define MW_PICO_I2S_STREAM_H

#include "snd_sample.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocation-free RP2350 I2S streaming transport.
 *
 * The transport owns one PIO state machine, one DMA channel, a short packed
 * I2S ring, and Core 1. The producer callback runs only on Core 1. DMA IRQ work
 * is intentionally limited to retiring the previous period and starting an
 * already-rendered period. On underrun the transport sends one silence period
 * rather than replaying stale audio.
 *
 * Compile-time capacities can be overridden per firmware target. */
#ifndef MW_PICO_I2S_PERIOD_FRAMES
#define MW_PICO_I2S_PERIOD_FRAMES 512
#endif
#ifndef MW_PICO_I2S_RING_COUNT
#define MW_PICO_I2S_RING_COUNT 4
#endif
#ifndef MW_PICO_I2S_RING_TARGET
#define MW_PICO_I2S_RING_TARGET 2
#endif
#ifndef MW_PICO_I2S_PRODUCER_FRAMES
#define MW_PICO_I2S_PRODUCER_FRAMES 256
#endif
#ifndef MW_PICO_I2S_CORE1_STACK_BYTES
#define MW_PICO_I2S_CORE1_STACK_BYTES 8192
#endif
#ifndef MW_PICO_I2S_TIMING_WINDOW
#define MW_PICO_I2S_TIMING_WINDOW 32
#endif

typedef enum mw_pico_i2s_output_mode {
  MW_PICO_I2S_OUTPUT_STEREO = 0,
  MW_PICO_I2S_OUTPUT_MONO_MIXDOWN = 1
} mw_pico_i2s_output_mode_t;

/* Fill up to `frames` interleaved MicroWave samples for `channels` (1 or 2).
 * Return the number of frames produced. The transport fills any remainder with
 * silence, so a short producer result advances real audio time rather than
 * replaying old data. */
typedef unsigned int (*mw_pico_i2s_producer_fn)(snd_sample_t *dst,
                                                 unsigned int frames,
                                                 unsigned int channels,
                                                 void *user);

typedef void (*mw_pico_i2s_control_fn)(void *user);

typedef struct mw_pico_i2s_stream_config {
  unsigned int sample_rate;
  unsigned int channels;
  unsigned int pin_bclk;
  unsigned int pin_lrclk;
  unsigned int pin_data;
  unsigned int pio_index;
  unsigned int state_machine;
  int dma_channel; /* -1 claims any unused channel; >= 0 claims that channel */
  mw_pico_i2s_output_mode_t output_mode;
  mw_pico_i2s_producer_fn producer;
  void *producer_user;
} mw_pico_i2s_stream_config_t;

typedef struct mw_pico_i2s_stream_diag {
  unsigned long underruns;
  unsigned long refills;
  unsigned long control_timeouts;
  uint32_t last_refill_us;
  uint32_t min_refill_us;
  uint32_t avg_refill_us;
  uint32_t max_refill_us;
  unsigned int ring_ready;
  unsigned int ring_count;
  unsigned int ring_target;
  unsigned int ring_low_water;
  unsigned int ring_high_water;
  unsigned int pio_index;
  unsigned int state_machine;
  int dma_channel;
  unsigned int period_frames;
  unsigned int producer_frames;
  unsigned int period_us;
  unsigned int core1_stack_bytes;
  unsigned int core1_stack_used_bytes;
  int core1_running;
  int control_pending;
  int ready;
} mw_pico_i2s_stream_diag_t;

/* Whole-platform recovery matching the proven FastDoom Pico boot order.
 * Call once near the start of platform boot, before stdio and before other
 * code claims Core 1 or DMA.  This resets Core 1 and clears global DMA IRQ/
 * channel state, so reusable libraries should not call it implicitly. */
int mw_pico_i2s_platform_recover(void);

int mw_pico_i2s_stream_start(const mw_pico_i2s_stream_config_t *config);
void mw_pico_i2s_stream_stop(void);
int mw_pico_i2s_stream_ready(void);

/* Compatibility/service hook. Once Core 1 is running, calls from Core 0 are
 * intentionally cheap no-ops. */
void mw_pico_i2s_stream_service(void);

/* Execute `fn` on the audio-owning core at a complete producer-block boundary.
 * Only one synchronous transaction may be outstanding. A timeout does not
 * revoke a callback already published to Core 1, so `user` must remain valid
 * until that late callback can complete. A timeout of zero uses 250 ms. */
int mw_pico_i2s_stream_run_control(mw_pico_i2s_control_fn fn,
                                   void *user,
                                   unsigned int timeout_us);

/* Non-zero while a published control callback has not completed. Callers that
 * reuse persistent payload storage after a timeout can use this before
 * modifying that storage for another request. */
int mw_pico_i2s_stream_control_pending(void);

void mw_pico_i2s_stream_get_diag(mw_pico_i2s_stream_diag_t *diag);

#ifdef __cplusplus
}
#endif

#endif /* MW_PICO_I2S_STREAM_H */
