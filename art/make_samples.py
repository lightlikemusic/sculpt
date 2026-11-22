#!/usr/bin/env python3
"""Synthesise Sculpt's four built-in loops into ../Content/Samples/.

Run:  python3 plugins/Sculpt/art/make_samples.py
Pure Python (no numpy), fixed seeds, so the output is reproducible and entirely original.
Each loop is two bars of 4/4 at 120 BPM (4.000 s), mono, 44.1 kHz, 16-bit, and loops cleanly.

  1 Kalimba.wav  plucked tines playing a pentatonic figure
  2 Tape.wav     a dusty drum loop with wow, saturation and hiss
  3 Choir.wav    a slow vowel pad moving through two chords
  4 Glass.wav    inharmonic bell tones in a falling arpeggio

You can replace any of these files with your own (mono or stereo WAV, any rate); Sculpt
loads whatever is in Content/Samples/ under these four names when the plugin starts.
"""
import math
import os
import random
import struct

SR = 44100
BPM = 120.0
BEAT = 60.0 / BPM
N = int(round(SR * BEAT * 8))          # two bars
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Content", "Samples")
TAU = 2.0 * math.pi


def write_wav(path, data, peak=0.89):
    m = max(1e-9, max(abs(x) for x in data))
    g = peak / m
    with open(path, "wb") as f:
        frames = b"".join(struct.pack("<h", int(max(-32767, min(32767, round(x * g * 32767))))) for x in data)
        f.write(b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SR, SR * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(frames)) + frames)


def add_wrapped(buf, start, samples):
    """Mix samples into buf starting at `start`, wrapping past the end so tails loop."""
    n = len(buf)
    for i, v in enumerate(samples):
        buf[(start + i) % n] += v


def midi_hz(n):
    return 440.0 * 2.0 ** ((n - 69) / 12.0)


# ---------------------------------------------------------------- kalimba
def kalimba():
    buf = [0.0] * N
    rnd = random.Random(11)
    # C minor pentatonic figure, eighth notes with a few rests and doubles
    notes = [72, 75, 79, 82, 79, 75, 77, None, 72, 70, 75, 79, 84, 82, 79, None]
    for step, n in enumerate(notes):
        if n is None:
            continue
        f = midi_hz(n)
        start = int(step * BEAT / 2 * SR)
        length = int(1.6 * SR)
        amp = 0.6 + 0.4 * rnd.random()
        out = []
        for i in range(length):
            t = i / SR
            # tine: fundamental + slightly sharp 3rd-ish partial that dies fast + a click
            v = math.sin(TAU * f * t) * math.exp(-t * 3.2)
            v += 0.35 * math.sin(TAU * f * 5.93 * t) * math.exp(-t * 22.0)
            v += 0.12 * math.sin(TAU * f * 2.0 * t) * math.exp(-t * 6.0)
            if i < 60:
                v += (rnd.random() * 2 - 1) * 0.3 * (1 - i / 60)
            out.append(v * amp)
        add_wrapped(buf, start, out)
    return buf


# ---------------------------------------------------------------- tape drums
def tape():
    buf = [0.0] * N
    rnd = random.Random(23)
    six = BEAT / 4

    def kick(start):
        out, ph = [], 0.0
        for i in range(int(0.45 * SR)):
            t = i / SR
            f = 48 + 110 * math.exp(-t * 28)
            ph += TAU * f / SR
            out.append(math.sin(ph) * math.exp(-t * 7.5))
        add_wrapped(buf, start, out)

    def snare(start, amp=1.0):
        out, lp = [], 0.0
        for i in range(int(0.3 * SR)):
            t = i / SR
            nz = rnd.random() * 2 - 1
            lp += 0.45 * (nz - lp)
            v = (nz - lp) * math.exp(-t * 14) * 0.7 + math.sin(TAU * 190 * t) * math.exp(-t * 20) * 0.5
            out.append(v * amp)
        add_wrapped(buf, start, out)

    def hat(start, amp):
        out, prev = [], 0.0
        for i in range(int(0.07 * SR)):
            t = i / SR
            nz = rnd.random() * 2 - 1
            hp = nz - prev
            prev = nz
            out.append(hp * math.exp(-t * 70) * amp * 0.35)
        add_wrapped(buf, start, out)

    kicks = [0, 6, 10, 16, 22, 25]
    snares = [4, 12, 20, 28]
    for s in kicks:
        kick(int(s * six * SR))
    for s in snares:
        snare(int(s * six * SR))
    snare(int(15 * six * SR), 0.35)
    snare(int(31 * six * SR), 0.3)
    for s in range(32):
        hat(int(s * six * SR), 0.9 if s % 2 == 0 else 0.45)

    # tape: gentle wow (resample with a slow wobble), saturation, hiss
    out = [0.0] * N
    for i in range(N):
        w = 14.0 * math.sin(TAU * i / N * 2) + 4.0 * math.sin(TAU * i / N * 7)  # whole cycles: loops cleanly
        p = (i + w) % N
        a = int(p)
        fr = p - a
        x = buf[a] * (1 - fr) + buf[(a + 1) % N] * fr
        x = math.tanh(1.8 * x) / math.tanh(1.8)
        out[i] = x + (rnd.random() * 2 - 1) * 0.012
    # one-pole lowpass for a dull tape top (run twice so the state wraps)
    lp = 0.0
    for _ in range(2):
        for i in range(N):
            lp += 0.42 * (out[i] - lp)
            out[i] = lp if _ else out[i]
    return out


# ---------------------------------------------------------------- choir
def choir():
    buf = [0.0] * N
    # formant centre frequencies and widths for a soft "ah" moving to "oh"
    vowels = [((730, 90), (1090, 110), (2440, 160)), ((570, 80), (840, 100), (2410, 160))]
    chords = [[48, 55, 63, 67, 72], [44, 51, 60, 63, 68]]   # Cm(add) -> Ab
    rnd = random.Random(5)
    half = N // 2
    for ci, chord in enumerate(chords):
        for v_i, n in enumerate(chord):
            f0 = midi_hz(n)
            vib_rate = 4.6 + 0.4 * rnd.random()
            vib_ph = rnd.random() * TAU
            nh = int(5000 / f0)
            # harmonic weights from this chord's vowel formants
            weights = []
            for h in range(1, nh + 1):
                fh = f0 * h
                wa = sum(math.exp(-((fh - c) / bw) ** 2 * 0.5) for c, bw in vowels[ci]) + 0.02
                weights.append(wa / h ** 0.3)
            norm = 1.0 / max(1e-9, sum(weights))
            ph = [rnd.random() * TAU for _ in weights]
            seg = []
            fade = int(0.25 * SR)
            L = half + 2 * fade
            for i in range(L):
                t = i / SR
                vib = 1.0 + 0.006 * math.sin(TAU * vib_rate * t + vib_ph)
                s = 0.0
                for h, w in enumerate(weights):
                    s += w * math.sin(ph[h] + TAU * f0 * (h + 1) * t * vib)
                env = min(1.0, i / fade, (L - i) / fade)
                seg.append(s * norm * env * 0.35)
            add_wrapped(buf, ci * half - fade, seg)
    # breath
    rnd2 = random.Random(9)
    lp = 0.0
    for i in range(N):
        nz = rnd2.random() * 2 - 1
        lp += 0.08 * (nz - lp)
        buf[i] += lp * 0.04
    return buf


# ---------------------------------------------------------------- glass
def glass():
    buf = [0.0] * N
    rnd = random.Random(31)
    ratios = [1.0, 2.76, 5.40, 8.93]
    decays = [1.4, 2.8, 5.5, 9.0]
    seq = [91, 86, 82, 79, 86, 82, 79, 74, 89, 84, 81, 77, 84, 81, 77, 72]
    for step, n in enumerate(seq):
        f = midi_hz(n - 12)
        start = int(step * BEAT / 2 * SR)
        amp = 0.5 + 0.5 * rnd.random()
        out = []
        for i in range(int(2.5 * SR)):
            t = i / SR
            v = 0.0
            for r, d, g in zip(ratios, decays, (1.0, 0.5, 0.3, 0.18)):
                v += g * math.sin(TAU * f * r * t) * math.exp(-t * d)
            out.append(v * amp * min(1.0, i / 40.0))
        add_wrapped(buf, start, out)
    return buf


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, fn in (("1 Kalimba", kalimba), ("2 Tape", tape), ("3 Choir", choir), ("4 Glass", glass)):
        data = fn()
        path = os.path.join(OUT, name + ".wav")
        write_wav(path, data)
        print("wrote", os.path.relpath(path), len(data), "frames")


if __name__ == "__main__":
    main()
