#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record a demo video of the TUI: headless app, real playback, screenshots + audio -> MP4.

    PYTHONPATH=src python3 tools/record_demo.py demo.mp4          # the short demo
    PYTHONPATH=src python3 tools/record_demo.py --tour tour.mp4   # the format tour, with captions

Needs rsvg-convert and ffmpeg. The music goes to a WAV file, not the speakers.
"""

from __future__ import annotations

import asyncio
import os
import shutil
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

from bbmp.backends.base import FRAME_BYTES, RATE
from bbmp.library import Library
from bbmp.settings import Settings
from bbmp.tui import ModlandApp, quiet_stderr

FPS = 10
SIZE = (124, 38)


class Recorder:
    """An audio output that keeps real time and records to memory, padding gaps with silence."""

    def __init__(self, t0: float):
        self.t0 = t0
        self.pcm = bytearray()

    def _now_frames(self) -> int:
        return int((time.monotonic() - self.t0) * RATE)

    def write(self, pcm: bytes) -> None:
        have = len(self.pcm) // FRAME_BYTES
        now = self._now_frames()
        if now > have + RATE // 10:   # a gap while a song loaded: fill it with silence
            self.pcm += bytes((now - have) * FRAME_BYTES)
        self.pcm += pcm
        ahead = len(self.pcm) / FRAME_BYTES / RATE - (time.monotonic() - self.t0)
        if ahead > 0.1:
            time.sleep(ahead - 0.1)

    def latency(self) -> float:
        return 0.0

    def flush(self) -> None:
        pass

    def drain(self) -> None:
        pass

    def close(self) -> None:
        pass


async def demo(app: ModlandApp, pilot, rec: Recorder, frames: list) -> None:
    async def capture():
        while True:
            frames.append((time.monotonic() - rec.t0, app.export_screenshot()))
            await asyncio.sleep(1 / FPS)

    async def wait(seconds: float):
        await pilot.pause(seconds)

    async def visual(mode):
        vis = app.query_one("#visual")
        while vis.mode != mode:
            await pilot.press("v")

    async def type_text(text: str):
        for ch in text:
            await pilot.press("space" if ch == " " else ch)
            await pilot.pause(0.09)

    async def pick(list_id: str, option_id: str):
        ol = app.query_one(list_id)
        for i in range(ol.option_count):
            if ol.get_option_at_index(i).id == option_id:
                ol.highlighted = i
                break
        await pilot.pause(0.6)
        await pilot.press("enter")

    async def playing(title_part: str, timeout=15):
        for _ in range(timeout * 10):
            st = app.player.status()
            if st.state == "playing" and st.song and title_part in st.song.path:
                return
            await pilot.pause(0.1)

    shooter = asyncio.create_task(capture())
    try:
        await wait(1.5)
        # 1. Browse to Protracker / Captain and play Space Debris
        await pick("#formats", "Protracker")
        await wait(0.8)
        await pick("#artists", "Captain")
        await wait(0.8)
        table = app.query_one("#browse-songs")
        row = next(i for i, s in enumerate(table.songs) if s.name == "space_debris.mod")
        for _ in range(row):
            await pilot.press("down")
            await pilot.pause(0.15)
        await pilot.press("enter")
        await playing("space_debris")
        await wait(2)
        await visual("spectrum")
        await wait(8)
        await visual("scope")
        await wait(7)
        await visual("channels")
        await wait(7)
        await visual("pattern")
        await wait(8)

        # 2. Search for Turrican: Chris Huelsbeck's TFMX original, played by UADE
        await pilot.press("slash")
        await type_text("turrican flight")
        await wait(0.8)
        await pilot.press("enter")
        await wait(0.8)
        await pilot.press("enter")
        await playing("turrican flight")
        await visual("spectrum")
        await wait(9)
        await visual("scope")
        await wait(6)

        # 3. The C64: Rob Hubbard's Monty on the Run through libsidplayfp
        await pilot.press("slash")
        await type_text("hubbard monty run")
        await wait(0.8)
        await pilot.press("enter")
        await wait(0.8)
        await pilot.press("enter")
        await playing("Monty_on_the_Run")
        await wait(7)
        await visual("spectrum")
        await wait(7)

        # 4. Radio mode on AHX
        await pilot.press("1")
        await wait(0.5)
        app.query_one("#formats").focus()
        ol = app.query_one("#formats")
        ol.highlighted = next(i for i in range(ol.option_count) if ol.get_option_at_index(i).id == "AHX")
        await wait(1)
        await pilot.press("r")
        await wait(9)
        await visual("channels")
        await wait(5)
        await pilot.press("n")
        await wait(2)
        await visual("spectrum")
        await wait(7)
    finally:
        shooter.cancel()


# ---- the format tour -------------------------------------------------------------

# (search, Modland path, [(visualiser, seconds), ...], caption title, caption line)
TOUR = [
    ("elwood homecoming", "Fasttracker 2/Elwood/homecoming.xm",
     [("channels", 8), ("pattern", 6)], "PC · FASTTRACKER 2 · 32 CHANNELS", "Elwood – Homecoming   ·   libopenmpt"),
    ("necros mindspring", "Impulsetracker/Necros/mindspring.it",
     [("spectrum", 8)], "PC · IMPULSE TRACKER · 24 CHANNELS", "Necros – Mindspring   ·   libopenmpt"),
    ("link's awakening", "Gameboy Sound System/M. Hamano/legend of zelda - link's awakening.gbs",
     [("scope", 9)], "NINTENDO GAME BOY", "The Legend of Zelda: Link's Awakening   ·   Game Music Emu"),
    ("aria sorrow confrontation",
     "Gameboy Sound Format/Michiru Yamane/Castlevania - Aria Of Sorrow/18 confrontation.minigsf",
     [("spectrum", 9)], "GAME BOY ADVANCE", "Castlevania: Aria of Sorrow – Confrontation   ·   ZXTune (mGBA)"),
    ("mario kart ds", "Nintendo DS Sound Format/Shinobu Tanaka/Mario Kart DS/ntr-amce-usa-0010.mini2sf",
     [("spectrum", 9)], "NINTENDO DS", "Mario Kart DS   ·   ZXTune (vio2sf)"),
    ("virtual boy golf title", "Video Game Music/Nintendo Virtual Boy/Ken Kojima/Golf/title screen.vgz",
     [("scope", 9)], "NINTENDO VIRTUAL BOY", "Golf – title screen   ·   ZXTune (libvgm)"),
    ("gradius higashino", "HES/Miki Higashino/gradius.hes",
     [("scope", 10)], "NEC PC ENGINE", "Gradius – jingle, then on to track 2   ·   Game Music Emu"),
    ("miker jambala", "Slight Atari Player/Miker/7 gates of jambala.sap",
     [("spectrum", 9)], "ATARI 8-BIT · POKEY", "Miker – 7 Gates of Jambala   ·   Game Music Emu"),
    ("mad max tune 3", "SNDH/Mad_Max/Demos/Little_Sound_Demo/Mad_Max_Tune_3.sndh",
     [("scope", 9)], "ATARI ST · SNDH", "Mad Max – Little Sound Demo   ·   psgplay (68000 + YM2149)"),
    ("hippel scout", "YM/Jochen Hippel/scout.ym",
     [("spectrum", 8)], "ATARI ST · YM", "Jochen Hippel – Scout   ·   StSound"),
    ("ys08", "MDX/- unknown/Ys/ys08.mdx",
     [("spectrum", 10)], "SHARP X68000 · MDX", "Ys – Music 08   ·   mdxmini (YM2151 + ADPCM)"),
    ("rtype06", "MDX/- unknown/R-Type/rtype06.mdx",
     [("scope", 8)], "SHARP X68000 · MDX", "R-Type (arcade) – stage music   ·   mdxmini"),
    ("handsup4", "Spectrum/Pro Tracker 3/Soundliner/handsup4.pt3",
     [("scope", 9)], "ZX SPECTRUM · PRO TRACKER 3", "Soundliner – Hands Up 4   ·   ZXTune (AY-3-8912)"),
    ("fred493", "Sam Coupe COP/- unknown/fred493.cop",
     [("spectrum", 8)], "SAM COUPÉ · SAA1099", "FRED disk magazine tune   ·   ZXTune"),
    ("microcosm loader", "Video Game Music/BBC Micro/Melvin Wright/Microcosm/microcosm - loader.vgz",
     [("scope", 8)], "BBC MICRO · SN76489", "Microcosm – loader   ·   Game Music Emu"),
    ("fantasy zone level 1", "Ad Lib/Reality AdLib Tracker/Farmer/fantasy zone - level 1.rad",
     [("spectrum", 9)], "PC · ADLIB (OPL3)", "Farmer – Fantasy Zone level 1   ·   AdPlug + Nuked OPL3"),
    ("alundra kline", "Playstation Sound Format/Kohei Tanaka/Alundra/112 kline's dream.psf",
     [("spectrum", 10)], "SONY PLAYSTATION", "Alundra – Kline's Dream   ·   ZXTune (Highly Experimental)"),
]


async def tour(app: ModlandApp, pilot, rec: Recorder, frames: list, captions: list) -> None:
    async def capture():
        while True:
            frames.append((time.monotonic() - rec.t0, app.export_screenshot()))
            await asyncio.sleep(1 / FPS)

    async def visual(mode):
        vis = app.query_one("#visual")
        while vis.mode != mode:
            await pilot.press("v")

    shooter = asyncio.create_task(capture())
    try:
        await pilot.pause(2.5)
        for query, path, visuals, title, line in TOUR:
            await pilot.press("slash")
            for ch in query:
                await pilot.press("space" if ch == " " else ch)
                await pilot.pause(0.07)
            await pilot.press("enter")
            await pilot.pause(0.6)
            table = app.query_one("#search-songs")
            row = next(i for i, s in enumerate(table.songs) if s.path == path)
            for _ in range(row):
                await pilot.press("down")
                await pilot.pause(0.12)
            await pilot.press("enter")
            for _ in range(200):
                st = app.player.status()
                if st.state == "playing" and st.song and st.song.path == path:
                    break
                await pilot.pause(0.1)
            begin = time.monotonic() - rec.t0
            for mode, seconds in visuals:
                await visual(mode)
                await pilot.pause(seconds)
            captions.append((begin, time.monotonic() - rec.t0, title, line))
    finally:
        shooter.cancel()


# ---- recording --------------------------------------------------------------------

FONT_BOLD = "/usr/share/fonts/noto/NotoSans-Bold.ttf"
FONT = "/usr/share/fonts/noto/NotoSans-Regular.ttf"


def caption_filter(work: Path, captions: list, start: float) -> str:
    """ffmpeg drawtext filters for the captions (text goes via files: no escaping trouble)."""
    parts = []
    for i, (a, b, title, line) in enumerate(captions):
        a, b = a - start, b - start
        (work / f"t{i}.txt").write_text(title)
        (work / f"l{i}.txt").write_text(line)
        when = f"enable='between(t,{a:.2f},{b:.2f})'"
        box = "box=1:boxcolor=0x101018@0.82:boxborderw=16"
        parts.append(f"drawtext=fontfile={FONT_BOLD}:textfile={work}/t{i}.txt:fontsize=40:"
                     f"fontcolor=0xffd75f:x=w-tw-60:y=150:{box}:{when}")
        parts.append(f"drawtext=fontfile={FONT}:textfile={work}/l{i}.txt:fontsize=26:"
                     f"fontcolor=white:x=w-tw-60:y=222:{box}:{when}")
    return ("," + ",".join(parts)) if parts else ""


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    is_tour = "--tour" in sys.argv
    out = Path(args[0] if args else ("tour.mp4" if is_tour else "demo.mp4")).resolve()
    work = Path(tempfile.mkdtemp(prefix="modland-demo-"))
    frames: list[tuple[float, str]] = []
    captions: list = []
    t0 = time.monotonic()
    rec = Recorder(t0)
    lib = Library(work / "user.db")

    async def run():
        app = ModlandApp(library=lib, output=rec, settings=Settings(resume=False),
                         session_path=work / "session.json")
        async with app.run_test(size=SIZE) as pilot:
            if is_tour:
                await tour(app, pilot, rec, frames, captions)
            else:
                await demo(app, pilot, rec, frames)
            app.player.stop()
            await pilot.pause(0.3)

    with quiet_stderr():
        asyncio.run(run())
    print(f"captured {len(frames)} frames, {len(rec.pcm) / FRAME_BYTES / RATE:.1f}s of audio")

    # frames -> PNG (rsvg drops leading spaces unless whitespace is preserved)
    for i, (_, svg) in enumerate(frames):
        (work / f"f{i:05d}.svg").write_text(svg.replace("<svg ", '<svg xml:space="preserve" ', 1))
    subprocess.run(["bash", "-c", f"cd {work} && ls f*.svg | xargs -P {os.cpu_count()} -I{{}} "
                    "sh -c 'rsvg-convert -w 1600 \"$1\" -o \"${1%.svg}.png\"' _ {}"], check=True)

    # an ffmpeg concat list with the real time each frame was on screen
    lines = []
    for i, (t, _) in enumerate(frames):
        nxt = frames[i + 1][0] if i + 1 < len(frames) else t + 1 / FPS
        lines += [f"file 'f{i:05d}.png'", f"duration {max(0.01, nxt - t):.3f}"]
    lines.append(f"file 'f{len(frames) - 1:05d}.png'")
    (work / "frames.txt").write_text("\n".join(lines) + "\n")
    start = frames[0][0]
    audio = rec.pcm[int(start * RATE) * FRAME_BYTES:]
    with wave.open(str(work / "audio.wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(audio))
    vf = "fps=30,scale=trunc(iw/2)*2:trunc(ih/2)*2" + caption_filter(work, captions, start) + ",format=yuv420p"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                    "-i", str(work / "frames.txt"), "-i", str(work / "audio.wav"),
                    "-vf", vf, "-c:v", "libx264", "-preset", "slow", "-crf", "20", "-tune", "stillimage",
                    "-c:a", "aac", "-b:a", "192k", "-shortest", "-movflags", "+faststart",
                    str(out)], check=True)
    shutil.rmtree(work)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
