# Doom / DMX compatibility adapters

MicroWave contains small adapters for the byte formats and mixer conventions
used by the classic Doom DMX sound path. They deliberately stop at the same
boundary as the existing MUS and GENMIDI adapters: the game owns its WAD or
resource system and passes already-located bytes into MicroWave.

## Type-3 SFX lumps

`snd_dmx_sfx` decodes a complete Doom type-3 sound lump into an `snd_clip_t`.
It performs no allocation and the returned clip points directly into the
caller-owned lump memory.

```c
snd_clip_t clip;
int error = snd_dmx_sfx_load(&clip, lump_data, lump_size);
if (error != SND_DMX_SFX_OK) {
    /* snd_dmx_sfx_error_string(error) */
}
```

The adapter validates the type-3 header, little-endian sample rate and declared
length, then applies the classic Doom/DMX layout:

```text
8-byte DMX header
16 leading pad bytes
unsigned 8-bit mono PCM
16 trailing pad bytes
```

The resulting `snd_clip_t` uses `SND_CLIP_PCM8`, one channel, and the rate
stored in the lump. `snd_clip_validate()` is run before success is returned.

The adapter does **not** know how to find a WAD lump, cache it, allocate game
channels, choose priorities, or perform positional attenuation.

## Optional historical gain/pan helpers

`snd_dmx_mix` reproduces the useful compatibility portion of the historical
DMX/Multivoc SFX gain path without making it part of the byte decoder.

`snd_dmx_gain_8_8()` quantizes a 0..255 volume through the old `volume >> 2`
0..63 scale and returns MicroWave 8.8 gain.

`snd_dmx_pan_gains_8_8()` accepts a 0..255 volume and a Doom-style 0..254
separation, derives left/right channel volumes, applies that same quantization,
and can optionally reverse stereo.

This helper is optional. Games that want their own pan law should use
MicroWave's normal voice gain/pan API instead.

## Build

The adapters are independent optional shared targets:

```cmake
set(MW_BUILD_DMX_SFX ON)
set(MW_BUILD_DMX_MIX ON)
add_subdirectory(path/to/microwave/shared)

target_link_libraries(my_game PRIVATE
    microwave::dmx_sfx
    microwave::dmx_mix)
```

Host coverage is included in the normal MicroWave test suite:

```powershell
.\mw.bat test all
```
