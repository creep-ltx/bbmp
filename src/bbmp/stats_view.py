# SPDX-License-Identifier: GPL-3.0-or-later
"""The Stats tab: listening statistics rendered as Rich text."""

from __future__ import annotations

import datetime as dt
import time

from rich.console import Group
from rich.table import Table
from rich.text import Text

from . import theme

PERIODS = {
    "week": ("Last 7 days", 7),
    "month": ("Last 30 days", 30),
    "year": ("Last 365 days", 365),
    "all": ("All time", None),
}
SPARK = " ▁▂▃▄▅▆▇█"


def since(period: str, now: float | None = None) -> int:
    days = PERIODS[period][1]
    return 0 if days is None else int((now or time.time()) - days * 86400)


def hours(seconds: float) -> str:
    seconds = int(seconds)
    if seconds < 60:
        return f"{seconds} s"
    h, m = divmod(seconds // 60, 60)
    return f"{h} h {m:02d} min" if h else f"{m} min"


def _bar(n: int, top: int, width: int = 18) -> Text:
    filled = round(width * n / top) if top else 0
    return Text("█" * filled + "░" * (width - filled), style=theme.current.accent)


def _per_day(rows: list[tuple[str, int, float]], days: int) -> Text:
    """A sparkline of plays per day over the last `days` days (at most 60 columns)."""
    counts = {d: n for d, n, _ in rows}
    today = dt.date.today()
    span = [today - dt.timedelta(days=i) for i in range(days - 1, -1, -1)]
    values = [counts.get(d.isoformat(), 0) for d in span]
    if len(values) > 60:   # a year: one column per week
        values = [sum(values[i:i + 7]) for i in range(0, len(values), 7)]
    peak = max(values) or 1
    t = Text()
    t.append("".join(SPARK[min(8, round(v / peak * 8))] if v else "·" for v in values), style=theme.current.primary)
    t.append(f"   peak {peak}", style="dim")
    return t


def render(stats: dict, period: str) -> Group:
    label, days = PERIODS[period]
    if not stats["plays"]:
        return Group(Text(f"{label}: nothing played yet.", style="bold"),
                     Text("\nPlays count once you've listened for 20 seconds.", style="dim"))
    head = Text()
    head.append(f"{label}\n", style="bold")
    head.append(f"{stats['plays']:,}", style=f"bold {theme.current.accent}")
    head.append(" plays   ")
    head.append(hours(stats["seconds"]), style=f"bold {theme.current.accent}")
    head.append(" listened   ")
    head.append(f"{stats['songs']:,}", style=f"bold {theme.current.accent}")
    head.append(" different songs")

    parts = [head]
    span = days or max(1, int((time.time() - (stats["first"] or time.time())) // 86400) + 1)
    per = Text("\nPlays per " + ("week" if span > 60 else "day") + "\n", style="dim")
    parts += [per, _per_day(stats["per_day"], min(span, 365))]

    songs = Table(title="Most played songs", title_justify="left", title_style="bold",
                  box=None, pad_edge=False, expand=True)
    songs.add_column("#", justify="right", style="dim", width=3)
    songs.add_column("Song", ratio=3, no_wrap=True, overflow="ellipsis")
    songs.add_column("Artist", ratio=2, no_wrap=True, overflow="ellipsis", style="italic")
    songs.add_column("Format", ratio=2, no_wrap=True, overflow="ellipsis", style="dim")
    songs.add_column("Plays", justify="right")
    songs.add_column("Time", justify="right", style="dim")
    for i, (_, title, artist, fmt, n, secs) in enumerate(stats["top_songs"], 1):
        songs.add_row(str(i), title, artist, fmt, str(n), hours(secs))
    parts += [Text(""), songs]

    def ranking(title: str, rows: list[tuple[str, int, float]]) -> Table:
        t = Table(title=title, title_justify="left", title_style="bold", box=None,
                  pad_edge=False, show_header=False, expand=True)
        t.add_column(ratio=3, no_wrap=True, overflow="ellipsis")
        t.add_column(no_wrap=True)
        t.add_column(justify="right")
        top = rows[0][1] if rows else 0
        for name, n, _ in rows:
            t.add_row(name, _bar(n, top), f"{n}")
        return t

    side = Table.grid(expand=True, padding=(0, 3))
    side.add_column(ratio=1)
    side.add_column(ratio=1)
    side.add_row(ranking("Top artists", stats["top_artists"]),
                 ranking("Top formats", stats["top_formats"]))
    parts += [Text(""), side]
    return Group(*parts)
