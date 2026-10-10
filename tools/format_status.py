#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write docs/formats.md: every format, whether it plays, and what is known about it.

    tools/format_status.py [--html FILE]

It is made from three things: the catalog (the formats and how many songs each has),
the census (tools/format_census.json: how many of a sample of each format's songs
play), and the notes (docs/format-notes.toml: what was checked, and what is known to
be wrong). The same goes into src/bbmp/format_notes.py, which the player
shows formats' notes from. --html also writes it as a piece of HTML, for the status
page.
"""

from __future__ import annotations

import argparse
import html
import json
import re
import sys
import tomllib
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from bbmp.backends import backend_for, is_supported  # noqa: E402
from bbmp.index import Index  # noqa: E402

GROUPS = [
    ("verified", "Verified against the original",
     "The replayer's output, or what it writes to the sound chip, was compared with the "
     "original replay routine and is the same. This is as close to certain as it gets "
     "without listening on the real machine."),
    ("fine", "Plays, no fault known",
     "Every song that was tried plays, and nothing is known to be wrong. Most of these "
     "are played by libraries that have been in use for years. That is not the same as "
     "verified: nobody has compared them with the original for this player."),
    ("faults", "Plays with faults or gaps",
     "Something is known to be wrong or missing, or some of the songs don't play."),
    ("off", "Switched off",
     "A replayer exists and the player doesn't use it, until it is decided whether it should."),
    ("no", "Doesn't play", ""),
]


def reason(note: str) -> str:
    """A failure's reason without what is particular to the one song."""
    note = re.sub(r"\S*/\S+", "", note)
    note = re.sub(r"[\"'“”][^\"'“”]{1,80}[\"'“”]", "…", note)
    note = re.sub(r"\(\d+% of its notes\)", "", note)
    note = re.sub(r"\d+(\.\d+)?", "n", note)
    return re.sub(r"\s+", " ", note).strip(" :;,.")[:110]


# What a replayer says about a file that holds no song: an empty file, a damaged one, a
# piece of an archive. Such files are the collection's trouble, not the player's: they
# are not counted against their format (the notes say how many there are).
NOT_SONGS = re.compile(
    r"the song is empty|an empty track of the rip|isn't packed the way EdLib packs"
    r"|the file on Modland is damaged|the file is not on the server|holds no song"
    r"|damaged on Modland|cut short on Modland|a broken rip")


def no_song(t: dict) -> bool:
    return bool(NOT_SONGS.search(t.get("note", "")))


def trouble(c: dict) -> str:
    """What the census found wrong, in a sentence. Files that hold no song are left out."""
    if "crashed" in c:
        return f"The census could not be finished: {c['crashed']}."
    parts = []
    for state, text in (("failed", "can't be played"), ("silent", "silent"), ("noise", "noise")):
        songs = [t for t in c["trouble"] if t["state"] == state and not no_song(t)]
        # the census's own finds, and what was found another way (marked "by")
        n = (min(c.get(state, 0), sum(1 for t in songs if not t.get("by")))
             + sum(1 for t in songs if t.get("by")))
        if not n:
            continue
        why = Counter(reason(t["note"]) for t in songs if t["note"]).most_common(2)
        verb = "" if state == "failed" else ("is " if n == 1 else "are ")
        text = f"{n} of {c['tried']} tried {verb}{text}"
        if state == "failed" and why:
            text += " (" + "; ".join(f"{w}" + (f", {k} times" if k > 1 and len(why) > 1 else "")
                                     for w, k in why) + ")"
        parts.append(text)
    return ". ".join(parts) + "." if parts else ""


def rows() -> list[dict]:
    notes = tomllib.loads((ROOT / "docs/format-notes.toml").read_text())
    census_file = ROOT / "tools/format_census.json"
    census = json.loads(census_file.read_text()) if census_file.exists() else {}
    out = []
    for fmt, songs, _ in Index().formats():
        n, c = notes.get(fmt, {}), census.get(fmt)
        status = n.get("status")
        playable = is_supported(fmt)
        counted = c is not None and not c.get("uncounted") and ("tried" in c or "crashed" in c)
        found = trouble(c) if counted else ""
        if not playable:
            group = "off" if status == "off" else "no"
        elif status == "faults" or found:
            group = "faults"
        elif status == "verified":
            group = "verified"
        else:
            group = "fine"
        if n.get("all"):
            plays = n["all"]
        elif counted and "tried" in c:
            plays = f"{c['plays']} of {c['tried']}" + (" tried" if c["tried"] < songs else "")
        else:
            plays = "" if playable else "0"
        basis = n.get("basis", "")
        text = " ".join(x for x in (
            (basis[0].upper() + basis[1:] + ".") if basis else "",
            n.get("note", ""),
            # what the census found is said where the notes don't already say it
            found if found and not n.get("all") else "") if x)
        if not playable and not text:
            text = "Not looked into."
        share = None
        if playable:
            if n.get("all"):
                a, b = (int(x) for x in n["all"].split(" of "))
                share = a / b
            elif counted and c.get("tried"):
                share = c["plays"] / c["tried"]
        nosong = n.get("nosong", "")
        if not nosong and counted and "tried" in c:
            k = min(c["tried"], sum(1 for t in c.get("trouble", []) if no_song(t)))
            if k:
                nosong = f"{k} of {c['tried']} tried hold no song"
        out.append({"format": fmt, "songs": songs, "group": group, "plays": plays,
                    "backend": backend_for(fmt) or "", "text": text, "share": share,
                    "status": status or "", "nosong": nosong})
    return out


def sampled() -> list[str]:
    """Lines on where the census took its songs from: formats and songs tried, by
    collection."""
    census_file = ROOT / "tools/format_census.json"
    census = json.loads(census_file.read_text()) if census_file.exists() else {}
    formats_of, tried_of = Counter(), Counter()
    for c in census.values():
        if "tried" not in c:
            continue
        for collection in (c.get("from") or {"modland": 0}):
            formats_of[collection] += 1
            tried_of[collection] += c["tried"]
    names = {"modland": "Modland", "hvsc": "HVSC", "zxart": "ZX-Art", "asma": "ASMA",
             "sndh": "SNDH", "keygenmusic": "Keygen Music"}
    parts = [f"{names.get(k, k)} ({formats_of[k]} format{'s' * (formats_of[k] != 1)}, "
             f"{tried_of[k]:,} songs)"
             for k in sorted(formats_of, key=lambda k: (k != "modland", k))]
    if not parts:
        return []
    return ["The census tries a sample of each format's songs (40 at the most): Modland's "
            "where Modland has the format, else those of the collections that have it. "
            "A format that two collections have is counted on Modland's copies. Tried, by "
            "collection: " + ", ".join(parts) + ". ZX-Art's songs (its site isn't "
            "mirrored) are five a format at the most; HVSC's are counted both as Modland's "
            "format HVSC and as HVSC's own three groups.", ""]


def markdown(table: list[dict]) -> str:
    total = sum(r["songs"] for r in table)
    known = [r for r in table if r["share"] is not None]
    playing = sum(r["songs"] * r["share"] for r in known)
    lines = ["# Formats", "",
             "Every format in the collections, whether the player plays it, and what is "
             "known about how well. Made by `tools/format_status.py` from the census "
             "(`tools/format_census.json`) and the notes (`docs/format-notes.toml`); "
             "change those and run it again.", "",
             "Nobody has listened to all of this, and nothing was compared with a real "
             "machine. \"Plays\" means: the replayer gives sound that is neither silence "
             "nor noise.", "",
             *sampled(),
             "| | Formats | Songs |", "|---|---|---|"]
    for key, title, _ in GROUPS:
        group = [r for r in table if r["group"] == key]
        lines.append(f"| {title} | {len(group)} | {sum(r['songs'] for r in group):,} |")
    lines += [f"| **All** | **{len(table)}** | **{total:,}** |", ""]
    counted = sum(r["songs"] for r in known)
    rest = [r for r in table if r["share"] is None and r["group"] not in ("off", "no")]
    if known and not rest:
        lines += [f"Counting every format by the share of its tried songs that play, about "
                  f"**{playing / total:.1%}** of the {total:,} songs play.", ""]
    elif known:
        lines += [f"Of the songs in the {len(known)} formats the census tried, about "
                  f"{playing / counted:.1%} play. {len(rest)} formats "
                  f"({sum(r['songs'] for r in rest):,} songs) are not tried yet.", ""]
    for key, title, about in GROUPS:
        group = sorted((r for r in table if r["group"] == key), key=lambda r: -r["songs"])
        if not group:
            continue
        lines += [f"## {title}", ""]
        if about:
            lines += [about, ""]
        if key in ("off", "no"):
            lines += ["| Format | Songs | Why |", "|---|---|---|"]
            lines += [f"| {r['format']} | {r['songs']:,} | {r['text']} |" for r in group]
        else:
            lines += ["| Format | Songs | Play | Replayer | Notes |", "|---|---|---|---|---|"]
            lines += [f"| {r['format']} | {r['songs']:,} | {r['plays']} | {r['backend']} | "
                      f"{r['text']} |" for r in group]
        lines.append("")
    return "\n".join(lines)


def as_html(table: list[dict]) -> str:
    e = html.escape
    out = []
    for key, title, about in GROUPS:
        group = sorted((r for r in table if r["group"] == key), key=lambda r: -r["songs"])
        if not group:
            continue
        songs = sum(r["songs"] for r in group)
        out.append(f'<details class="formats" {"open" if key == "verified" else ""}>'
                   f'<summary><span class="pill {key}">{len(group)}</span> {e(title)}'
                   f' <span class="count">{songs:,} songs</span></summary>')
        if about:
            out.append(f"<p>{e(about)}</p>")
        out.append('<div class="scroll"><table><thead><tr><th>Format</th><th class="num">Songs</th>'
                   + ("" if key in ("off", "no") else '<th>Play</th><th>Replayer</th>')
                   + "<th>Notes</th></tr></thead><tbody>")
        for r in group:
            middle = "" if key in ("off", "no") else (
                f"<td>{e(r['plays'])}</td><td>{e(r['backend'])}</td>")
            out.append(f"<tr><td>{e(r['format'])}</td><td class=\"num\">{r['songs']:,}</td>"
                       f"{middle}<td>{e(r['text'])}</td></tr>")
        out.append("</tbody></table></div></details>")
    return "\n".join(out) + "\n"


def module(table: list[dict]) -> str:
    """What the player itself shows about formats."""
    lines = ['"""What is known about how well each format plays.', "",
             "Generated by tools/format_status.py from the census and docs/format-notes.toml;",
             'edit those and run it again.', '"""', "",
             "# format: (group, songs that play, what is known)",
             "NOTES: dict[str, tuple[str, str, str]] = {"]
    for r in sorted(table, key=lambda r: r["format"]):
        if r["group"] == "fine" and not r["text"] and not r["plays"]:
            continue
        lines.append(f"    {r['format']!r}: ({r['group']!r}, {r['plays']!r},\n"
                     f"        {r['text']!r}),")
    lines += ["}", ""]
    return "\n".join(lines)


SPDX = "# SPDX-License-Identifier: GPL-3.0-or-later\n"   # the files it writes are the player's


def known_trouble() -> str:
    """The songs the census found failing in formats that otherwise play, as a module."""
    census = json.loads((ROOT / "tools/format_census.json").read_text())
    found = {}
    for fmt, c in sorted(census.items()):
        if not c.get("best"):
            continue            # the format doesn't play at all; the player says so already
        for t in c.get("trouble", []):
            state = t["state"]
            why = t.get("note", "").split("\n")[0].strip()
            text = {"silent": "stays silent", "noise": "comes out as noise"}.get(state, "fails")
            # songs of other collections than Modland are kept as their keys
            song = t["song"]
            key = song if c.get("from") and re.match(r"[a-z][a-z0-9]*:", song) else "modland:" + song
            found[key] = (state, (text + (f": {why}" if why else ""))[:200])
    lines = ['"""Songs the format census found failing, in formats that otherwise play.',
             "", "Made by tools/format_status.py from tools/format_census.json; don't edit.",
             '"""', "", "KNOWN = {"]
    lines += [f"    {k!r}: {v!r}," for k, v in found.items()]
    lines += ["}", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--html", type=Path)
    args = ap.parse_args(argv)
    table = rows()
    (ROOT / "docs/formats.md").write_text(markdown(table))
    (ROOT / "src/bbmp/format_notes.py").write_text(SPDX + module(table))
    (ROOT / "src/bbmp/known_trouble.py").write_text(SPDX + known_trouble())
    if args.html:
        args.html.write_text(as_html(table))
    counts = Counter(r["group"] for r in table)
    print(", ".join(f"{counts[k]} {t.lower()}" for k, t, _ in GROUPS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
