# SPDX-License-Identifier: GPL-3.0-or-later
"""Visualisers for the TUI: spectrum analysers, a spectrogram, oscilloscopes, channel
bars and scopes, the tracker pattern and demo effects.

Each renderer turns the player's latest audio into a Rich Text of a given size. They are
drawn about twelve times a second, so each is numpy over whole rows and a frame costs
around a millisecond (tests/test_visuals.py measures them).
"""

from __future__ import annotations

import functools
import math
import time

import numpy as np
from rich.style import Style
from rich.text import Span, Text

from . import theme
from .backends.base import RATE

EIGHTHS = " ▁▂▃▄▅▆▇█"
MODES = ("spectrum", "bars", "mirror", "spectrogram", "scope", "scopes", "channels",
         "pattern", "demo")
TITLES = {"spectrum": "Spectrum", "bars": "Gradient spectrum", "mirror": "Mirrored spectrum",
          "spectrogram": "Spectrogram", "scope": "Oscilloscope",
          "scopes": "Channel scopes (approximate)", "channels": "Channels",
          "pattern": "Pattern", "demo": "Demo"}
# drawn from the sound itself, so every song has them
FROM_SOUND = ("spectrum", "bars", "mirror", "spectrogram", "scope", "demo")
# need the replayer's channel levels
FROM_CHANNELS = ("channels", "scopes")
# need the replayer's pattern (the scopes take their notes from it when it is there)
WANTS_PATTERN = ("pattern", "scopes")


def _heat(frac: float) -> str:
    """Colour for a bar at `frac` of the height: the theme's gradient, or quiet/loud/limit."""
    return theme.current.bar(frac)


def _mono(wave: bytes) -> np.ndarray:
    a = np.frombuffer(wave, dtype=np.int16).astype(np.float32) / 32768.0
    return (a[0::2] + a[1::2]) * 0.5 if a.size >= 2 else a


def _vertical_bars(heights: list[float], rows: int, width: int, peaks: list[float] | None = None,
                   gap: int = 1) -> Text:
    """Draw bars (0..1) bottom-up with eighth-block resolution, spread over `width` columns."""
    n = len(heights)
    t = Text(no_wrap=True, overflow="crop")
    if not n:
        return t
    bar_w = max(1, (width + gap) // n - gap)
    h = np.asarray(heights, dtype=np.float64)
    pk = np.asarray(peaks, dtype=np.float64) if peaks else None
    eighths = np.array(list(EIGHTHS))
    for row in range(rows):
        level_top = (rows - row) / rows      # this row covers (level_top - 1/rows, level_top]
        level_bot = (rows - row - 1) / rows
        full = h >= level_top
        part = ~full & (h > level_bot)
        idx = np.clip(((h - level_bot) * rows * 8).astype(np.int64), 0, 8)
        chars = np.where(full, "█", np.where(part, eighths[idx], " "))
        marks = np.zeros(n, dtype=bool)
        if pk is not None:
            marks = ~full & ~part & (pk > level_bot) & (pk <= level_top)
            chars = np.where(marks, "▔", chars)
        style, text_style = _heat(level_top), theme.current.text
        run, run_mark = [], bool(marks[0])
        for i in range(n):
            if bool(marks[i]) != run_mark:
                t.append("".join(run), style=text_style if run_mark else style)
                run, run_mark = [], bool(marks[i])
            run.append(chars[i] * bar_w)
            if i < n - 1:
                run.append(" " * gap)
        t.append("".join(run), style=text_style if run_mark else style)
        if row < rows - 1:
            t.append("\n")
    return t


_HANNING = np.hanning(4096)


@functools.lru_cache(maxsize=8)
def _bands(n_bars: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """For each bar the FFT bins from `first` up to `last`, and the nearest bin to its
    middle for the low bars that are narrower than one bin."""
    freqs = np.fft.rfftfreq(4096, 1 / RATE)
    edges = np.geomspace(40, 16000, n_bars + 1)
    first = np.searchsorted(freqs, edges[:-1], side="left")
    last = np.searchsorted(freqs, edges[1:], side="left")
    mid = np.sqrt(edges[:-1] * edges[1:])
    near = np.abs(freqs[None, :] - mid[:, None]).argmin(axis=1)
    return first, last, near


def _band_levels(x: np.ndarray, n_bars: int) -> np.ndarray:
    """The loudest FFT bin in each of `n_bars` log-spaced bands, 0..1 (about -4..+48 dB);
    zeros when there is too little sound to look at."""
    size = 4096
    if x.size < size:
        return np.zeros(n_bars)
    mag = np.abs(np.fft.rfft(x[-size:] * _HANNING))
    first, last, near = _bands(n_bars)
    # a band ends where the next begins, so one reduceat finds every band's loudest bin
    peak = np.maximum.reduceat(mag, np.append(first, last[-1]))[:n_bars]
    peak = np.where(last > first, peak, mag[near])
    return np.clip((20 * np.log10(peak + 1e-9) + 4) / 52, 0, 1)


class Spectrum:
    """Log-frequency bars from an FFT, with falling peak markers."""

    def __init__(self):
        self._bars: np.ndarray | None = None
        self._peaks: np.ndarray | None = None

    def render(self, wave: bytes, rows: int, width: int) -> Text:
        n_bars = max(8, min(100, (width + 1) // 3))   # 2-column bars with 1-column gaps
        levels = _band_levels(_mono(wave), n_bars)
        if self._bars is None or self._bars.size != n_bars:
            self._bars, self._peaks = levels, levels.copy()
        else:
            # rise instantly, fall smoothly; peaks hang and drop slowly
            self._bars = np.maximum(levels, self._bars * 0.75)
            self._peaks = np.maximum(self._bars, self._peaks - 0.02)
        return _vertical_bars(list(self._bars), rows, width, list(self._peaks))


class Scope:
    """An oscilloscope drawn with braille dots (2x4 dots per character)."""

    def render(self, wave: bytes, rows: int, width: int) -> Text:
        x = _mono(wave)
        dots_w, dots_h = width * 2, rows * 4
        grid = np.zeros((dots_h, dots_w), dtype=bool)
        if x.size >= dots_w:
            # start on a rising zero crossing so the trace stands still
            seg = x[-min(x.size, 2048):]
            zc = np.where((seg[:-1] <= 0) & (seg[1:] > 0))[0]
            start = zc[0] if zc.size and zc[0] + 1024 < seg.size else 0
            y = seg[start:start + 1024]
            idx = np.linspace(0, y.size - 1, dots_w).astype(int)
            ys = ((1 - (np.clip(y[idx] * 1.6, -1, 1) + 1) / 2) * (dots_h - 1)).astype(int)
            before = np.concatenate((ys[:1], ys[:-1]))
            lo, hi = np.minimum(ys, before), np.maximum(ys, before)
            lines = np.arange(dots_h)[:, None]
            grid = (lines >= lo[None, :]) & (lines <= hi[None, :])
        else:
            grid[dots_h // 2, :] = True
        cells = grid.reshape(rows, 4, width, 2)
        weights = np.array([[0x01, 0x08], [0x02, 0x10], [0x04, 0x20], [0x40, 0x80]])
        codes = (cells * weights[None, :, None, :]).sum(axis=(1, 3)) + 0x2800
        t = Text(no_wrap=True, overflow="crop")
        for r in range(rows):
            t.append(codes[r].astype("<u2").tobytes().decode("utf-16-le"),
                     style=theme.current.primary)
            if r < rows - 1:
                t.append("\n")
        return t


class Channels:
    """One bar per tracker channel (or L/R for other formats), with smooth decay."""

    def __init__(self):
        self._bars: list[float] = []

    def render(self, channels: list[float], levels: tuple[float, float], rows: int,
               width: int) -> Text:
        vals = list(channels) if channels else list(levels)
        vals = [min(1.0, v * (1.3 if channels else 1.0)) for v in vals[:32]]
        if len(self._bars) != len(vals):
            self._bars = vals
        else:
            self._bars = [max(v, b * 0.8) for v, b in zip(vals, self._bars)]
        labels = [f"{i + 1}" for i in range(len(vals))] if channels else ["L", "R"]
        gap = 2
        t = _vertical_bars(self._bars, rows - 1, width, gap=gap)
        n = len(vals)
        if n:
            bar_w = max(1, (width + gap) // n - gap)
            t.append("\n")
            t.append((" " * gap).join(lb.center(bar_w)[:bar_w] for lb in labels), style="dim")
        return t


def pattern(pattern: tuple[int, list[str]] | None, rows: int) -> Text:
    if not pattern:
        return Text("No pattern data for this format (tracker modules only).", style="dim")
    cur, lines = pattern
    t = Text(no_wrap=True, overflow="crop")
    for i, line in enumerate(lines[:rows]):
        style = "reverse bold" if i == cur else "dim" if abs(i - cur) > 2 else ""
        t.append(line, style=style)
        if i < len(lines) - 1:
            t.append("\n")
    return t


# --- drawing helpers for the newer visualisers --------------------------------------------

@functools.lru_cache(maxsize=256)
def _rgb(colour: str) -> tuple[int, int, int]:
    from rich.color import Color
    return Color.parse(colour).get_truecolor()


def _shade(colour: str, bright: float) -> str:
    """The colour at `bright` (0 = black, 1 = itself, above 1 toward white)."""
    r = _rgb(colour)
    if bright <= 1:
        mixed = [c * bright for c in r]
    else:
        mixed = [c + (255 - c) * min(1.0, bright - 1) for c in r]
    return "#%02x%02x%02x" % tuple(max(0, min(255, round(c))) for c in mixed)


def _stops(pal) -> tuple[str, ...]:
    """The theme's bar colours bottom to top, or its quiet/loud/limit colours."""
    return pal.bars or (pal.good, pal.warn, pal.bad)


@functools.lru_cache(maxsize=32)
def _intensity_ramp(pal, n: int) -> tuple[str, ...]:
    """n colours for 1..n of loudness: the theme's bar colours, the quiet end darkened so
    that soft sound fades into the background."""
    return tuple(_shade(theme._blend(_stops(pal), k / n), 0.25 + 0.75 * min(1.0, k / n * 1.6))
                 for k in range(1, n + 1))


@functools.lru_cache(maxsize=32)
def _cyclic_ramp(pal, n: int) -> tuple[str, ...]:
    """n colours going up the theme's bar colours and back down, for plasma and rasters."""
    stops = _stops(pal)
    stops = stops + stops[-2::-1] if len(stops) > 1 else stops * 2
    return tuple(theme._blend(stops, k / n) for k in range(n))


@functools.lru_cache(maxsize=64)
def _pair_table(colours: tuple[str, ...]) -> tuple[np.ndarray, list]:
    """For two pixels stacked in one cell, each 0 (nothing) or 1..n (colours[k - 1]): the
    character (code = top * (n + 1) + bottom) and its style. An upper half block with the
    top pixel in front and the bottom one behind; a lone bottom pixel is a lower half
    block, so that nothing is painted where there is nothing."""
    cols = (None,) + tuple(colours)
    chars, styles = [], []
    for top in cols:
        for bottom in cols:
            chars.append("▀" if top else "▄" if bottom else " ")
            styles.append(Style(color=top or bottom, bgcolor=bottom if top else None)
                          if top or bottom else None)
    return np.array(chars), styles


def _grid_text(chars: np.ndarray, codes: np.ndarray, styles: list) -> Text:
    """A Text from a (rows, width) array of characters and one of style numbers (an index
    into `styles`, whose entry may be None for unstyled): one span for each run of equal
    styles in a row, the runs found with numpy rather than cell by cell."""
    rows, width = codes.shape
    flat = np.ascontiguousarray(chars, dtype="<U1")
    plain = "\n".join(flat[r].tobytes().decode("utf-32-le") for r in range(rows))
    t = Text(plain, no_wrap=True, overflow="crop")
    if not rows or not width:
        return t
    change = np.ones((rows, width), dtype=bool)
    change[:, 1:] = codes[:, 1:] != codes[:, :-1]
    pos = np.flatnonzero(change)
    r, c = np.divmod(pos, width)
    end = np.where(np.append(r[1:], -1) == r, np.append(c[1:], width), width)
    base = r * (width + 1)
    t.spans = [Span(a, b, styles[k]) for a, b, k in zip((base + c).tolist(), (base + end).tolist(),
                                                        codes.ravel()[pos].tolist())
               if styles[k] is not None]
    return t


def _braille(grid: np.ndarray, rows: int, width: int) -> np.ndarray:
    """(rows * 4, width * 2) dots to a (rows, width) array of braille characters."""
    cells = grid.reshape(rows, 4, width, 2)
    weights = np.array([[0x01, 0x08], [0x02, 0x10], [0x04, 0x20], [0x40, 0x80]])
    codes = (cells * weights[None, :, None, :]).sum(axis=(1, 3)) + 0x2800
    return codes.astype("<u4").view("<U1").reshape(rows, width).copy()


class _Pulse:
    """How loud the music is now (0..1) and a beat: a jump in loudness over its recent
    average, which then dies away. For the demo effects to move with the music."""

    def __init__(self):
        self.level = 0.0
        self.beat = 0.0
        self._avg = 0.0

    def feed(self, wave: bytes) -> None:
        x = _mono(wave[-4096:])
        e = float(np.mean(x * x)) if x.size else 0.0
        self.level = min(1.0, math.sqrt(e) * 4)
        if e > 1e-5 and e > self._avg * 1.6:
            self.beat = 1.0
        else:
            self.beat *= 0.8
        self._avg = self._avg * 0.85 + e * 0.15


# --- spectra ------------------------------------------------------------------------------

class GradientSpectrum:
    """Tall, smooth one-column bars shaded bottom to top with the theme's bar colours (as
    rmpc draws them), each cell row its own colour and each top in eighths of a cell.
    `mirror` draws them up and down from the middle instead."""

    def __init__(self, mirror: bool = False):
        self.mirror = mirror
        self._bars: np.ndarray | None = None

    def _levels(self, wave: bytes, n: int) -> np.ndarray:
        levels = _band_levels(_mono(wave), n)
        if n >= 3:      # a little smoothing across the bars: hills rather than needles
            side = np.pad(levels, 1, mode="edge")
            levels = np.maximum(levels * 0.6 + 0.2 * (side[:-2] + side[2:]), levels * 0.85)
        if self._bars is None or self._bars.size != n:
            self._bars = levels
        else:           # rise quickly, fall slowly
            self._bars = np.where(levels > self._bars, self._bars * 0.3 + levels * 0.7,
                                  self._bars * 0.82 + levels * 0.18)
        return self._bars

    def render(self, wave: bytes, rows: int, width: int) -> Text:
        n = max(4, min(160, (width + 1) // 2))         # 1-column bars, 1-column gaps
        h = self._levels(wave, n)
        if not self.mirror or rows < 2:
            return _vertical_bars(list(h), rows, width)
        up = rows - rows // 2                           # rows above and below the middle
        down = rows - up
        t = _vertical_bars(list(h), up, width)
        # below, the bars hang down as a darker reflection, coarser (there are no
        # upper-eighth blocks to draw them finer)
        bar_w = max(1, (width + 1) // n - 1)
        for row in range(down):
            fill = np.clip(h * down - row, 0, 1)
            chars = np.where(fill >= 0.75, "█", np.where(fill >= 0.4, "▀",
                             np.where(fill >= 0.1, "▔", " ")))
            t.append("\n")
            t.append(" ".join(ch * bar_w for ch in chars.tolist()),
                     style=_shade(_heat((row + 1) / down), 0.55))
        return t


class Spectrogram:
    """Frequency (low at the bottom) over time (newest at the right), the colour how loud:
    two frequency bands to a character with half blocks."""

    LEVELS = 12

    def __init__(self):
        self._hist: np.ndarray | None = None

    def render(self, wave: bytes, rows: int, width: int) -> Text:
        bands = rows * 2
        col = _band_levels(_mono(wave), bands)[::-1]    # top row = highest band
        # the faintest stays background, and most music is loud: spread out the top
        col = np.clip((col - 0.3) / 0.7, 0, 1) ** 1.6
        if self._hist is None or self._hist.shape != (bands, width):
            self._hist = np.zeros((bands, width))
        self._hist[:, :-1] = self._hist[:, 1:]
        self._hist[:, -1] = col
        n = self.LEVELS
        q = np.ceil(self._hist * n).astype(np.int64)    # 0 = nothing, 1..n
        chars, styles = _pair_table(_intensity_ramp(theme.current, n))
        codes = q[0::2] * (n + 1) + q[1::2]
        return _grid_text(chars[codes], codes, styles)


# --- channel scopes -----------------------------------------------------------------------

_NOTES = {"C-": 0, "C#": 1, "D-": 2, "D#": 3, "E-": 4, "F-": 5, "F#": 6, "G-": 7, "G#": 8,
          "A-": 9, "A#": 10, "B-": 11}


def _row_notes(pattern: tuple[int, list[str]] | None) -> list[tuple[int | None, int | None]]:
    """(note number, instrument) for each channel in the pattern's current row, None where
    the row starts no note there. The cells are libopenmpt's: "C-5 01 v64 A0F"."""
    if not pattern:
        return []
    cur, lines = pattern
    if not 0 <= cur < len(lines) or "│" not in lines[cur]:
        return []
    out = []
    for cell in lines[cur].split("│")[1:]:
        note = inst = None
        name, octave = cell[:2], cell[2:3]
        if name in _NOTES and octave.isdigit():
            note = _NOTES[name] + 12 * int(octave)
        try:
            inst = int(cell[4:6])
        except ValueError:
            pass
        out.append((note, inst))
    return out


def _layout(n: int, rows: int, width: int) -> tuple[int, int]:
    """Columns and rows of a grid for n scopes, each about four times as wide as high."""
    best, best_score = (n, 1), None
    for cols in range(1, n + 1):
        grid_rows = -(-n // cols)
        if grid_rows > rows:
            continue
        w = (width - (cols - 1)) // cols
        if w < 3:
            break
        h = rows // grid_rows
        score = abs(math.log(w / (4.0 * h)))
        if best_score is None or score < best_score:
            best, best_score = (cols, grid_rows), score
    return best


class ChannelScopes:
    """One small oscilloscope per channel, like a tracker's quadrascope, in a grid.

    The replayers give each channel's loudness but not its sound, so the trace is made up:
    its height is the channel's loudness, its pitch the last note the pattern played in it
    and its shape chosen by the instrument number (sine, square, saw, triangle). An
    approximation, and the title says so."""

    def __init__(self):
        self._bars: list[float] = []
        self._notes: list[int] = []
        self._insts: list[int] = []

    def render(self, channels: list[float], pattern, rows: int, width: int,
               now: float | None = None) -> Text:
        n = min(len(channels), 32)
        if not n:
            return Text("No channel levels for this format (tracker modules only).",
                        style="dim")
        if len(self._bars) != n:
            self._bars = [0.0] * n
            self._notes = [48 + 7 * (c % 3) for c in range(n)]
            self._insts = list(range(n))
        self._bars = [max(min(1.0, v * 1.3), b * 0.8) for v, b in zip(channels, self._bars)]
        for c, (note, inst) in enumerate(_row_notes(pattern)[:n]):
            if note is not None:
                self._notes[c] = note
            if inst is not None:
                self._insts[c] = inst
        phase = ((time.monotonic() if now is None else now) * 3.0) % (2 * math.pi)

        cols, grid_rows = _layout(n, rows, width)
        w = max(1, (width - (cols - 1)) // cols)
        h = max(1, rows // grid_rows)
        shown = min(n, cols * grid_rows)
        dots_w, dots_h = w * 2, h * 4
        grid = np.zeros((rows * 4, width * 2), dtype=bool)
        x = np.linspace(0, 1, dots_w, endpoint=False)
        lines = np.arange(dots_h)[:, None]
        for c in range(shown):
            gr, gc = divmod(c, cols)
            # 1 to 6 cycles across the scope, twice as many for every octave up
            cycles = min(6.0, max(1.0, 2.0 * 2 ** ((self._notes[c] - 48) / 12)))
            ph = 2 * math.pi * cycles * x + phase * (1 + c % 3)
            shape = self._insts[c] % 4
            if shape == 0:
                y = np.sin(ph)
            elif shape == 1:
                y = np.sign(np.sin(ph)) * 0.8
            elif shape == 2:
                y = (ph / math.pi) % 2 - 1
            else:
                y = 2 * np.abs((ph / math.pi) % 2 - 1) - 1
            ys = ((1 - (y * self._bars[c] * 0.95 + 1) / 2) * (dots_h - 1)).astype(int)
            before = np.concatenate((ys[:1], ys[:-1]))
            lo, hi = np.minimum(ys, before), np.maximum(ys, before)
            top, left = gr * h * 4, gc * (w + 1) * 2
            grid[top:top + dots_h, left:left + dots_w] = (lines >= lo) & (lines <= hi)
        chars = _braille(grid, rows, width)
        # each scope coloured by how loud its channel is, its number in its corner
        styles: list = [None]
        style_of = np.zeros((rows, width), dtype=np.int64)
        label = Style(color=theme.current.text, dim=True)
        for c in range(shown):
            gr, gc = divmod(c, cols)
            r0, c0 = gr * h, gc * (w + 1)
            styles.append(Style(color=_heat(0.15 + 0.85 * self._bars[c])))
            style_of[r0:r0 + h, c0:c0 + w] = len(styles) - 1
            tag = str(c + 1)
            if len(tag) < w:
                styles.append(label)
                chars[r0, c0:c0 + len(tag)] = list(tag)
                style_of[r0, c0:c0 + len(tag)] = len(styles) - 1
        return _grid_text(chars, style_of, styles)


# --- demo effects -------------------------------------------------------------------------

class Demo:
    """Demo effects that move with the music, taking turns: copper bars, a starfield, a
    plasma and a sine scroller with the song's name. `scene` names the one shown."""

    SCENES = ("copper bars", "starfield", "plasma", "sine scroller")
    SECONDS = 20        # each scene's turn
    STARS = 160

    def __init__(self, seed: int = 1):
        self.pulse = _Pulse()
        self.scene = self.SCENES[0]
        self._start: float | None = None
        self._last: float | None = None
        self._t = 0.0
        self._scroll = 0.0
        self._rng = np.random.default_rng(seed)
        self._stars = self._new_stars(self.STARS, 0.05)

    def render(self, wave: bytes, title: str, rows: int, width: int,
               now: float | None = None, scene: str | None = None) -> Text:
        now = time.monotonic() if now is None else now
        if self._start is None:
            self._start = now
        dt = 0.08 if self._last is None else min(0.25, max(0.0, now - self._last))
        self._last = now
        self.pulse.feed(wave)
        # time runs faster when the music is loud, and jumps on a beat
        self._t += dt * (0.6 + 1.2 * self.pulse.level + 1.5 * self.pulse.beat)
        self.scene = scene or self.SCENES[int((now - self._start) // self.SECONDS)
                                          % len(self.SCENES)]
        if self.scene == "copper bars":
            return self._copper(rows, width)
        if self.scene == "starfield":
            self._fly(dt, rows, width, 0.15 + 0.8 * self.pulse.level + 1.2 * self.pulse.beat)
            return _grid_text(*self._starry(rows, width))
        if self.scene == "plasma":
            return self._plasma(rows, width)
        return self._scroller(title, rows, width, dt)

    def _copper(self, rows: int, width: int) -> Text:
        """Raster bars sliding up and down, wider apart and brighter as the music is loud."""
        px = rows * 2
        colours = _cyclic_ramp(theme.current, 8)[::2]       # four bars
        shades = 6
        flash = round(self.pulse.beat * 3) / 10         # a few steps, so few colours in all
        ramp = tuple(_shade(c, 0.3 + 0.7 * s / (shades - 1) + flash)
                     for c in colours for s in range(shades))
        line = np.zeros(px, dtype=np.int64)             # 0 = background
        y = np.arange(px)
        half = max(1.5, px / 11)
        swing = (px - 1) / 2 * (0.55 + 0.45 * self.pulse.level)
        for b in range(len(colours)):
            centre = (px - 1) / 2 + swing * math.sin(self._t * 1.7 + b * 0.9)
            d = np.abs(y - centre) / half
            s = np.round((1 - np.minimum(d, 1)) * (shades - 1)).astype(np.int64)
            line = np.where(d < 1, 1 + b * shades + s, line)
        chars, styles = _pair_table(ramp)
        n = len(ramp)
        codes = np.repeat((line[0::2] * (n + 1) + line[1::2])[:, None], width, axis=1)
        return _grid_text(chars[codes], codes, styles)

    def _new_stars(self, k: int, near: float) -> np.ndarray:
        r = self._rng
        return np.column_stack((r.uniform(-1, 1, k), r.uniform(-1, 1, k), r.uniform(near, 1, k)))

    def _fly(self, dt: float, rows: int, width: int, speed: float) -> None:
        """Stars toward you; those gone past or off the screen start again far away."""
        st = self._stars
        st[:, 2] -= dt * speed
        z = np.maximum(st[:, 2], 1e-3)
        sx = st[:, 0] / z * (width / 2) + width / 2
        sy = st[:, 1] / z * (rows / 2) + rows / 2
        gone = (st[:, 2] <= 0.02) | (sx < 0) | (sx >= width) | (sy < 0) | (sy >= rows)
        if gone.any():
            st[gone] = self._new_stars(int(gone.sum()), 0.6)

    def _starry(self, rows: int, width: int) -> tuple[np.ndarray, np.ndarray, list]:
        st = self._stars
        sx = (st[:, 0] / st[:, 2] * (width / 2) + width / 2).astype(int)
        sy = (st[:, 1] / st[:, 2] * (rows / 2) + rows / 2).astype(int)
        far_first = np.argsort(-st[:, 2])
        far_first = far_first[((sx >= 0) & (sx < width) & (sy >= 0) & (sy < rows))[far_first]]
        depth = np.clip((1 - st[:, 2]) * 4, 0, 3).astype(int)     # 0 far .. 3 near
        chars = np.full((rows, width), " ", dtype="<U1")
        codes = np.zeros((rows, width), dtype=np.int64)
        chars[sy[far_first], sx[far_first]] = np.array(["·", "∙", "+", "*"])[depth[far_first]]
        codes[sy[far_first], sx[far_first]] = 1 + depth[far_first]
        styles = [None] + [Style(color=_shade(theme.current.text, b))
                           for b in (0.45, 0.7, 1.0, 1.3)]
        return chars, codes, styles

    def _plasma(self, rows: int, width: int) -> Text:
        """The old sum of sines in the theme's colours, brighter as the music is loud."""
        n = 16
        t = self._t
        x = np.arange(width)[None, :] * 0.11
        y = np.arange(rows * 2)[:, None] * 0.22
        v = (np.sin(x + t) + np.sin(y * 1.3 - t * 1.1) + np.sin((x + y) * 0.7 + t * 0.6)
             + np.sin(np.sqrt((x - 4 - 3 * math.sin(t * 0.3)) ** 2 + (y - 2) ** 2) * 1.2 - t))
        q = (np.floor((v / 8 + 0.5 + t * 0.05) * n).astype(np.int64) % n) + 1
        bright = round((0.6 + 0.4 * self.pulse.level + 0.3 * self.pulse.beat) * 5) / 5
        colours = tuple(_shade(c, bright) for c in _cyclic_ramp(theme.current, n))
        chars, styles = _pair_table(colours)
        codes = q[0::2] * (n + 1) + q[1::2]
        return _grid_text(chars[codes], codes, styles)

    def _scroller(self, title: str, rows: int, width: int, dt: float) -> Text:
        """The song's name running along a sine wave over slow stars."""
        self._fly(dt, rows, width, 0.08 + 0.3 * self.pulse.level)
        chars, codes, styles = self._starry(rows, width)
        msg = f"{title or 'ByteBandit Music Player'}   ·   "
        self._scroll += dt * (10 + 14 * self.pulse.level)
        xs = np.arange(width)
        letters = np.array(list(msg))[(xs + int(self._scroll)) % len(msg)]
        amp = (rows - 1) / 2 * (0.45 + 0.55 * self.pulse.level)
        ys = np.clip(np.round((rows - 1) / 2 + amp * np.sin(xs * 0.07 + self._t * 2.2)),
                     0, rows - 1).astype(int)
        colours = _cyclic_ramp(theme.current, 24)
        first = len(styles)
        styles = styles + [Style(color=c, bold=True) for c in colours]
        hue = (xs // 2 + int(self._t * 8)) % len(colours)
        show = letters != " "
        chars[ys[show], xs[show]] = letters[show]
        codes[ys[show], xs[show]] = first + hue[show]
        return _grid_text(chars, codes, styles)
