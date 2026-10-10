#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find out which replayer plays each format, and how many of its songs play.

    tools/format_census.py [--songs 40] [--only "TFMX,SC68"] [--again] [--choose]
                           [--from hvsc]

For every format a sample of its songs is downloaded (all of them, for a format that
has no more than --songs): Modland's songs where Modland has the format, else those of
the collections that have it (ZX-Art's Spectrum formats, ASMA's TIATracker). --from
takes the formats of one collection, as it files them, and its songs only (HVSC's
three groups). Set BBMP_MIRROR to a home mirror so that the archives' servers are
not asked; ZX-Art, which isn't mirrored, gives at most ZXART_SONGS songs a format, one
every ZXART_PAUSE seconds, and what the player has cached of it is used first.
Two things are done with the songs:

1. **The replayer is chosen**, for a format that has none yet (or, with --choose, for
   every format): every replayer is tried on the first songs, and the one that plays
   most of them is the format's. A replayer of the player's own that plays at least
   half of them is preferred, because it was made for the format.
2. **The songs are counted**: each is opened as the player opens it and played for 20
   seconds. A song counts as

       plays    sound that is neither silence nor noise
       silent   the replayer plays it and nothing is heard in those 20 seconds
       noise    the sound looks like noise, or is clipped for a fifth of the time
       failed   no replayer takes it; the reason is kept

The result is kept in tools/format_census.json; formats that are in it are not done
again without --again. From it are made: backends/formats.py (which replayer plays
which format), and, with the notes in docs/format-notes.toml, docs/formats.md and
what the player shows about formats (tools/format_status.py does that part).

Every format runs in a process of its own, so a replayer that crashes takes one
format's count with it and not the census. Formats that need an extra file are
measured with it: see `bbmp extras`, and BBMP_EXTRAS for a folder that has the
files. Nothing is played through the speakers.
"""

from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "src"))
sys.path.insert(0, str(HERE))
from try_helper import judge  # noqa: E402

from bbmp import backends  # noqa: E402
from bbmp import ending  # noqa: E402
from bbmp.backends import OPENERS, Unsupported, open_song  # noqa: E402
from bbmp.backends.base import RATE  # noqa: E402
from bbmp.backends.pipe import OWN  # noqa: E402
from bbmp import collection as collections_  # noqa: E402
from bbmp import paths  # noqa: E402
from bbmp.fetch import Fetcher, link_or_copy  # noqa: E402
from bbmp.index import Index  # noqa: E402

SECONDS = 20
# ZX-Art's files are not mirrored, and its site is asked gently
ZXART_SONGS = 5
ZXART_PAUSE = 2.0
GENTLE = {"zxart": (ZXART_SONGS, ZXART_PAUSE)}      # collection: (songs, seconds between)
CHOOSE_FROM = 6         # songs a replayer is chosen by
OUT = HERE / "format_census.json"
FORMATS = HERE.parent / "src/bbmp/backends/formats.py"
SPDX = "# SPDX-License-Identifier: GPL-3.0-or-later\n"   # the files it writes are the player's
# Replayers of the player's own come first: each plays one format and refuses everything
# else. On a tie between the others, the first wins.
PREFERENCE = (*OWN, "openmpt", "uade", "gme", "sidplayfp", "zxtune", "vgm", "adplug", "ym", "sndh",
              "mdx", "pmdc", "sc68", "fmp", "kss", "eup", "organya", "pxtone", "furnace", "qsf",
              "snsf", "sunvox", "xmp", "ixalance", "klystrack")


# Formats where the replayer is known, and the census can't hear the difference: a
# Startrekker AM song makes some sound in any MOD player, but its AM synth
# instruments (in the .nt file beside it) only in libxmp.
CHOSEN = {"Startrekker AM": "xmp",
          "Slight Atari Player": "zxtune",   # all SAP types, stereo, no clipping (29 Sep 2026)
          "KSS": "kss",
          "AY STRC": "strc",
          "Monotone": "monotone",
          "Digitrekker": "digitrekker",
          "Vic-Tracker": "victracker",
          "BoyScout": "boyscout",
          "Ad Lib - Pixel Painters FMF": "pixelpainters",
          "Ad Lib - Pixel Painters FTF": "pixelpainters",
          "Ad Lib - EdLib D01": "d01",
          "Ad Lib - Sound Images Generation 2": "sig",
          "Ad Lib - Sound Interface System": "lem",
          "Megadrive CYM": "cym",          # the log made into VGM (1 Oct 2026)
          "Pollytracker": "polly",         # the module behind HVSC's Polly player (1 Oct 2026)   # MoonBlaster and FM-PAC rips, and the songs a file really has (29 Sep 2026)
          # (2 Oct 2026) ZXTune leaves some of these silent or refuses them; these
          # play every song tried: Furnace all 40 TFM songs (ZXTune 36), libxmp all
          # 40 Music Editor songs (ZXTune 37), all 13 Liquid Tracker songs (ZXTune 11)
          # and all 5 Audio Sculpture songs with their .as synth instruments (libopenmpt
          # 1, silent without them); UADE the old two-file MusicMaker V8 songs (.sdata
          # with its .ip instruments), which ZXTune doesn't know
          "TFM Music Maker": "furnace",
          "Music Editor": "xmp",
          "Liquid Tracker": "xmp",
          "Audio Sculpture": "xmp",
          "MusicMaker V8 Old": "uade"}

# Formats no replayer here plays, though one makes sound of their files. ZXTune
# finds the instruments' samples inside a Renoise song (a zip) and plays those one
# after the other: the census heard sound, but not the song (2 Oct 2026).
NOT_THE_SONG = {"Renoise"}


def sounds(backend: str, path: Path, lengths: list[float]) -> bool:
    """Does this replayer make sound of the file?"""
    try:
        d = OPENERS[backend](path, lengths)
    except (Unsupported, OSError):
        return False
    try:
        pcm = b"".join(d.read(RATE // 10) for _ in range(80))       # 8 s
    finally:
        d.close()
    j = judge(pcm)
    return j["seconds"] >= 2 and j["rms"] >= 30


def choose(files: list[tuple[Path, list[float]]]) -> tuple[str | None, dict[str, int]]:
    wins = {b: sum(sounds(b, f, lengths) for f, lengths in files) for b in PREFERENCE}
    best = max(PREFERENCE, key=lambda b: (wins[b], -PREFERENCE.index(b)))
    ours = [b for b in OWN if files and wins[b] * 2 >= len(files)]
    if ours:
        best = max(ours, key=lambda b: wins[b])
    return (best if wins[best] else None), {b: n for b, n in wins.items() if n}


def sample(ix: Index, fmt: str, only: str = "") -> tuple[list[str], dict[str, int]]:
    """The format's songs as keys, and how many each collection has: Modland's where
    Modland has the format (as the census always did), else every collection's that
    has it; with `only`, that collection's."""
    rows = ix._all("SELECT collection, path FROM files WHERE hidden = 0 AND format = ?"
                   " ORDER BY collection, path", (fmt,))
    has: dict[str, int] = {}
    for c, _ in rows:
        has[c] = has.get(c, 0) + 1
    take = {only} if only else ({"modland"} if "modland" in has else set(has))
    keys = [collections_.key(c, p) for c, p in rows if c in take]
    return keys, {c: n for c, n in has.items() if c in take}


def cached_elsewhere(fetcher: Fetcher, key: str) -> bool:
    """The player's own cache has the file: it is put in the census's, not downloaded."""
    collection, path = collections_.split(key)
    mine = fetcher.local_path(key)
    if mine.exists():
        return True
    theirs = paths.FILES_DIR / collection / path
    if theirs.is_file():
        mine.parent.mkdir(parents=True, exist_ok=True)
        link_or_copy(theirs, mine)
        return True
    return False


def fetch(fetcher: Fetcher, ix: Index, key: str) -> Path | None:
    """The song's file; None when the server doesn't answer, which is not the
    format's fault. FileNotFoundError when the server doesn't have it."""
    gentle = GENTLE.get(collections_.split(key)[0])
    if gentle and not all(cached_elsewhere(fetcher, k) for k in (key, *ix.companions(key))):
        time.sleep(gentle[1])
    for attempt in range(3):
        try:
            return fetcher.get(key, ix.companions(key))
        except FileNotFoundError:
            raise
        except (OSError, EOFError):
            time.sleep(3)
    return None


def one(fmt: str, songs: int, cache: Path, seed: int, known: str | None,
        only: str = "") -> dict:
    ix = Index()
    fetcher = Fetcher(cache, checksum=ix.md5)
    keys, sampled = sample(ix, fmt, only)
    total = len(keys)
    random.Random(seed).shuffle(keys)
    # a site that isn't mirrored: fewer songs, those the player has already first
    gentle = [GENTLE[c][0] for c in sampled if c in GENTLE]
    if gentle and len(sampled) == 1:
        keys.sort(key=lambda k: not cached_elsewhere(fetcher, k))
        songs = min(songs, gentle[0])
    keys = keys[:songs]

    best, wins = CHOSEN.get(fmt, known), {}
    if fmt in NOT_THE_SONG:
        best = None
    elif best is None:
        files = []
        for key in keys[:CHOOSE_FROM]:
            try:
                file = fetch(fetcher, ix, key)
            except FileNotFoundError:
                continue
            if file is not None:
                files.append((file, ix.lengths(key)))
        best, wins = choose(files)
    if best is None:
        fetcher.close()
        return {"songs": total, "best": None, "wins": wins, "tried": len(keys[:CHOOSE_FROM]),
                "plays": 0, "silent": 0, "noise": 0, "failed": len(keys[:CHOOSE_FROM]),
                "backends": {}, "trouble": [], **where(sampled)}
    # the songs are opened as the player opens them, with this replayer as the format's
    backends.BACKEND_BY_FORMAT[fmt] = best
    backends.UNSUPPORTED_FORMATS.discard(fmt)

    counts = {"plays": 0, "silent": 0, "noise": 0, "failed": 0}
    used: dict[str, int] = {}
    trouble = []
    for key in keys:
        path = song_of(key)
        state, note, backend = "failed", "", ""
        try:
            file = fetch(fetcher, ix, key)
            if file is None:
                continue
            d = open_song(file, fmt, ix.lengths(key))
            try:
                backend = d.backend
                pcm = b"".join(d.read(RATE // 10) for _ in range(SECONDS * 10))
                subsongs = d.info.subsongs
                if subsongs == 1 and judge(pcm)["rms"] < 20:
                    # the player waits SILENT_SONG seconds for a song's first sound
                    # (Oktalyzer's second halves start 20-25 seconds in): so does this
                    pcm = b"".join(d.read(RATE // 10) for _ in range(
                        int((ending.SILENT_SONG - SECONDS) * 10)))
                # a subsong that stays silent is skipped by the player: so it is here
                first = d.info.subsong
                # (HES rips often start their music at track 40 or so)
                for step in range(1, subsongs):
                    if judge(pcm)["rms"] >= 20:
                        break
                    try:
                        d.select_subsong((first + step) % subsongs)
                    except Unsupported:
                        break
                    pcm = b"".join(d.read(RATE // 10) for _ in range(30))   # 3 s, as the player
            finally:
                d.close()
            j = judge(pcm)
            if j["seconds"] < 0.2 and subsongs > 1:
                state = "plays"          # a sound effect first; the player goes on to the next
            elif j["seconds"] < 0.2:
                state, note = "failed", "opens and gives no sound"
            elif j["rms"] < 20:
                state, note = "silent", ""
            elif j["crossings"] > 0.35 or j["clipped"] > 0.2:
                state = "noise"
                note = f"crossings {j['crossings']:.2f}, clipped {j['clipped']:.0%}"
            else:
                state = "plays"
        except FileNotFoundError:
            state, note = "failed", "the file is not on the server"
        except (Unsupported, OSError, EOFError) as e:
            state, note = "failed", str(e)[:300]
        counts[state] += 1
        if backend:
            used[backend] = used.get(backend, 0) + 1
        if state != "plays":
            trouble.append({"song": path, "state": state, "note": note, "backend": backend})
    fetcher.close()
    return {"songs": total, "best": best, "wins": wins, "tried": sum(counts.values()), **counts,
            "backends": used, "trouble": trouble, **where(sampled)}


def where(sampled: dict[str, int]) -> dict:
    """What a result says of the collections its songs came from: nothing for Modland."""
    return {} if set(sampled) <= {"modland"} else {"from": sampled}


def song_of(key: str) -> str:
    """What a result calls a song: its path for Modland's, its key for the others'."""
    collection, path = collections_.split(key)
    return path if collection == "modland" else key


def write_formats(census: dict) -> None:
    """backends/formats.py: which replayer plays which format."""
    best = {f: r["best"] for f, r in sorted(census.items()) if r.get("best")}
    none = sorted(f for f, r in census.items() if "best" in r and not r["best"])
    lines = ['"""Which backend plays which Modland format.', "",
             "Generated by tools/format_census.py from real files; edit by re-running it.",
             '"""', "", "BACKEND_BY_FORMAT: dict[str, str] = {"]
    lines += [f"    {f!r}: {b!r}," for f, b in best.items()]
    lines += ["}", "", "# No backend produced sound for any of the songs that were tried.",
              "UNSUPPORTED_FORMATS: set[str] = {"]
    lines += [f"    {f!r}," for f in none]
    lines += ["}", ""]
    FORMATS.write_text(SPDX + "\n".join(lines))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--songs", type=int, default=40)
    ap.add_argument("--only", default="", help="these formats, with commas between them")
    ap.add_argument("--again", action="store_true", help="also formats that were done before")
    ap.add_argument("--choose", action="store_true", help="choose the replayer anew")
    ap.add_argument("--cache", type=Path, default=Path.home() / ".cache/bbmp/samples")
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--one", help=argparse.SUPPRESS)
    ap.add_argument("--known", help=argparse.SUPPRESS)
    ap.add_argument("--from", dest="only_from", default="", metavar="COLLECTION",
                    help="the formats of this collection, as it files them, from its songs")
    args = ap.parse_args()
    if args.one:
        print(json.dumps(one(args.one, args.songs, args.cache, args.seed, args.known or None,
                             args.only_from)))
        return 0

    census = json.loads(OUT.read_text()) if OUT.exists() else {}
    formats = [f for f, _, _ in Index().formats(args.only_from)]
    if args.only:
        formats = [f for f in formats if f in args.only.split(",")]
    todo = [f for f in formats if args.again or args.choose or f not in census]
    for n, fmt in enumerate(todo, 1):
        # (a format no census has seen goes by what the player gives it: backends/__init__)
        known = None if args.choose else ((census[fmt] or {}).get("best") if fmt in census
                                          else backends.BACKEND_BY_FORMAT.get(fmt))
        command = [sys.executable, __file__, "--one", fmt, "--songs", str(args.songs),
                   "--cache", str(args.cache), "--seed", str(args.seed),
                   *(["--from", args.only_from] if args.only_from else [])]
        try:
            done = subprocess.run(command + (["--known", known] if known else []),
                                  capture_output=True, timeout=60 * 40)
            result = json.loads(done.stdout.decode().strip().splitlines()[-1])
        except subprocess.TimeoutExpired:
            result = {"crashed": "took more than 40 minutes", "best": known}
        except (ValueError, IndexError):
            result = {"crashed": f"the process ended with {done.returncode}", "best": known}
        result.setdefault("songs", (census.get(fmt) or {}).get("songs", 0))
        # songs another tool found (tools/find_silent.py marks them "by") stay known,
        # and where the census found the same of a song, what was found out about it stays
        tried = {t["song"]: t for t in result.get("trouble", ())}
        for t in (census.get(fmt) or {}).get("trouble", ()):
            if not t.get("by"):
                continue
            mine = tried.get(t["song"])
            if mine is None:
                result.setdefault("trouble", []).append(t)
            elif mine["state"] == t["state"]:
                mine.update(note=t.get("note", ""), by=t["by"])
        census[fmt] = result
        OUT.write_text(json.dumps(census, indent=1, sort_keys=True, ensure_ascii=False) + "\n")
        if "crashed" in result:
            print(f"[{n}/{len(todo)}] {fmt}: CRASHED, {result['crashed']}", flush=True)
        elif not result["best"]:
            print(f"[{n}/{len(todo)}] {fmt}: no replayer plays it", flush=True)
        else:
            rest = ", ".join(f"{result[k]} {k}" for k in ("silent", "noise", "failed") if result[k])
            print(f"[{n}/{len(todo)}] {fmt}: {result['best']}, {result['plays']} of "
                  f"{result['tried']} play" + (f" ({rest})" if rest else ""), flush=True)

    if len(census) >= 200:          # a census of a few formats would wipe the table
        write_formats(census)
        import format_status
        format_status.main([])
        playing = sum(1 for r in census.values() if r.get("best"))
        print(f"{playing} of {len(census)} formats have a replayer")
    else:
        print(f"the census has only {len(census)} formats; formats.py is left alone")
    return 0


if __name__ == "__main__":
    sys.exit(main())
