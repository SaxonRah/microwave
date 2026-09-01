#!/usr/bin/env python3
"""MicroWave asset packer.

Writes an MWP1 pack, which is byte-compatible with MicroRender's MRP1
container: 12-byte header, 44-byte directory entries, kind-tagged payloads.
Only the magic and the set of entry kinds differ.

That compatibility is the point. MicroRender already reserved entry kind 10 for
8-bit audio, so:

  * MicroWave reads a GAME.MRP produced by mr_pack.py directly. If your project
    already has a renderer pack, you do not need a second asset pipeline and
    you do not ship the samples twice.
  * MicroWave-only kinds are numbered from 32 up, above anything mr_pack.py
    emits, so MicroRender's reader reports them as unknown rather than
    misreading them.

Usage:

    python mw_pack.py --out GAME.MWP --wav pickup=assets/pickup.wav
    python mw_pack.py --out GAME.MWP --wav music=song.wav --format adpcm
    python mw_pack.py --inspect GAME.MRP

Input WAVs must be mono PCM (8- or 16-bit). Everything else is a conversion
problem and belongs in whatever tool you already use for conversion.
"""

import argparse
import os
import struct
import sys
import wave

MAGIC_MW = b"MWP1"
MAGIC_MR = b"MRP1"

HEADER_SIZE = 12
DIR_ENTRY_SIZE = 44
NAME_MAX = 31

# Kinds 1..11 belong to MicroRender and are passed through unchanged.
KIND_AUDIO_U8 = 10
KIND_PROJECT_INFO = 11
# MicroWave extensions, deliberately above MicroRender's range.
KIND_AUDIO_S16 = 32
KIND_AUDIO_ADPCM4 = 33
KIND_SONG = 34
KIND_INSTRUMENT = 35

KIND_NAMES = {
    KIND_AUDIO_U8: "audio_u8",
    KIND_PROJECT_INFO: "project_info",
    KIND_AUDIO_S16: "audio_s16",
    KIND_AUDIO_ADPCM4: "audio_adpcm4",
    KIND_SONG: "song",
    KIND_INSTRUMENT: "instrument",
}

# Must match SND_ADPCM_BLOCK_FRAMES in shared/src/snd_config.h. A mismatch here
# produces a pack the decoder will read at the wrong offsets, so it is asserted
# rather than assumed.
ADPCM_BLOCK_FRAMES = 505
ADPCM_BLOCK_BYTES = 4 + ((ADPCM_BLOCK_FRAMES - 1 + 1) // 2)

IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def read_wav_mono(path):
    """Return (samples_int16, rate). Refuses anything that is not mono PCM."""
    with wave.open(path, "rb") as w:
        channels = w.getnchannels()
        width = w.getsampwidth()
        rate = w.getframerate()
        frames = w.readframes(w.getnframes())

    if channels != 1:
        raise SystemExit(
            "%s is %d-channel; MicroWave clips are mono. Downmix it first."
            % (path, channels)
        )

    if width == 1:
        # WAV 8-bit is unsigned, centred on 128.
        samples = [(b - 128) * 256 for b in frames]
    elif width == 2:
        samples = list(struct.unpack("<%dh" % (len(frames) // 2), frames))
    else:
        raise SystemExit("%s is %d-byte samples; expected 8- or 16-bit." % (path, width))

    return samples, rate


def encode_adpcm(samples):
    """IMA ADPCM in self-restarting blocks.

    Each block begins with its own predictor and step index, which is what
    makes seeking into a compressed clip an O(1) operation instead of a decode
    from the start of the file. It is the same trick MicroRender's RLE sprites
    use with their per-row start index.
    """
    # Pad to a whole number of blocks with silence. The decoder derives its
    # frame count from the byte count, so a partial final block would otherwise
    # decode a tail of frames the source never had -- and because IMA's zero
    # nibble is a small positive step, that tail would drift upward rather than
    # sit still. Padding with zeroes makes the extra frames an audible fade to
    # silence instead, and keeps snd_clip_frames_for_bytes() honest.
    samples = list(samples)
    remainder = len(samples) % ADPCM_BLOCK_FRAMES
    if remainder:
        samples += [0] * (ADPCM_BLOCK_FRAMES - remainder)

    out = bytearray()
    total = len(samples)
    block_count = (total + ADPCM_BLOCK_FRAMES - 1) // ADPCM_BLOCK_FRAMES

    for b in range(block_count):
        first = b * ADPCM_BLOCK_FRAMES
        count = min(ADPCM_BLOCK_FRAMES, total - first)

        pred = samples[first]
        index = 0

        block = bytearray(ADPCM_BLOCK_BYTES)
        block[0] = pred & 0xFF
        block[1] = (pred >> 8) & 0xFF
        block[2] = index
        block[3] = 0

        for i in range(1, count):
            step = IMA_STEP[index]
            diff = samples[first + i] - pred
            nib = 0
            if diff < 0:
                nib = 8
                diff = -diff

            vpdiff = step >> 3
            if diff >= step:
                nib |= 4
                diff -= step
                vpdiff += step
            step >>= 1
            if diff >= step:
                nib |= 2
                diff -= step
                vpdiff += step
            step >>= 1
            if diff >= step:
                nib |= 1
                vpdiff += step

            pred = pred - vpdiff if (nib & 8) else pred + vpdiff
            pred = max(-32768, min(32767, pred))

            index = max(0, min(88, index + IMA_INDEX[nib]))

            slot = 4 + ((i - 1) >> 1)
            if (i - 1) & 1:
                block[slot] |= nib << 4
            else:
                block[slot] |= nib

        out += block

    return bytes(out)


def audio_payload(samples, rate, fmt):
    """Build the payload, including the descriptor MicroRender's reader also
    understands: u16 rate, u16 bits, u32 byte count."""
    if fmt == "u8":
        data = bytes(((max(-32768, min(32767, s)) >> 8) + 128) & 0xFF for s in samples)
        bits = 8
        kind = KIND_AUDIO_U8
    elif fmt == "s16":
        data = struct.pack("<%dh" % len(samples), *(max(-32768, min(32767, s)) for s in samples))
        bits = 16
        kind = KIND_AUDIO_S16
    elif fmt == "adpcm":
        data = encode_adpcm(samples)
        bits = 4
        kind = KIND_AUDIO_ADPCM4
    else:
        raise SystemExit("unknown format %r" % fmt)

    header = struct.pack("<HHI", rate, bits, len(data))
    return kind, header + data


def write_pack(path, entries):
    """entries: list of (name, kind, payload_bytes)."""
    for name, _, _ in entries:
        if len(name.encode("ascii")) > NAME_MAX:
            raise SystemExit("entry name %r exceeds %d bytes" % (name, NAME_MAX))

    data_offset = HEADER_SIZE + DIR_ENTRY_SIZE * len(entries)

    directory = bytearray()
    blob = bytearray()
    for name, kind, payload in entries:
        raw = name.encode("ascii")
        entry = bytearray(DIR_ENTRY_SIZE)
        entry[0] = len(raw)
        entry[1 : 1 + len(raw)] = raw
        struct.pack_into("<H", entry, 32, kind)
        struct.pack_into("<I", entry, 36, len(blob))
        struct.pack_into("<I", entry, 40, len(payload))
        directory += entry
        blob += payload

    with open(path, "wb") as f:
        f.write(MAGIC_MW)
        f.write(struct.pack("<H", len(entries)))
        f.write(b"\0\0")
        f.write(struct.pack("<I", data_offset))
        f.write(directory)
        f.write(blob)

    print("wrote %s: %d entries, %d bytes" % (path, len(entries), data_offset + len(blob)))
    for name, kind, payload in entries:
        print("  %-20s %-14s %8d bytes" % (name, KIND_NAMES.get(kind, "kind%d" % kind), len(payload)))


def inspect(path):
    """Read either container and describe it. Useful for confirming that a
    MicroRender pack really does carry audio MicroWave can use."""
    with open(path, "rb") as f:
        raw = f.read()

    if len(raw) < HEADER_SIZE:
        raise SystemExit("%s is too short to be a pack" % path)

    magic = raw[:4]
    if magic == MAGIC_MW:
        which = "MicroWave MWP1"
    elif magic == MAGIC_MR:
        which = "MicroRender MRP1"
    else:
        raise SystemExit("%s: unrecognised magic %r" % (path, magic))

    count = struct.unpack_from("<H", raw, 4)[0]
    data_offset = struct.unpack_from("<I", raw, 8)[0]
    print("%s: %s, %d entries, data at %d" % (path, which, count, data_offset))

    usable = 0
    for i in range(count):
        base = HEADER_SIZE + i * DIR_ENTRY_SIZE
        namelen = raw[base]
        name = raw[base + 1 : base + 1 + namelen].decode("ascii", "replace")
        kind = struct.unpack_from("<H", raw, base + 32)[0]
        offset, size = struct.unpack_from("<II", raw, base + 36)
        label = KIND_NAMES.get(kind)
        if label is None:
            label = "kind%d (renderer asset)" % kind
        else:
            usable += 1
        extra = ""
        if kind in (KIND_AUDIO_U8, KIND_AUDIO_S16, KIND_AUDIO_ADPCM4) and size >= 8:
            rate, bits, length = struct.unpack_from("<HHI", raw, data_offset + offset)
            extra = "  %d Hz, %d-bit, %d bytes" % (rate, bits, length)
        print("  %-20s %-26s %8d bytes%s" % (name, label, size, extra))

    print("%d of %d entries are usable by MicroWave" % (usable, count))


def main():
    ap = argparse.ArgumentParser(description="MicroWave asset packer")
    ap.add_argument("--out", help="output pack path")
    ap.add_argument(
        "--wav",
        action="append",
        default=[],
        metavar="NAME=PATH",
        help="add a mono WAV as entry NAME; repeatable",
    )
    ap.add_argument(
        "--format",
        default="u8",
        choices=("u8", "s16", "adpcm"),
        help="storage format (default u8, which MicroRender can also read)",
    )
    ap.add_argument("--inspect", metavar="PACK", help="describe an existing pack and exit")
    args = ap.parse_args()

    assert ADPCM_BLOCK_BYTES == 256, (
        "ADPCM block geometry drifted from snd_config.h; a pack written now "
        "would not decode"
    )

    if args.inspect:
        inspect(args.inspect)
        return 0

    if not args.out or not args.wav:
        ap.print_help()
        return 1

    entries = []
    for spec in args.wav:
        if "=" not in spec:
            raise SystemExit("--wav expects NAME=PATH, got %r" % spec)
        name, path = spec.split("=", 1)
        if not os.path.exists(path):
            raise SystemExit("no such file: %s" % path)
        samples, rate = read_wav_mono(path)
        kind, payload = audio_payload(samples, rate, args.format)
        entries.append((name, kind, payload))

    write_pack(args.out, entries)
    return 0


if __name__ == "__main__":
    sys.exit(main())
