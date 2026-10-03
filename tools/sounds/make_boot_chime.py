#!/usr/bin/env python3
"""Generate the 8-bit style boot chime for Herdr Panel.

Output: main/assets/sounds/boot.pcm

Format matches the existing assets (done.pcm / request.pcm) so panel_audio.cpp
can play it through the same I2S path:
  16000 Hz, 16-bit signed, stereo (left == right), interleaved little-endian.

The tone is a square wave with a 25% duty cycle, which is what gives 8-bit
game consoles their characteristic timbre. Each note gets a short exponential
release so the DAC does not click, and the whole clip is wrapped in silence
so the ES8311 never sees a DC step at note boundaries.

Usage:
    python tools/sounds/make_boot_chime.py [--tempo 120] [--out PATH]

Notes are given as (name, beats). Name is a scientific pitch name such as
"C5"; sharps use '#' (for example "C#5"). 'R' is a rest.
"""

from __future__ import annotations

import argparse
import math
import os
import struct
import sys

SAMPLE_RATE = 16000
BITS = 16
CHANNELS = 2  # stereo, duplicated; matches the codec's channel_mask = 0x03

# Semitone offsets from C for each letter, and the octave multiplier.
_NOTE_BASE = {"C": -9, "D": -7, "E": -5, "F": -4, "G": -2, "A": 0, "B": 2}

DEFAULT_MELODY = [
    # Rising fourths: reads as a short "hello". Kept under ~1.6 s so the
    # splash is not over before the chime finishes.
    ("C5", 0.5), ("E5", 0.5), ("G5", 0.5), ("C6", 1.0),
]

DUTY = 0.25          # square-wave duty cycle for the 8-bit timbre
PEAK = 0.72          # headroom below full scale, avoids clipping after filtering
LEAD_IN_S = 0.03     # silence before the first note
TAIL_OUT_S = 0.06    # silence after the last note
RELEASE = 0.008      # exponential decay per note, prevents clicks
TRIANGLE_HARMONIC = 0.22  # a touch of 3rd harmonic, softens the square edge


def note_to_hz(name: str) -> float:
    """'C5' -> 523.25 Hz. 'C#5' / 'Db5' are both accepted."""
    letter = name[0].upper()
    if letter not in _NOTE_BASE:
        raise ValueError(f"bad note name: {name!r}")
    semitones = _NOTE_BASE[letter]
    idx = 1
    if len(name) > 1 and name[1] in "#b":
        semitones += 1 if name[1] == "#" else -1
        idx = 2
    octave = int(name[idx:])
    # MIDI note: C4 == 60 -> 261.63 Hz
    midi = (octave + 1) * 12 + semitones
    return 440.0 * (2.0 ** ((midi - 69) / 12.0))


def render_note(freq: float, seconds: float, sr: int = SAMPLE_RATE) -> list[float]:
    """Render one note as float samples in [-1, 1]."""
    n = int(round(seconds * sr))
    if n <= 0:
        return []
    out = [0.0] * n
    if freq <= 0.0:
        return out

    release_n = max(1, int(RELEASE * sr))
    two_pi_f = 2.0 * math.pi * freq / sr
    for i in range(n):
        phase = (i * freq / sr) % 1.0
        if phase < DUTY:
            v = 1.0
        elif phase < 0.5:
            v = -1.0
        else:
            v = 0.0
        # Soften the leading edge a little; a raw square sounds harsh.
        v = math.tanh(v * 1.5)
        v += TRIANGLE_HARMONIC * math.sin(two_pi_f * 3.0 * i)
        # Exponential release over the tail of the note.
        if i >= n - release_n:
            k = (i - (n - release_n)) / release_n
            v *= math.exp(-4.0 * k)
        out[i] = v
    return out


def build_samples(melody, bpm: float, sr: int = SAMPLE_RATE) -> list[float]:
    beat_s = 60.0 / bpm
    samples: list[float] = [0.0] * int(LEAD_IN_S * sr)
    for name, beats in melody:
        freq = 0.0 if name.upper() == "R" else note_to_hz(name)
        samples.extend(render_note(freq, beats * beat_s, sr))
    samples.extend([0.0] * int(TAIL_OUT_S * sr))
    return samples


def write_pcm(samples: list[float], path: str) -> int:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    frames = bytearray()
    for v in samples:
        clipped = max(-1.0, min(1.0, v * PEAK))
        val = int(round(clipped * 32767.0))
        # duplicate to both channels: (L, R) interleaved, little-endian
        frames += struct.pack("<hh", val, val)
    with open(path, "wb") as f:
        f.write(frames)
    return len(frames)


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.normpath(
        os.path.join(here, "..", "..", "main", "assets", "sounds", "boot.pcm")
    )

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tempo", type=float, default=120.0,
                    help="beats per minute (default: 120)")
    ap.add_argument("--out", default=default_out, help="output .pcm path")
    args = ap.parse_args()

    samples = build_samples(DEFAULT_MELODY, args.tempo)
    written = write_pcm(samples, args.out)

    seconds = len(samples) / SAMPLE_RATE
    print(f"melody: {' '.join(n for n, _ in DEFAULT_MELODY)}")
    print(f"tempo : {args.tempo:g} BPM   duration: {seconds:.2f} s")
    print(f"format: {SAMPLE_RATE} Hz, {BITS}-bit, {CHANNELS}ch")
    print(f"wrote : {args.out} ({written} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
