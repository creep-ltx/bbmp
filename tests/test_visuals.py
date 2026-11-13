# SPDX-License-Identifier: GPL-3.0-or-later
"""The visualisers draw a Text of the size asked for, whatever the sound, and cheaply."""

import time

import numpy as np
import pytest

from bbmp import theme, visuals
from bbmp.backends.base import RATE

SIZES = ((7, 78), (13, 200), (1, 10), (3, 20))


def _wave(seconds: float = 0.1, freq: float = 440.0, loud: float = 0.5) -> bytes:
    t = np.arange(int(RATE * seconds)) / RATE
    x = loud * np.sin(2 * np.pi * freq * t) + 0.1 * np.sin(2 * np.pi * 3 * freq * t)
    x = (x * 32767).astype("<i2")
    return np.column_stack((x, x)).ravel().tobytes()


PATTERN = (1, ["00│C-5 01 .. ...│... .. .. ...", "01│E-6 03 .. A20│G#4 12 .. ...",
               "02│... .. .. ...│... .. .. ..."])
CHANNELS = [0.8, 0.3, 0.0, 1.0]


def _channel(wave: bytes) -> bytes:
    """A channel's own sound as the replayer gives it (Status.waves): mono, 512 samples."""
    return np.frombuffer(wave, dtype="<i2")[0::2][:512].tobytes()


def _renderers():
    """Each new visualiser as a function (wave, rows, width, channels, pattern, now)."""
    bars, mirror = visuals.GradientSpectrum(), visuals.GradientSpectrum(mirror=True)
    gram, scopes = visuals.Spectrogram(), visuals.ChannelScopes()
    out = {"bars": lambda w, r, c, ch, p, t: bars.render(w, r, c),
           "mirror": lambda w, r, c, ch, p, t: mirror.render(w, r, c),
           "spectrogram": lambda w, r, c, ch, p, t: gram.render(w, r, c),
           "scopes": lambda w, r, c, ch, p, t: scopes.render([_channel(w)] * len(ch), ch, r, c)}
    for scene in visuals.Demo.SCENES:
        demo = visuals.Demo()
        out[f"demo {scene}"] = (lambda d, s: lambda w, r, c, ch, p, t:
                                d.render(w, "Space Debris by Captain", r, c, now=t, scene=s))(demo, scene)
    return out


def _check(text, rows, width):
    lines = text.plain.split("\n")
    assert len(lines) == rows
    assert all(len(line) <= width for line in lines)


@pytest.mark.parametrize("rows,width", SIZES)
def test_every_new_visualiser_draws_the_size_asked_for(rows, width):
    for name, draw in _renderers().items():
        for k in range(3):
            text = draw(_wave(), rows, width, CHANNELS, PATTERN, 10 + k / 12)
            _check(text, rows, width)


@pytest.mark.parametrize("rows,width", SIZES)
def test_silence_and_no_data_draw_without_trouble(rows, width):
    for name, draw in _renderers().items():
        for wave in (b"", b"\0" * 400, bytes(4096 * 4)):
            text = draw(wave, rows, width, CHANNELS, None, 0.0)
            if name != "scopes":
                _check(text, rows, width)
    # no channel sound: the scopes say so
    assert "No channel sound" in visuals.ChannelScopes().render(None, [], 7, 78).plain
    assert "Getting" in visuals.ChannelScopes().render(None, CHANNELS, 7, 78).plain


def test_the_spectrogram_scrolls_left_with_the_newest_sound_at_the_right():
    gram = visuals.Spectrogram()
    gram.render(bytes(4096 * 4), 4, 30)
    text = gram.render(_wave(loud=0.9), 4, 30)
    lines = text.plain.split("\n")
    assert any(line[-1] != " " for line in lines)          # the newest column has sound
    assert all(set(line[:-1]) == {" "} for line in lines)  # the older ones were silence


def test_each_scope_draws_its_own_channels_sound():
    loud, silent = _channel(_wave(loud=0.9, freq=220)), bytes(1024)
    text = visuals.ChannelScopes().render([loud, silent], [0.9, 0.0], 8, 40)
    lines = text.plain.split("\n")
    left = [line[:19] for line in lines]
    right = [line[21:] for line in lines]
    # the loud one goes up and down through every row; the silent one is a flat line
    assert sum(1 for line in left if line[1:].strip(" ⠀")) >= 6
    assert sum(1 for line in right if line[1:].strip(" ⠀")) == 1
    assert "1" in text.plain and "2" in text.plain                  # each scope numbered


def test_a_held_note_stands_still_in_its_scope():
    # the same tone caught at different moments is drawn the same: each trace starts
    # where the sound rises through zero
    whole = np.frombuffer(_wave(seconds=0.2, freq=375), dtype="<i2")[0::2]
    drawn = {visuals.ChannelScopes().render([whole[k:k + 512].tobytes()], [0.5], 6, 30).plain
             for k in (0, 17, 40, 333)}
    assert len(drawn) == 1


def test_the_pattern_view_has_the_row_heard_in_the_middle():
    lines = [f"{r:02X}│C-5 01 .. ..." for r in range(64)]
    for cur in (0, 5, 63):
        text = visuals.pattern((cur, lines), 9)
        shown = text.plain.split("\n")
        assert len(shown) == 9 and shown[4] == lines[cur]
        assert any(span.start == text.plain.index(lines[cur]) and "reverse" in str(span.style)
                   for span in text.spans)


def test_the_scope_grid_fits_many_channels_in_a_small_box():
    assert visuals._layout(4, 7, 78) == (4, 1)
    cols, grid_rows = visuals._layout(32, 7, 78)
    assert grid_rows <= 7 and cols * grid_rows >= 32 or cols * 3 <= 78
    _check(visuals.ChannelScopes().render([_channel(_wave())] * 32, [0.5] * 32, 7, 78), 7, 78)


def test_the_demo_takes_turns_and_moves_with_the_music():
    demo = visuals.Demo()
    seen = set()
    for k in range(0, 100, 5):
        demo.render(_wave(), "x", 7, 40, now=float(k))
        seen.add(demo.scene)
    assert seen == set(visuals.Demo.SCENES)
    loud, quiet = visuals.Demo(), visuals.Demo()
    for k in range(10):
        loud.render(_wave(loud=0.9), "x", 7, 40, now=k / 12, scene="plasma")
        quiet.render(bytes(4096 * 4), "x", 7, 40, now=k / 12, scene="plasma")
    assert loud._t > quiet._t * 1.5                         # louder music: faster effects
    text = visuals.Demo().render(_wave(), "Hello", 7, 40, now=0, scene="sine scroller")
    assert set("Hello") <= set(text.plain)


def test_the_new_visualisers_follow_the_theme():
    try:
        theme.use({"modland-bars": "#ff0000,#0000ff"})
        text = visuals.Spectrogram().render(_wave(loud=0.9), 7, 40)
        colours = {str(s.style.color.triplet.hex) for s in text.spans if s.style.color}
        assert any(c.startswith("#") for c in colours)
        assert all(int(c[3:5], 16) == 0 for c in colours)     # only red to blue, no green
    finally:
        theme.use({})


def test_the_vectorised_bands_match_the_loop():
    x = np.random.default_rng(1).normal(0, 0.1, 5000).astype(np.float32)
    for n in (8, 26, 100):
        first, last, near = visuals._bands(n)
        mag = np.abs(np.fft.rfft(x[-4096:] * visuals._HANNING))
        ref = np.array([mag[first[i]:last[i]].max() if last[i] > first[i] else mag[near[i]]
                        for i in range(n)])
        ref = np.clip((20 * np.log10(ref + 1e-9) + 4) / 52, 0, 1)
        assert np.allclose(ref, visuals._band_levels(x, n))


def test_a_frame_costs_a_few_milliseconds_at_most():
    """At 200x13 each new visualiser draws a frame in about 0.1-0.4 ms (measured 3 October
    2026); the limit here is generous, for slow test machines."""
    wave = _wave(seconds=0.2)
    for name, draw in _renderers().items():
        draw(wave, 13, 200, CHANNELS, PATTERN, 0.0)
        start = time.perf_counter()
        for k in range(20):
            draw(wave, 13, 200, CHANNELS, PATTERN, k / 12)
        ms = (time.perf_counter() - start) / 20 * 1000
        assert ms < 5, (name, ms)
