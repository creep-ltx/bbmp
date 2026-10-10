# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads with a ceiling: what a server sends is read, written or unpacked only up to
a size no honest answer comes near. A bad mirror, someone on the network (plain http and
ftp servers), or a zip bomb can't then fill the memory or the disk; past the size the
download stops with TooBig, an OSError, and the next server is asked."""

from __future__ import annotations

import zipfile
from typing import BinaryIO

# A song: Modland's largest file is 75 MB (an Impulse Tracker module, October 2026)
SONG = 256 << 20
# A collection's list, page or notes, downloaded or unpacked: Modland's file list, the
# largest, is 21 MB
LIST = 256 << 20
# HVSC's archive of itself (about 80 MB)
ARCHIVE = 1 << 30

PIECE = 1 << 20


class TooBig(OSError):
    """More came than the most that is taken."""


def read(r: BinaryIO, limit: int = LIST, what: str = "the answer") -> bytes:
    """All of `r`, or TooBig past `limit` bytes."""
    data = r.read(limit + 1)
    if len(data) > limit:
        raise TooBig(f"{what} is larger than {limit >> 20} MB: not taken")
    return data


def copy(r: BinaryIO, out: BinaryIO, limit: int = SONG, what: str = "the file") -> int:
    """`r` written to `out`, a piece at a time; TooBig past `limit` bytes."""
    done = 0
    while piece := r.read(PIECE):
        done += len(piece)
        if done > limit:
            raise TooBig(f"{what} is larger than {limit >> 20} MB: not taken")
        out.write(piece)
    return done


class Writer:
    """For a callback that is given the pieces (ftplib): writes them to `out`, and
    raises TooBig past `limit` bytes."""

    def __init__(self, out, limit: int = SONG, what: str = "the file"):
        self.out, self.limit, self.what, self.done = out, limit, what, 0

    def __call__(self, piece: bytes) -> None:
        self.done += len(piece)
        if self.done > self.limit:
            raise TooBig(f"{self.what} is larger than {self.limit >> 20} MB: not taken")
        self.out.write(piece)


def unzip_first(z: zipfile.ZipFile, limit: int = LIST, what: str = "the list") -> bytes:
    """The first file in a zip, unpacked up to `limit` bytes (what the zip says of its
    size is not trusted)."""
    names = z.namelist()
    if not names:
        raise zipfile.BadZipFile(f"{what}: an empty zip")
    with z.open(names[0]) as f:
        return read(f, limit, what)
