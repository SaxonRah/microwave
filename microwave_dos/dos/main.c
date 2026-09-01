/* MicroWave 16-bit DOS frontend: Sound Blaster, 8-bit auto-init DMA.
 *
 * Build with Open Watcom via the repository's mw.bat:  .\mw.bat build dos
 *
 * WHAT THIS FILE IS
 *
 * Everything above the drain callback is the same mixer the host tests run.
 * This file contributes exactly three things a desktop build does not need:
 *
 *   1. snd_pack_u8() in the drain, turning the canonical S16 block into the
 *      8-bit unsigned bytes the DSP wants. This is the audio counterpart of
 *      MicroRender quantizing RGB565 to a VGA palette during presentation, and
 *      it happens in the same place for the same reason.
 *   2. A DMA buffer allocated below 1 MB and inside a single 64 KiB physical
 *      page, because the 8237 cannot carry across a page boundary.
 *   3. Auto-init DMA with a half-buffer interrupt, so the mixer fills one half
 *      while the card plays the other. That is snd_render_blocked_pipelined's
 *      contract, expressed in 8237 registers.
 *
 * HONESTY NOTE
 *
 * The mixer in this repository is tested exhaustively on the host. The port
 * I/O below is not, and cannot be: CI has no Sound Blaster. It is written from
 * the documented DSP and 8237 programming sequences and is structured so that
 * the untestable part stays small and legible. Treat it as a starting point you
 * verify in DOSBox-X, not as something that has been proven correct. The parts
 * that CAN be tested -- the mix, the format conversion, the block scheduling --
 * already are, which is the entire argument for splitting the frontend from the
 * library.
 */

#include "mw_music_demo.h"
#include "snd.h"
#include "snd_pack.h"

#include <conio.h>
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MW_RATE 11025 /* a 386 with an SB has better things to do than 22 kHz */
#define MW_BLOCK 512
#define MW_DMA_HALVES 2
#define MW_DMA_BYTES (MW_BLOCK * MW_DMA_HALVES)

/* Blaster environment variable defaults, overridden by parse_blaster(). */
static unsigned g_base = 0x220u;
static unsigned g_irq = 7u;
static unsigned g_dma = 1u;

static snd_sample_t g_block[MW_BLOCK];
static mw_demo_t g_demo;

/* The DMA buffer must be below 1 MB and must not straddle a 64 KiB physical
   page. Over-allocate and pick a page-aligned window inside it. */
static unsigned char *g_dma_raw = NULL;
static unsigned char *g_dma_buf = NULL;
static unsigned long g_dma_phys = 0uL;
static volatile int g_half_playing = 0;
static volatile long g_frames_sent = 0;

static unsigned long far_to_phys(void __far *p) {
  return ((unsigned long)FP_SEG(p) << 4) + (unsigned long)FP_OFF(p);
}

static int dma_alloc(void) {
  unsigned long phys, page_start, page_end;

  /* Twice the buffer plus slack guarantees a page-clean window exists. */
  g_dma_raw = (unsigned char *)malloc(MW_DMA_BYTES * 2 + 16);
  if (!g_dma_raw)
    return 0;

  phys = far_to_phys((void __far *)g_dma_raw);
  page_start = phys & ~0xFFFFuL;
  page_end = page_start + 0x10000uL;

  if (phys + MW_DMA_BYTES <= page_end) {
    g_dma_buf = g_dma_raw;
    g_dma_phys = phys;
  } else {
    unsigned long skip = page_end - phys;
    g_dma_buf = g_dma_raw + skip;
    g_dma_phys = phys + skip;
  }

  if (g_dma_phys >= 0x100000uL) {
    /* Above 1 MB: the 8237 cannot reach it. In a real build this is where you
       would fall back to a DOS memory allocation via int 21h/48h. */
    free(g_dma_raw);
    g_dma_raw = NULL;
    return 0;
  }

  memset(g_dma_buf, 0x80, MW_DMA_BYTES); /* 0x80 is silence for unsigned 8-bit */
  return 1;
}

/* ------------------------------------------------------------------ */
/* DSP                                                                 */
/* ------------------------------------------------------------------ */

#define DSP_RESET (g_base + 0x6u)
#define DSP_READ (g_base + 0xAu)
#define DSP_WRITE (g_base + 0xCu)
#define DSP_READ_STATUS (g_base + 0xEu)

static void dsp_write(unsigned char v) {
  int spin = 0;
  while ((inp(DSP_WRITE) & 0x80u) != 0u) {
    if (++spin > 30000)
      return;
  }
  outp(DSP_WRITE, v);
}

static int dsp_reset(void) {
  int i, spin;

  outp(DSP_RESET, 1);
  for (i = 0; i < 100; ++i)
    (void)inp(DSP_RESET);
  outp(DSP_RESET, 0);

  for (spin = 0; spin < 10000; ++spin) {
    if ((inp(DSP_READ_STATUS) & 0x80u) && inp(DSP_READ) == 0xAAu)
      return 1;
  }
  return 0;
}

static void parse_blaster(void) {
  const char *env = getenv("BLASTER");
  const char *p;

  if (!env) {
    printf("BLASTER not set; assuming A220 I7 D1\n");
    return;
  }
  for (p = env; *p; ++p) {
    switch (*p) {
    case 'A':
    case 'a':
      g_base = (unsigned)strtoul(p + 1, NULL, 16);
      break;
    case 'I':
    case 'i':
      g_irq = (unsigned)strtoul(p + 1, NULL, 10);
      break;
    case 'D':
    case 'd':
      g_dma = (unsigned)strtoul(p + 1, NULL, 10);
      break;
    default:
      break;
    }
  }
  printf("BLASTER: base %03Xh irq %u dma %u\n", g_base, g_irq, g_dma);
}

/* ------------------------------------------------------------------ */
/* 8237 DMA                                                            */
/* ------------------------------------------------------------------ */

static void dma_program(void) {
  static const unsigned char page_port[4] = {0x87u, 0x83u, 0x81u, 0x82u};
  unsigned ch = g_dma & 3u;
  unsigned long len = MW_DMA_BYTES - 1uL;

  outp(0x0Au, (int)(0x04u | ch));            /* mask channel            */
  outp(0x0Cu, 0);                            /* clear flip-flop         */
  outp(0x0Bu, (int)(0x58u | ch));            /* auto-init, read, single */
  outp((int)(ch << 1), (int)(g_dma_phys & 0xFFu));
  outp((int)(ch << 1), (int)((g_dma_phys >> 8) & 0xFFu));
  outp((int)page_port[ch], (int)((g_dma_phys >> 16) & 0xFFu));
  outp((int)((ch << 1) + 1u), (int)(len & 0xFFu));
  outp((int)((ch << 1) + 1u), (int)((len >> 8) & 0xFFu));
  outp(0x0Au, (int)ch);                      /* unmask                  */
}

static void dsp_start_autoinit(void) {
  unsigned tc = 65536uL - (256000000uL / (unsigned long)MW_RATE) / 1000uL;

  dsp_write(0xD1u); /* speaker on */
  dsp_write(0x40u); /* set time constant */
  dsp_write((unsigned char)((256u - (1000000u / MW_RATE)) & 0xFFu));
  (void)tc;

  /* Block size for the auto-init transfer: one half buffer. */
  dsp_write(0x48u);
  dsp_write((unsigned char)((MW_BLOCK - 1) & 0xFF));
  dsp_write((unsigned char)(((MW_BLOCK - 1) >> 8) & 0xFF));

  dsp_write(0x1Cu); /* 8-bit auto-init DMA output */
}

static void dsp_stop(void) {
  dsp_write(0xD0u); /* pause DMA */
  dsp_write(0xDAu); /* exit auto-init */
  dsp_write(0xD3u); /* speaker off */
}

/* ------------------------------------------------------------------ */
/* the drain                                                           */
/* ------------------------------------------------------------------ */

/* Called once per mixed block. Converts to the DSP's format and drops the
   result into whichever half the card is not currently reading.
   snd_pack_u8() is the entire target-specific transformation. */
static void mw_drain_dos(snd_mixer_t *m, long frame, int frames,
                         const snd_sample_t *samples, void *user) {
  unsigned char *dst;

  (void)frame;
  (void)user;
  (void)m;

  dst = g_dma_buf + (g_half_playing ? 0 : MW_BLOCK);

  if (!samples) {
    /* Silent block: memset is cheaper than converting a buffer of zeroes, and
       this is the whole payoff of SND_RENDER_SKIP_SILENT on a 386. */
    memset(dst, 0x80, (size_t)frames);
  } else {
    snd_pack_u8(samples, dst, (long)frames);
  }

  g_frames_sent += frames;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  snd_mixer_t mixer;
  long total, done = 0;
  int seconds = 0;

  if (argc > 1)
    seconds = atoi(argv[1]);

  printf("MicroWave DOS frontend\n");
  parse_blaster();

  if (!dma_alloc()) {
    printf("could not obtain a DMA-safe buffer below 1 MB\n");
    return 1;
  }
  printf("DMA buffer at %05lXh, %d bytes\n", g_dma_phys, MW_DMA_BYTES);

  if (!dsp_reset()) {
    printf("no Sound Blaster DSP at %03Xh\n", g_base);
    free(g_dma_raw);
    return 1;
  }

  snd_init(&mixer, MW_RATE, 1, g_block, MW_BLOCK, mw_drain_dos, NULL);
  snd_set_master_gain(&mixer, SND_GAIN_UNITY);
  mw_demo_init(&g_demo, &mixer, 0, 1);

  total = seconds > 0 ? (long)seconds * MW_RATE
                      : mw_demo_length_frames(&g_demo, &mixer);

  /* Prime both halves before starting the card, so it never plays a buffer
     the mixer has not written. */
  snd_render_one_block(&mixer, 0, MW_BLOCK, mw_demo_mix, &g_demo, 0u);
  g_half_playing = 1;
  snd_render_one_block(&mixer, MW_BLOCK, MW_BLOCK, mw_demo_mix, &g_demo, 0u);
  done = MW_BLOCK * 2;

  dma_program();
  dsp_start_autoinit();

  printf("playing; press any key to stop\n");

  /* Polled rather than interrupt-driven. An IRQ handler is the right answer
     for a game; polling keeps this file readable and is adequate for a demo,
     because the mixer runs hundreds of times faster than realtime. */
  while (done < total && !kbhit()) {
    /* Follow the 8237 current-address register to see which half is playing.
       This is the poll an IRQ handler would replace. */
    unsigned ch = g_dma & 3u;
    unsigned lo, hi;
    unsigned long remaining;
    int half;

    outp(0x0Cu, 0);
    lo = (unsigned)inp((int)((ch << 1) + 1u));
    hi = (unsigned)inp((int)((ch << 1) + 1u));
    remaining = ((unsigned long)hi << 8) | lo;
    half = (remaining >= (unsigned long)MW_BLOCK) ? 0 : 1;

    if (half != g_half_playing) {
      g_half_playing = half;
      snd_render_one_block(&mixer, done, MW_BLOCK, mw_demo_mix, &g_demo,
                           SND_RENDER_SKIP_SILENT);
      done += MW_BLOCK;
    }
  }

  dsp_stop();
  if (kbhit())
    (void)getch();
  free(g_dma_raw);

  printf("done: %ld frames\n", done);
  return 0;
}
