# SPDX-License-Identifier: GPL-3.0-or-later
"""Sizes and dates as they are shown."""

from __future__ import annotations

import time


def size(n: int | float) -> str:
    """"11.9 GB", "9.0 MB", "19 KB"; "" for nothing known."""
    if not n:
        return ""
    if n >= 1e9:
        return f"{n / 1e9:,.1f} GB"
    if n >= 1e6:
        return f"{n / 1e6:,.1f} MB"
    if n >= 1e3:
        return f"{n / 1e3:,.0f} KB"
    return f"{int(n)} bytes"


def folder(path) -> str:
    """A folder as it is shown: "~/Music/ByteBandit Music Player" for one in the user's own."""
    from pathlib import Path
    try:
        return "~/" + str(Path(path).relative_to(Path.home()))
    except ValueError:
        return str(path)


def day(text: str) -> str:
    """"2026-09-27" as "27 Sep 2026"."""
    try:
        return time.strftime("%-d %b %Y", time.strptime(text, "%Y-%m-%d"))
    except ValueError:
        return text


def moment(seconds: int | float) -> str:
    """A time since 1970 as "29 Sep 2026, 03:14"."""
    return time.strftime("%-d %b %Y, %H:%M", time.localtime(seconds)) if seconds else "not known"
