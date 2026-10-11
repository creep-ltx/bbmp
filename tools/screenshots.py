#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Screenshots of the player for showing it: the real app, headless, with real songs
playing (into memory, never the speakers), saved as SVG and PNG.

    PYTHONPATH=src python3 tools/screenshots.py [folder]      (default: screenshots/)

Settings, favorites and statistics live in a temporary folder; nothing is saved (the
Settings tab shows the saved music's usual folder, which nothing here writes to); songs
are downloaded into the normal cache. Needs rsvg-convert.
"""

from __future__ import annotations

import asyncio
import re
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

WORK = Path(tempfile.mkdtemp(prefix="bbmp-shots-", dir=Path.home() / ".cache/bbmp/work"))

sys.path.insert(0, str(Path(__file__).resolve().parent))
from record_demo import Recorder  # noqa: E402

SIZE = (132, 40)
SONGS = {
    "debris": "modland:Protracker/Captain/space_debris.mod",
    "superhero": "modland:Fasttracker 2/Rez/coop-Kenet/unreeeal superhero 3.xm",
    "monty": "modland:HVSC/MUSICIANS/H/Hubbard_Rob/Monty_on_the_Run.sid",
    "gostraight": "modland:Video Game Music/Sega Megadrive/YK-2/Streets Of Rage 2/02 go straight.vgz",
    "turrican": "modland:TFMX/Chris Huelsbeck/mdat.turrican 2 level 1-desert",
    "elysium": "modland:Protracker/Jester/elysium.mod",
    "beyond": "zxart:19636/MmcM_-_Beyond_Time_(2013)_(Chaos_Constructions_2013,_1).mt3",
    "chrono": "modland:Nintendo SPC/Yasunori Mitsuda/Chrono Trigger/102 - chrono trigger.spc",
}


async def shots(app, pilot, out: Path) -> None:
    ix = app.index

    def save(name: str) -> None:
        (out / f"{name}.svg").write_text(app.export_screenshot())
        print("  ", name)

    async def visual(mode):
        vis = app.query_one("#visual")
        for _ in range(6):
            if vis.mode == mode:
                return
            await pilot.press("v")

    async def play(key: str, seconds: float = 6.0, at: float = 0) -> None:
        app.player.play_list([ix.resolve(key)])
        for _ in range(300):
            st = app.player.status()
            if st.state == "playing" and st.song and st.song.key == key and st.elapsed > 1:
                break
            await pilot.pause(0.1)
        if at:
            app.player.seek(at)         # where the song is in full swing
        await pilot.pause(seconds)

    async def highlight(list_id: str, option_id: str, enter: bool = True) -> None:
        ol = app.query_one(list_id)
        for _ in range(50):
            ids = [ol.get_option_at_index(i).id for i in range(ol.option_count)]
            if option_id in ids:
                break
            await pilot.pause(0.1)
        ol.highlighted = ids.index(option_id)
        ol.scroll_to_highlight(top=True)
        await pilot.pause(0.6)
        if enter:
            await pilot.press("enter")
            await pilot.pause(0.4)

    async def to_row(table_id: str, key: str) -> None:
        table = app.query_one(table_id)
        for _ in range(50):
            rows = [i for i, s in enumerate(table.songs) if s.key == key]
            if rows:
                break
            await pilot.pause(0.1)
        table.focus()
        table.move_cursor(row=rows[0])
        await pilot.pause(0.4)

    async def browse(*path, cursor=None) -> None:
        """Browse to a path of steps (browse.py), the cursor on an entry of the last list."""
        app._placed = False
        app._browse_to(list(path), cursor)
        for _ in range(100):
            if app._placed:
                break
            await pilot.pause(0.05)
        await pilot.pause(0.6)

    async def search(query: str) -> None:
        await pilot.press("slash")
        app.query_one("#q").value = query
        await pilot.pause(0.3)
        await pilot.press("enter")
        await pilot.pause(1.2)

    async def pick(option_id: str) -> None:
        await pilot.pause(0.4)
        ol = app.screen.query_one("#pick")
        ol.highlighted = next(i for i in range(ol.option_count)
                              if ol.get_option_at_index(i).id == option_id)
        await pilot.pause(0.3)
        await pilot.press("enter")
        await pilot.pause(0.6)

    # 1. Browse, Space Debris, the tracker pattern
    await visual("pattern")
    await play(SONGS["debris"], 2)
    await browse(("collection", "modland"), ("menu", "formats"),
                 ("format", "Protracker"), ("artist", "Captain"))
    await to_row("#browse-songs", SONGS["debris"])
    await pilot.pause(4)
    save("01-browse-amiga-protracker-pattern")

    # 2. Keygen Music, channel bars
    await visual("channels")
    await browse(("collection", "keygenmusic"), ("menu", "formats"),
                 ("format", "Fasttracker 2"), cursor=("artist", "Razor1911"))
    await play(SONGS["superhero"], 5, at=45)
    save("02-keygen-music-channels")

    # 3. The Amiga, by platform
    await browse(("collection", ""), ("menu", "platforms"), ("platform", "amiga"), ("format", "modland\x1fTFMX"),
                 ("artist", "Chris Huelsbeck"))
    await to_row("#browse-songs", SONGS["turrican"])
    await visual("spectrum")
    await play(SONGS["turrican"], 5)
    save("03-platform-view-amiga-tfmx")

    # 4. The SID 6581, by chip
    await browse(("collection", ""), ("menu", "chips"), ("chip", "sid6581"), ("format", "modland\x1fHVSC"),
                 ("artist", "Rob Hubbard"))
    await to_row("#browse-songs", SONGS["monty"])
    await visual("scope")
    await play(SONGS["monty"], 5)
    save("04-chip-view-sid-6581-scope")
    await browse(cursor=("collection", "modland"))     # All and the collections
    save("04b-browse-collections")
    await browse(("collection", ""), cursor=("menu", "formats"))
    save("04c-browse-all")

    # 5. Search with a chip, Streets of Rage 2
    await visual("spectrum")
    await search("streets of rage 2 chip:ym2612")
    await play(SONGS["gostraight"], 6)
    save("05-search-chip-ym2612-spectrum")

    # 6. A song several collections have, opened
    await search("hubbard monty on the run")
    table = app.query_one("#search-songs")
    table.focus()
    table.move_cursor(row=0)
    await pilot.press("right")
    await pilot.pause(1)
    save("06-one-song-several-collections")

    # 7. ZX-Art, the Spectrum
    await visual("spectrum")
    await search("beyond time mmcm")
    await play(SONGS["beyond"], 6)
    save("07-zx-art-spectrum-pt3")

    # 8. The Info tab, the artist page
    await visual("spectrum")
    await play(SONGS["elysium"], 3)
    await pilot.press("i")
    await pilot.pause(1.5)
    save("08-info-tab-instrument-names")
    await pilot.press("m")
    await pilot.pause(2)
    save("09-artist-page")

    # 9. Browse: Jester's songs, Elysium playing
    await visual("spectrum")
    await browse(("collection", "modland"), ("menu", "formats"),
                 ("format", "Protracker"), ("artist", "Jester"))
    await to_row("#browse-songs", SONGS["elysium"])
    await pilot.press("enter")          # Elysium again, from the list
    for _ in range(100):
        st = app.player.status()
        if st.state == "playing" and 0 < st.elapsed < 5:
            break
        await pilot.pause(0.1)
    await pilot.pause(1)
    app.player.seek(30)
    await pilot.pause(4)
    save("10-browse-jester-elysium")

    # 10. Queue, statistics, settings, help
    app.player.enqueue([ix.resolve(SONGS[k]) for k in ("debris", "monty", "gostraight", "beyond")])
    await pilot.press("3")
    await pilot.pause(1.5)
    save("11-queue")
    await visual(None)
    await pilot.press("9")
    await pilot.pause(1.5)
    save("12-statistics")
    await pilot.press("0")
    await pilot.pause(1.5)
    save("13-settings")
    await pilot.press("question_mark")
    await pilot.pause(1.5)
    save("14-help")

    # 11. A small terminal
    await pilot.press("1")
    await visual("spectrum")
    await pilot.resize_terminal(80, 24)
    await pilot.pause(2)
    save("15-small-terminal-80x24")
    await pilot.resize_terminal(*SIZE)
    await pilot.pause(1)



# Textual's SVGs ask for Fira Code, else monospace; this machine's monospace (Noto Sans
# Mono) lacks the rounded frame corners, which then come from a wide font and stand off
# the frame. Meslo has them all.
# Its letters are also a hair narrower (0.602 of the font size) than the cells Textual
# places them in (0.61), and rsvg-convert doesn't stretch a line to its cells, so every
# frame line stopped short of its right-hand corner: the difference goes between letters.
BOX_FONT = "MesloLGSDZ Nerd Font Mono"
BOX_FONT_WIDTH = 1233 / 2048          # its advance, in ems
CELL_WIDTH = 0.61                     # Textual's (Rich's export_svg font_aspect_ratio)


def box_font(svg: str) -> str:
    svg = svg.replace("Fira Code, monospace", f"{BOX_FONT}, monospace")
    # rsvg-convert drops a text's leading spaces ("Modland  493,396" became "Modland493,396")
    # unless whitespace is preserved, as record_demo.py does
    if 'xml:space="preserve"' not in svg:
        svg = svg.replace("<svg ", '<svg xml:space="preserve" ', 1)

    def spaced(m: re.Match) -> str:
        size = float(m.group(1))
        return f"{m.group(0)}\n        letter-spacing: {size * (CELL_WIDTH - BOX_FONT_WIDTH):.3f}px;"
    return re.sub(r"(?<=monospace;\n)\s*font-size: ([0-9.]+)px;", spaced, svg, count=1)


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "screenshots").resolve()
    out.mkdir(parents=True, exist_ok=True)
    from bbmp.fetch import Fetcher
    from bbmp.index import Index
    from bbmp.library import Library
    from bbmp.settings import Settings
    from bbmp.tui import ModlandApp, quiet_stderr

    index, fetcher = Index(), Fetcher()
    for key in SONGS.values():
        if index.resolve(key) is None:
            raise SystemExit(f"not in the catalog: {key}")
        fetcher.get(key, index.companions(key))
    library = Library(WORK / "user.db")
    library.COUNTED_SECONDS = 3
    rec = Recorder(time.monotonic())

    async def run():
        app = ModlandApp(index, fetcher, library, rec, settings=Settings(resume=False),
                         session_path=WORK / "session.json")
        app.export_dir = Path.home() / "Music/ByteBandit Music Player/Exports"
        async with app.run_test(size=SIZE) as pilot:
            await pilot.pause(0.5)
            await shots(app, pilot, out)
            app.player.stop()
            await pilot.pause(0.3)

    print("taking screenshots...")
    with quiet_stderr():
        asyncio.run(run())
    for svg in sorted(out.glob("*.svg")):
        svg.write_text(box_font(svg.read_text()))
        subprocess.run(["rsvg-convert", "-w", "2400", str(svg), "-o", str(svg.with_suffix(".png"))],
                       check=True)
    print(f"wrote {len(list(out.glob('*.png')))} screenshots to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
