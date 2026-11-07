#!/usr/bin/env python3
"""Synthesizes Lancer's sound effects.

Writes each sound as a .wav (to listen to) and a .vsnd (Vircon32's
native format: "V32-VSND", uint32 sample count, then 16-bit stereo
samples at 44100 Hz), into the sounds/ folder. The order of SOUNDS
is the order they must appear in the ROM definition XML.
"""
import os
import struct
import wave
import numpy as np

RATE = 44100
rng = np.random.default_rng(1983)


def t_axis(seconds):
    return np.arange(int(RATE * seconds)) / RATE


def envelope(n, attack=0.002, release=None, curve=3.0):
    """Short attack, then exponential-ish decay to zero."""
    t = np.linspace(0, 1, n)
    env = (1 - t) ** curve
    a = int(attack * RATE)
    if a > 0:
        env[:a] *= np.linspace(0, 1, a)
    return env


def lowpass(x, cutoff):
    """One-pole low-pass; cutoff may be a scalar or per-sample array."""
    cutoff = np.broadcast_to(np.asarray(cutoff, dtype=float), x.shape)
    alpha = 1 - np.exp(-2 * np.pi * cutoff / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc += alpha[i] * (x[i] - acc)
        y[i] = acc
    return y


def sweep(f0, f1, seconds, shape="square", exp=True):
    t = t_axis(seconds)
    if exp:
        freq = f0 * (f1 / f0) ** (t / seconds)
    else:
        freq = f0 + (f1 - f0) * t / seconds
    phase = 2 * np.pi * np.cumsum(freq) / RATE
    if shape == "square":
        return np.sign(np.sin(phase))
    if shape == "saw":
        return 2 * ((phase / (2 * np.pi)) % 1.0) - 1
    if shape == "tri":
        return 2 * np.abs(2 * ((phase / (2 * np.pi)) % 1.0) - 1) - 1
    return np.sin(phase)


def normalize(x, peak):
    m = np.max(np.abs(x))
    return x * (peak / m) if m > 0 else x


def bang(seconds, cutoff_start, cutoff_end, peak):
    n = int(RATE * seconds)
    noise = rng.uniform(-1, 1, n)
    cut = np.geomspace(cutoff_start, cutoff_end, n)
    s = lowpass(lowpass(noise, cut), cut)
    return normalize(s * envelope(n, curve=2.2), peak)


def notes(freqs, note_len, shape="square", peak=0.4, tail=1.0):
    out = []
    for f in freqs:
        s = sweep(f, f, note_len, shape)
        out.append(s * envelope(len(s), curve=tail))
    s = np.concatenate(out)
    return normalize(lowpass(s, 5000), peak)


# ---------------------------------------------------------------- sounds

def looped(x):
    """makes a sample loop cleanly: the chip loops at length - 1, so the
    last sample repeats the first"""
    return np.concatenate([x, x[:1]])



def laser():
    # twin cockpit lasers: a fast falling zap with a buzzy edge
    n = int(RATE * 0.22)
    z = sweep(2600, 380, 0.22, "saw") * 0.6 + sweep(1300, 190, 0.22, "square") * 0.3
    return normalize(lowpass(z, 7000) * envelope(n, curve=1.6), 0.32)


def explode():
    # an interceptor or turret blown apart
    n = int(RATE * 0.9)
    rumble = sweep(120, 35, 0.9, "sine") * envelope(n, curve=1.8)
    return normalize(bang(0.9, 6000, 120, 1.0) + rumble * 0.5, 0.6)


def fireball():
    # an enemy fireball launched: a wobbling rising whoosh
    n = int(RATE * 0.45)
    t = np.arange(n) / RATE
    tone = sweep(220, 520, 0.45, "tri") * (0.6 + 0.4 * np.sin(2 * np.pi * 18 * t))
    hiss = lowpass(rng.uniform(-1, 1, n), 2500)
    return normalize((tone * 0.6 + hiss * 0.5) * envelope(n, curve=1.2), 0.3)


def fireball_pop():
    # a fireball shot down: a bright crackle
    n = int(RATE * 0.3)
    s = bang(0.3, 9000, 1500, 1.0) + sweep(900, 1800, 0.3, "square") * 0.25
    return normalize(s * envelope(n, curve=2.5), 0.4)


def shield_hit():
    # your shield takes a hit: a heavy buzz and a crunch
    n = int(RATE * 0.6)
    t = np.arange(n) / RATE
    buzz = np.sign(np.sin(2 * np.pi * 70 * t)) * 0.5 + np.sign(np.sin(2 * np.pi * 103 * t)) * 0.3
    s = lowpass(buzz, 1200) + bang(0.6, 3000, 200, 0.8)
    return normalize(s * envelope(n, curve=1.4), 0.7)


def big_boom():
    # the dreadnought explodes: a long, deep, rolling blast
    n = int(RATE * 3.0)
    t = np.arange(n) / RATE
    s = bang(3.0, 3500, 40, 1.0) + sweep(70, 22, 3.0, "sine") * 0.7
    roll = 0.75 + 0.25 * np.sin(2 * np.pi * 3.0 * t)
    return normalize(s * roll * np.exp(-t * 0.9), 0.8)


def banner():
    # a new phase begins: a rising three-note call
    return notes([392, 523, 784], 0.1, "square", 0.3, 0.8)


def beep():
    # a warning blip (catwalk ahead, the vent in range)
    t = t_axis(0.08)
    return normalize(np.sign(np.sin(2 * np.pi * 1760 * t)) * np.exp(-t * 30), 0.22)


def tower_down():
    # a gun tower destroyed: a metallic clang into a boom
    n = int(RATE * 0.7)
    t = np.arange(n) / RATE
    clang = sum(np.sin(2 * np.pi * f * t) * np.exp(-t * d) for f, d in [(820, 9), (1230, 12), (1710, 15)])
    return normalize(clang * 0.4 + bang(0.7, 4000, 150, 0.9), 0.55)


def hum():
    # the engine: a looping low drone, 0.4 s
    seconds = 0.4
    t = t_axis(seconds)
    s = np.sin(2 * np.pi * 55 * t) * 0.6 + np.sin(2 * np.pi * 82.5 * t) * 0.3 + np.sin(2 * np.pi * 110 * t) * 0.2
    return looped(normalize(s, 0.35))


def start():
    return notes([262, 330, 392, 523, 659], 0.08, "square", 0.32, 0.8)


def game_over():
    return notes([392, 330, 262, 196], 0.18, "square", 0.32, 1.2)


def extra_shield():
    return notes([659, 784, 1047, 1319], 0.07, "square", 0.3, 0.6)


SOUNDS = [
    ("laser", laser), ("explode", explode), ("fireball", fireball), ("fireball_pop", fireball_pop),
    ("shield_hit", shield_hit), ("big_boom", big_boom), ("banner", banner), ("beep", beep),
    ("tower_down", tower_down), ("hum", hum), ("start", start), ("game_over", game_over),
    ("extra_shield", extra_shield),
]

# music instruments come last (see make_music.py and chipmusic.h)
from make_music import INSTRUMENTS, write_music_data
SOUNDS += INSTRUMENTS


def write(name, samples, outdir):
    """samples: a mono array, or a (left, right) pair for stereo music"""
    if isinstance(samples, tuple):
        left, right = samples
        pcm = np.clip(np.stack([left, right], axis=1) * 32767, -32768, 32767).astype("<i2")
        stereo = pcm.reshape(-1)
        pcm = pcm[:, 0]
    else:
        pcm = np.clip(samples * 32767, -32768, 32767).astype("<i2")
        stereo = np.repeat(pcm, 2)

    with wave.open(os.path.join(outdir, name + ".wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(stereo.tobytes())

    with open(os.path.join(outdir, name + ".vsnd"), "wb") as f:
        f.write(b"V32-VSND")
        f.write(struct.pack("<I", len(pcm)))
        f.write(stereo.tobytes())


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    write_music_data(os.path.join(here, "music_data.h"))
    outdir = os.path.join(here, "sounds")
    os.makedirs(outdir, exist_ok=True)
    for i, (name, fn) in enumerate(SOUNDS):
        s = fn()
        write(name, s, outdir)
        n = len(s[0]) if isinstance(s, tuple) else len(s)
        print(f"{i:2d} {name:14s} {n / RATE:.2f}s")
