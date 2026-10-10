# SPDX-License-Identifier: GPL-3.0-or-later
"""The Artist, Album and Info pages.

Everything shown here comes from one of three places, and says which: the catalog
of the collections (counts, formats, albums, collaborations), the music file itself (titles,
comments, instrument names), or your own listening history. There are no
biographies: scene handles are shared and reused, so any text matched to an artist by
name would often describe someone else. The web look-up keys open a search and leave
the judging to you.
"""

from __future__ import annotations

import urllib.parse
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from rich.console import Group
from rich.table import Table
from rich.text import Text

from . import archives, collection as collections_, formats_info, replayer_facts, theme
from .backends import Details, is_supported
from .index import Index, Song
from .library import Library
from .stats_view import hours

NO_ARTIST = {"", "- unknown", "unknown"}


@dataclass
class Page:
    kind: str                       # "artist" or "album"
    title: str
    songs: list[Song]
    facts: Group
    partners: list[tuple[str, int]] = field(default_factory=list)   # (name, songs together)


def _counts(title: str, counts: Counter, limit: int = 8) -> Table:
    t = Table(title=title, title_justify="left", title_style="bold", box=None, pad_edge=False,
              show_header=False, expand=True)
    t.add_column(ratio=1, no_wrap=True, overflow="ellipsis")
    t.add_column(justify="right", style="dim")
    for name, n in counts.most_common(limit):
        t.add_row(name, f"{n:,}")
    if len(counts) > limit:
        t.add_row(Text(f"and {len(counts) - limit} more", style="dim"), "")
    return t


def artist_page(index: Index, library: Library, favorites: set[str], artist: str) -> Page | None:
    if artist in NO_ARTIST:
        return None
    songs = index.artist_songs(artist)
    if not songs:
        return None
    partners: Counter = Counter()
    for s in songs:
        if s.artist == artist and s.partner:
            partners[s.partner] += 1
        elif s.artist != artist:
            partners[s.artist] += 1
    formats = Counter(s.format for s in songs)
    albums = Counter(s.album_name for s in songs if s.album_name)
    playable = sum(1 for s in songs if is_supported(s.format))

    head = Text()
    head.append(f"{len(songs):,}", style=f"bold {theme.current.accent}")
    where = Counter(collections_.name(s.collection) for s in songs)
    noun = " songs" if len(songs) != 1 else " song"
    head.append(f"{noun} on {next(iter(where))}" if len(where) == 1
                else f"{noun} in {len(where)} collections")
    if playable < len(songs):
        head.append(f" ({len(songs) - playable} in formats that can't be played)", style="dim")
    parts: list = [head, Text("")]
    if len(where) > 1:
        parts += [_counts("Collections", where), Text(""), ]
    parts.append(_counts("Formats", formats))
    if albums:
        parts += [Text(""), _counts("Albums and games", albums)]

    mine = library.artist_stats(artist)
    favs = sum(1 for s in songs if s.key in favorites)
    you = Text("\nYou\n", style="bold")
    if mine["plays"] or favs:
        you.append(f"{mine['plays']} plays · {hours(mine['seconds'])} · {favs} favorites\n", style="")
        for title, n in mine["top"]:
            you.append(f"  {title}  ", style="italic")
            you.append(f"{n}×\n", style="dim")
    else:
        you.append("Nothing played yet\n", style="dim")
    parts.append(you)
    look = Text("Look up\n", style="bold")
    look.append("w", style=f"bold {theme.current.primary}")
    look.append(" web search   ")
    look.append("d", style=f"bold {theme.current.primary}")
    look.append(" Demozoo\n")
    look.append("Counts come from the catalog of " + " and ".join(where) + ". The player knows "
                "nothing else about who this is.", style="dim")
    if len(where) > 1:
        look.append("\nThese are the songs filed under this name. The same name in two "
                    "collections can be two people.", style="dim")
    parts.append(look)
    return Page("artist", artist, songs, Group(*parts), partners.most_common())


def album_page(index: Index, song: Song) -> Page | None:
    if song.local:
        inside = archives.split(song.path)
        if inside is not None:          # a song of an archive: the archive is its album
            songs = index.archive_songs(inside[0])
            facts = Group(Text.assemble((f"{len(songs)}", f"bold {theme.current.accent}"),
                                        " songs in\n", (str(inside[0]), "dim")))
            return Page("album", inside[0].name, songs, facts) if songs else None
        folder = Path(song.path).parent
        songs = index.local_songs(folder)
        facts = Group(Text.assemble((f"{len(songs)}", f"bold {theme.current.accent}"), " music files in\n",
                                    (str(folder), "dim")))
        return Page("album", folder.name, songs, facts) if songs else None
    if not song.album:
        return None
    songs = index.album_songs(song)
    t = Text()
    t.append(f"{len(songs)}", style=f"bold {theme.current.accent}")
    t.append(" songs\n\n" if len(songs) != 1 else " song\n\n")
    t.append("Format   ", style="dim")
    t.append(f"{song.format}\n")
    if song.artist not in NO_ARTIST:
        t.append("Artist   ", style="dim")
        t.append(f"{song.artist}\n")
        t.append("\nm", style=f"bold {theme.current.primary}")
        t.append(" more by this artist\n")
    if song.partner:
        t.append("With     ", style="dim")
        t.append(f"{song.partner}\n")
    return Page("album", song.album_name or song.album, songs, Group(t))


def lookup_url(site: str, artist: str) -> str:
    q = urllib.parse.quote_plus(artist)
    if site == "demozoo":
        return f"https://demozoo.org/search/?q={q}"
    return "https://duckduckgo.com/?q=" + urllib.parse.quote_plus(f'"{artist}" music')


def info_page(song: Song, details: Details, file: Path | None, backend: str,
              subsong: tuple[int, int], width: int,
              level: tuple[float | None, float] | None = None, kept: bool = False,
              trouble: str | None = None, note: str = "") -> Group:
    head = Text()
    head.append(song.title, style="bold")
    if song.artist not in NO_ARTIST:
        head.append(f"   {song.artist}", style="italic")
    if song.partner:
        head.append(f" with {song.partner}", style="italic")

    facts = Table(box=None, pad_edge=False, show_header=False)
    facts.add_column(style="dim", no_wrap=True)
    facts.add_column(overflow="fold")
    for label, value in details.facts:
        facts.add_row(label, value)
    if subsong[1] > 1:
        facts.add_row("Subsong", f"{subsong[0] + 1} of {subsong[1]}")
    facts.add_row("Played by", replayer_facts.line(backend))
    if level is not None and level[0] is not None:
        by = abs(level[1])
        how = "as it is" if by < 0.05 else f"{by:.1f} dB {'softer' if level[1] < 0 else 'louder'}"
        facts.add_row("Loudness", f"{level[0]:.1f} LUFS, played {how}")
    if file is not None and file.exists():
        facts.add_row("File size", f"{file.stat().st_size:,} bytes")
    if kept:
        facts.add_row("Saved", "on this computer, and played from there")
    if trouble:
        facts.add_row("Known trouble", Text(trouble + " (it is tried again when you play it, "
                                            "and taken off the list once it plays)",
                                            style=theme.current.warn))
    if song.local:
        facts.add_row("File", song.path)
    else:
        facts.add_row(f"On {collections_.name(song.collection)}", song.path)
        if song.also:
            facts.add_row("Also on", ", ".join(collections_.name(c) for c in song.also))
        if song.album_name:
            facts.add_row("Album or game", song.album_name)

    about = formats_info.about(song.format)
    colour = {"verified": theme.current.good, "faults": theme.current.warn}.get(about.group, "")
    how = Text(no_wrap=False)
    how.append(f"\nThe format: {song.format}\n", style="bold")
    how.append(about.title, style=colour)
    if about.counted:
        how.append(f"  ·  {about.counted}", style="dim")
    for text in (about.text, about.files):
        if text:
            how.append(f"\n{text}")
    parts: list = [head, Text(""), facts, how]
    if note:            # what HVSC's keepers wrote about the tune (STIL)
        block = Text(no_wrap=False)
        block.append("\nAbout this tune (HVSC's STIL)\n", style="bold")
        block.append(note + "\n")
        parts.append(block)
    for heading, lines in details.texts:
        block = Text(no_wrap=True, overflow="ellipsis")
        block.append(f"\n{heading}\n", style="bold")
        numbered = heading.endswith("names")
        for i, line in enumerate(lines, 1):
            if numbered:
                block.append(f"{i:>3} ", style="dim")
            block.append(line[:max(20, width - 8)] + "\n", style=theme.current.primary if numbered else "")
        parts.append(block)
    if not details.texts:
        parts.append(Text("\nThis file carries no comments or instrument names.", style="dim"))
    parts.append(Text("\nRead from the file itself, exactly as its author or ripper wrote it.",
                      style="dim"))
    return Group(*parts)
