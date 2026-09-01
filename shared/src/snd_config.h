#ifndef SND_CONFIG_H
#define SND_CONFIG_H

/* MicroWave C99 configuration.

   This file is the audio counterpart of MicroRender's gfx_config.h and makes
   the same promises about the same three targets.

   DOS 16-bit large-model note:
   - Do not put a whole song's worth of mixed audio in one object.
   - The default 256-frame mono S16 block is 512 bytes and is nowhere near a
     64 KiB segment. A stereo block at 1024 frames is still only 4 KiB.
   - What *does* approach a segment on DOS is PCM sample data, not the mix
     block. Individual clip pixel pools must stay under 64 KiB exactly as
     MicroRender's RLE pixel pools must; see snd_clip_validate().
   - In Open Watcom large model, ordinary data pointers are far by default.
   - If your compiler needs explicit far annotations, define SND_PTR before
     including the mixer headers, e.g. #define SND_PTR __far.
*/

#ifndef SND_INLINE
#if defined(__GNUC__)
#define SND_INLINE static inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define SND_INLINE static __inline
#else
#define SND_INLINE static
#endif
#endif

#ifndef SND_PTR
#define SND_PTR
#endif

#ifndef SND_ROM
#define SND_ROM const
#endif

#ifndef SND_RESTRICT
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
#define SND_RESTRICT restrict
#else
#define SND_RESTRICT
#endif
#endif

/* 16-bit int detection.

   Open Watcom's DOS targets use a 16-bit int even in the large memory model,
   where only pointers become far. Frame-addressing math in the mixer is long
   math for exactly this reason, but clip sizes are still capped so that a
   caller cannot hand us a surface whose frame count cannot be expressed.
   Override by defining SND_INT_IS_16BIT yourself. */
#ifndef SND_INT_IS_16BIT
#include <limits.h>
#if INT_MAX < 2147483647L
#define SND_INT_IS_16BIT 1
#else
#define SND_INT_IS_16BIT 0
#endif
#endif

/* Wide mix accumulator.

   The shipping mix format is S16, and voices are summed with a saturating
   add. That is deliberate: it needs no second buffer, which matters on DOS and
   RP2350, and it is what period trackers actually did.

   Saturating adds are order-dependent once a mix genuinely clips. On a target
   with cycles to spare, SND_WIDE_ACCUM=1 sums into a 32-bit scratch and clamps
   once at the end of the block, which is order-independent and audibly cleaner
   when many voices overlap. It costs one extra int32 per frame of block, so it
   is enabled only for flat-pointer 32-bit targets, matching the reasoning
   behind MicroRender's GFX_FAST_WORD_COPY.

   Override by defining SND_WIDE_ACCUM yourself. The mixer produces the same
   output either way until the sum actually exceeds full scale. */
#ifndef SND_WIDE_ACCUM
#if !SND_INT_IS_16BIT && !defined(__WATCOMC__)
#define SND_WIDE_ACCUM 1
#else
#define SND_WIDE_ACCUM 0
#endif
#endif

/* Frames per mix block. 256 frames at 22050 Hz is 11.6 ms, which is a
   reasonable latency floor for a DMA double buffer and still large enough that
   per-block overhead disappears. */
#ifndef SND_DEFAULT_BLOCK_FRAMES
#define SND_DEFAULT_BLOCK_FRAMES 256
#endif

/* Logical mix rate. Every target mixes at this rate and resamples on the way
   out if its hardware disagrees, exactly as every target rasterizes at
   320x240 and rescales on the way out. */
#ifndef SND_DEFAULT_RATE
#define SND_DEFAULT_RATE 22050
#endif

/* Linear interpolation on resample. Nearest-neighbour is the default because
   it is what a 386 and an RP2350 can afford per voice; linear costs roughly
   one extra multiply and add per frame per voice. Compare GFX_ENABLE_TRIANGLES:
   correctness does not depend on it, output quality does. */
#ifndef SND_ENABLE_LERP
#define SND_ENABLE_LERP 1
#endif

/* IMA ADPCM decode support. Compile out on a target that only ships raw PCM
   and wants the code space back. */
#ifndef SND_ENABLE_ADPCM
#define SND_ENABLE_ADPCM 1
#endif

/* Frames per self-contained IMA ADPCM block. Each block restarts the decoder
   from an explicit predictor, which is what makes seeking into a compressed
   clip possible at all; it is the direct analogue of MicroRender's
   row-start-index RLE. */
#ifndef SND_ADPCM_BLOCK_FRAMES
#define SND_ADPCM_BLOCK_FRAMES 505
#endif

/* Voices a snd_bank_t can hold. */
#ifndef SND_BANK_MAX_VOICES
#define SND_BANK_MAX_VOICES 16
#endif

#endif /* SND_CONFIG_H */
