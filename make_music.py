#!/usr/bin/env python3
"""Lancer's chiptune music, played by a note sequencer on the console.

The songs are written below as note strings. Instead of rendering them to
audio (which made the ROM about 14 MB), this script exports:

  * a handful of tiny instrument samples: three pulse (square) waves with
    different duty cycles, a stepped triangle, and four drums; and
  * music_data.h, the note events for each song, which chipmusic.h plays
    by pitching those samples with the sound chip's channel speed and
    shaping each note's volume frame by frame.

Tempos are chosen so a sixteenth note is a whole number of frames
(6, 7 and 8 frames at 60 fps), so every note starts exactly on a frame.

make_sounds.py imports INSTRUMENTS from here (they go into the ROM after
the sound effects) and calls write_music_data() to regenerate the header.
"""
import numpy as np

RATE = 44100
FPS = 60
MAX_VOICES = 7          # sound channels the engine may use
GAIN_MAX = 0.6          # loudest gain used in the songs -> velocity 127
rng = np.random.default_rng(1983)

NOTE_INDEX = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5,
              "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}


def midi(name):
    """'A4' -> 69"""
    return 12 * (int(name[-1]) + 1) + NOTE_INDEX[name[:-1]]


def freq(name):
    return 440.0 * 2 ** ((midi(name) - 69) / 12)


# ------------------------------------------------------------- instruments
#
# Pitched samples loop: their length is a whole number of waveform periods,
# plus one extra sample equal to the first (the sound chip loops at
# length - 1). Each instrument also carries a fixed stereo position, since
# the chip's channels have no panning of their own.

PULSE_BASE = 441.0      # period of exactly 100 samples
TRI_BASE = 110.25       # period of exactly 400 samples

INS_P12, INS_P25, INS_P50, INS_TRI, INS_KICK, INS_SNARE, INS_HAT, INS_OHAT = range(8)
DUTY_TO_INS = {0.125: INS_P12, 0.25: INS_P25, 0.5: INS_P50}


def looped(wave):
    return np.concatenate([wave, wave[:1]])


def pulse_sample(duty, pan):
    phase = (np.arange(4400) * PULSE_BASE / RATE) % 1.0
    w = looped(np.where(phase < duty, 1.0, -1.0) * 0.5)
    return (w * min(1.0, 1.0 - pan), w * min(1.0, 1.0 + pan))


def triangle_sample():
    phase = (np.arange(4000) * TRI_BASE / RATE) % 1.0
    tri = 2 * np.abs(2 * phase - 1) - 1
    w = looped(np.round(tri * 7.5) / 7.5 * 0.9)
    return (w, w)


def kick_sample():
    t = np.arange(int(RATE * 0.25)) / RATE
    f = 140 * np.exp(-t * 28) + 45
    w = np.sin(2 * np.pi * np.cumsum(f) / RATE) * np.exp(-t * 14) * 0.9
    return (w, w)


def snare_sample():
    t = np.arange(int(RATE * 0.2)) / RATE
    w = (rng.uniform(-1, 1, len(t)) * np.exp(-t * 22) * 0.8
         + np.sin(2 * np.pi * 190 * t) * np.exp(-t * 30) * 0.5) * 0.7
    return (w, w)


def hat_sample(open_):
    t = np.arange(int(RATE * (0.25 if open_ else 0.06))) / RATE
    n = rng.uniform(-1, 1, len(t))
    n = n - np.concatenate([[0], n[:-1]])
    w = n * np.exp(-t * (18 if open_ else 70)) * 0.5
    return (w * 0.8, w)


# name, function returning (left, right); same order as the INS_ numbers
INSTRUMENTS = [
    ("ins_pulse12", lambda: pulse_sample(0.125, 0.3)),
    ("ins_pulse25", lambda: pulse_sample(0.25, -0.25)),
    ("ins_pulse50", lambda: pulse_sample(0.5, -0.3)),
    ("ins_triangle", triangle_sample),
    ("drum_kick", kick_sample),
    ("drum_snare", snare_sample),
    ("drum_hat", lambda: hat_sample(False)),
    ("drum_ohat", lambda: hat_sample(True)),
]

DRUM_INS = {"kick": INS_KICK, "snare": INS_SNARE, "hat": INS_HAT, "ohat": INS_OHAT}


# ------------------------------------------------------------- recording songs

class Song:
    """Same interface the songs were written against, but it records
    note events (in frames) instead of rendering audio."""

    def __init__(self, bpm, steps_per_beat, bars, beats_per_bar=4):
        frames = 60.0 * FPS / bpm / steps_per_beat
        assert abs(frames - round(frames)) < 1e-6, "tempo must give whole frames per step"
        self.fps = int(round(frames))                    # frames per step
        self.total_steps = bars * steps_per_beat * beats_per_bar
        self.length = self.total_steps * self.fps        # in frames
        self.events = []

    def tone(self, start_step, steps, note, kind, gain, pan=0.0,
             duty=0.5, decay=0.0, sustain=1.0, gate=0.9, vibrato=0.0):
        ins = INS_TRI if kind == "triangle" else DUTY_TO_INS[duty]
        dur = max(1, int(round(steps * gate * self.fps)))
        self.events.append(dict(start=start_step * self.fps, dur=dur, note=midi(note), gain=gain,
                                ins=ins, decay=decay, sustain=sustain, vibrato=vibrato))

    def drum(self, start_step, kind, gain, pan=0.0):
        # drums are one-shots; a later hit on the same voice may cut the tail
        self.events.append(dict(start=start_step * self.fps, dur=1, note=0, gain=gain,
                                ins=DRUM_INS[kind], decay=0.0, sustain=1.0, vibrato=0.0))

    def melody(self, start_step, text, kind, gain, pan=0.0, **kw):
        """'A4 - C5 E5 . D5' : one token per step; '-' holds, '.' rests."""
        tokens = text.split()
        i = 0
        while i < len(tokens):
            tok = tokens[i]
            if tok in "-.":
                i += 1
                continue
            length = 1
            while i + length < len(tokens) and tokens[i + length] == "-":
                length += 1
            self.tone(start_step + i, length, tok, kind, gain, pan, **kw)
            i += length


def layer(song_factory, fill):
    s = song_factory()
    fill(s)
    return s.events, None


def mix_layers(song, layers):
    """Collects each part; an echo becomes one extra delayed, quieter part."""
    song.parts = []
    for events, _, fx in layers:
        song.parts.append(events)
        if fx and getattr(song, "echo_ok", True):
            delay_steps, _feedback, mix = fx
            if mix >= 0.3:                      # only the prominent echoes get a voice
                echo = []
                for e in events:
                    e2 = dict(e)
                    e2["start"] = (e["start"] + delay_steps * song.fps) % song.length
                    e2["gain"] = e["gain"] * mix
                    e2["vibrato"] = 0.0
                    echo.append(e2)
                song.parts.append(echo)


class _Rendered:
    def __init__(self, song, loop):
        self.song, self.loop = song, loop


def _render(self, loop):
    return (_Rendered(self, loop), None)


Song.render = _render


def finish(rendered, _none, peak):
    return rendered


# ------------------------------------------------------------- chords (D minor)

CHORDS = {
    "Dm": ["D4", "F4", "A4"], "Bb": ["A#3", "D4", "F4"], "C": ["C4", "E4", "G4"],
    "Gm": ["G3", "A#3", "D4"], "A": ["A3", "C#4", "E4"], "F": ["F3", "A3", "C4"],
    "Eb": ["D#4", "G4", "A#4"], "D": ["D4", "F#4", "A4"], "G": ["G3", "B3", "D4"],
}
ROOTS = {"Dm": "D1", "Bb": "A#1", "C": "C2", "Gm": "G1", "A": "A1", "F": "F1",
         "Eb": "D#1", "D": "D1", "G": "G1"}
PAD = {"Dm": "F4", "Bb": "F4", "C": "E4", "Gm": "D4", "A": "C#4", "F": "F4",
       "Eb": "D#4", "D": "F#4", "G": "D4"}


def up(n, octaves=1):
    return n[:-1] + str(int(n[-1]) + octaves)


def eighths(text):
    """An eighth-note line (one token per eighth) on the sixteenth grid."""
    return " ".join(t + " -" if t != "." else ". ." for t in text.split())


def fifth_of(ch, root):
    fifth = CHORDS[ch][2][:-1] + root[-1]
    if midi(fifth) < midi(root):
        fifth = up(fifth)
    return fifth


# ------------------------------------------------------------- title theme

def title_theme():
    """The heroic call: D minor, 112.5 BPM (8 frames per sixteenth), 16 bars
    (34 s), for the title pages, the attract demo and initials entry.

      bars  1-4   the fanfare call over a low pedal and a timpani-like kick
      bars  5-12  the march: snare, root-and-fifth bass, the theme
      bars 13-16  the theme climbs and hangs on A major to loop

    Six voices: lead, pad, bass, kick, snare, hats."""
    BPM, SPB, BARS = 112.5, 4, 16
    new = lambda: Song(BPM, SPB, BARS)
    progression = ["Dm", "Dm", "Bb", "C", "Dm", "Bb", "Gm", "A",
                   "F", "C", "Dm", "Bb", "Gm", "C", "A", "A"]

    lead = eighths("""
    A4 -  D5 -  -  -  E5 F5    E5 -  -  -  D5 -  A4 -     F5 -  -  -  D5 -  A#4 D5   C5 -  -  -  -  -  -  -
    D5 -  A5 -  -  -  G5 F5    F5 -  -  -  D5 -  F5 G5    G5 -  -  -  A#5 - A5 G5    A5 -  -  -  -  -  -  -
    C6 -  -  -  A5 -  F5 -     G5 -  -  -  E5 -  C5 E5    F5 -  -  -  D5 -  A5 -     A#5 - -  -  A5 -  F5 -
    G5 -  -  -  A#5 - D6 -     C6 -  -  -  A#5 - G5 E5    A5 -  -  -  -  -  C#6 -    E6 -  -  -  -  -  -  -
    """)
    assert len(lead.split()) == BARS * 16

    def lead_part(s):
        s.melody(0, lead, "pulse", 0.25, duty=0.25, decay=0.8, sustain=0.6,
                 gate=0.95, vibrato=0.008)

    def pad_part(s):
        for bar, ch in enumerate(progression):
            s.tone(bar * 16, 16, PAD[ch], "pulse", 0.1, duty=0.5, decay=0.9, sustain=0.55, gate=0.98)

    def bass_part(s):
        for bar, ch in enumerate(progression):
            root = up(ROOTS[ch])
            if bar < 4:
                for k in range(0, 16, 2):                 # a low pedal in eighths
                    s.tone(bar * 16 + k, 1, up(ROOTS["Dm"]), "triangle", 0.34, gate=0.6)
                continue
            fifth = fifth_of(ch, root)
            for k, note in ((0, root), (4, fifth), (6, fifth), (8, root), (12, fifth)):
                s.tone(bar * 16 + k, 2, note, "triangle", 0.42, gate=0.7)

    def kick_part(s):
        for bar in range(BARS):
            if bar < 4:
                s.drum(bar * 16, "kick", 0.5)             # timpani on the downbeat
                if bar == 3:
                    for k in (10, 12, 14):
                        s.drum(bar * 16 + k, "kick", 0.4)
            else:
                for k in (0, 8):
                    s.drum(bar * 16 + k, "kick", 0.55)

    def snare_part(s):
        figure = (0, 3, 4, 8, 11, 12, 14)
        for bar in range(4, BARS):
            for k in figure:
                accent = 1.4 if k in (4, 12) else 1.0
                s.drum(bar * 16 + k, "snare", 0.18 * accent)
        for k in range(8, 16):                            # a roll into the loop
            s.drum((BARS - 1) * 16 + k, "snare", 0.1 + 0.02 * (k - 8))

    def hat_part(s):
        for bar in range(8, BARS):
            for k in (2, 6, 10, 14):
                s.drum(bar * 16 + k, "hat", 0.06)

    song = new()
    mix_layers(song, [
        (*layer(new, lead_part), None),
        (*layer(new, pad_part), None),
        (*layer(new, bass_part), None),
        (*layer(new, kick_part), None),
        (*layer(new, snare_part), None),
        (*layer(new, hat_part), None),
    ])
    return finish(*song.render(loop=True), 0.6)


# ------------------------------------------------------------- the interceptors

PHASE_BPM = 900.0 / 7                    # 7 frames per sixteenth, about 128.6 BPM


def space_theme():
    """The dogfight: D minor, about 128.6 BPM, 16 bars (30 s). A driving
    octave bass, a sixteenth arpeggio, backbeat drums and a soaring lead.
    Seven voices."""
    BARS = 16
    new = lambda: Song(PHASE_BPM, 4, BARS)
    progression = ["Dm", "Dm", "Bb", "C", "Dm", "Dm", "Gm", "A",
                   "Bb", "C", "Dm", "Bb", "Gm", "Bb", "C", "A"]

    lead = eighths("""
    D5 -  F5 A5 -  -  G5 F5    E5 -  D5 -  A4 -  -  -     D5 -  F5 A#5 - -  A5 F5    G5 -  -  -  E5 -  C5 -
    D5 -  F5 A5 -  -  D6 -     C6 -  A5 -  F5 -  E5 -     D5 -  G5 A#5 - -  A5 G5    A5 -  -  -  C#6 - E6 -
    F6 -  -  -  D6 -  A#5 -    G5 -  -  -  E5 -  C6 -     A5 -  -  -  F5 -  D5 -     F5 -  -  -  A#5 - D6 -
    D6 -  -  -  A#5 - G5 -     F5 -  A#5 - D6 -  F6 -     E6 -  -  -  C6 -  G5 -     A5 -  -  -  E5 -  C#5 E5
    """)
    assert len(lead.split()) == BARS * 16

    def lead_part(s):
        s.melody(0, lead, "pulse", 0.22, duty=0.25, decay=0.6, sustain=0.55,
                 gate=0.95, vibrato=0.008)

    def arp_part(s):
        for bar, ch in enumerate(progression):
            a, b, c = CHORDS[ch]
            seq = [up(a), up(b), up(c), up(b)]
            for k in range(16):
                accent = 1.25 if k % 4 == 0 else 1.0
                s.tone(bar * 16 + k, 1, seq[k % 4], "pulse", 0.045 * accent, duty=0.125,
                       decay=0.05, sustain=0.0, gate=0.85)

    def bass_part(s):
        for bar, ch in enumerate(progression):
            root = up(ROOTS[ch])
            for k in range(0, 16, 2):
                note = up(root) if k % 4 == 2 else root    # octave jumps on the off-beats
                s.tone(bar * 16 + k, 2, note, "triangle", 0.42 if k % 4 == 0 else 0.34, gate=0.65)

    def kick_part(s):
        for bar in range(BARS):
            for k in (0, 6, 8):
                s.drum(bar * 16 + k, "kick", 0.55 if k != 6 else 0.4)

    def snare_part(s):
        for bar in range(BARS):
            for k in (4, 12):
                s.drum(bar * 16 + k, "snare", 0.26)
            if bar % 4 == 3:
                for k in (13, 14, 15):
                    s.drum(bar * 16 + k, "snare", 0.16)

    def hat_part(s):
        for bar in range(BARS):
            for k in range(0, 16, 2):
                s.drum(bar * 16 + k, "ohat" if k % 4 == 2 else "hat", 0.1 if k % 4 == 2 else 0.06)

    def pad_part(s):
        for bar, ch in enumerate(progression):
            s.tone(bar * 16, 16, PAD[ch], "pulse", 0.07, duty=0.5, decay=1.0, sustain=0.5, gate=0.98)

    song = new()
    mix_layers(song, [
        (*layer(new, lead_part), None),
        (*layer(new, arp_part), None),
        (*layer(new, bass_part), None),
        (*layer(new, kick_part), None),
        (*layer(new, snare_part), None),
        (*layer(new, hat_part), None),
        (*layer(new, pad_part), None),
    ])
    return finish(*song.render(loop=True), 0.6)


# ------------------------------------------------------------- the hull

def hull_theme():
    """Over the hull: menacing, same tempo, 8 bars (15 s). Syncopated stabs
    with the Neapolitan E flat chord, a pounding bass and four-on-the-floor.
    Six voices."""
    BARS = 8
    new = lambda: Song(PHASE_BPM, 4, BARS)
    progression = ["Dm", "Dm", "Eb", "Dm", "Dm", "Dm", "Bb", "A"]

    riff = eighths("""
    D5 .  .  D5 .  .  F5 E5    D5 .  .  A4 .  .  C5 D5    D#5 . .  D#5 . . G5 F5     D5 -  -  -  -  -  .  .
    A5 .  .  A5 .  .  C6 A5    G5 .  .  F5 .  .  E5 F5    F5 .  .  D5 .  .  A#4 D5   C#5 - -  -  E5 -  A5 -
    """)
    assert len(riff.split()) == BARS * 16

    def lead_part(s):
        s.melody(0, riff, "pulse", 0.2, duty=0.5, decay=0.15, sustain=0.35, gate=0.8)

    def bass_part(s):
        for bar, ch in enumerate(progression):
            root = up(ROOTS[ch])
            for k in range(16):
                if k % 4 == 3:
                    continue                              # a pounding x x x .
                note = up(root) if k == 14 else root
                s.tone(bar * 16 + k, 1, note, "triangle", 0.42 if k % 4 == 0 else 0.32, gate=0.6)

    def kick_part(s):
        for bar in range(BARS):
            for k in (0, 4, 8, 12):
                s.drum(bar * 16 + k, "kick", 0.55)

    def snare_part(s):
        for bar in range(BARS):
            for k in (4, 12):
                s.drum(bar * 16 + k, "snare", 0.24)
            if bar == BARS - 1:
                for k in (13, 14, 15):
                    s.drum(bar * 16 + k, "snare", 0.18)

    def hat_part(s):
        for bar in range(BARS):
            for k in range(1, 16, 2):
                s.drum(bar * 16 + k, "hat", 0.05)

    def pad_part(s):
        for bar, ch in enumerate(progression):
            s.tone(bar * 16, 16, PAD[ch], "pulse", 0.08, duty=0.5, decay=1.0, sustain=0.5, gate=0.98)

    song = new()
    mix_layers(song, [
        (*layer(new, lead_part), None),
        (*layer(new, bass_part), None),
        (*layer(new, kick_part), None),
        (*layer(new, snare_part), None),
        (*layer(new, hat_part), None),
        (*layer(new, pad_part), None),
    ])
    return finish(*song.render(loop=True), 0.6)


# ------------------------------------------------------------- the trench

def trench_theme():
    """The trench run: urgent, 150 BPM (6 frames per sixteenth), 8 bars
    (12.8 s). A sixteenth-note bass pedal lurching up to E flat, arpeggiated
    stabs, four-on-the-floor with a snare on the backbeat. Six voices."""
    BPM, BARS = 150, 8
    new = lambda: Song(BPM, 4, BARS)
    progression = ["Dm", "Eb", "Dm", "Eb", "Bb", "C", "Dm", "A"]

    riff = eighths("""
    D5 .  A5 .  D6 .  A5 .     D#6 . A#5 . G5 .  A#5 .    D6 .  A5 .  F5 .  A5 .     D#6 - -  -  D6 -  -  -
    D6 .  A#5 . F5 .  D6 .     E6 .  C6 .  G5 .  C6 .     F6 -  -  -  E6 -  D6 -     C#6 - -  -  E6 -  A6 -
    """)
    assert len(riff.split()) == BARS * 16

    def lead_part(s):
        s.melody(0, riff, "pulse", 0.2, duty=0.5, decay=0.15, sustain=0.35, gate=0.8)

    def bass_part(s):
        for bar, ch in enumerate(progression):
            root = up(ROOTS[ch])
            for k in range(16):
                s.tone(bar * 16 + k, 1, root, "triangle", 0.42 if k % 4 == 0 else 0.3, gate=0.6)

    def arp_part(s):
        for bar, ch in enumerate(progression):
            a, b, c = CHORDS[ch]
            seq = [up(c), up(a, 2), up(b), up(a, 2)]
            for k in range(0, 16, 2):
                s.tone(bar * 16 + k, 2, seq[(k // 2) % 4], "pulse", 0.045, duty=0.125,
                       decay=0.05, sustain=0.0, gate=0.7)

    def kick_part(s):
        for bar in range(BARS):
            for k in (0, 4, 8, 12):
                s.drum(bar * 16 + k, "kick", 0.55)

    def snare_part(s):
        for bar in range(BARS):
            for k in (4, 12):
                s.drum(bar * 16 + k, "snare", 0.28)
            if bar % 4 == 3:
                for k in (13, 14, 15):
                    s.drum(bar * 16 + k, "snare", 0.2)

    def hat_part(s):
        for bar in range(BARS):
            for k in range(1, 16, 2):
                s.drum(bar * 16 + k, "hat" if k % 4 == 1 else "ohat", 0.07 if k % 4 == 1 else 0.1)

    song = new()
    mix_layers(song, [
        (*layer(new, lead_part), None),
        (*layer(new, bass_part), None),
        (*layer(new, arp_part), None),
        (*layer(new, kick_part), None),
        (*layer(new, snare_part), None),
        (*layer(new, hat_part), None),
    ])
    return finish(*song.render(loop=True), 0.6)


# ------------------------------------------------------------- victory and defeat

def victory_fanfare():
    """The dreadnought is destroyed: a triumphant turn to D major, about 5 s."""
    BPM, BARS = 112.5, 3
    new = lambda: Song(BPM, 4, BARS)

    def lead_part(s):
        s.melody(0, "A4 - D5 - F#5 - A5 - - - F#5 - A5 - - -  B5 - - - G5 - B5 - C#6 - - - E6 - - -  "
                    "D6 - - - - - - - - - - . . . . .",
                 "pulse", 0.25, duty=0.25, decay=0.8, sustain=0.6, gate=0.95, vibrato=0.008)

    def pad_part(s):
        s.tone(0, 16, "F#4", "pulse", 0.1, duty=0.5, decay=1.2, sustain=0.5, gate=0.98)
        s.tone(16, 8, "D4", "pulse", 0.1, duty=0.5, decay=1.2, sustain=0.5, gate=0.98)
        s.tone(24, 8, "E4", "pulse", 0.1, duty=0.5, decay=1.2, sustain=0.5, gate=0.98)
        s.tone(32, 12, "F#4", "pulse", 0.1, duty=0.5, decay=1.2, sustain=0.5, gate=0.98)

    def bass_part(s):
        s.tone(0, 16, "D2", "triangle", 0.42, gate=0.95)
        s.tone(16, 8, "G2", "triangle", 0.42, gate=0.95)
        s.tone(24, 8, "A2", "triangle", 0.42, gate=0.95)
        s.tone(32, 12, "D2", "triangle", 0.42, gate=0.95)

    def drum_part(s):
        for k in (0, 8, 16, 24, 32):
            s.drum(k, "kick", 0.5)

    def roll_part(s):
        for k in range(24, 32):
            s.drum(k, "snare", 0.08 + 0.025 * (k - 24))
        s.drum(32, "ohat", 0.16)

    song = new()
    mix_layers(song, [
        (*layer(new, lead_part), None),
        (*layer(new, pad_part), None),
        (*layer(new, bass_part), None),
        (*layer(new, drum_part), None),
        (*layer(new, roll_part), None),
    ])
    return finish(*song.render(loop=False), 0.6)


def gameover_sting():
    """Game over: the tune sinks through B flat and the Neapolitan E flat
    onto D minor, about 6 s."""
    BPM, BARS = 100, 3
    new = lambda: Song(BPM, 4, BARS)

    def lead_part(s):
        s.melody(0, "D6 - - - - - C6 - A#5 - - - A5 - - -  G5 - - - - - A#5 - G5 - - - D#5 - - -  "
                    "D5 - - - - - - - - - - - - - . .",
                 "pulse", 0.25, duty=0.25, decay=0.7, sustain=0.6, gate=0.95, vibrato=0.008)

    def pad_part(s):
        s.tone(0, 16, "F4", "pulse", 0.1, duty=0.5, decay=1.2, sustain=0.5, gate=0.98)
        s.tone(16, 16, "G4", "pulse", 0.1, duty=0.5, decay=1.2, sustain=0.5, gate=0.98)
        s.tone(32, 14, "F4", "pulse", 0.1, duty=0.5, decay=1.2, sustain=0.5, gate=0.98)

    def bass_part(s):
        s.tone(0, 16, "A#1", "triangle", 0.42, gate=0.95)
        s.tone(16, 16, "D#2", "triangle", 0.42, gate=0.95)
        s.tone(32, 14, "D2", "triangle", 0.42, gate=0.95)

    def drum_part(s):
        s.drum(0, "kick", 0.5)
        s.drum(16, "kick", 0.4)
        s.drum(32, "kick", 0.55)

    def roll_part(s):
        for k in range(24, 32):
            s.drum(k, "snare", 0.08 + 0.02 * (k - 24))

    song = new()
    mix_layers(song, [
        (*layer(new, lead_part), None),
        (*layer(new, pad_part), None),
        (*layer(new, bass_part), None),
        (*layer(new, drum_part), None),
        (*layer(new, roll_part), None),
    ])
    return finish(*song.render(loop=False), 0.6)


# ------------------------------------------------------------- the secret core

def core_theme():
    """Inside the dreadnought: eerie and glittering, about 128.6 BPM, 8 bars
    (15 s). A sixteenth arpeggio climbing over D minor add-9 colours, long
    vibrato notes high above, a slow pulse of kick and soft hats. Six voices."""
    BARS = 8
    new = lambda: Song(PHASE_BPM, 4, BARS)
    progression = ["Dm", "Bb", "F", "C", "Dm", "Bb", "Gm", "A"]
    colour = {"Dm": ["D5", "E5", "F5", "A5"], "Bb": ["A#4", "D5", "F5", "C6"],
              "F": ["F5", "G5", "A5", "C6"], "C": ["C5", "E5", "G5", "D6"],
              "Gm": ["G4", "A#4", "D5", "A5"], "A": ["A4", "C#5", "E5", "G5"]}

    lead = eighths("""
    A6 -  -  -  -  -  -  -     F6 -  -  -  -  -  -  -     C6 -  -  -  -  -  -  -     D6 -  -  -  E6 -  -  -
    F6 -  -  -  -  -  -  -     D6 -  -  -  -  -  -  -     D6 -  -  -  A#5 - -  -     C#6 - -  -  E6 -  -  -
    """)
    assert len(lead.split()) == BARS * 16

    def lead_part(s):
        s.melody(0, lead, "pulse", 0.13, duty=0.125, decay=1.2, sustain=0.5, gate=0.95, vibrato=0.012)

    def arp_part(s):
        for bar, ch in enumerate(progression):
            seq = colour[ch]
            for k in range(16):
                note = seq[k % 4] if (k // 4) % 2 == 0 else up(seq[3 - k % 4], 0)
                s.tone(bar * 16 + k, 1, note, "pulse", 0.06 if k % 4 == 0 else 0.045, duty=0.25,
                       decay=0.05, sustain=0.0, gate=0.8)

    def bass_part(s):
        for bar, ch in enumerate(progression):
            s.tone(bar * 16, 8, up(ROOTS[ch]), "triangle", 0.4, gate=0.95)
            s.tone(bar * 16 + 8, 8, up(ROOTS[ch], 2), "triangle", 0.3, gate=0.9)

    def kick_part(s):
        for bar in range(BARS):
            s.drum(bar * 16, "kick", 0.45)
            s.drum(bar * 16 + 10, "kick", 0.3)

    def hat_part(s):
        for bar in range(BARS):
            for k in range(2, 16, 4):
                s.drum(bar * 16 + k, "hat", 0.05)
            s.drum(bar * 16 + 8, "ohat", 0.08)

    def pad_part(s):
        for bar, ch in enumerate(progression):
            s.tone(bar * 16, 16, PAD[ch], "pulse", 0.07, duty=0.5, decay=1.2, sustain=0.5, gate=0.98)

    song = new()
    mix_layers(song, [
        (*layer(new, lead_part), None),
        (*layer(new, arp_part), None),
        (*layer(new, bass_part), None),
        (*layer(new, kick_part), None),
        (*layer(new, hat_part), None),
        (*layer(new, pad_part), None),
    ])
    return finish(*song.render(loop=True), 0.6)


TRACKS = [
    ("title", title_theme),
    ("space", space_theme),
    ("hull", hull_theme),
    ("trench", trench_theme),
    ("victory", victory_fanfare),
    ("gameover", gameover_sting),
    ("core", core_theme),
]

# ------------------------------------------------------------- export

def allocate_voices(parts):
    """Gives each part its own channel(s). Notes in one part that overlap in
    time are spread over extra channels (chords, drums hit together)."""
    voices = []
    for part in parts:
        mine = []
        for e in sorted(part, key=lambda e: e["start"]):
            for v in mine:
                if v[-1]["start"] + v[-1]["dur"] <= e["start"]:
                    v.append(e)
                    break
            else:
                mine.append([e])
        voices += mine
    return voices


def write_music_data(path):
    decays, vibratos = [0.0], [0.0]
    tracks = []
    for name, fn in TRACKS:
        r = fn()
        song = r.song
        voices = allocate_voices(song.parts)
        assert len(voices) <= MAX_VOICES, f"{name} needs {len(voices)} voices"
        if r.loop:
            length = song.length
        else:
            length = max(e["start"] + e["dur"] for v in voices for e in v) + 40
        for v in voices:
            for e in v:
                for table, key in ((decays, "decay"), (vibratos, "vibrato")):
                    if e[key] not in table:
                        table.append(e[key])
        tracks.append((name, r.loop, length, voices))
    assert len(decays) <= 16 and len(vibratos) <= 4

    events, firsts, counts = [], [], []
    for name, loop, length, voices in tracks:
        for v in range(MAX_VOICES):
            evs = sorted(voices[v], key=lambda e: e["start"]) if v < len(voices) else []
            firsts.append(len(events) // 2)
            counts.append(len(evs))
            for e in evs:
                vel = max(1, min(127, int(round(e["gain"] / GAIN_MAX * 127))))
                sus = int(round(e["sustain"] * 15))
                w0 = e["start"] | (min(e["dur"], 0x7FFF) << 16)
                w1 = (e["note"] | (vel << 7) | (e["ins"] << 14) | (decays.index(e["decay"]) << 17)
                      | (sus << 21) | (vibratos.index(e["vibrato"]) << 25))
                events += [w0, w1]

    def c_array(ctype, name, values, per_line=8, fmt="{}"):
        rows = [", ".join(fmt.format(v) for v in values[i:i + per_line])
                for i in range(0, len(values), per_line)]
        return f"{ctype}[ {len(values)} ] {name} =\n{{\n    " + ",\n    ".join(rows) + "\n};\n"

    k = [0.0 if d == 0 else float(np.exp(-1.0 / (FPS * d))) for d in decays]
    with open(path, "w") as f:
        f.write("// generated by make_music.py: note events for chipmusic.h\n\n")
        f.write(f"#define MUSIC_TRACKS {len(tracks)}\n")
        for i, (name, *_ ) in enumerate(tracks):
            f.write(f"#define MUSIC_{name.upper()} {i}\n")
        f.write("\n")
        f.write(c_array("int", "mus_track_length", [t[2] for t in tracks]))
        f.write(c_array("int", "mus_track_loops", [1 if t[1] else 0 for t in tracks]))
        f.write(c_array("int", "mus_voice_first", firsts))
        f.write(c_array("int", "mus_voice_count", counts))
        f.write("// per-frame volume multiplier for each decay setting (0 = no decay)\n")
        f.write(c_array("float", "mus_decay_k", k, fmt="{:.6f}"))
        f.write(c_array("float", "mus_vibrato_depth", vibratos + [0.0] * (4 - len(vibratos)), fmt="{:.4f}"))
        f.write("// two words per note: start | duration << 16, then\n"
                "// note | velocity << 7 | instrument << 14 | decay << 17 | sustain << 21 | vibrato << 25\n")
        f.write(c_array("int", "mus_events", events, per_line=6, fmt="0x{:08X}"))
    for name, loop, length, voices in tracks:
        n = sum(len(v) for v in voices)
        print(f"  {name:9s} {length / FPS:5.1f}s  {'loop' if loop else 'once'}  {len(voices)} voices  {n} notes")
    print(f"  music_data.h: {len(events) // 2} notes, {len(events) * 4} bytes")


if __name__ == "__main__":
    write_music_data("music_data.h")
