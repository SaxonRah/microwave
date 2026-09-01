#ifndef SND_SAMPLE_H
#define SND_SAMPLE_H

#include <stdint.h>

/* Sample-format selection.

   Default and shipping format: signed 16-bit. Pico, DOS, Raylib and normal
   host tests all run the same 22050 Hz S16 logical mixer. DOS converts to its
   physical 8-bit unsigned DMA buffer only in the drain callback, exactly the
   way MicroRender quantizes RGB565 to a VGA palette only while presenting.

   Define SND_SAMPLE_U8=1 only for the optional legacy host compatibility test.
   The mixer core stays source-compatible while snd_sample_t becomes the 8-bit
   unsigned format a Sound Blaster actually wants.
*/

#define SND_SAMPLE_FORMAT_S16 1
#define SND_SAMPLE_FORMAT_U8 2

#if defined(SND_SAMPLE_U8) && (SND_SAMPLE_U8 != 0)

#define SND_SAMPLE_FORMAT SND_SAMPLE_FORMAT_U8

typedef uint8_t snd_sample_t;

#define SND_SAMPLE_SILENCE ((snd_sample_t)0x80u)
#define SND_SAMPLE_MIN ((snd_sample_t)0x00u)
#define SND_SAMPLE_MAX ((snd_sample_t)0xFFu)

/* Signed working range the mixer sums in, before storing back. */
#define SND_MIX_MIN (-128)
#define SND_MIX_MAX (127)

#define SND_SAMPLE_TO_MIX(s) ((int)(s) - 128)
#define SND_MIX_TO_SAMPLE(v) ((snd_sample_t)((v) + 128))

/* Full-scale amplitude, for synth and test code that wants to be format
   independent. */
#define SND_FULL_SCALE 127

#else

#define SND_SAMPLE_FORMAT SND_SAMPLE_FORMAT_S16

typedef int16_t snd_sample_t;

#define SND_SAMPLE_SILENCE ((snd_sample_t)0)
#define SND_SAMPLE_MIN ((snd_sample_t)(-32768))
#define SND_SAMPLE_MAX ((snd_sample_t)32767)

#define SND_MIX_MIN (-32768)
#define SND_MIX_MAX (32767)

#define SND_SAMPLE_TO_MIX(s) ((int)(s))
#define SND_MIX_TO_SAMPLE(v) ((snd_sample_t)(v))

#define SND_FULL_SCALE 32767

#endif

/* Gain is 8.8 unsigned-ish fixed point: SND_GAIN_UNITY is 1.0, and gains above
   unity are allowed and will clip, which is the caller's business. */
#define SND_GAIN_SHIFT 8
#define SND_GAIN_UNITY ((int16_t)(1 << SND_GAIN_SHIFT))
#define SND_GAIN_SILENT ((int16_t)0)

#endif /* SND_SAMPLE_H */
