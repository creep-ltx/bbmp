# SPDX-License-Identifier: GPL-3.0-or-later
"""HippoPlayer's shared playlists, from hippoplayer.se.

HippoPlayer (the Amiga module player) has playlists people share. The site lists them
as JSON at /api/v1/playlists (filename, title, author, genre) and gives each one, a
HippoPlayer list file, at /api/v1/playlists/<filename>. Only those two routes are
read here: the site's other routes are its own.
"""

from __future__ import annotations

import json
import urllib.parse
import urllib.request

from . import playlists

API = "https://hippoplayer.se/api/v1/playlists"
NAME = "HippoPlayer"


def _get(url: str, timeout: float) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "bbmp"})
    with urllib.request.urlopen(request, timeout=timeout) as r:
        return r.read(4 << 20)


def shared_lists(timeout: float = 20) -> list[tuple[str, str]]:
    """The shared lists as (filename, what to show), sorted by title.

    Raises OSError if the site can't be reached, ValueError if its answer can't be read.
    """
    try:
        found = json.loads(_get(API, timeout))
    except json.JSONDecodeError as e:
        raise ValueError("the site's answer is not a list of playlists") from e
    if not isinstance(found, list):
        raise ValueError("the site's answer is not a list of playlists")
    out = []
    for item in found:
        if not isinstance(item, dict):
            continue
        filename = str(item.get("filename") or "").strip()
        if not filename or "/" in filename or filename in (".", ".."):
            continue
        title = str(item.get("title") or "").strip() or playlists.list_name(filename)
        label = title
        if author := str(item.get("author") or "").strip():
            label += f"  by {author}"
        if genre := str(item.get("genre") or "").strip():
            label += f"  ({genre})"
        out.append((filename, label, title))
    out.sort(key=lambda t: t[2].casefold())
    return [(f, label) for f, label, _ in out]


def title_of(label: str) -> str:
    """The list's title in a label of shared_lists()."""
    return label.split("  by ", 1)[0].split("  (", 1)[0]


def read_shared(filename: str, timeout: float = 30) -> list[str]:
    """The keys of the songs a shared list names.

    Raises OSError if it can't be fetched, ValueError if it isn't a list that can be read.
    """
    raw = _get(f"{API}/{urllib.parse.quote(filename)}", timeout)
    if raw[:4] == b"XPKF":
        raise ValueError("the list is packed with XPK, which can't be read here yet")
    text = playlists.decode(raw)
    if not playlists.is_hippo_list(text):
        raise ValueError("that is not a HippoPlayer list")
    return playlists.parse_hippo(text)
