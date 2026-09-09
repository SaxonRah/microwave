# Apply MicroWave MIDI FM foundation

Prepared against `SaxonRah/microwave` main commit:

`3ed5ea335d06894c5367e439d2ac356e42eb9a91`

From `C:\microwave`:

```powershell
git status --short

git apply --check .\microwave-midi-fm-foundation.patch
git apply .\microwave-midi-fm-foundation.patch

.\mw.bat test all
```

After applying, the host matrix should contain eight CTest entries per variant:

- `unit`
- `midi`
- `mus`
- `midi_fm`
- four deterministic fuzz seeds

The new standalone shared-library option is:

```text
-DMW_BUILD_MIDI_FM=ON
```

which exposes `microwave::midi_fm` and brings in `microwave::snd` and
`microwave::midi` as dependencies.

## What this milestone is

`snd_midi_fm` is a deterministic two-operator software FM backend for
`snd_midi`. It supports one- or two-layer caller-owned instruments, bounded
absolute-frame MIDI event queuing, 9/18 voice budgets, sustain, pitch bend,
volume/expression/pan, deterministic voice stealing, and note-indexed
percussion.

It is deliberately **OPL-shaped rather than a register-accurate OPL emulator**.
The next Doom-specific-but-still-generic music milestone should be a separate
`snd_genmidi` bank adapter that translates caller-owned DMX `GENMIDI` bytes
into `snd_midi_fm_bank_t` instruments.
