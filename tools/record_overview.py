#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record the overview video: a guided tour of the player's features and formats.

    PYTHONPATH=src python3 tools/record_overview.py overview.mp4

The real app runs headlessly and plays real songs from Modland; the sound goes to the
video, not the speakers. Every frame is the app's own screenshot with a caption band
added underneath, and the title and end cards show the spectrum of the music that is
playing at that moment. Needs rsvg-convert and ffmpeg.

Nothing of yours is touched: settings, favorites, statistics and the session all live
in a temporary folder. (Songs are downloaded into the player's normal cache.)
"""

from __future__ import annotations

import asyncio
import html
import os
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
from record_demo import Recorder  # noqa: E402

from bbmp.backends.base import RATE  # noqa: E402
from bbmp.fetch import Fetcher  # noqa: E402
from bbmp.index import Index  # noqa: E402
from bbmp.library import Library  # noqa: E402
from bbmp.settings import Settings  # noqa: E402
from bbmp.tui import ModlandApp, quiet_stderr  # noqa: E402

FPS = 12
SIZE = (124, 38)
BAND = 132            # height of the caption band, in the screenshot's own units
INTRO, OUTRO, FADE = 5.0, 6.0, 0.7

NAVY, INK, MUTED, ORANGE, BLUE = "#0b1420", "#f2f5fa", "#9fb0c8", "#ff9a3c", "#4aa3ff"
SANS = "Noto Sans, DejaVu Sans, sans-serif"
MONO = "MesloLGS Nerd Font Mono, DejaVu Sans Mono, monospace"

SONGS = {
    "elysium": "Protracker/Jester/elysium.mod",
    "onward": "Fasttracker 2/Jugi/onward.xm",
    "dracula": "MDX/- unknown/Akumajou Dracula (X68)/dra11.mdx",
    "wings": "SNDH/Mad_Max/Games/Wings_Of_Death.sndh",
    "megaman": "Nintendo Sound Format/C. Manami/mega man 2.nsf",
    "vectrex": "Video Game Music/Vectrex/Lenny Carlson/Clean Sweep/title screen.vgz",
    "dreamer": "Video Game Music/Sega Megadrive/YK-2/Streets Of Rage 2/07 dreamer.vgz",
    "commando": "HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid",
}


@dataclass
class Caption:
    begin: float
    kicker: str
    title: str
    line: str
    end: float = 1e9


@dataclass
class Frame:
    t: float
    svg: str
    visual: str | None


# ---- the tour ---------------------------------------------------------------------

async def tour(app: ModlandApp, pilot, rec: Recorder, frames: list[Frame],
               captions: list[Caption], songs: list[tuple[float, str]]) -> None:
    ix = app.index
    now = lambda: time.monotonic() - rec.t0   # noqa: E731

    async def capture():
        last = None
        while True:
            vis = app.query_one("#visual")
            frames.append(Frame(now(), app.export_screenshot(), vis.mode))
            st = app.player.status()
            key = (st.song.path, st.info.subsong if st.info else 0) if st.song else None
            if key != last and st.state == "playing":
                songs.append((now(), key[0]))
                last = key
            await asyncio.sleep(1 / FPS)

    def cap(kicker: str, title: str, line: str) -> None:
        if captions:
            captions[-1].end = min(captions[-1].end, now())
        captions.append(Caption(now(), kicker, title, line))

    async def wait(seconds: float) -> None:
        await pilot.pause(seconds)

    async def visual(mode: str | None) -> None:
        vis = app.query_one("#visual")
        while vis.mode != mode:
            await pilot.press("v")

    async def type_text(text: str) -> None:
        for ch in text:
            await pilot.press("space" if ch == " " else ch)
            await pilot.pause(0.06)

    async def playing(path: str) -> None:
        for _ in range(300):
            st = app.player.status()
            if st.state == "playing" and st.song and st.song.path == path:
                return
            await pilot.pause(0.1)
        raise RuntimeError(f"never started: {path}")

    async def highlight(list_id: str, option_id: str) -> None:
        ol = app.query_one(list_id)
        ol.highlighted = next(i for i in range(ol.option_count)
                              if ol.get_option_at_index(i).id == option_id)
        await pilot.pause(0.9)

    async def search(query: str, path: str, steps: int = 3) -> None:
        """Type a query, go to the results and put the cursor on `path`."""
        await pilot.press("slash")
        await type_text(query)
        await pilot.pause(0.7)
        await pilot.press("enter")
        await pilot.pause(0.5)
        table = app.query_one("#search-songs")
        row = next(i for i, s in enumerate(table.songs) if s.path == path)
        for _ in range(min(row, steps)):
            await pilot.press("down")
            await pilot.pause(0.12)
        if row > steps:
            table.move_cursor(row=row)
        await pilot.pause(0.5)

    # The title card covers the screen while the first song starts.
    await visual("pattern")
    app.player.play_list([ix.by_path(SONGS["elysium"])])
    await playing(SONGS["elysium"])
    shooter = asyncio.create_task(capture())
    try:
        await wait(INTRO)
        cap("FORMAT", "Amiga · Protracker",
            "Jester – Elysium · four channels of 8-bit samples · played by libopenmpt")
        await wait(6)

        cap("FEATURE", "Browse", "Format, artist, song: all 493,848 songs in Modland's catalog")
        await highlight("#formats", "Fasttracker 2")
        await pilot.press("enter")
        await highlight("#artists", "Jugi")
        await pilot.press("enter")
        await wait(0.6)
        table = app.query_one("#browse-songs")
        for _ in range(next(i for i, s in enumerate(table.songs) if s.path == SONGS["onward"])):
            await pilot.press("down")
            await pilot.pause(0.12)
        await wait(0.4)
        await pilot.press("enter")
        await playing(SONGS["onward"])
        await visual("channels")
        cap("FORMAT", "PC · Fasttracker 2", "Jugi – Onward · 28 channels · played by libopenmpt")
        await wait(7)

        cap("FEATURE", "Search", "Titles, artists, albums and formats, with results as you type")
        await search("akumajou dracula x68", SONGS["dracula"])
        await pilot.press("enter")
        await playing(SONGS["dracula"])
        await visual("spectrum")
        cap("FORMAT", "Sharp X68000 · MDX",
            "Akumajou Dracula – Vampire Killer · YM2151 FM and ADPCM samples · played by mdxmini")
        await wait(10)

        await search("wings of death mad max", SONGS["wings"])
        await pilot.press("enter")
        await playing(SONGS["wings"])
        await visual("scope")
        cap("FORMAT", "Atari ST · SNDH",
            "Mad Max – Wings of Death · YM2149 on an emulated 68000 · played by psgplay")
        await wait(7.5)

        await search("mega man 2 manami", SONGS["megaman"])
        await pilot.press("enter")
        await playing(SONGS["megaman"])
        await visual("spectrum")
        cap("FORMAT", "Nintendo NES · NSF",
            "Mega Man 2 · one file holds all 24 songs · played by Game Music Emu")
        await wait(4.5)
        cap("FEATURE", "Subsongs",
            "] and [ step through a file's songs · left alone they play like an album, empty ones skipped")
        for _ in range(3):
            await pilot.press("right_square_bracket")
            await wait(3.0)

        await search("vectrex clean sweep title", SONGS["vectrex"])
        await pilot.press("enter")
        await playing(SONGS["vectrex"])
        await visual("scope")
        cap("FORMAT", "Vectrex · AY-3-8912",
            "Clean Sweep – title screen · a seven-second jingle from 1982 · played by ZXTune")
        await wait(6.2)

        await search("streets of rage 2 dreamer", SONGS["dreamer"])
        await pilot.press("enter")
        await playing(SONGS["dreamer"])
        await visual("spectrum")
        cap("FORMAT", "Sega Mega Drive · VGM",
            "Streets of Rage 2 – Dreamer · YM2612 FM · played by Game Music Emu")
        await wait(5)
        cap("FEATURE", "Favorites and queue",
            "f marks a favorite · a adds a song to the queue · e repeats · z shuffles")
        await pilot.press("f")
        await wait(1.2)
        await search("hubbard commando", SONGS["commando"])
        await pilot.press("a")
        await wait(1.2)
        await pilot.press("3")
        await wait(1.5)
        queue = app.query_one("#queue-songs")
        for _ in range(next(i for i, s in enumerate(queue.songs) if s.path == SONGS["commando"])):
            await pilot.press("down")
            await pilot.pause(0.25)
        await wait(0.8)
        await pilot.press("enter")
        await playing(SONGS["commando"])
        await visual("scope")
        cap("FORMAT", "Commodore 64 · SID",
            "Rob Hubbard – Commando · 19 subsongs, lengths from HVSC · played by libsidplayfp")
        await wait(6.5)

        await pilot.press("1")
        await wait(0.4)
        app.query_one("#formats").focus()
        await highlight("#formats", "AHX")
        cap("FEATURE", "Radio",
            "Endless random songs from all of Modland, one format or one artist · here: Amiga AHX")
        await pilot.press("r")
        for _ in range(300):
            st = app.player.status()
            if st.state == "playing" and st.song and st.song.format == "AHX":
                break
            await pilot.pause(0.1)
        await visual("spectrum")
        await wait(3)
        st = app.player.status()
        size = app.fetcher.local_path(st.song.path).stat().st_size
        cap("FORMAT", "Amiga · AHX",
            f"{st.song.artist} – {st.song.title} · the whole song is {size / 1024:.1f} kB of synth data · "
            f"original replayer on UADE")
        await wait(5.5)

        await visual(None)   # the Stats and Settings tabs get the whole screen
        await pilot.press("9")
        cap("FEATURE", "Statistics",
            "Most played songs, artists and formats by week, month, year or all time · "
            "these are the songs from this video")
        await wait(6)

        await pilot.press("0")
        cap("FEATURE", "Settings",
            "Saved the moment they change · cache limit · resume where you left off")
        await wait(1.5)
        await pilot.press("down")
        await wait(0.5)
        await pilot.press("enter")
        await wait(3.5)
        captions[-1].end = now()
        await pilot.press("1")
        await wait(OUTRO)
    finally:
        shooter.cancel()


# ---- audio ------------------------------------------------------------------------

def level_audio(pcm: bytes, starts: list[float], target: float = 0.13) -> np.ndarray:
    """Bring every song to a similar loudness, so the machines can be compared."""
    a = np.frombuffer(pcm, dtype=np.int16).astype(np.float32).reshape(-1, 2) / 32768
    edges = sorted({0, *(int(t * RATE) for t in starts), len(a)})
    gain = np.ones(len(a), dtype=np.float32)
    for lo, hi in zip(edges, edges[1:]):
        seg = a[lo:hi]
        loud = seg[np.abs(seg).max(axis=1) > 0.003]
        if len(loud) < RATE // 2:
            continue
        rms = float(np.sqrt((loud ** 2).mean()))
        gain[lo:hi] = float(np.clip(target / rms, 0.4, 3.0))
    ramp = RATE // 20   # smooth the steps between songs
    kernel = np.ones(ramp, dtype=np.float32) / ramp
    gain = np.convolve(np.pad(gain, ramp, mode="edge"), kernel, mode="same")[ramp:-ramp]
    a = a * gain[:, None]
    fade_in, fade_out = RATE // 2, int(2.5 * RATE)
    a[:fade_in] *= np.linspace(0, 1, fade_in, dtype=np.float32)[:, None]
    a[-fade_out:] *= np.linspace(1, 0, fade_out, dtype=np.float32)[:, None]
    return np.tanh(a * 1.1) / np.tanh(1.1)   # soft limit instead of clipping


class Spectrum:
    """Log-spaced bars from the recorded audio, for the title and end cards."""

    def __init__(self, audio: np.ndarray, bars: int = 64):
        self.mono = audio.mean(axis=1)
        self.bars = bars
        self.state = np.zeros(bars)
        freqs = np.fft.rfftfreq(4096, 1 / RATE)
        edges = np.geomspace(35, 16000, bars + 1)
        self.bins = [np.where((freqs >= lo) & (freqs < hi))[0] for lo, hi in zip(edges, edges[1:])]
        self.nearest = [int(np.abs(freqs - np.sqrt(lo * hi)).argmin()) for lo, hi in zip(edges, edges[1:])]
        self.window = np.hanning(4096)

    def at(self, t: float) -> np.ndarray:
        end = max(4096, min(len(self.mono), int(t * RATE)))
        mag = np.abs(np.fft.rfft(self.mono[end - 4096:end] * self.window))
        levels = np.array([mag[b].max() if len(b) else mag[n] for b, n in zip(self.bins, self.nearest)])
        levels = np.clip((20 * np.log10(levels + 1e-9) + 8) / 46, 0, 1)
        self.state = np.maximum(levels, self.state * 0.8)
        return self.state


# ---- drawing ----------------------------------------------------------------------

def esc(text: str) -> str:
    return html.escape(text, quote=True)


def ease(x: float) -> float:
    x = min(1.0, max(0.0, x))
    return x * x * (3 - 2 * x)


def draw_band(w: float, h: float, cap: Caption | None, t: float, progress: float,
              visual: str | None) -> str:
    out = [f'<rect x="0" y="{h}" width="{w}" height="{BAND}" fill="{NAVY}"/>',
           f'<rect x="0" y="{h}" width="{w}" height="3" fill="#1d2c42"/>',
           f'<rect x="0" y="{h}" width="{w * progress:.1f}" height="3" fill="{ORANGE}"/>']
    if visual:
        out.append(f'<text x="{w - 36}" y="{h + 40}" text-anchor="end" font-family="{MONO}" '
                   f'font-size="15" letter-spacing="2" fill="{MUTED}">VISUALIZER '
                   f'<tspan fill="{BLUE}">{esc(visual.upper())}</tspan></text>')
    if cap and cap.begin <= t < cap.end:
        a = ease((t - cap.begin) / 0.35) * ease((cap.end - t) / 0.25)
        dy = (1 - ease((t - cap.begin) / 0.35)) * 14
        color = ORANGE if cap.kicker == "FORMAT" else BLUE
        kw = 12 + 10.2 * len(cap.kicker)
        out.append(
            f'<g opacity="{a:.3f}" transform="translate(0 {dy:.1f})">'
            f'<rect x="36" y="{h + 22}" width="{kw:.0f}" height="24" rx="3" fill="none" '
            f'stroke="{color}" stroke-width="1.5"/>'
            f'<text x="{36 + kw / 2:.0f}" y="{h + 39}" text-anchor="middle" font-family="{MONO}" '
            f'font-size="14" letter-spacing="2" fill="{color}">{esc(cap.kicker)}</text>'
            f'<text x="36" y="{h + 84}" font-family="{SANS}" font-weight="700" font-size="35" '
            f'fill="{INK}">{esc(cap.title)}</text>'
            f'<text x="36" y="{h + 115}" font-family="{SANS}" font-size="19.5" fill="{MUTED}">'
            f'{esc(cap.line)}</text></g>')
    return "".join(out)


def draw_card(w: float, total_h: float, opacity: float, bars: np.ndarray, kind: str) -> str:
    base = total_h * 0.86
    n = len(bars)
    gap = 5
    bw = (w - 2 * 60 - gap * (n - 1)) / n
    max_h = total_h * 0.43
    shapes = []
    for i, v in enumerate(bars):
        x = 60 + i * (bw + gap)
        bh = max(3.0, float(v) * max_h)
        shapes.append(f'<rect x="{x:.1f}" y="{base - bh:.1f}" width="{bw:.1f}" height="{bh:.1f}" '
                      f'fill="url(#heat)"/>')
        shapes.append(f'<rect x="{x:.1f}" y="{base + 6:.1f}" width="{bw:.1f}" height="{bh * 0.28:.1f}" '
                      f'fill="url(#heat)" opacity="0.14"/>')
    if kind == "intro":
        words = (
            f'<text x="{w / 2}" y="{total_h * 0.12:.0f}" text-anchor="middle" font-family="{MONO}" '
            f'font-size="19" letter-spacing="6" fill="{ORANGE}">FTP.MODLAND.COM IN YOUR TERMINAL</text>'
            f'<text x="{w / 2}" y="{total_h * 0.25:.0f}" text-anchor="middle" font-family="{SANS}" '
            f'font-weight="800" font-size="118" letter-spacing="3" fill="{INK}">ByteBandit Music Player</text>'
            f'<text x="{w / 2}" y="{total_h * 0.33:.0f}" text-anchor="middle" font-family="{SANS}" '
            f'font-size="27" fill="{MUTED}">493,848 songs · 316 formats · 40 years of computer '
            f'and console music</text>')
    else:
        cols = [("98.8%", "of Modland plays"), ("316", "formats"), ("20", "replayer libraries")]
        words = ""
        for i, (big, small) in enumerate(cols):
            x = w * (0.2 + 0.3 * i)
            words += (
                f'<text x="{x:.0f}" y="{total_h * 0.20:.0f}" text-anchor="middle" font-family="{SANS}" '
                f'font-weight="800" font-size="92" fill="{INK}">{big}</text>'
                f'<text x="{x:.0f}" y="{total_h * 0.25:.0f}" text-anchor="middle" font-family="{MONO}" '
                f'font-size="18" letter-spacing="3" fill="{ORANGE}">{small.upper()}</text>')
        words += (
            f'<text x="{w / 2}" y="{total_h * 0.34:.0f}" text-anchor="middle" font-family="{SANS}" '
            f'font-size="24" fill="{MUTED}">Amiga · Atari ST · C64 · X68000 · PC-98 · MSX · Spectrum · '
            f'NES · SNES · Mega Drive · PlayStation · and the rest</text>')
    return (f'<g opacity="{opacity:.3f}"><rect width="{w}" height="{total_h}" fill="{NAVY}"/>'
            f'{"".join(shapes)}{words}</g>')


def compose(frame: Frame, captions: list[Caption], start: float, total: float,
            spectrum: Spectrum) -> str:
    inner = re.sub(r"^<\?xml[^>]*\?>\s*", "", frame.svg)
    w, h = map(float, re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', inner).groups())
    inner = inner.replace("<svg ", f'<svg x="0" y="0" width="{w}" height="{h}" xml:space="preserve" ', 1)
    t = frame.t - start
    cap = next((c for c in captions if c.begin - start <= t < c.end - start), None)
    if cap:
        cap = Caption(cap.begin - start, cap.kicker, cap.title, cap.line, cap.end - start)
    card = ""
    if t < INTRO:
        card = draw_card(w, h + BAND, 1 - ease((t - (INTRO - FADE)) / FADE), spectrum.at(frame.t), "intro")
    elif t > total - OUTRO:
        card = draw_card(w, h + BAND, ease((t - (total - OUTRO)) / FADE), spectrum.at(frame.t), "outro")
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h + BAND}">'
        f'<defs><linearGradient id="heat" gradientUnits="userSpaceOnUse" x1="0" x2="0" '
        f'y1="{(h + BAND) * 0.86:.0f}" y2="{(h + BAND) * 0.43:.0f}">'
        f'<stop offset="0" stop-color="#2f7de1"/><stop offset="0.55" stop-color="#c850c0"/>'
        f'<stop offset="1" stop-color="{ORANGE}"/></linearGradient></defs>'
        f'<rect width="{w}" height="{h + BAND}" fill="{NAVY}"/>{inner}'
        f'{draw_band(w, h, cap, t, t / total, frame.visual)}{card}</svg>')


# ---- main -------------------------------------------------------------------------

def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "overview.mp4").resolve()
    work = Path(tempfile.mkdtemp(prefix="modland-overview-"))
    index, fetcher = Index(), Fetcher()
    print("fetching the songs...")
    for path in SONGS.values():
        fetcher.get(path, index.companions(path))

    frames: list[Frame] = []
    captions: list[Caption] = []
    songs: list[tuple[float, str]] = []
    rec = Recorder(time.monotonic())
    library = Library(work / "user.db")
    library.COUNTED_SECONDS = 5   # the clips are short; let them count for the Stats tab

    async def run():
        app = ModlandApp(index, fetcher, library, rec, settings=Settings(resume=False),
                         session_path=work / "session.json")
        async with app.run_test(size=SIZE) as pilot:
            await pilot.pause(0.5)
            await tour(app, pilot, rec, frames, captions, songs)
            app.player.stop()
            await pilot.pause(0.3)

    print("recording...")
    with quiet_stderr():
        asyncio.run(run())
    start, total = frames[0].t, frames[-1].t - frames[0].t
    print(f"captured {len(frames)} frames over {total:.0f} s")

    first = int(start * RATE) * 4
    audio = level_audio(bytes(rec.pcm[first:first + int(total * RATE) * 4]),
                        [t - start for t, _ in songs])
    with wave.open(str(work / "audio.wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes((audio * 32767).astype(np.int16).tobytes())

    print("drawing frames...")
    spectrum = Spectrum(audio)
    lines = []
    for i, frame in enumerate(frames):
        shifted = Frame(frame.t, frame.svg, frame.visual)
        svg = compose(shifted, captions, start, total, _Shift(spectrum, start))
        (work / f"f{i:05d}.svg").write_text(svg)
        nxt = frames[i + 1].t if i + 1 < len(frames) else frame.t + 1 / FPS
        lines += [f"file 'f{i:05d}.png'", f"duration {max(0.01, nxt - frame.t):.3f}"]
    lines.append(f"file 'f{len(frames) - 1:05d}.png'")
    (work / "frames.txt").write_text("\n".join(lines) + "\n")
    subprocess.run(["bash", "-c", f"cd {work} && ls f*.svg | xargs -P {os.cpu_count()} -I{{}} "
                    "sh -c 'rsvg-convert -w 1920 \"$1\" -o \"${1%.svg}.png\"' _ {}"], check=True)

    print("encoding...")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                    "-i", str(work / "frames.txt"), "-i", str(work / "audio.wav"),
                    "-vf", "fps=30,scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p",
                    "-c:v", "libx264", "-preset", "slow", "-crf", "19",
                    "-c:a", "aac", "-b:a", "224k", "-shortest", "-movflags", "+faststart",
                    str(out)], check=True)
    if "--keep" in sys.argv:
        print(f"frames kept in {work}")
    else:
        shutil.rmtree(work)
    print(f"wrote {out}")
    return 0


class _Shift:
    """The spectrum is indexed by video time; frames carry recording time."""

    def __init__(self, spectrum: Spectrum, start: float):
        self.spectrum, self.start = spectrum, start

    def at(self, t: float) -> np.ndarray:
        return self.spectrum.at(t - self.start)


if __name__ == "__main__":
    raise SystemExit(main())
