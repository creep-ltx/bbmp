#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make a video to listen to formats by: a snippet of one or two songs of each.

    tools/record_formats.py out.mp4 "Musicline Editor" "SoundSmith" ...
    tools/record_formats.py out.mp4 --backends musicline soundsmith ...

Every snippet is played by the player's own code (the backend the format is given in
backends/formats.py), so what is heard is what the player plays. The picture shows
what is playing and a spectrum of it; an index with the times of all snippets is
written next to the video. Nothing is played through the speakers.
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from bbmp.backends import Unsupported, backend_for, open_song  # noqa: E402
from bbmp.backends.base import RATE  # noqa: E402
from bbmp.fetch import Fetcher  # noqa: E402
from bbmp.index import Index, Song  # noqa: E402

W, H, FPS = 960, 540, 30
FONT = "DejaVu Sans"


def snippet(ix: Index, fetcher: Fetcher, song: Song, seconds: float, out: Path) -> dict | None:
    """The song's first subsong that sounds, from a little way in. None if it can't be played."""
    try:
        file = fetcher.get(song.key, ix.companions(song.key))
        d = open_song(file, song.format, ix.lengths(song.key))
    except (Unsupported, OSError, EOFError) as e:
        print(f"    can't play {song.path}: {e}", file=sys.stderr)
        return None
    try:
        length = d.info.duration or 0
        # past the first bars, where songs are often thin; short songs from the start
        start = 12.0 if length > 60 or not length else 0.0
        if start and not d.seek(start):
            start = 0.0
        pcm = d.read(int(seconds * RATE))
        info = d.info
        backend = d.backend
    finally:
        d.close()
    if len(pcm) < RATE * 4 * 3:
        print(f"    {song.path}: under 3 seconds of sound", file=sys.stderr)
        return None
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)
    return {"backend": backend, "start": start, "seconds": len(pcm) / (RATE * 4),
            "length": length, "subsongs": info.subsongs, "type": info.format}


def choose(ix: Index, fmt: str, seed: int) -> list[Song]:
    """The format's songs in the order to try them: ones with a named artist first."""
    songs = [Song(*row) for row in ix._all(
        "SELECT id, path, format, artist, album, name, collection FROM files"
        " WHERE hidden = 0 AND format = ?", (fmt,))]
    random.Random(seed).shuffle(songs)
    songs.sort(key=lambda s: s.artist in ("", "- unknown"))
    return songs


def mmss(t: float) -> str:
    return f"{int(t) // 60}:{int(t) % 60:02d}"


def clip(wav: Path, texts: dict, out: Path, tmp: Path, n: int) -> None:
    """One snippet as a video: what is playing in words, and a spectrum of it."""
    files = {}
    for key, text in texts.items():
        if key.startswith("_"):
            continue
        f = tmp / f"{n}_{key}.txt"
        f.write_text(text or " ")
        files[key] = str(f).replace("\\", "\\\\").replace(":", "\\:")

    def draw(key: str, y: int, size: int, colour: str, x: str = "60") -> str:
        # expansion=none: a "%" in a caption is a percent sign and no command
        return (f"drawtext=font='{FONT}':textfile='{files[key]}':expansion=none:x={x}:y={y}"
                f":fontsize={size}:fontcolor={colour}")
    graph = (
        # the spectrum in grey, coloured by level, so that mono and stereo songs look alike
        f"[0:a]showcqt=s={W}x{H - 240}:fps={FPS}:bar_g=5:sono_g=3:axis=0:count=2,"
        f"format=gray,format=yuv444p,pseudocolor=preset=magma[cqt];"
        f"color=c=0x0b1420:s={W}x{H}:r={FPS}[bg];"
        f"[bg][cqt]overlay=0:240:shortest=1,"
        + draw("format", 30, 46, "0xffb04a") + ","
        + draw("song", 96, 28, "0xf2f5fa") + ","
        + draw("how", 142, 20, "0x9fb0c8") + ","
        + draw("note", 178, 20, "0xd8c08a") + ","
        + draw("count", 40, 24, "0x9fb0c8", x="w-tw-60") + "[v]")
    seconds = float(texts["_seconds"])
    subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-i", str(wav), "-filter_complex",
         graph + f";[0:a]afade=t=in:d=0.15,afade=t=out:st={max(0.0, seconds - 0.6):.2f}:d=0.6[a]",
         "-map", "[v]", "-map", "[a]", "-c:v", "libx264", "-preset", "medium", "-crf", "28",
         "-pix_fmt", "yuv420p", "-r", str(FPS), "-c:a", "aac", "-b:a", "160k", "-ar", str(RATE),
         "-t", f"{seconds:.2f}", str(out)], check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("formats", nargs="*", help="formats as the player shows them")
    ap.add_argument("--backends", nargs="*", default=[],
                    help="every format these backends are the replayer of")
    ap.add_argument("--seconds", type=float, default=12.0, help="length of a snippet")
    ap.add_argument("--two-from", type=int, default=100,
                    help="formats with this many songs or more get two snippets")
    ap.add_argument("--cache", type=Path, default=Path.home() / ".cache/bbmp/samples")
    ap.add_argument("--seed", type=int, default=3)
    ap.add_argument("--notes", type=Path, help="a file of lines 'Format: note' to show with a format")
    args = ap.parse_args()

    ix, fetcher = Index(), Fetcher(args.cache)
    counts = {f: n for f, n, _ in ix.formats()}
    formats = list(args.formats)
    formats += [f for f in sorted(counts) if backend_for(f) in args.backends and f not in formats]
    unknown = [f for f in formats if f not in counts]
    if unknown:
        print("no such format: " + ", ".join(unknown), file=sys.stderr)
        return 2
    notes = {}
    if args.notes:
        for line in args.notes.read_text().splitlines():
            fmt, sep, note = line.partition(": ")
            if sep:
                notes[fmt.strip()] = note.strip()

    tmp = Path(tempfile.mkdtemp(prefix="modland-formats-", dir=args.out.parent))
    plan = []
    for fmt in formats:
        want = 2 if counts[fmt] >= args.two_from else 1
        print(f"{fmt} ({counts[fmt]} songs)")
        got = 0
        for tried, song in enumerate(choose(ix, fmt, args.seed)):
            if tried >= 60:
                break
            wav = tmp / f"{len(plan):03d}.wav"
            made = snippet(ix, fetcher, song, args.seconds, wav)
            if made is None:
                continue
            plan.append((fmt, song, made, wav))
            got += 1
            if got == want:
                break
        if not got:
            print(f"    NOTHING of {fmt} could be played", file=sys.stderr)
    fetcher.close()
    if not plan:
        return 1

    index, clips, at = [], [], 0.0
    for n, (fmt, song, made, wav) in enumerate(plan):
        artist = song.artist if song.artist not in ("", "- unknown") else "artist unknown"
        how = f"played by {made['backend']}"
        if made["type"] and made["type"] != fmt:
            how += f"  ·  the file says: {made['type']}"
        where = f"from {mmss(made['start'])}"
        if made["length"]:
            where += f" of {mmss(made['length'])}"
        if made["subsongs"] > 1:
            where += f"  ·  subsong 1 of {made['subsongs']}"
        texts = {"format": fmt, "song": f"{song.title}  —  {artist}", "how": f"{how}  ·  {where}",
                 "note": notes.get(fmt, ""), "count": f"{n + 1} / {len(plan)}   {mmss(at)}",
                 "_seconds": made["seconds"]}
        out = tmp / f"{n:03d}.mp4"
        clip(wav, {k: v for k, v in texts.items()}, out, tmp, n)
        clips.append(out)
        index.append(f"{mmss(at)}  {fmt}: {song.title} ({artist})  [{made['backend']}]  {song.path}")
        at += made["seconds"]

    listing = tmp / "clips.txt"
    listing.write_text("".join(f"file '{c}'\n" for c in clips))
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", str(listing),
                    "-c", "copy", "-movflags", "+faststart", str(args.out)], check=True)
    args.out.with_suffix(".txt").write_text("\n".join(index) + "\n")
    for f in tmp.iterdir():
        f.unlink()
    tmp.rmdir()
    print(f"{args.out}: {len(plan)} snippets, {mmss(at)}; index in {args.out.with_suffix('.txt')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
