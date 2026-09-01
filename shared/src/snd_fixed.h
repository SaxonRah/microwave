#ifndef SND_FIXED_H
#define SND_FIXED_H

#include <stdint.h>

/* 16.16 fixed point by default, used for the per-voice phase accumulator and
   resample step.

   16.16 caps a single voice's addressable clip at 65535 frames, which is about
   three seconds at 22050 Hz. That is a real limit and it is deliberate: it
   keeps the inner mix loop to a 32-bit add on every target including a 386,
   and long-form audio wants streaming rather than a resident clip anyway. Use
   SND_FIXED_WIDE for a 32.32 accumulator on 64-bit-friendly hosts if you need
   to point a voice at a multi-minute buffer.

   For very old 16-bit DOS compilers where int64_t code generation is painful,
   define SND_FIXED_NO_INT64; the 16.16 layout is unchanged and only the
   multiply/divide intermediates narrow.
*/

#ifdef SND_FIXED_WIDE

typedef int64_t snd_fixed_t;
#define SND_FIXED_SHIFT 32
#define SND_FIXED_ONE ((snd_fixed_t)1 << SND_FIXED_SHIFT)
/* Multiply, not shift: x may be negative and shifting a negative value
   left is undefined in C99. Compilers emit the shift anyway. */
#define SND_TO_FIXED(x) ((snd_fixed_t)((int64_t)(x) * ((int64_t)1 << SND_FIXED_SHIFT)))
#define SND_FROM_FIXED(x) ((long)((x) >> SND_FIXED_SHIFT))
#define SND_FIXED_FRAC(x) ((uint32_t)((x) & 0xFFFFFFFFu))
#define SND_FIXED_MUL(a, b) ((snd_fixed_t)(((a) >> 16) * ((b) >> 16)))
#define SND_FIXED_DIV(a, b) ((snd_fixed_t)(((a) << 16) / ((b) >> 16)))

#else

typedef int32_t snd_fixed_t;
#define SND_FIXED_SHIFT 16
#define SND_FIXED_ONE ((snd_fixed_t)(1L << SND_FIXED_SHIFT))
/* Multiply, not shift: x may be negative and shifting a negative value
   left is undefined in C99. Compilers emit the shift anyway. */
#define SND_TO_FIXED(x) ((snd_fixed_t)((int32_t)(x) * (int32_t)SND_FIXED_ONE))
#define SND_FROM_FIXED(x) ((long)((x) >> SND_FIXED_SHIFT))
#define SND_FIXED_FRAC(x) ((uint32_t)((uint32_t)(x) & 0xFFFFu))

#ifdef SND_FIXED_NO_INT64
#define SND_FIXED_MUL(a, b)                                                    \
  ((snd_fixed_t)(((int32_t)(a) >> 8) * ((int32_t)(b) >> 8)))
#define SND_FIXED_DIV(a, b)                                                    \
  ((snd_fixed_t)((((int32_t)(a)) / ((int32_t)(b) >> 8)) << 8))
#else
#define SND_FIXED_MUL(a, b)                                                    \
  ((snd_fixed_t)(((int64_t)(a) * (int64_t)(b)) >> SND_FIXED_SHIFT))
#define SND_FIXED_DIV(a, b)                                                    \
  ((snd_fixed_t)(((int64_t)(a) << SND_FIXED_SHIFT) / (int64_t)(b)))
#endif

#endif

/* Largest clip a 16.16 phase can address without wrapping. */
#if SND_FIXED_SHIFT == 16
#define SND_MAX_CLIP_FRAMES 32767L
#else
#define SND_MAX_CLIP_FRAMES 0x7FFFFFFFL
#endif

#endif /* SND_FIXED_H */
