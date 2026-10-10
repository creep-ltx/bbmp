# SPDX-License-Identifier: GPL-3.0-or-later
"""Loudness: every song at the same level, so that the volume is set once.

Replayers differ in how loud they play, and so do songs. A song's loudness is
measured the way broadcasters do it (ITU-R BS.1770, in LUFS: the ear's sensitivity to
pitch is taken into account, and silent passages don't count), and the song is played
louder or softer by what it is off the target.

A song's loudness is known after it has been played once, and is kept. The first time,
it is measured on the song's first second, before it starts (each second read before
costs start-up time). In its first seconds (SETTLE) the level follows the measurement as
it grows, either way and gently; after that, if the song turns out louder than its
beginning, it is turned down gently as it plays, and never up again: a level that goes
up and down with the music is worse than one that is a little off.
"""

from __future__ import annotations

import math

import numpy as np

from .backends.base import RATE

TARGET = -18.0          # LUFS; what ReplayGain 2 aims at
MOST_BOOST = 12.0       # dB: a very quiet song is not made louder than by this
MOST_CUT = 24.0         # dB
PREVIEW = 1.0           # seconds of a song's beginning that are measured before it starts (each costs start-up time)
SETTLE = 5.0            # seconds in which the level may still go up, as the measurement grows
ENOUGH = 20.0           # seconds a song must have been measured for its loudness to be kept
BLOCK = int(0.4 * RATE)     # loudness is measured in blocks of 400 ms
HOP = int(0.1 * RATE)       # that overlap by three quarters
CEILING = 0.97          # of full scale: what the limiter keeps the sound under


def _weighting() -> np.ndarray:
    """How much each frequency of a block counts: the K-weighting of BS.1770, which is
    two filters, as the square of their gain at every frequency of the block's
    spectrum. (The standard's numbers are for 48 kHz, which is the player's rate.)"""
    w = np.exp(-2j * np.pi * np.fft.rfftfreq(BLOCK))

    def gain(b: tuple, a: tuple) -> np.ndarray:
        return np.abs((b[0] + b[1] * w + b[2] * w * w) / (a[0] + a[1] * w + a[2] * w * w)) ** 2
    shelf = gain((1.53512485958697, -2.69169618940638, 1.19839281085285),
                 (1.0, -1.69065929318241, 0.73248077421585))
    highpass = gain((1.0, -2.0, 1.0), (1.0, -1.99004745483398, 0.99007225036621))
    weights = shelf * highpass
    # a block's power is the sum over its spectrum, where every frequency but the
    # lowest and the highest stands for two
    weights[1:-1] *= 2
    return weights / (BLOCK * BLOCK)


_WEIGHTS = _weighting()


# Blocks are counted by their loudness, in steps of STEP LU between the absolute gate
# and LOUDEST: a song of hours takes no more memory or time to measure than one of
# seconds. Each step keeps its blocks' number and summed power, so the mean is exact;
# only where the relative gate falls inside a step is that step counted whole or not.
GATE = -70.0
LOUDEST = 10.0
STEP = 0.05
_STEPS = int((LOUDEST - GATE) / STEP) + 1
_GATE_POWER = 10 ** ((GATE + 0.691) / 10)


class Meter:
    """Measures the loudness of sound as it is given to it (s16le stereo)."""

    def __init__(self) -> None:
        self._rest = np.zeros((0, 2), dtype=np.float32)
        self._count = np.zeros(_STEPS)      # blocks above the absolute gate, by loudness
        self._power = np.zeros(_STEPS)      # and their power, summed
        self.seconds = 0.0

    def feed(self, pcm: bytes) -> None:
        if not pcm:
            return
        new = np.frombuffer(pcm, dtype="<i2").reshape(-1, 2).astype(np.float32) / 32768.0
        self.seconds += len(new) / RATE
        sound = np.concatenate((self._rest, new)) if len(self._rest) else new
        blocks = (len(sound) - BLOCK) // HOP + 1 if len(sound) >= BLOCK else 0
        if blocks:
            windows = np.lib.stride_tricks.sliding_window_view(sound, BLOCK, axis=0)[::HOP]
            spectra = np.fft.rfft(windows[:blocks], axis=2)       # (block, channel, frequency)
            power = (np.abs(spectra) ** 2 * _WEIGHTS).sum(axis=(1, 2))
            heard = power[power > _GATE_POWER]                   # quieter is silence
            if len(heard):
                lufs = -0.691 + 10 * np.log10(heard)
                step = np.clip(((lufs - GATE) / STEP).astype(int), 0, _STEPS - 1)
                np.add.at(self._count, step, 1)
                np.add.at(self._power, step, heard)
        self._rest = sound[blocks * HOP:]

    def loudness(self) -> float | None:
        """In LUFS, or None when nothing but silence was heard."""
        n = self._count.sum()
        if not n:
            return None
        # what is more than 10 units quieter than the rest is a quiet passage
        gate = self._power.sum() / n * 0.1
        used = self._count > 0
        mean_of_step = np.divide(self._power, self._count, out=np.zeros(_STEPS), where=used)
        counted = used & (mean_of_step > gate)
        if counted.any():
            mean = self._power[counted].sum() / self._count[counted].sum()
        else:
            mean = self._power.sum() / n
        return -0.691 + 10 * math.log10(mean)


def measure(pcm: bytes) -> float | None:
    meter = Meter()
    meter.feed(pcm)
    return meter.loudness()


def gain_db(loudness: float | None) -> float:
    """By how much a song of this loudness is played louder (or, below 0, softer)."""
    if loudness is None:
        return 0.0
    return max(-MOST_CUT, min(MOST_BOOST, TARGET - loudness))


class Leveller:
    """Plays one song at the level it should have.

    `known` is the song's loudness if it was measured before. Sound goes through
    process(), which measures it, turns it up or down, and keeps it from clipping.
    """

    def __init__(self, known: float | None = None, preview: float | None = None):
        self.meter = Meter()
        self.known = known
        first = known if known is not None else preview
        self.db = gain_db(first)            # what the song is played at now
        self._limit = 1.0                   # what the limiter takes off, as a factor
        self._checked = 0.0

    def process(self, pcm: bytes, volume: float = 1.0) -> bytes:
        """`volume` is what the user has set (and a fade), as a factor."""
        self.meter.feed(pcm)
        settling = self.meter.seconds < SETTLE
        if self.known is None and self.meter.seconds - self._checked >= (0.25 if settling else 1.0):
            self._checked = self.meter.seconds
            measured = self.meter.loudness()
            should = gain_db(measured)
            if settling:
                # measured on a second only, the level follows what more of the song
                # says, either way, by at most two dB a second (sound that is too loud
                # meanwhile is held under full scale by the limiter)
                if measured is not None and abs(should - self.db) >= 0.25:
                    self.db += max(-0.5, min(0.5, should - self.db))
            elif should < self.db - 1.0:
                # a song that is louder than its beginning was: down, by half a dB a second
                self.db = max(should, self.db - 0.5)
        sound = np.frombuffer(pcm, dtype="<i2").astype(np.float32)
        factor = 10 ** (self.db / 20)
        peak = float(np.abs(sound).max()) * factor if len(sound) else 0.0
        needed = min(1.0, CEILING * 32767 / peak) if peak > 0 else 1.0
        if needed < self._limit:
            self._limit = needed                                    # at once
            ramp = np.full(len(sound), needed, dtype=np.float32)
        else:
            # and back up slowly: 3 dB a second
            back = min(needed, self._limit * 10 ** (3.0 * len(sound) / 2 / RATE / 20))
            ramp = np.linspace(self._limit, back, len(sound), dtype=np.float32)
            self._limit = back
        sound = sound * (factor * volume) * ramp
        return np.clip(np.rint(sound), -32768, 32767).astype("<i2").tobytes()

    def result(self, ended: bool = False) -> float | None:
        """The song's loudness, if enough of it was heard to say: twenty seconds, or
        all of a song that is shorter (`ended`: it played to its end)."""
        enough = self.meter.seconds >= ENOUGH or (ended and self.meter.seconds >= 3.0)
        return self.meter.loudness() if enough else None
