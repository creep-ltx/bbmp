#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record the short ad: the icon's level bars jumping to a keygen tune, then the player
at work with a tune from each machine and a banner for each feature, the numbers, and
how to start it.

    PYTHONPATH=src python3 tools/record_ad.py ad.mp4 [--share ad-share.mp4] [--keep]

The machinery is the showcase's (record_showcase.py, record_overview.py): the real app
runs headlessly and plays real songs, the sound goes into the video, never to the
speakers, and every frame is the app's own screenshot with a caption band under it.
What is new here: the picture is 16:9 (1920 x 1080, 24 frames a second), the songs
cross-fade (the end of each song is rendered again, offline, from where the player left
it, and faded out under the next), the cards are drawn over the app, and the loudness
is the player's own (BS.1770, every song at the same level), checked at the end.
Settings, favorites and saved music live in a temporary folder. Needs rsvg-convert and
ffmpeg.
"""

from __future__ import annotations

import asyncio
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
import wave
from dataclasses import dataclass
from pathlib import Path

import numpy as np

WORK = Path(tempfile.mkdtemp(prefix="bbmp-ad-", dir=Path.home() / ".cache/bbmp/work"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import record_showcase  # noqa: E402
from record_demo import Recorder  # noqa: E402
from record_overview import (BAND, BLUE, INK, MONO, MUTED, NAVY, ORANGE, SANS,  # noqa: E402
                             Caption, Frame, Spectrum, ease, esc)
from record_showcase import GREEN, KICKERS, draw_band  # noqa: E402
from screenshots import box_font  # noqa: E402

# importing the showcase made a folder of its own for saved music: this one is used
shutil.rmtree(record_showcase.WORK, ignore_errors=True)
os.environ["BBMP_KEPT"] = str(WORK / "kept")

from bbmp import loudness  # noqa: E402
from bbmp.backends import open_song  # noqa: E402
from bbmp.backends.base import FRAME_BYTES, RATE  # noqa: E402

FPS = 24                  # of the video; the app is photographed at CAPTURE a second
CAPTURE = 12
SIZE = (150, 38)          # a terminal as wide as a 16:9 picture wants
SCALE = 1.3               # the caption band, larger than the showcase's
INTRO, OUTRO, FADE = 9.5, 7.5, 0.8
NUMBERS = 8.5
TAIL = 2.2                # seconds a song goes on under the next one, fading
SEED = 40                 # radio picks the same song every time
LED_ON = ("#4ebf71",) * 5 + ("#ffa62b",) * 3 + ("#e0566f",) * 2   # the icon, bottom up
LED_OFF = "#262c37"

KICKERS.update({"PLATFORM": ORANGE, "SEARCH": BLUE, "RADIO": GREEN})

SONGS = {
    "superhero": "modland:Fasttracker 2/Rez/coop-Kenet/unreeeal superhero 3.xm",
    "debris": "modland:Protracker/Captain/space_debris.mod",
    "monty": "modland:HVSC/MUSICIANS/H/Hubbard_Rob/Monty_on_the_Run.sid",
    "gostraight": "modland:Video Game Music/Sega Megadrive/YK-2/Streets Of Rage 2/02 go straight.vgz",
    "wings": "sndh:Mad_Max/Games/Wings_Of_Death.sndh",
    "megaman": "modland:Nintendo Sound Format/C. Manami/mega man 2.nsf",
    "turrican": "modland:TFMX/Chris Huelsbeck/mdat.turrican 2 level 1-desert",
    "dubmood": "modland:Fasttracker 2/Dubmood/fff keygen intro.xm",
}

PLAYED_BY = {
    "openmpt": "played by libopenmpt",
    "uade": "played by the Amiga's own replay routine, emulated (UADE)",
    "sidplayfp": "played on an emulated SID chip (libsidplayfp)",
    "sndh": "the ST's own 68000 code and YM chip, emulated (psgplay)",
    "ym": "played by StSound",
    "psgplay": "the ST's own 68000 code and YM chip, emulated (psgplay)",
    "sc68": "the ST's own 68000 code and YM chip, emulated (sc68)",
    "gme": "played by Game Music Emu",
    "zxtune": "the AY chip emulated (ZXTune)",
}


def played_by(backend: str) -> str:
    return PLAYED_BY.get(backend, f"played by {record_showcase.replayer(backend)}")


# ---- the sound: which song each piece of it is ---------------------------------------

@dataclass
class Piece:
    """A stretch of the recording that one song (and subsong) made."""
    key: str
    subsong: int
    begin: int            # frames into the recording
    end: int
    played: int           # frames of the song the player had played by `end`
    gain: float = 1.0     # what the player played it at: the volume and its loudness level


class MarkingRecorder(Recorder):
    """The showcase's recorder, which also notes which song every piece of sound is
    from and how far into it, so that its end can be rendered again for a cross-fade."""

    def __init__(self, t0: float):
        super().__init__(t0)
        self.player = None
        self.pieces: list[Piece] = []

    def write(self, pcm: bytes) -> None:
        super().write(pcm)            # (called in the player's thread)
        end = len(self.pcm) // FRAME_BYTES
        st = self.player._status if self.player else None
        if st is None or st.song is None:
            return
        key, sub = st.song.key, st.info.subsong if st.info else 0
        played = self.player._played
        gain = st.volume / 100 * 10 ** (st.level_db / 20)
        last = self.pieces[-1] if self.pieces else None
        if last and (last.key, last.subsong) == (key, sub) and played >= last.played:
            last.end, last.played, last.gain = end, played, gain
        else:
            self.pieces.append(Piece(key, sub, end - len(pcm) // FRAME_BYTES, end, played, gain))


def render_tail(index, fetcher, piece: Piece, before: int, after: int) -> np.ndarray:
    """The song of `piece` rendered offline from `before` frames ahead of where the
    player left it to `after` frames past it, as float stereo."""
    song = index.resolve(piece.key)
    file = fetcher.get(piece.key, index.companions(piece.key))
    dec = open_song(file, song.format, index.lengths(piece.key), local=False)
    try:
        if piece.subsong != dec.info.subsong:
            dec.select_subsong(piece.subsong)
        skip, want = max(0, piece.played - before), before + after
        got = bytearray()
        while skip > 0:
            pcm = dec.read(min(skip, 8192))
            if not pcm:
                break
            skip -= len(pcm) // FRAME_BYTES
        if skip < 0:                     # read past: keep the part beyond `skip`
            got += pcm[len(pcm) + skip * FRAME_BYTES:]
        while len(got) < want * FRAME_BYTES:
            pcm = dec.read(8192)
            if not pcm:
                break
            got += pcm
    finally:
        dec.close()
    a = np.frombuffer(bytes(got[:want * FRAME_BYTES]), dtype=np.int16)
    a = a.astype(np.float32).reshape(-1, 2) / 32768
    return np.pad(a, ((0, want - len(a)), (0, 0)))


def soundtrack(rec: MarkingRecorder, first: int, length: int, index, fetcher) -> np.ndarray:
    """The recording from `first` for `length` frames, each song cross-faded into the
    next, the whole at -16 LUFS (the player has already made every song as loud as
    the others), faded in and out."""
    raw = bytes(rec.pcm[first * FRAME_BYTES:(first + length) * FRAME_BYTES])
    a = np.frombuffer(raw, dtype=np.int16).astype(np.float32).reshape(-1, 2) / 32768
    a = np.pad(a, ((0, length - len(a)), (0, 0)))
    pieces = [p for p in rec.pieces if p.end - p.begin > RATE // 4]
    # The player levels a song by its first 15 seconds; one that grows louder after its
    # intro stays louder. Even out what is left between the songs, by up to 6 dB.
    corr, levels = {}, {}
    for p in pieces:
        lo, hi = max(0, p.begin - first), min(length, p.end - first)
        if hi - lo > 3 * RATE:
            levels[id(p)] = loudness.measure((np.clip(a[lo:hi], -1, 1) * 32767)
                                             .astype(np.int16).tobytes())
    known = [v for v in levels.values() if v is not None]
    gain_curve = np.ones(length, dtype=np.float32)
    for p in pieces:
        lufs = levels.get(id(p))
        db = float(np.clip(np.median(known) - lufs, -6, 6)) if lufs is not None else 0.0
        corr[id(p)] = 10 ** (db / 20) if abs(db) > 1 else 1.0
        lo, hi = max(0, p.begin - first), min(length, p.end - first)
        gain_curve[lo:hi] = corr[id(p)]
        if abs(db) > 1:
            print(f"  {p.key.split('/')[-1]}: {lufs:.1f} LUFS, {db:+.1f} dB to match the rest")
    ramp = RATE // 5                     # a moving average, so the steps are not heard
    c = np.cumsum(np.pad(gain_curve, (ramp, ramp), mode="edge"), dtype=np.float64)
    gain_curve = ((c[2 * ramp:] - c[:-2 * ramp]) / (2 * ramp))[:length].astype(np.float32)
    a *= gain_curve[:, None]
    tail, look = int(TAIL * RATE), RATE
    for p, nxt in zip(pieces, pieces[1:]):
        end = p.end - first
        if not 0 < end < length:
            continue
        try:
            more = render_tail(index, fetcher, p, look, tail)
        except Exception as e:           # a tail is a nicety; the cut stays
            print(f"  no tail for {p.key}: {e}")
            continue
        gain = p.gain * corr[id(p)]      # as loud as the player made it
        out = more[look:] * gain
        n = min(len(out), length - end)
        fade = (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, n, dtype=np.float32)))[:, None]
        a[end:end + n] += out[:n] * fade
        # the next song comes in over a moment rather than at once
        start = nxt.begin - first
        if 0 <= start < length:
            m = min(RATE // 6, length - start)
            a[start:start + m] *= np.linspace(0.3, 1, m, dtype=np.float32)[:, None]
        print(f"  cross-fade at {end / RATE:6.1f} s: {p.key.split('/')[-1]} "
              f"(tail gain {gain:.2f})")
    lufs = loudness.measure((np.clip(a, -1, 1) * 32767).astype(np.int16).tobytes())
    if lufs is not None:
        a *= 10 ** ((-16.0 - lufs) / 20)
    fade_in, fade_out = RATE // 3, int(3.0 * RATE)
    a[:fade_in] *= np.linspace(0, 1, fade_in, dtype=np.float32)[:, None]
    a[-fade_out:] *= np.linspace(1, 0, fade_out, dtype=np.float32)[:, None]
    return np.tanh(a * 1.05) / np.tanh(1.05)        # a soft limit instead of clipping


# ---- the tour ----------------------------------------------------------------------

@dataclass
class Marks:
    numbers: tuple[float, float] = (1e9, 1e9)


async def tour(app, pilot, rec: MarkingRecorder, frames: list[Frame],
               captions: list[Caption], marks: Marks) -> None:
    ix = app.index
    now = lambda: time.monotonic() - rec.t0   # noqa: E731

    async def capture():
        while True:
            frames.append(Frame(now(), app.export_screenshot(), app.query_one("#visual").mode))
            await asyncio.sleep(1 / CAPTURE)

    def cap(kicker: str, title: str, line: str) -> None:
        if captions:
            captions[-1].end = min(captions[-1].end, now())
        captions.append(Caption(now(), kicker, title, line))

    def uncap() -> None:
        if captions:
            captions[-1].end = min(captions[-1].end, now())

    wait = pilot.pause

    async def visual(mode: str | None) -> None:
        vis = app.query_one("#visual")
        for _ in range(6):
            if vis.mode == mode:
                return
            await pilot.press("v")

    async def type_text(text: str) -> None:
        for ch in text:
            await pilot.press({" ": "space", ":": "colon", "-": "minus"}.get(ch, ch))
            await pilot.pause(0.03)

    async def playing(key: str | None = None, before=None):
        """Wait for the song (any song but `before` when `key` is None); gives its status."""
        for _ in range(400):
            st = app.player.status()
            if st.state == "playing" and st.song and (
                    st.song.key == key if key else st.song is not before):
                return st
            await pilot.pause(0.1)
        raise RuntimeError(f"never started: {key}")

    async def highlight(list_id: str, option_id: str, then_enter: bool = True) -> None:
        ol = app.query_one(list_id)
        for _ in range(50):
            ids = [ol.get_option_at_index(i).id for i in range(ol.option_count)]
            if option_id in ids:
                break
            await pilot.pause(0.1)
        ol.highlighted = ids.index(option_id)
        await pilot.pause(0.45)
        if then_enter:
            await pilot.press("enter")
            await pilot.pause(0.4)

    async def to_row(table_id: str, key: str, steps: int = 3) -> None:
        table = app.query_one(table_id)
        for _ in range(50):
            rows = [i for i, s in enumerate(table.songs) if s.key == key]
            if rows:
                break
            await pilot.pause(0.1)
        row = rows[0]
        table.focus()
        if row > steps:
            table.move_cursor(row=row - steps)
        for _ in range(min(row, steps)):
            await pilot.press("down")
            await pilot.pause(0.1)
        await pilot.pause(0.3)

    async def play_search(query: str, key: str, vis: str | None):
        await pilot.press("slash")
        app.query_one("#q").value = ""
        await type_text(query)
        await pilot.pause(0.5)
        await pilot.press("enter")
        await pilot.pause(0.4)
        await to_row("#search-songs", key)
        await pilot.press("enter")
        st = await playing(key)
        await visual(vis)
        return st

    async def pick(option_id: str) -> None:
        await pilot.pause(0.3)
        ol = app.screen.query_one("#pick")
        ol.highlighted = next(i for i in range(ol.option_count)
                              if ol.get_option_at_index(i).id == option_id)
        await pilot.pause(0.45)
        await pilot.press("enter")
        await pilot.pause(0.35)

    async def collection(name: str) -> None:
        for _ in range(8):
            if app._collection == name:
                return
            await pilot.press("c")
            await pilot.pause(0.35)

    # The title card covers the screen while the first song starts.
    await visual("pattern")
    app.player.play_list([ix.resolve(SONGS["superhero"])])
    st = await playing(SONGS["superhero"])
    rec.player = app.player              # made when the app started
    ix.random_song(view=("platform", "zx"))   # the radio's list, found before the camera rolls
    shooter = asyncio.create_task(capture())
    try:
        await wait(INTRO)
        cap("KEYGEN", "Rez & Kenet · Unreeeal Superhero 3",
            f"The keygen tune of all keygen tunes, 1996 · Fasttracker 2 · {played_by(st.backend)}")
        await wait(4.5)

        cap("FEATURE", "Six collections, one player",
            "Modland, HVSC, Keygen Music, ASMA, ZX-Art and SNDH · c shows one, or all")
        await collection("keygenmusic")
        app.query_one("#formats").focus()
        await highlight("#formats", "Fasttracker 2")
        await highlight("#artists", "Razor1911", then_enter=False)
        await wait(0.8)
        await collection("")

        # ---- Amiga, by platform ----
        cap("FEATURE", "Browse by platform",
            "b: all the music, machine by machine · Amiga, C64, Atari ST, Mega Drive, NES...")
        await pilot.press("b")
        await pick("platform")
        await pick("amiga")
        await wait(0.6)
        await highlight("#formats", "modland\x1fProtracker")
        await highlight("#artists", "Captain")
        await to_row("#browse-songs", SONGS["debris"], steps=4)
        await pilot.press("enter")
        st = await playing(SONGS["debris"])
        await visual("pattern")
        cap("PLATFORM", "Amiga · ProTracker",
            f"Captain – Space Debris, 1991 · four channels of 8-bit samples · {played_by(st.backend)}")
        await wait(4.5)
        cap("FEATURE", "See the music",
            "v: the tracker pattern, an oscilloscope, a spectrum analyser, the channel bars")
        for mode in ("scope", "spectrum", "channels"):
            await visual(mode)
            await wait(1.0)

        # ---- C64, by sound chip ----
        cap("FEATURE", "Browse by sound chip",
            "SID, Paula, YM2612, AY, OPL... down to the SID 6581 or 8580, as each tune says")
        await pilot.press("b")
        await pick("chip")
        await pick("sid6581")
        await wait(0.6)
        await highlight("#formats", "modland\x1fHVSC")
        await highlight("#artists", "Rob Hubbard")
        await to_row("#browse-songs", SONGS["monty"], steps=4)
        await pilot.press("enter")
        st = await playing(SONGS["monty"])
        await visual("scope")
        cap("PLATFORM", "Commodore 64 · SID 6581",
            f"Rob Hubbard – Monty on the Run, 1985 · {played_by(st.backend)}")
        await wait(6)
        app._set_view("")            # back to all the music, by collection
        await wait(0.3)

        # ---- Mega Drive, by searching for its chip ----
        cap("SEARCH", "Search by chip or platform",
            "chip:ym2612 · platform:atarist · artist: · format: · results as you type")
        st = await play_search("chip:ym2612 go straight", SONGS["gostraight"], "spectrum")
        cap("PLATFORM", "Sega Mega Drive · YM2612",
            f"Yuzo Koshiro – Streets of Rage 2: Go Straight, 1992 · {played_by(st.backend)}")
        await wait(6)

        # ---- Atari ST ----
        st = await play_search("platform:atarist mad max wings", SONGS["wings"], "scope")
        cap("PLATFORM", "Atari ST · YM2149",
            f"Jochen Hippel (Mad Max) – Wings of Death, 1990 · from SNDH · {played_by(st.backend)}")
        await wait(6)

        # ---- NES, and subsongs ----
        st = await play_search("mega man 2", SONGS["megaman"], "spectrum")
        cap("PLATFORM", "Nintendo NES · 2A03",
            f"Takashi Tateishi – Mega Man 2, 1988 · one file, every song of the game · "
            f"] steps through them · {played_by(st.backend)}")
        await wait(6)

        # ---- Amiga TFMX, and the Info tab ----
        st = await play_search("turrican 2 desert", SONGS["turrican"], "scope")
        cap("PLATFORM", "Amiga · TFMX",
            f"Chris Hülsbeck – Turrican II: The Desert, 1991 · {played_by(st.backend)}")
        await wait(4.5)
        cap("FEATURE", "Who plays it, and how well",
            "i: the Info tab · what the file says, which replayer plays the format, how well it is checked")
        await pilot.press("i")
        await wait(3.5)

        # ---- ZX Spectrum, by radio ----
        await pilot.press("1")
        await wait(0.3)
        cap("RADIO", "Radio from any view",
            "Ctrl+R: endless random songs from what is on show · here, all of the ZX Spectrum")
        await pilot.press("b")
        await pick("platform")
        await pick("zx")
        random.seed(SEED)
        before = app.player.status().song
        await pilot.press("ctrl+r")
        st = await playing(before=before)
        await visual("spectrum")
        await wait(1.5)
        song = st.song
        artist = song.artist if song.artist and song.artist != "- unknown" else "unknown"
        cap("PLATFORM", "ZX Spectrum · AY-3-8912",
            f"{artist} – {song.title} · {song.format.removeprefix('Spectrum - ')} · "
            f"picked by the radio · {played_by(st.backend)}")
        await wait(5)

        # ---- keeping things ----
        cap("FEATURE", "Favorites and playlists",
            "f marks a favorite · l adds to a playlist · a queues it · all saved as you go")
        await pilot.press("f")
        await wait(0.8)
        await pilot.press("l")
        await wait(1.6)
        await pilot.press("escape")
        await wait(0.3)
        await pilot.press("4")
        await wait(1.3)
        await pilot.press("1")
        await wait(0.3)
        cap("FEATURE", "Keep it for offline",
            "s keeps a song, an album, an artist or a whole collection on this computer")
        await pilot.press("s")
        await wait(2.6)
        await pilot.press("escape")
        await wait(0.3)
        cap("FEATURE", "Export to FLAC or MP3",
            "e writes FLAC, MP3, Ogg or WAV files, at the same even loudness")
        await pilot.press("e")
        await wait(2.6)
        await pilot.press("escape")
        await wait(0.3)

        # ---- the last song: under the loudness banner, the numbers and the end ----
        app._set_view("")            # back to all the music, by collection
        await wait(0.3)
        st = await play_search("dubmood fff keygen", SONGS["dubmood"], "pattern")
        cap("FEATURE", "Every song at the same loudness",
            "Measured as broadcasters do (BS.1770): set the volume once, for any machine")
        await wait(4.5)
        uncap()
        marks.numbers = (now(), now() + NUMBERS)
        await wait(NUMBERS + OUTRO)
    finally:
        shooter.cancel()


# ---- drawing ---------------------------------------------------------------------

class Meter:
    """The icon's six columns of lights, one to a band of the sound (bass to treble),
    each scaled to how loud that band gets in the whole video, with peaks that hold."""

    BANDS = (40, 160, 450, 1100, 2600, 6000, 16000)     # Hz

    def __init__(self, audio: np.ndarray):
        self.mono = audio.mean(axis=1)
        freqs = np.fft.rfftfreq(4096, 1 / RATE)
        self.bins = [(freqs >= lo) & (freqs < hi) for lo, hi in zip(self.BANDS, self.BANDS[1:])]
        self.window = np.hanning(4096)
        times = np.arange(0, len(self.mono) / RATE, 0.1)
        every = np.array([self._raw(t) for t in times])
        loud = every.max(axis=1) > -60
        # the title card's song is scaled on its own, so that its bars use all ten lights
        self.scales = [(np.percentile(every[loud & which], 12, axis=0),
                        np.percentile(every[loud & which], 97, axis=0))
                       for which in (times < INTRO, times >= 0)]
        self.level = np.zeros(6)
        self.peaks = np.zeros(6)
        self.held = np.zeros(6)

    def _raw(self, t: float) -> np.ndarray:
        end = max(4096, min(len(self.mono), int(t * RATE)))
        power = np.abs(np.fft.rfft(self.mono[end - 4096:end] * self.window)) ** 2
        return np.array([10 * np.log10(power[b].sum() + 1e-12) for b in self.bins])

    def lit(self, t: float) -> tuple[np.ndarray, np.ndarray]:
        lo, hi = self.scales[0 if t < INTRO else 1]
        v = np.clip((self._raw(t) - lo) / np.maximum(hi - lo, 3), 0, 1)
        self.level = np.maximum(v, self.level * 0.72)        # quick up, slower down
        n = np.clip(np.round(1 + self.level * 9), 1, 10)
        for i in range(6):
            if n[i] >= self.peaks[i]:
                self.peaks[i], self.held[i] = n[i], 9
            elif self.held[i] > 0:
                self.held[i] -= 1
            else:
                self.peaks[i] = max(n[i], self.peaks[i] - 0.5)
        return n, np.ceil(self.peaks)


def icon(x: float, y: float, size: float, lit: np.ndarray, peaks: np.ndarray) -> str:
    """The player's icon (packaging/flatpak/io.github.bytebandit.bbmp.svg), its columns
    lit as the music says."""
    s = size / 256
    out = [f'<g transform="translate({x:.1f} {y:.1f}) scale({s:.4f})">'
           '<rect x="8" y="8" width="240" height="240" rx="48" fill="url(#iconbg)" '
           'stroke="#343b48" stroke-width="3"/>']
    for c in range(6):
        for r in range(10):
            on = r < lit[c] or r == peaks[c] - 1
            out.append(f'<rect x="{46 + 32 * c}" y="{188 - 16 * r}" width="24" height="12" '
                       f'rx="3" fill="{LED_ON[r] if on else LED_OFF}"/>')
    out.append("</g>")
    return "".join(out)


def text(x: float, y: float, size: float, fill: str, words: str, family: str = SANS,
         **more) -> str:
    extra = " ".join(f'{k.replace("_", "-")}="{v}"' for k, v in more.items())
    return (f'<text x="{x:.0f}" y="{y:.0f}" text-anchor="middle" font-family="{family}" '
            f'font-size="{size}" fill="{fill}" {extra}>{esc(words)}</text>')


def floor_bars(W: float, H: float, bars: np.ndarray, opacity: float) -> str:
    n, gap = len(bars), 6
    bw = (W - 2 * 40 - gap * (n - 1)) / n
    out = []
    for i, v in enumerate(bars):
        bh = max(3.0, float(v) * H * 0.22)
        out.append(f'<rect x="{40 + i * (bw + gap):.1f}" y="{H - bh:.1f}" width="{bw:.1f}" '
                   f'height="{bh:.1f}" fill="url(#heat)" opacity="{opacity}"/>')
    return "".join(out)


def intro_card(W: float, H: float, t: float, bars, lit, peaks) -> str:
    size = H * 0.50
    return (f'<rect width="{W}" height="{H}" fill="{NAVY}"/>'
            + floor_bars(W, H, bars, 0.16)
            + f'<g opacity="{ease(t / 0.6):.3f}">'
            + text(W / 2, H * 0.105, 24, ORANGE, "KEYGENS · CRACKTROS · DEMOS · GAMES",
                   family=MONO, letter_spacing=7)
            + text(W / 2, H * 0.215, 120, INK, "ByteBandit Music Player", font_weight=800,
                   letter_spacing=2) + "</g>"
            + f'<g opacity="{ease((t - 0.3) / 0.6):.3f}">'
            + icon(W / 2 - size / 2, H * 0.265, size, lit, peaks) + "</g>"
            + f'<g opacity="{ease((t - 0.9) / 0.6):.3f}">'
            + text(W / 2, H * 0.865, 38, MUTED,
                   "Tracker and chip music from 40 years of computers, in your terminal")
            + "</g>")


def numbers_card(W: float, H: float, t: float, bars, lit, peaks) -> str:
    stats = [("392", "of Modland's 423 formats play"),
             ("22", "formats verified against the original replay routines"),
             ("540,000", "songs, give or take, in six collections"),
             ("Sandboxed", "every replayer shut in: no network, a crash ends only its song")]
    out = [f'<rect width="{W}" height="{H}" fill="{NAVY}"/>', floor_bars(W, H, bars, 0.12),
           icon(W / 2 - 75, H * 0.05, 150, lit, peaks),
           text(W / 2, H * 0.245, 24, ORANGE, "IN NUMBERS", family=MONO, letter_spacing=7)]
    for i, (big, small) in enumerate(stats):
        a = ease((t - 0.4 - 0.5 * i) / 0.5)
        x, y = W * (0.29 + 0.42 * (i % 2)), H * (0.42 + 0.25 * (i // 2))
        out.append(f'<g opacity="{a:.3f}" transform="translate(0 {(1 - a) * 18:.1f})">'
                   + text(x, y, 104, INK, big, font_weight=800)
                   + text(x, y + 52, 30, MUTED, small) + "</g>")
    out.append(f'<g opacity="{ease((t - 2.6) / 0.6):.3f}">'
               + text(W / 2, H * 0.90, 27, BLUE, "Modland · HVSC · Keygen Music · ASMA · "
                      "ZX-Art · SNDH", family=MONO, letter_spacing=2) + "</g>")
    return "".join(out)


def outro_card(W: float, H: float, t: float, bars, lit, peaks) -> str:
    size = H * 0.36
    pill_w = 360
    return (f'<rect width="{W}" height="{H}" fill="{NAVY}"/>' + floor_bars(W, H, bars, 0.16)
            + icon(W / 2 - size / 2, H * 0.09, size, lit, peaks)
            + text(W / 2, H * 0.58, 104, INK, "ByteBandit Music Player", font_weight=800,
                   letter_spacing=2)
            + f'<g opacity="{ease((t - 0.6) / 0.6):.3f}">'
            + f'<rect x="{W / 2 - pill_w / 2:.0f}" y="{H * 0.64:.0f}" width="{pill_w}" '
              f'height="92" rx="14" fill="#141c29" stroke="{GREEN}" stroke-width="2.5"/>'
            + f'<text x="{W / 2:.0f}" y="{H * 0.64 + 64:.0f}" text-anchor="middle" '
              f'font-family="{MONO}" font-size="56" fill="{INK}" xml:space="preserve">'
              f'<tspan fill="{GREEN}">$ </tspan>bbmp</text></g>'
            + f'<g opacity="{ease((t - 1.2) / 0.6):.3f}">'
            + text(W / 2, H * 0.84, 34, MUTED,
                   "Install the Flatpak, then type bbmp in any terminal")
            + text(W / 2, H * 0.90, 24, ORANGE, "AMIGA · C64 · ATARI ST · ZX SPECTRUM · NES · "
                   "MEGA DRIVE · AND THE REST", family=MONO, letter_spacing=4) + "</g>")


def compose(frame: Frame, t: float, total: float, captions: list[Caption], start: float,
            marks: Marks, bars, meter: Meter, geometry) -> str:
    """One frame: the screenshot at the top in the middle, the caption band (the
    showcase's, larger) under it, and a card over everything when one is up."""
    W, H, w, h = geometry
    inner = re.sub(r"^<\?xml[^>]*\?>\s*", "", frame.svg)
    inner = inner.replace("<svg ", f'<svg x="{(W - w) / 2:.1f}" y="0" width="{w}" height="{h}" '
                          'xml:space="preserve" ', 1)
    cap = next((c for c in captions if c.begin - start <= t < c.end - start), None)
    if cap:
        cap = Caption(cap.begin - start, cap.kicker, cap.title, cap.line, cap.end - start)
    band = (f'<g transform="translate(0 {h}) scale({SCALE})">'
            f'{draw_band(W / SCALE, 0, cap, t, t / total, frame.visual)}</g>')
    lit, peaks = meter.lit(t)
    nb, ne = marks.numbers[0] - start, marks.numbers[1] - start
    card = ""
    if t < INTRO:
        card = (f'<g opacity="{1 - ease((t - (INTRO - FADE)) / FADE):.3f}">'
                f'{intro_card(W, H, t, bars, lit, peaks)}</g>')
    elif nb <= t < ne:
        a = ease((t - nb) / FADE) * (1 - ease((t - (ne - FADE)) / FADE))
        card = f'<g opacity="{a:.3f}">{numbers_card(W, H, t - nb, bars, lit, peaks)}</g>'
    elif t >= total - OUTRO:
        b = total - OUTRO
        card = (f'<g opacity="{ease((t - b) / FADE):.3f}">'
                f'{outro_card(W, H, t - b, bars, lit, peaks)}</g>')
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W:.1f} {H:.1f}">'
        f'<defs><linearGradient id="heat" gradientUnits="userSpaceOnUse" x1="0" x2="0" '
        f'y1="{H:.0f}" y2="{H * 0.78:.0f}">'
        f'<stop offset="0" stop-color="#2f7de1"/><stop offset="0.55" stop-color="#c850c0"/>'
        f'<stop offset="1" stop-color="{ORANGE}"/></linearGradient>'
        f'<linearGradient id="iconbg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" '
        f'stop-color="#232833"/><stop offset="1" stop-color="#12151b"/></linearGradient></defs>'
        f'<rect width="{W:.1f}" height="{H:.1f}" fill="{NAVY}"/>{inner}{band}{card}</svg>')


# ---- main ------------------------------------------------------------------------

def encode(src: Path, out: Path, width: int, video: list[str], audio_kbps: int) -> None:
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(src),
                    "-vf", f"scale={width}:-2:flags=lanczos,format=yuv420p", *video,
                    "-c:a", "aac", "-b:a", f"{audio_kbps}k", "-movflags", "+faststart",
                    str(out)], check=True)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = Path(args[0] if args else "ad.mp4").resolve()
    share = None
    if "--share" in sys.argv:
        share = Path(sys.argv[sys.argv.index("--share") + 1]).resolve()
        args = [a for a in args if Path(a).resolve() != share]
    work = WORK
    from bbmp.fetch import Fetcher
    from bbmp.index import Index
    from bbmp.library import Library
    from bbmp.settings import Settings
    from bbmp.tui import ModlandApp, quiet_stderr

    index, fetcher = Index(), Fetcher()
    print("fetching the songs...")
    for key in SONGS.values():
        if index.resolve(key) is None:
            raise SystemExit(f"not in the catalog: {key}")
        fetcher.get(key, index.companions(key))

    frames: list[Frame] = []
    captions: list[Caption] = []
    marks = Marks()
    rec = MarkingRecorder(time.monotonic())
    library = Library(work / "user.db")

    async def run():
        app = ModlandApp(index, fetcher, library, rec, settings=Settings(resume=False),
                         session_path=work / "session.json")
        rec.player = app.player
        app.export_dir = Path.home() / "Music/ByteBandit Music Player/Exports"
        async with app.run_test(size=SIZE) as pilot:
            await pilot.pause(0.5)
            await tour(app, pilot, rec, frames, captions, marks)
            app.player.stop()
            await pilot.pause(0.3)

    print("recording...")
    with quiet_stderr():
        asyncio.run(run())
    start, total = frames[0].t, frames[-1].t - frames[0].t
    print(f"captured {len(frames)} frames over {total:.1f} s")
    for c in captions:
        print(f"  {c.begin - start:6.1f}  {c.kicker:8} {c.title}  |  {c.line}")
    print(f"  {marks.numbers[0] - start:6.1f}  NUMBERS card;  {total - OUTRO:6.1f}  OUTRO")

    print("the sound...")
    first, length = int(start * RATE), int(total * RATE)
    audio = soundtrack(rec, first, length, index, fetcher)
    with wave.open(str(work / "audio.wav"), "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(RATE)
        wf.writeframes((audio * 32767).astype(np.int16).tobytes())

    print("drawing frames...")
    w, h = map(float, re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', frames[0].svg).groups())
    H = h + BAND * SCALE
    W = max(w, H * 16 / 9)
    H = W * 9 / 16
    spectrum, meter = Spectrum(audio), Meter(audio)
    times = np.array([f.t - start for f in frames])
    count = int(total * FPS)
    for k in range(count):
        t = k / FPS
        frame = frames[max(0, int(np.searchsorted(times, t, side="right")) - 1)]
        svg = compose(frame, t, total, captions, start, marks, spectrum.at(t), meter,
                      (W, H, w, h))
        (work / f"f{k:05d}.svg").write_text(box_font(svg))
    subprocess.run(["bash", "-c", f"cd {work} && ls f*.svg | xargs -P {os.cpu_count()} -n 8 "
                    "sh -c 'for f; do rsvg-convert -w 1920 -h 1080 \"$f\" -o \"${f%.svg}.png\" "
                    "&& rm \"$f\"; done' _"], check=True)

    print("encoding...")
    master = work / "master.mp4"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(FPS),
                    "-i", str(work / "f%05d.png"), "-i", str(work / "audio.wav"),
                    "-vf", "format=yuv420p", "-c:v", "libx264", "-preset", "slow",
                    "-crf", "18", "-tune", "animation", "-c:a", "aac", "-b:a", "256k",
                    "-shortest", "-movflags", "+faststart", str(master)], check=True)
    shutil.copy(master, out)
    print(f"wrote {out} ({out.stat().st_size / 1e6:.1f} MB)")
    if share:
        limit = 14_000_000 * 0.92                    # bytes, with room for the container
        seconds = count / FPS
        audio_kbps = 128
        video_kbps = int(limit * 8 / seconds / 1000) - audio_kbps
        for _ in range(3):
            passlog = str(work / "pass")
            common = ["-c:v", "libx264", "-preset", "slow", "-b:v", f"{video_kbps}k",
                      "-passlogfile", passlog, "-tune", "animation"]
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(master),
                            "-vf", "scale=1280:720:flags=lanczos,format=yuv420p", *common,
                            "-pass", "1", "-an", "-f", "null", "/dev/null"], check=True)
            encode(master, share, 1280, [*common, "-pass", "2"], audio_kbps)
            size = share.stat().st_size
            print(f"wrote {share} ({size / 1e6:.2f} MB at {video_kbps} kb/s)")
            if size <= 14_000_000:
                break
            video_kbps = int(video_kbps * 14_000_000 / size * 0.95)
    if "--keep" in sys.argv:
        print(f"frames kept in {work}")
    else:
        shutil.rmtree(work)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
