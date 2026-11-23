#!/usr/bin/env python3
"""Synthesise Sculpt's built-in loops into ../Content/Samples/.

Run:  python3 plugins/Sculpt/art/make_samples.py
Pure Python (no numpy), fixed seeds, so the output is reproducible and entirely original.
Each loop is two bars of 4/4 at 120 BPM (4.000 s), mono, 44.1 kHz, 16-bit, and loops cleanly.

  1 Kalimba.wav  plucked tines playing a pentatonic figure
  2 Tape.wav     a dusty drum loop with wow, saturation and hiss
  3 Haze.wav     a warm detuned-saw pad moving through two chords
  4 Glass.wav    inharmonic bell tones in a falling arpeggio
  5 Drone.wav    a low beating D drone over soft wind
  6 Analog Pad.wav  square and triangle voices in a gently pulsing chord
  7 Synth Arp.wav    a plucky pulse arpeggio in sixteenths
  8 Air Pad.wav  high airy clusters with breath
  9 Dark Drone.wav   low saws under a slow resonant sweep
  10 Club Kick.wav   a tight four-on-the-floor kick

You can replace any of these files with your own (mono or stereo WAV, any rate); Sculpt
loads the first four as the tracks' starting sounds; all of them show in the browser's Sculpt folder.
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


# ---------------------------------------------------------------- synth helpers
# Every pitch and wobble is rounded to a whole number of cycles in the 4 s loop, and every filter
# runs twice round the loop, so each loop joins with no seam.
def loop_hz(f):
    return max(0.25, round(f * 4.0) / 4.0)


_TABLES = {}


def wave_table(kind, f):
    """One band-limited cycle (2048 points) of a saw, square or triangle at pitch f."""
    top = max(1, int(9000 / f))
    key = (kind, top)
    if key not in _TABLES:
        T = 2048
        tab = [0.0] * T
        for h in range(1, top + 1):
            if kind == "saw":
                g = 1.0 / h
            elif kind == "square":
                g = 1.0 / h if h % 2 else 0.0
            else:   # triangle
                g = (1.0 / (h * h)) * (1 if (h // 2) % 2 == 0 else -1) if h % 2 else 0.0
            if g == 0.0:
                continue
            for i in range(T):
                tab[i] += g * math.sin(TAU * h * i / T)
        m = max(abs(v) for v in tab)
        _TABLES[key] = [v / m for v in tab]
    return _TABLES[key]


def osc(buf, kind, f, amp, phase=0.0, env=None):
    """Add an oscillator (looping cleanly) into buf; env(i) scales it if given."""
    f = loop_hz(f)
    tab = wave_table(kind, f)
    T = len(tab)
    inc = f * T / SR
    p = phase * T
    for i in range(N):
        g = amp if env is None else amp * env(i)
        if g:
            buf[i] += tab[int(p) & (T - 1)] * g
        p += inc


def lowpass(buf, cutoff_of_i, q=0.7):
    """A resonant low-pass whose cutoff can move (cutoff_of_i(i) in Hz); run twice so it loops."""
    lp = bp = 0.0
    out = [0.0] * N
    for _ in range(2):
        for i in range(N):
            g = math.tan(math.pi * min(cutoff_of_i(i), 18000) / SR)
            k = 1.0 / q
            hp = (buf[i] - (k + g) * bp - lp) / (1 + g * (k + g))
            bp += g * hp
            lp += g * bp
            bp += g * hp
            lp += g * bp
            out[i] = lp
    return out


def lfo(cycles, i, phase=0.0):
    return math.sin(TAU * (cycles * i / N + phase))


def chord_env(k, count, fade=0.3):
    """Gain for chord k of `count`, each a stretch of the loop, crossfading into the next (and round)."""
    seg = N / count
    fl = fade * SR

    def env(i):
        x = (i - k * seg) % N
        if x < seg - fl / 2:
            return 1.0 if x >= fl / 2 or k == 0 and False else min(1.0, 0.5 + x / fl)
        if x < seg + fl / 2:
            return max(0.0, 0.5 - (x - seg) / fl)
        if x > N - fl / 2:
            return min(1.0, 0.5 + (x - N) / fl)
        return 0.0
    return env


def noise_bed(seed, cutoff, amp):
    rnd = random.Random(seed)
    nz = [rnd.random() * 2 - 1 for _ in range(N)]
    return [v * amp for v in lowpass(nz, lambda i: cutoff)]


# ---------------------------------------------------------------- haze (pad)
def haze():
    """Detuned saws in two warm chords (Cm9 to Abmaj7), under a slowly breathing filter."""
    buf = [0.0] * N
    chords = [[48, 55, 58, 62, 67], [44, 51, 55, 60, 63]]
    rnd = random.Random(5)
    for k, ch in enumerate(chords):
        env = chord_env(k, 2)
        for n in ch:
            for det in (-0.12, 0.0, 0.11):
                osc(buf, "saw", midi_hz(n + det), 0.07, rnd.random(), env)
    return lowpass(buf, lambda i: 900 + 600 * (0.5 + 0.5 * lfo(1, i)), 0.9)


# ---------------------------------------------------------------- drone
def drone():
    """A low D with its fifth and octave, slowly beating, over a soft wind."""
    buf = [0.0] * N
    for n, a, det in ((38, 0.5, 0.0), (38, 0.35, 0.06), (45, 0.3, -0.04), (50, 0.22, 0.05), (57, 0.1, 0.0)):
        osc(buf, "saw", midi_hz(n + det), a * 0.5)
        osc(buf, "triangle", midi_hz(n - det), a * 0.5)
    out = lowpass(buf, lambda i: 320 + 380 * (0.5 + 0.5 * lfo(1, i, 0.25)), 1.6)
    wind = noise_bed(41, 700, 0.5)
    return [o + w * (0.6 + 0.4 * lfo(2, i)) for i, (o, w) in enumerate(zip(out, wind))]


# ---------------------------------------------------------------- analog pad
def analog_pad():
    """Square and triangle voices in an Eb major 9 chord with a gentle tremolo."""
    buf = [0.0] * N
    rnd = random.Random(17)
    for n in (51, 58, 62, 65, 70):
        osc(buf, "square", midi_hz(n + 0.07), 0.05, rnd.random())
        osc(buf, "triangle", midi_hz(n - 0.07), 0.09, rnd.random())
    out = lowpass(buf, lambda i: 1500 + 500 * lfo(1, i), 0.8)
    return [v * (0.85 + 0.15 * lfo(8, i)) for i, v in enumerate(out)]


# ---------------------------------------------------------------- synth arp
def synth_arp():
    """A plucky pulse arpeggio in sixteenths (C minor), each note's filter snapping shut."""
    buf = [0.0] * N
    six = N // 32
    seq = [60, 63, 67, 70, 72, 70, 67, 63, 58, 62, 65, 70, 74, 70, 65, 62]
    out = [0.0] * N
    for s in range(32):
        n = seq[s % 16]
        f = loop_hz(midi_hz(n))
        tab = wave_table("square", f)
        T, inc, p = len(tab), f * len(tab) / SR, 0.0
        lp = bp = 0.0
        L = int(0.32 * SR)
        for i in range(L):
            t = i / SR
            x = tab[int(p) & (T - 1)] * math.exp(-t * 9) * (0.7 if s % 4 else 1.0)
            p += inc
            g = math.tan(math.pi * (300 + 4200 * math.exp(-t * 18)) / SR)
            hp = (x - (1.2 + g) * bp - lp) / (1 + g * (1.2 + g))
            bp += g * hp; lp += g * bp; bp += g * hp; lp += g * bp
            out[(s * six + i) % N] += lp * 0.5
    return out


# ---------------------------------------------------------------- air pad
def air_pad():
    """High, airy sine clusters with breath, shimmering slowly."""
    buf = [0.0] * N
    rnd = random.Random(29)
    for n in (72, 79, 84, 86, 91):
        for det in (-0.08, 0.08):
            osc(buf, "triangle", midi_hz(n + det), 0.06, rnd.random(),
                (lambda ph: (lambda i: 0.6 + 0.4 * lfo(1, i, ph)))(rnd.random()))
    breath = noise_bed(7, 3500, 0.35)
    return [b + a * (0.5 + 0.5 * lfo(2, i, 0.3)) for i, (b, a) in enumerate(zip(buf, breath))]


# ---------------------------------------------------------------- dark drone
def dark_drone():
    """Low detuned saws (C and G) under a resonant filter sweeping slowly up and down."""
    buf = [0.0] * N
    rnd = random.Random(3)
    for n, a in ((36, 0.45), (43, 0.3), (48, 0.2)):
        for det in (-0.15, 0.0, 0.14):
            osc(buf, "saw", midi_hz(n + det), a * 0.33, rnd.random())
    return lowpass(buf, lambda i: 180 + 900 * (0.5 + 0.5 * lfo(1, i, -0.25)) ** 2, 3.5)


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


# ---------------------------------------------------------------- club beat
def club():
    """A tight four-on-the-floor club kick: a punchy kick on every beat, nothing else. Dry and short."""
    buf = [0.0] * N
    rnd = random.Random(101)
    six = N // 32

    def kick(start):
        out, ph = [], 0.0
        for i in range(int(0.32 * SR)):
            t = i / SR
            f = 50 + 140 * math.exp(-t * 40)              # fast pitch drop: punch
            ph += TAU * f / SR
            v = math.sin(ph) * math.exp(-t * 9.0)
            v += (rnd.random() * 2 - 1) * 0.25 * math.exp(-t * 400)   # click
            out.append(math.tanh(1.6 * v) * 1.0)
        add_wrapped(buf, start, out)

    def clap(start):
        out, lp, bp = [], 0.0, 0.0
        g = math.tan(math.pi * 1300 / SR)
        for i in range(int(0.18 * SR)):
            t = i / SR
            nz = rnd.random() * 2 - 1
            hp = (nz - 1.6 * bp - lp) / (1 + g * (1.6 + g))
            bp += g * hp; lp += g * bp; bp += g * hp; lp += g * bp
            # three quick hits, then a short tail
            e = sum(math.exp(-(t - d) * 180) for d in (0.0, 0.010, 0.021) if t >= d) + 0.6 * math.exp(-t * 28) * (t > 0.021)
            out.append(bp * e * 0.55)
        add_wrapped(buf, start, out)

    def hat(start, length, decay, amp):
        out, prev, prev2 = [], 0.0, 0.0
        for i in range(int(length * SR)):
            t = i / SR
            nz = rnd.random() * 2 - 1
            hp = nz - 2 * prev + prev2                     # bright: second difference
            prev2, prev = prev, nz
            out.append(hp * math.exp(-t * decay) * amp)
        add_wrapped(buf, start, out)

    for s in range(0, 32, 4):   # just the kick, on every beat
        kick(s * six)
    return buf


def main():
    os.makedirs(OUT, exist_ok=True)
    for gone in ("3 Choir.wav", "10 Club Beat.wav"):
        old = os.path.join(OUT, gone)
        if os.path.exists(old):
            os.remove(old)
    for name, fn in (("1 Kalimba", kalimba), ("2 Tape", tape), ("3 Haze", haze), ("4 Glass", glass),
                     ("5 Drone", drone), ("6 Analog Pad", analog_pad), ("7 Synth Arp", synth_arp),
                     ("8 Air Pad", air_pad), ("9 Dark Drone", dark_drone), ("10 Club Kick", club)):
        data = fn()
        path = os.path.join(OUT, name + ".wav")
        write_wav(path, data)
        print("wrote", os.path.relpath(path), len(data), "frames")


if __name__ == "__main__":
    main()
