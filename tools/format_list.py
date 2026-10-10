#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write a short list of which formats play, for reading on a phone.

    tools/format_list.py [OUT]       (default: format_list.md in the current folder)

Three parts: the formats that play fully (every song the census tried plays and nothing
is known to be wrong), those that play partly, and those that don't play. It is made
from the same rows as docs/formats.md (tools/format_status.py), so run that first.
"""

from __future__ import annotations

import datetime
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import format_status  # noqa: E402

OUT = Path("format_list.md")


def first_sentence(text: str) -> str:
    m = re.match(r"(.+?[.!?])(\s|$)", text)
    return m.group(1) if m else text


def why(r: dict, notes: dict) -> str:
    """What to say about a format that doesn't play fully: the note's first sentence,
    else what the census found."""
    n = notes.get(r["format"], {})
    if n.get("note"):
        return first_sentence(n["note"])
    return first_sentence(r["text"]) if r["text"] else ""


def write(table: list[dict], notes: dict, day: datetime.date) -> str:
    full = [r for r in table if r["group"] in ("verified", "fine")]
    part = [r for r in table if r["group"] == "faults"]
    none = [r for r in table if r["group"] in ("no", "off")]
    count = lambda rows: sum(r["songs"] for r in rows)  # noqa: E731
    checked = sum(1 for r in full if r["group"] == "verified")
    lines = ["# bbmp: which formats play", "",
             f"Made {day.day} {day:%B %Y} from the player's format census (`docs/formats.md` in "
             "the project has every detail). Songs = how many the collections have. \"Plays\" "
             "means sound that is neither silence nor noise; nobody has listened to all of it. "
             "Files that hold no song (empty, or damaged in the collection) are not counted "
             "against a format; the list says how many there are.", "",
             f"- **Plays fully:** {len(full)} formats, {count(full):,} songs "
             f"({checked} of them checked against the original program)",
             f"- **Plays partly:** {len(part)} formats, {count(part):,} songs",
             f"- **Doesn't play:** {len(none)} formats, {count(none):,} songs", ""]
    lines += ["## Doesn't play", ""]
    for r in sorted(none, key=lambda r: -r["songs"]):
        lines.append(f"- **{r['format']}** ({r['songs']:,}): {why(r, notes)}")
    lines += ["", "## Plays partly", "", "Some songs don't play, or something is known to be wrong.", ""]
    for r in sorted(part, key=lambda r: -r["songs"]):
        plays = f", plays {r['plays']}" if r["plays"] else ""
        lines.append(f"- **{r['format']}** ({r['songs']:,}{plays}): {why(r, notes)}")
    lines += ["", "## Plays fully", "",
              "✓ = checked against the original program, sample for sample or write for write.", ""]
    for r in sorted(full, key=lambda r: -r["songs"]):
        mark = " ✓" if r["group"] == "verified" else ""
        extra = f"; {r['nosong']}" if r.get("nosong") else ""
        lines.append(f"- {r['format']}{mark} ({r['songs']:,}{extra})")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    out = Path(args[0]) if args else OUT
    import tomllib
    notes = tomllib.loads((format_status.ROOT / "docs/format-notes.toml").read_text())
    out.write_text(write(format_status.rows(), notes, datetime.date.today()))
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
