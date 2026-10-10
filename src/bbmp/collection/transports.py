# SPDX-License-Identifier: GPL-3.0-or-later
"""The ways collections hand out their files: FTP and HTTP, and several servers that
have the same files (mirrors), of which the next is taken when one doesn't answer."""

from __future__ import annotations

import ftplib
import http.client
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Callable

from .. import capped
from .base import Transport

# How long a server may take to answer a request (its first byte), and how long the
# body of a file may stall between two pieces. A server that is silent for longer than
# the first is passed over for the next one at once; only when none answers in that
# time are they asked again and waited for as long as the second.
FIRST_BYTE = 3.0
BODY = 30.0


class FtpTransport(Transport):
    """Anonymous FTP. The connection stays open, since logging in again for every file
    would cost more time than the transfer itself."""

    def __init__(self, host: str, root: str, top: str = ""):
        self.host = host
        self.root = root.rstrip("/")     # the folder the collection's paths start in
        self.top = top.rstrip("/")       # where a mirror has the server's own top folder
        self._ftp: ftplib.FTP | None = None
        self.patience = BODY             # seconds to wait for a new connection's answer
        self.waited: float | None = None     # until the last file's first byte came

    def download(self, path: str, dest: Path) -> None:
        self.waited = None
        with open(dest, "wb") as f:
            def fetch(ftp: ftplib.FTP) -> None:
                f.seek(0)
                f.truncate()
                began = time.perf_counter()

                keep = capped.Writer(f, capped.SONG, path)

                def write(data: bytes) -> None:
                    if self.waited is None:
                        self.waited = time.perf_counter() - began
                    keep(data)
                ftp.retrbinary(f"RETR {self.root}/{path}", write)
            self._retry(fetch, path, attempts=3)

    def read(self, server_path: str) -> bytes:
        """A file anywhere on the server, not only below the collection's root."""
        chunks: list[bytes] = []

        def fetch(ftp: ftplib.FTP) -> None:
            chunks.clear()
            ftp.retrbinary(f"RETR {self.top}{server_path}",
                           capped.Writer(_Pieces(chunks), capped.LIST, server_path))
        self._retry(fetch, server_path)
        return b"".join(chunks)

    def list_dir(self, server_path: str) -> list[tuple[str, bool]]:
        """(name, is a folder) for everything in a folder on the server."""
        lines: list[str] = []

        def fetch(ftp: ftplib.FTP) -> None:
            lines.clear()
            ftp.retrlines(f"LIST {self.top}{server_path}", lines.append)
        self._retry(fetch, server_path)
        out = []
        for line in lines:
            parts = line.split(None, 8)
            if len(parts) == 9 and parts[8] not in (".", ".."):
                out.append((parts[8], line.startswith("d")))
        return sorted(out, key=lambda e: (not e[1], e[0].lower()))

    def _retry(self, fetch, what: str, attempts: int = 2) -> None:
        for attempt in range(attempts):
            try:
                fetch(self._connect())
                return
            except ftplib.error_perm as e:
                # 550 is "no such file"; the rest (530: too many connections, not
                # logged in) is the server's trouble, not the file's, and a song isn't
                # marked as failing for it
                if str(e).startswith("550"):
                    raise FileNotFoundError(what) from None
                self._drop()
                if attempt == attempts - 1:
                    raise OSError(f"{what}: {e}") from None
            except (OSError, EOFError, ftplib.Error) as e:
                self._drop()
                # a server that is silent is not asked again: the next mirror is
                if attempt == attempts - 1 or isinstance(e, TimeoutError):
                    if isinstance(e, ftplib.Error):
                        raise OSError(f"{self.host}: {e}") from None
                    raise

    def __str__(self) -> str:
        return f"ftp://{self.host}{self.root}"

    def _connect(self) -> ftplib.FTP:
        if self._ftp is None:
            ftp = ftplib.FTP(self.host, timeout=self.patience, encoding="utf-8")
            ftp.login()
            ftp.voidcmd("TYPE I")
            ftp.timeout = BODY          # once it has answered, a file may take its time
            if ftp.sock is not None:
                ftp.sock.settimeout(BODY)
            self._ftp = ftp
        return self._ftp

    def _drop(self) -> None:
        if self._ftp is not None:
            try:
                self._ftp.close()
            except OSError:
                pass
            self._ftp = None

    def close(self) -> None:
        if self._ftp is not None:
            try:
                self._ftp.quit()
            except (OSError, EOFError, ftplib.Error):
                pass
        self._drop()


class _Pieces:
    """A list that pieces are written to, for capped.Writer."""

    def __init__(self, pieces: list[bytes]):
        self.pieces = pieces

    def write(self, piece: bytes) -> None:
        self.pieces.append(piece)


class HttpTransport(Transport):
    """HTTP or HTTPS. The connection stays open for the next file, which takes a
    fraction of the time a new one does."""

    def __init__(self, base_url: str, agent: str = "bbmp"):
        self.base = base_url.rstrip("/")
        self.agent = agent
        self._con: http.client.HTTPConnection | None = None
        self.patience = BODY             # seconds to wait for the first byte of an answer
        self.waited: float | None = None     # until the last answer's first byte came

    def url(self, path: str) -> str:
        return f"{self.base}/{urllib.parse.quote(path)}"

    def __str__(self) -> str:
        return self.base

    def _connect(self) -> http.client.HTTPConnection:
        if self._con is None:
            at = urllib.parse.urlsplit(self.base)
            kind = (http.client.HTTPSConnection if at.scheme == "https"
                    else http.client.HTTPConnection)
            self._con = kind(at.netloc, timeout=self.patience)
        elif self._con.sock is not None:
            self._con.sock.settimeout(self.patience)
        return self._con

    def download(self, path: str, dest: Path) -> None:
        url = self.url(path)
        at = urllib.parse.urlsplit(url)
        self.waited = None
        for again in (False, True):
            fresh = self._con is None
            try:
                began = time.perf_counter()
                con = self._connect()
                con.request("GET", at.path, headers={"User-Agent": self.agent})
                r = con.getresponse()
                self.waited = time.perf_counter() - began
                if con.sock is not None:
                    con.sock.settimeout(BODY)   # it answered: the file may take its time
                if r.status == 200:
                    with open(dest, "wb") as f:     # too much: TooBig, closed below
                        capped.copy(r, f, capped.SONG, path)
                    return
                r.read()        # the connection is used again
                break
            except (http.client.HTTPException, ConnectionError, TimeoutError) as e:
                # the server may have closed a connection that was open for a while:
                # a new one, once (a new one that got no answer is not asked again)
                self.close()
                if again or fresh:
                    raise OSError(f"{self.base}: {e or type(e).__name__}") from None
            except OSError:
                self.close()
                raise
        if r.status in (403, 404, 410):
            raise FileNotFoundError(path)
        if r.status in (301, 302, 303, 307, 308):
            return self._follow(url, path, dest)
        raise OSError(f"{self.base}: HTTP {r.status} {r.reason}")

    def _follow(self, url: str, path: str, dest: Path) -> None:
        """The file is somewhere else: go where the server says."""
        request = urllib.request.Request(url, headers={"User-Agent": self.agent})
        try:
            with urllib.request.urlopen(request, timeout=30) as r, open(dest, "wb") as f:
                capped.copy(r, f, capped.SONG, path)
        except urllib.error.HTTPError as e:
            if e.code in (403, 404, 410):
                raise FileNotFoundError(path) from None
            raise OSError(f"{self.base}: {e}") from None

    def close(self) -> None:
        if self._con is not None:
            try:
                self._con.close()
            except OSError:
                pass
            self._con = None


def _secure(transport: Transport) -> bool:
    """Does the server's answer come over https, so that no one on the way can change it?"""
    if isinstance(transport, Below):
        transport = transport.transport
    return isinstance(transport, HttpTransport) and transport.base.startswith("https://")


class NotMine(FileNotFoundError):
    """The server has only part of the collection, and not the part the file is in:
    that says nothing of whether the file exists."""


class Below(Transport):
    """A server that has one folder of a collection, under another path: HVSC's own
    servers have what Modland has below HVSC/, and have it below C64Music/."""

    def __init__(self, folder: str, transport: Transport):
        self.folder = folder.rstrip("/") + "/"
        self.transport = transport

    @property
    def patience(self) -> float:
        return getattr(self.transport, "patience", BODY)

    @patience.setter
    def patience(self, seconds: float) -> None:
        if hasattr(self.transport, "patience"):
            self.transport.patience = seconds

    @property
    def waited(self) -> float | None:
        return getattr(self.transport, "waited", None)

    def download(self, path: str, dest: Path) -> None:
        if not path.startswith(self.folder):
            raise NotMine(path)
        self.transport.download(path[len(self.folder):], dest)

    def close(self) -> None:
        self.transport.close()

    def __str__(self) -> str:
        return f"{self.transport} (for {self.folder})"


# How fast each server has answered lately (seconds to the first byte, a running
# average), shared by every connection in this process: by its address.
_speed: dict[str, float] = {}
_speed_lock = threading.Lock()
SLOW = 2 * FIRST_BYTE + 1      # what a server that didn't answer counts as


# home mirrors (by address) that didn't answer in this run: away from home, they are
# waited for once, and not again for every collection and every song
_homes_down: set[str] = set()


def _host(transport: Transport) -> str:
    return urllib.parse.urlsplit(str(transport)).netloc


def _note_speed(server: str, seconds: float) -> None:
    with _speed_lock:
        old = _speed.get(server)
        _speed[server] = seconds if old is None else 0.7 * old + 0.3 * seconds


class Mirrors(Transport):
    """Servers that have the same files. The one that has answered fastest lately is
    asked first; before a server has been asked, the order they are given in says (the
    first, then the second...). When one doesn't answer within FIRST_BYTE, doesn't have
    the file, or gives another file than was asked for, the next one is asked. When
    none answers that quickly, they are asked again and given more time."""

    def __init__(self, transports: list[Transport]):
        self.transports = list(transports)
        self.first: int | None = None   # one to ask before the rest, whatever its speed
        self.used: str = ""         # the one the last file came from
        # the user's own copy (home_mirror()): asked first, before `first` too, unless
        # it has failed to answer in this run
        self.home: Transport | None = None

    def of(self, kind: type) -> Transport | None:
        """The first server that is reached in this way (FTP, to list folders)."""
        return next((t for t in self.transports if isinstance(t, kind)), None)

    def order(self) -> list[Transport]:
        """The servers in the order they are asked."""
        with _speed_lock:
            known = dict(_speed)
        # a server not yet heard from counts as a little slower than the one before it
        ranked = sorted(enumerate(self.transports),
                        key=lambda it: known.get(str(it[1]), 0.3 + it[0]))
        order = [t for _, t in ranked]
        if self.first is not None and 0 <= self.first < len(self.transports):
            chosen = self.transports[self.first]
            order.remove(chosen)
            order.insert(0, chosen)
        if self.home in order:
            order.remove(self.home)
            with _speed_lock:
                down = _host(self.home) in _homes_down
            # one that didn't answer (for any collection) is asked last in this run
            order.insert(len(order) if down else 0, self.home)
        return order

    def download(self, path: str, dest: Path,
                 verify: Callable[[Path], bool] | None = None) -> None:
        """`verify` says whether what was downloaded is the file that was asked for."""
        order = self.order()
        unreachable, other, answered, wrong = None, None, False, False
        silent: list[Transport] = []
        for patient in (False, True):
            for transport in (silent if patient else order):
                if hasattr(transport, "patience"):
                    transport.patience = BODY if patient else FIRST_BYTE
                try:
                    transport.download(path, dest)
                except NotMine:
                    continue
                except FileNotFoundError:
                    answered = True
                    self._timed(transport)
                    continue
                except (OSError, EOFError) as e:
                    unreachable = unreachable or e
                    transport.close()
                    _note_speed(str(transport), SLOW)
                    if transport is self.home:
                        with _speed_lock:
                            _homes_down.add(_host(transport))
                    if not patient:
                        silent.append(transport)
                    continue
                if verify is not None and not verify(dest):
                    # kept, in case no server has the right one; only from an https
                    # server: on plain http or ftp anyone on the way could have swapped it
                    wrong = True
                    if other is None and _secure(transport):
                        other = dest.with_name(dest.name + ".other")
                        dest.replace(other)
                    continue
                self._timed(transport)
                if other is not None:
                    other.unlink(missing_ok=True)
                self.used = str(transport)
                return
            if answered or other is not None or not silent:
                break       # a server answered: the others are not waited for
        if other is not None:
            # Every server has another file than the catalog says: the file was
            # changed, and the catalog is older than that.
            other.replace(dest)
            return
        dest.unlink(missing_ok=True)
        if wrong:
            raise OSError(f"{path}: the servers that answered give another file than the "
                          "catalog says (and not over https)")
        if unreachable is not None and not answered:
            raise OSError(f"no server could be reached ({unreachable})")
        raise FileNotFoundError(path)       # a server answered, and none has the file

    @staticmethod
    def _timed(transport: Transport) -> None:
        waited = getattr(transport, "waited", None)
        if waited is not None:
            _note_speed(str(transport), waited)

    def close(self) -> None:
        for transport in self.transports:
            transport.close()

    def __str__(self) -> str:
        return ", ".join(str(t) for t in self.transports)


def home_mirror() -> str:
    """The address of the user's own copy of the collections ("" when there is none):
    BBMP_MIRROR, or else the setting "Home mirror"."""
    from .. import paths
    from ..settings import Settings
    value = paths.env("MIRROR")
    if value is None:
        value = Settings.load().mirror
    value = (value or "").strip().rstrip("/")
    if value and "://" not in value:
        value = "http://" + value
    return value


def mirrored(collection: str, root: str, servers: list[Transport]) -> Mirrors:
    """A collection's servers, with the home mirror before them when there is one.

    A home mirror has each collection in a folder named by the collection's id, and
    below it the same paths as the collection's own server (`root`: where the
    collection's files start there, "/pub/modules" for Modland). It is asked first;
    when it doesn't have a file the collection's own servers are, and when it can't be
    reached (away from home) it is asked last for the rest of the run, for every
    collection, after one wait of FIRST_BYTE. What it gives is checked like any
    server's.
    """
    home = home_mirror()
    if not home:
        return Mirrors(servers)
    mine = HttpTransport(f"{home}/{collection}{root.rstrip('/')}")
    out = Mirrors([mine, *servers])
    out.home = mine
    return out
