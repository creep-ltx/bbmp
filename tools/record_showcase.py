#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Record the showcase video: keygen and cracktro music, a tune from each machine, and
the player's features, views, themes and terminal sizes, with captions underneath.

    PYTHONPATH=src python3 tools/record_showcase.py showcase.mp4

As record_overview.py: the real app runs headlessly and plays real songs; the sound
goes into the video, never to the speakers. Settings, favorites, statistics and the
session live in a temporary folder, and so does saved music (BBMP_KEPT); songs
are downloaded into the player's normal cache. Needs rsvg-convert and ffmpeg.
"""

from __future__ import annotations

import asyncio
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

import numpy as np

WORK = Path(tempfile.mkdtemp(prefix="bbmp-showcase-", dir=Path.home() / ".cache/bbmp/work"))
os.environ["BBMP_KEPT"] = str(WORK / "kept")     # saved music: not the user's, set first

sys.path.insert(0, str(Path(__file__).resolve().parent))
from record_demo import Recorder  # noqa: E402
from screenshots import box_font  # noqa: E402
from record_overview import (BAND, BLUE, INK, MONO, MUTED, NAVY, ORANGE, SANS,  # noqa: E402
                             Caption, Frame, Spectrum, ease, esc, level_audio)

FPS = 12
SIZE = (124, 38)                  # the normal terminal
SMALL = [(80, 24), (64, 20)]      # and the small ones it is shown in
INTRO, OUTRO, FADE = 5.0, 7.0, 0.7
PINK, GREEN = "#ff5fb4", "#5fd38d"
KICKERS = {"FORMAT": ORANGE, "KEYGEN": PINK, "CRACKTRO": PINK, "FEATURE": BLUE,
           "VIEW": GREEN, "THEME": GREEN, "TERMINAL": GREEN}

SONGS = {
    "superhero": "modland:Fasttracker 2/Rez/coop-Kenet/unreeeal superhero 3.xm",
    "dubmood": "modland:Fasttracker 2/Dubmood/fff keygen intro.xm",
    "debris": "modland:Protracker/Captain/space_debris.mod",
    "monty": "modland:HVSC/MUSICIANS/H/Hubbard_Rob/Monty_on_the_Run.sid",
    "gostraight": "modland:Video Game Music/Sega Megadrive/YK-2/Streets Of Rage 2/02 go straight.vgz",
    "megaman": "modland:Nintendo Sound Format/C. Manami/mega man 2.nsf",
    "turrican": "modland:TFMX/Chris Huelsbeck/mdat.turrican 2 level 1-desert",
}

REPLAYERS = {"openmpt": "libopenmpt", "uade": "UADE, the Amiga emulated", "gme": "Game Music Emu",
             "sidplayfp": "libsidplayfp", "zxtune": "ZXTune", "sc68": "sc68",
             "psgplay": "psgplay", "mdx": "mdxmini", "pmd": "PMDWin", "pmdc": "PMD (own driver)", "fmp": "FMPMD"}


def replayer(backend: str) -> str:
    return REPLAYERS.get(backend, backend)


# ---- the tour ---------------------------------------------------------------------

async def tour(app, pilot, rec: Recorder, frames: list[Frame], captions: list[Caption],
               songs: list[tuple[float, str]]) -> None:
    ix = app.index
    now = lambda: time.monotonic() - rec.t0   # noqa: E731

    async def capture():
        last = None
        while True:
            vis = app.query_one("#visual")
            frames.append(Frame(now(), app.export_screenshot(), vis.mode))
            st = app.player.status()
            key = (st.song.key, st.info.subsong if st.info else 0) if st.song else None
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
        for _ in range(6):
            if vis.mode == mode:
                return
            await pilot.press("v")

    async def type_text(text: str) -> None:
        for ch in text:
            await pilot.press({" ": "space", ":": "colon", "-": "minus"}.get(ch, ch))
            await pilot.pause(0.05)

    async def playing(key: str) -> str:
        """Wait for the song; gives the replayer that plays it."""
        for _ in range(400):
            st = app.player.status()
            if st.state == "playing" and st.song and st.song.key == key:
                return replayer(st.backend)
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
        await pilot.pause(0.8)
        if then_enter:
            await pilot.press("enter")
            await pilot.pause(0.5)

    async def to_row(table_id: str, key: str, steps: int = 4) -> None:
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
            await pilot.pause(0.12)
        await pilot.pause(0.4)

    async def search(query: str, key: str) -> None:
        await pilot.press("slash")
        q = app.query_one("#q")
        q.value = ""
        await type_text(query)
        await pilot.pause(0.9)
        await pilot.press("enter")
        await pilot.pause(0.6)
        await to_row("#search-songs", key)

    async def play_search(query: str, key: str, vis: str | None) -> str:
        await search(query, key)
        await pilot.press("enter")
        name = await playing(key)
        await visual(vis)
        return name

    async def pick(option_id: str) -> None:
        """Choose in the pop-up list that is open."""
        await pilot.pause(0.5)
        ol = app.screen.query_one("#pick")
        ol.highlighted = next(i for i in range(ol.option_count)
                              if ol.get_option_at_index(i).id == option_id)
        await pilot.pause(0.9)
        await pilot.press("enter")
        await pilot.pause(0.6)

    # The title card covers the screen while the first song starts.
    await visual("pattern")
    app.player.play_list([ix.resolve(SONGS["superhero"])])
    await playing(SONGS["superhero"])
    shooter = asyncio.create_task(capture())
    try:
        await wait(INTRO)
        cap("KEYGEN", "Rez & Kenet · Unreeeal Superhero 3",
            "The keygen tune of all keygen tunes, 1996 · Fasttracker 2 · the pattern as it plays")
        await wait(9)

        # ---- collections ----
        cap("FEATURE", "Three collections, one player",
            "Modland, HVSC and Keygen Music: 497,793 songs · c shows one collection, or all")
        for _ in range(4):
            if app._collection == "keygenmusic":
                break
            await pilot.press("c")
            await pilot.pause(0.9)
        app.query_one("#formats").focus()
        await highlight("#formats", "Fasttracker 2")
        await highlight("#artists", "Razor1911", then_enter=False)
        await wait(2)
        for _ in range(4):
            if app._collection == "":
                break
            await pilot.press("c")
            await pilot.pause(0.5)

        name = await play_search("dubmood fff keygen", SONGS["dubmood"], "channels")
        cap("KEYGEN", "Dubmood · FFF keygen intro",
            f"Search as you type · the channel bars · played by {name}")
        await wait(9)

        # ---- the platform view: the Amiga ----
        await pilot.press("1")
        cap("VIEW", "Browse by platform",
            "b: all the music, by platform or by sound chip · each collection keeps its layout")
        await pilot.press("b")
        await pick("platform")
        await pick("amiga")
        await wait(1.2)
        await highlight("#formats", "modland\x1fProtracker")
        await highlight("#artists", "Captain")
        await to_row("#browse-songs", SONGS["debris"], steps=6)
        await pilot.press("enter")
        name = await playing(SONGS["debris"])
        await visual("pattern")
        cap("FORMAT", "Amiga · Protracker",
            f"Captain – Space Debris, 1991 · four channels of 8-bit samples · {name}")
        await wait(11)

        # ---- the chip view: the SID 6581 ----
        cap("VIEW", "Browse by sound chip",
            "SID, Paula, YM2612, OPL... and the SID 6581 or 8580, as each C64 tune says itself")
        await pilot.press("b")
        await pick("chip")
        await pick("sid6581")
        await wait(1.2)
        await highlight("#formats", "modland\x1fHVSC")
        await highlight("#artists", "Rob Hubbard")
        await to_row("#browse-songs", SONGS["monty"], steps=6)
        await pilot.press("enter")
        name = await playing(SONGS["monty"])
        await visual("scope")
        cap("FORMAT", "Commodore 64 · SID 6581",
            f"Rob Hubbard – Monty on the Run, 1985 · one of 25,299 tunes for the first C64s · {name}")
        await wait(11)

        # ---- search by chip ----
        name = await play_search("go straight chip:ym2612", SONGS["gostraight"], "spectrum")
        cap("FORMAT", "Sega Mega Drive · YM2612",
            f"Yuzo Koshiro – Streets of Rage 2: Go Straight · found with chip:ym2612 · {name}")
        await wait(10)

        # ---- subsongs ----
        name = await play_search("mega man 2 manami", SONGS["megaman"], "scope")
        cap("FEATURE", "Subsongs",
            f"Mega Man 2 · one NSF file holds all 24 songs · ] and [ step through them · {name}")
        await wait(3)
        for _ in range(2):
            await pilot.press("right_square_bracket")
            await wait(2.8)

        # ---- themes and visualisers ----
        name = await play_search("turrican 2 desert", SONGS["turrican"], "spectrum")
        cap("FORMAT", "Amiga · TFMX",
            f"Chris Hülsbeck – Turrican II: the desert, 1991 · played by {name}")
        await wait(7)
        for theme_name, title, line in (
                ("amiga-workbench-1.3", "Workbench 1.3", "The Amiga's own blue and orange"),
                ("amiga-workbench-3.1", "Workbench 3.1", "Grey and blue, 1994"),
                ("amiga-copper", "Copper", "The colour bars of a thousand demos"),
                ("amiga-guru-meditation", "Guru Meditation", "Software failure. Press left "
                 "mouse button to continue."),
                ("textual-dark", "And Textual's own", "Every theme of the terminal library too")):
            app.theme = theme_name
            cap("THEME", title, line + " · Settings, or Ctrl+P")
            await wait(2.6)
        for mode in ("scope", "channels", "pattern", "spectrum"):
            await visual(mode)
            cap("FEATURE", "Visualisers", "v: spectrum, oscilloscope, channels, tracker pattern, off")
            await wait(1.5)

        # ---- songs in more than one collection ----
        cap("FEATURE", "One song, several collections",
            "Shown once, with every collection that has it · → opens it, ← closes")
        await search("hubbard monty on the run", SONGS["monty"])
        await wait(1.0)
        await pilot.press("right")
        await wait(2.3)
        await pilot.press("left")
        await wait(0.8)
        await pilot.press("f")

        cap("FEATURE", "What the file says",
            "i: the Info tab · title, composer, the file's comments, instrument names")
        await pilot.press("i")
        await wait(4)
        cap("FEATURE", "The artist page",
            "m: every song of the artist, in every format and collection, and who they worked with")
        await pilot.press("m")
        await wait(4.5)

        cap("FEATURE", "Queue, favorites, playlists",
            "f favorite · a queue · N plays next · l adds to a playlist · all saved as you go")
        await pilot.press("3")
        await wait(2.2)
        await pilot.press("4")
        await wait(2.2)

        cap("FEATURE", "Save and export",
            "s keeps a song, an album, an artist or a whole collection on this computer · "
            "e exports to FLAC, MP3 or Ogg at even loudness")
        await pilot.press("1")
        await wait(0.6)
        await pilot.press("s")
        await wait(2.6)
        await pilot.press("escape")
        await wait(0.4)
        await pilot.press("e")
        await wait(2.6)
        await pilot.press("escape")
        await wait(0.5)

        # ---- small terminals ----
        for size, line in ((SMALL[0], "80 × 24, the classic terminal: the lists make room"),
                           (SMALL[1], "64 × 20: still all there"),
                           (SIZE, f"{SIZE[0]} × {SIZE[1]} again")):
            await pilot.resize_terminal(*size)
            cap("TERMINAL", "Any terminal size", line)
            await wait(4)

        await visual(None)      # Stats and Settings get the whole screen
        await pilot.press("9")
        cap("FEATURE", "Statistics",
            "Most played songs, artists and formats · these are the songs of this video")
        await wait(4.5)
        await pilot.press("0")
        cap("FEATURE", "Settings",
            "Collections, catalog updates, saving at night or at a speed, loudness, themes, keys")
        await wait(4.5)
        captions[-1].end = now()
        await pilot.press("1")
        await visual("spectrum")
        await wait(OUTRO)
    finally:
        shooter.cancel()


# ---- drawing ----------------------------------------------------------------------

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
        color = KICKERS.get(cap.kicker, BLUE)
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
        shapes.append(f'<rect x="{x:.1f}" y="{base + 6:.1f}" width="{bw:.1f}" '
                      f'height="{bh * 0.28:.1f}" fill="url(#heat)" opacity="0.14"/>')

    def text(y: float, size: float, fill: str, words: str, **more) -> str:
        extra = " ".join(f'{k.replace("_", "-")}="{v}"' for k, v in more.items())
        family = SANS
        return (f'<text x="{w / 2}" y="{total_h * y:.0f}" text-anchor="middle" '
                f'font-family="{family}" font-size="{size}" fill="{fill}" {extra}>'
                f'{esc(words)}</text>')
    if kind == "intro":
        words = (text(0.12, 19, ORANGE, "KEYGENS · CRACKTROS · 40 YEARS OF CHIP MUSIC",
                      letter_spacing=6)
                 + text(0.25, 112, INK, "ByteBandit Music Player", font_weight=800,
                        letter_spacing=3)
                 + text(0.33, 27, MUTED, "Modland · HVSC · Keygen Music — 497,793 songs, "
                                         "423 formats, in your terminal"))
    else:
        words = ""
        for i, (big, small) in enumerate((("497,793", "songs"), ("41", "platforms"),
                                          ("38", "sound chips"))):
            x = w * (0.2 + 0.3 * i)
            words += (f'<text x="{x:.0f}" y="{total_h * 0.20:.0f}" text-anchor="middle" '
                      f'font-family="{SANS}" font-weight="800" font-size="88" fill="{INK}">'
                      f'{big}</text><text x="{x:.0f}" y="{total_h * 0.25:.0f}" '
                      f'text-anchor="middle" font-family="{MONO}" font-size="18" '
                      f'letter-spacing="3" fill="{ORANGE}">{small.upper()}</text>')
        words += text(0.34, 24, MUTED, "Amiga · C64 · Atari ST · Spectrum · MSX · X68000 · "
                                       "PC-98 · NES · SNES · Mega Drive · keygens · and the rest")
    return (f'<g opacity="{opacity:.3f}"><rect width="{w}" height="{total_h}" fill="{NAVY}"/>'
            f'{"".join(shapes)}{words}</g>')


def compose(frame: Frame, captions: list[Caption], start: float, total: float,
            spectrum, canvas: tuple[float, float]) -> str:
    """One frame of the video: the screen (a smaller terminal in the middle of the
    normal one's place, with its edge drawn), the caption band, the title and end cards."""
    inner = re.sub(r"^<\?xml[^>]*\?>\s*", "", frame.svg)
    w, h = map(float, re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', inner).groups())
    W, H = canvas
    x, y = (W - w) / 2, (H - h) / 2
    inner = inner.replace("<svg ", f'<svg x="{x}" y="{y}" width="{w}" height="{h}" '
                          'xml:space="preserve" ', 1)
    edge = ""
    if (w, h) != (W, H):
        edge = (f'<rect x="{x - 6}" y="{y - 6}" width="{w + 12}" height="{h + 12}" rx="8" '
                f'fill="none" stroke="{GREEN}" stroke-width="2" opacity="0.8"/>'
                f'<text x="{x + w + 6}" y="{y - 14}" text-anchor="end" font-family="{MONO}" '
                f'font-size="18" fill="{GREEN}">a smaller terminal</text>')
    t = frame.t - start
    cap = next((c for c in captions if c.begin - start <= t < c.end - start), None)
    if cap:
        cap = Caption(cap.begin - start, cap.kicker, cap.title, cap.line, cap.end - start)
    card = ""
    if t < INTRO:
        card = draw_card(W, H + BAND, 1 - ease((t - (INTRO - FADE)) / FADE),
                         spectrum.at(frame.t), "intro")
    elif t > total - OUTRO:
        card = draw_card(W, H + BAND, ease((t - (total - OUTRO)) / FADE),
                         spectrum.at(frame.t), "outro")
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H + BAND}">'
        f'<defs><linearGradient id="heat" gradientUnits="userSpaceOnUse" x1="0" x2="0" '
        f'y1="{(H + BAND) * 0.86:.0f}" y2="{(H + BAND) * 0.43:.0f}">'
        f'<stop offset="0" stop-color="#2f7de1"/><stop offset="0.55" stop-color="#c850c0"/>'
        f'<stop offset="1" stop-color="{ORANGE}"/></linearGradient></defs>'
        f'<rect width="{W}" height="{H + BAND}" fill="{NAVY}"/>{inner}{edge}'
        f'{draw_band(W, H, cap, t, t / total, frame.visual)}{card}</svg>')


# ---- main -------------------------------------------------------------------------

def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "showcase.mp4").resolve()
    work = WORK
    from bbmp.backends.base import RATE
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
    songs: list[tuple[float, str]] = []
    rec = Recorder(time.monotonic())
    library = Library(work / "user.db")
    library.COUNTED_SECONDS = 5   # the clips are short; let them count for the Stats tab

    async def run():
        app = ModlandApp(index, fetcher, library, rec, settings=Settings(resume=False),
                         session_path=work / "session.json")
        # as the pop-up shows it; nothing is exported (Esc)
        app.export_dir = Path.home() / "Music/ByteBandit Music Player/Exports"
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
    normal = next(f for f in frames)
    canvas = tuple(map(float, re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"',
                                        normal.svg).groups()))
    lines = []
    for i, frame in enumerate(frames):
        svg = compose(frame, captions, start, total, _Shift(spectrum, start), canvas)
        (work / f"f{i:05d}.svg").write_text(box_font(svg))
        nxt = frames[i + 1].t if i + 1 < len(frames) else frame.t + 1 / FPS
        lines += [f"file 'f{i:05d}.png'", f"duration {max(0.01, nxt - frame.t):.3f}"]
    lines.append(f"file 'f{len(frames) - 1:05d}.png'")
    (work / "frames.txt").write_text("\n".join(lines) + "\n")
    subprocess.run(["bash", "-c", f"cd {work} && ls f*.svg | xargs -P {os.cpu_count()} -I{{}} "
                    "sh -c 'rsvg-convert -w 1920 \"$1\" -o \"${1%.svg}.png\"' _ {}"], check=True)

    print("encoding...")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0",
                    "-i", str(work / "frames.txt"), "-i", str(work / "audio.wav"),
                    "-vf", "fps=30,scale=1920:-2,format=yuv420p",
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
