# SPDX-License-Identifier: GPL-3.0-or-later
"""What each replayer is made of: its sources' names, versions and licences, as
tools/replayer_facts.py writes them from the copy of every source the build uses
(data/replayers.json). For the Info tab and `bbmp replayers`."""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path

DATA = Path(__file__).parent / "data" / "replayers.json"


@lru_cache(maxsize=1)
def facts() -> dict[str, dict]:
    try:
        return json.loads(DATA.read_text())
    except (OSError, ValueError):
        return {}


def _licence(text: str) -> str:
    """"BSD-3-Clause (LICENSE); MIT (x)" -> "BSD-3-Clause, MIT"; "" when none was found."""
    names = []
    for part in text.split(";"):
        name = part.split("(")[0].strip()
        if name and name not in ("see", "none found") and name not in names:
            names.append(name)
    return ", ".join(names)


def line(engine: str) -> str:
    """"libopenmpt 0.8.9 (BSD-3-Clause)": what plays a song, in a line."""
    f = facts().get(engine)
    if f is None:
        return engine
    # the player's own code is the replayer (a port, a replayer written for it), or
    # only helps a library that is (a converter, a patch)
    own_player = any(w in f["own"] for w in ("port", "written for this player"))
    first = f["made_of"][0] if f["made_of"] and not own_player else None
    version = f" {first['version']}" if first and first["version"] not in ("", "-") else ""
    terms = f["terms"] or (_licence(first["licence"]) if first else "")
    if own_player and not terms:
        terms = "written for this player"
    return f"{f['name']}{version}" + (f" ({terms})" if terms else "")


def details(engine: str) -> list[tuple[str, str]]:
    """(label, text) rows: what a replayer is made of, part by part."""
    f = facts().get(engine)
    if f is None:
        return []
    rows = [(p["name"], " ".join(x for x in (p["version"] if p["version"] != "-" else "",
                                              f"({_licence(p['licence']) or 'licence not found'})")
                                 if x)) for p in f["made_of"]]
    if f["own"]:
        rows.append(("this player's own", f["own"]))
    if f["terms"]:
        rows.append(("terms", f["terms"]))
    return rows
