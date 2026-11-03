# SPDX-License-Identifier: GPL-3.0-or-later
"""Likes shared with HippoPlayer, DEViLBOX and other players.

A like belongs to a song's contents, not to where the file is: the key is the one
uade's song database uses (XXH32 of the module's first 256 KB, then its size), so the
same module from any archive or mirror gets the same likes. Each computer has a voter
token of its own, made once; the server keeps only a hash of it, and there are no
accounts. The server answers one line: "<count> <yours>", yours 5 when liked.

Nothing is sent unless "Share likes" is on in Settings.
"""

from __future__ import annotations

import secrets
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from . import __version__

API = "https://devilbox.uprough.net/api/hip"
PLAYER = f"bbmp/{__version__}"
HASHED = 262144                     # bytes of the module the key's hash covers

_P1, _P2, _P3, _P4, _P5 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D, 0x27D4EB2F, 0x165667B1
_M = 0xFFFFFFFF


def _rotl(x: int, r: int) -> int:
    return ((x << r) | (x >> (32 - r))) & _M


def xxh32(data: bytes, seed: int = 0) -> int:
    """The 32-bit xxHash of data."""
    n, at = len(data), 0
    if n >= 16:
        v1, v2 = (seed + _P1 + _P2) & _M, (seed + _P2) & _M
        v3, v4 = seed, (seed - _P1) & _M
        words = memoryview(data)[:n - n % 16].cast("I")    # xxHash reads little-endian
        if words.itemsize != 4 or int.from_bytes(b"\1\0\0\0", "little") != 1:
            raise RuntimeError("xxh32 here needs 4-byte words")
        for i in range(0, len(words), 4):
            v1 = (_rotl((v1 + words[i] * _P2) & _M, 13) * _P1) & _M
            v2 = (_rotl((v2 + words[i + 1] * _P2) & _M, 13) * _P1) & _M
            v3 = (_rotl((v3 + words[i + 2] * _P2) & _M, 13) * _P1) & _M
            v4 = (_rotl((v4 + words[i + 3] * _P2) & _M, 13) * _P1) & _M
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & _M
        at = n - n % 16
    else:
        h = (seed + _P5) & _M
    h = (h + n) & _M
    while at + 4 <= n:
        h = (_rotl((h + int.from_bytes(data[at:at + 4], "little") * _P3) & _M, 17) * _P4) & _M
        at += 4
    while at < n:
        h = (_rotl((h + data[at] * _P5) & _M, 11) * _P1) & _M
        at += 1
    h ^= h >> 15
    h = (h * _P2) & _M
    h ^= h >> 13
    h = (h * _P3) & _M
    return h ^ (h >> 16)


def song_key(data: bytes) -> str:
    """The key uade's song database (and HippoPlayer) give a module's bytes, unpacked."""
    return f"{xxh32(bytes(data[:HASHED])):08x}{len(data) & 0xFFFF:04x}"


def file_key(path: Path) -> str:
    return song_key(Path(path).read_bytes())


def token(where: Path) -> str:
    """This computer's voter token: 16 random bytes as hex, made the first time."""
    try:
        found = where.read_text().strip()
        if len(found) == 32 and all(c in "0123456789abcdef" for c in found):
            return found
    except OSError:
        pass
    made = secrets.token_hex(16)
    where.parent.mkdir(parents=True, exist_ok=True)
    tmp = where.with_suffix(".tmp")
    tmp.write_text(made + "\n")
    tmp.chmod(0o600)
    tmp.replace(where)
    return made


class LikeError(Exception):
    """The server refused or couldn't be reached; the message says which."""


def _ask(route: str, params: dict, timeout: float) -> tuple[int, bool]:
    url = f"{API}/{route}?{urllib.parse.urlencode(params)}"
    request = urllib.request.Request(url, headers={"User-Agent": PLAYER})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as r:
            line = r.read(200).decode("ascii", "replace")
    except urllib.error.HTTPError as e:
        body = e.read(200).decode("ascii", "replace").strip()
        if e.code == 429 or body == "ERR rate":
            raise LikeError("too many likes in an hour; try again later") from e
        raise LikeError(f"the likes server said {body or e.code}") from e
    except OSError as e:
        raise LikeError(f"can't reach the likes server: {getattr(e, 'reason', e)}") from e
    parts = line.split()
    if len(parts) != 2 or not all(p.isdigit() for p in parts):
        raise LikeError(f"the likes server said {line.strip()[:60]!r}")
    return int(parts[0]), parts[1] == "5"


def count(key: str, voter: str | None = None, timeout: float = 10) -> tuple[int, bool]:
    """How many like the song, and whether this computer does."""
    params = {"m": key}
    if voter:
        params["k"] = voter
    return _ask("votes", params, timeout)


def vote(key: str, voter: str, like: bool, name: str = "", timeout: float = 10) -> tuple[int, bool]:
    """Like the song or take the like back. Gives the count and whether it's liked now."""
    params = {"k": voter, "m": key, "r": "5" if like else "0", "p": PLAYER}
    if name:
        params["n"] = name[:255]
    return _ask("vote", params, timeout)
