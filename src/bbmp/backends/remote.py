# SPDX-License-Identifier: GPL-3.0-or-later
"""The big replayer libraries (libopenmpt, UADE, Game Music Emu, libsidplayfp,
SunVox), each in a sandboxed process of its own.

In the player they were loaded into its own process: a song that crashes one takes
the player with it, and a song made to attack one would have the player's rights.
Now the player starts `python -m bbmp.backends.remote`, shut in as the
helper programs are (sandbox.py), and that process loads the library and plays the
song. The player talks to it over a pipe: a command a line (JSON), and an answer of a
4-byte length, a JSON header and, for sound, the sound. Everything the library could
do in the player it does here: subsongs, seeking, channel levels, the pattern view,
the Info tab's details.

What comes back from the process is checked and never trusted beyond its data: no
pickling, sizes capped, text cut to length.
"""

from __future__ import annotations

import json
import os
import select
import struct
import subprocess
import sys
from pathlib import Path

from .. import downloads, sandbox
from .base import FRAME_BYTES, Decoder, Details, NeedsDownload, TrackInfo, Unsupported

LOCAL = ("openmpt", "uade", "gme", "sidplayfp", "sunvox")   # the libraries run this way
WAIT = 15.0          # seconds to wait for opening or seeking before the process is taken for hung
WAIT_READ = 3.0      # and for sound: the player hears no key while it waits
MAX_BODY = 64 << 20
MAX_HEADER = 1 << 20
TEXT = 2000          # characters of any one text from the process


# ---- the player's side ----

def _text(value, limit: int = TEXT) -> str:
    return str(value)[:limit] if isinstance(value, (str, int, float)) else ""


def _int(value, default: int) -> int:
    if not isinstance(value, (int, float)) or value != value or value in (float("inf"), float("-inf")):
        return default
    return int(max(-1e9, min(1e9, value)))


def _info(d: dict) -> TrackInfo:
    extra = d.get("extra") if isinstance(d.get("extra"), dict) else {}
    duration = d.get("duration")
    return TrackInfo(
        title=_text(d.get("title")), author=_text(d.get("author")),
        format=_text(d.get("format")),
        duration=float(duration) if isinstance(duration, (int, float)) and duration >= 0 else None,
        subsongs=max(1, min(_int(d.get("subsongs"), 1), 10000)),
        subsong=max(0, min(_int(d.get("subsong"), 0), 10000)),
        extra={_text(k, 100): _text(v) for k, v in list(extra.items())[:50]})


class Remote(Decoder):
    """A decoder whose library runs in a sandboxed process."""

    def __init__(self, backend: str, path: Path, lengths: list[float]):
        self.backend = backend
        self._levels: list[float] | None = None
        src = Path(__file__).resolve().parents[2]            # where bbmp is
        env = {**os.environ, "PYTHONPATH": str(src), "BBMP_REMOTE": "1",
               "PYTHONDONTWRITEBYTECODE": "1"}
        cmd = [sys.executable, "-m", "bbmp.backends.remote", backend, str(path),
               json.dumps([float(x) for x in lengths])]
        # and what it may read besides: the downloads it plays with (UADE's players)
        fetched = [Path(v) for v in downloads.env(backend).values() if v.startswith("/")]
        self._proc = sandbox.popen(
            sandbox.wrap(cmd, path, [src, Path(sys.executable).resolve().parent, *fetched]),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env)
        try:
            head, _ = self._answer(WAIT)
        except OSError as e:
            self.close()
            raise Unsupported(f"{backend}: {e}") from None
        if not head.get("ok"):
            self.close()
            why = _text(head.get("error")) or f"{backend} can't play this file"
            raise (NeedsDownload if head.get("needs_download") is True else Unsupported)(why)
        self.info = _info(head.get("info") or {})
        self.backend = _text(head.get("backend"), 40) or backend

    def _answer(self, wait: float) -> tuple[dict, bytes]:
        """The process's answer. Anything but a proper one is an OSError, and the
        process is ended: after garbage the pipe can't be trusted to be in step."""
        size = self._exactly(4, wait)
        (n,) = struct.unpack("<I", size)
        try:
            if n > MAX_HEADER:
                raise ValueError("too long a header")
            head = json.loads(self._exactly(n, wait))
            if not isinstance(head, dict):
                raise ValueError("not a header")
            size = head.get("bytes") or 0
            if not isinstance(size, int) or not 0 <= size <= MAX_BODY:
                raise ValueError("not a size")
        except ValueError:          # JSONDecodeError and UnicodeDecodeError are ValueErrors
            self._kill()
            raise OSError(f"{self.backend}'s process said something that makes no sense") from None
        body = self._exactly(size, wait) if size else b""
        if isinstance(head.get("info"), dict):
            self.info = _info(head["info"])
        if head.get("never_ends") is True:
            self.never_ends = True
        return head, body

    def _exactly(self, n: int, wait: float) -> bytes:
        fd = self._proc.stdout.fileno()
        got = bytearray()
        while len(got) < n:
            ready, _, _ = select.select([fd], [], [], wait)
            chunk = os.read(fd, n - len(got)) if ready else b""
            if not chunk:
                self._kill()
                raise OSError(f"{self.backend}'s process ended or hung")
            got += chunk
        return bytes(got)

    def _ask(self, wait: float = WAIT, **command) -> tuple[dict, bytes]:
        if self._proc is None or self._proc.poll() is not None:
            raise OSError(f"{self.backend}'s process is gone")
        try:
            self._proc.stdin.write((json.dumps(command) + "\n").encode())
            self._proc.stdin.flush()
        except (BrokenPipeError, OSError):
            self._kill()
            raise OSError(f"{self.backend}'s process is gone") from None
        head, body = self._answer(wait)
        if head.get("unsupported"):
            raise Unsupported(_text(head.get("error")))
        return head, body

    def read(self, frames: int) -> bytes:
        try:
            head, pcm = self._ask(WAIT_READ, op="read", frames=int(frames))
        except OSError:
            return b""                              # ends the song, as a crash would
        levels = head.get("levels")
        self._levels = ([min(1.0, max(0.0, float(x))) for x in levels[:64]
                         if isinstance(x, (int, float))] if isinstance(levels, list) else None)
        pcm = pcm[:frames * FRAME_BYTES]
        return pcm[:len(pcm) // FRAME_BYTES * FRAME_BYTES]

    def select_subsong(self, n: int) -> None:
        try:
            self._ask(op="subsong", n=int(n))
        except OSError as e:
            raise Unsupported(str(e)) from None

    def seek(self, seconds: float) -> bool:
        try:
            head, _ = self._ask(op="seek", seconds=float(seconds))
        except (OSError, Unsupported):
            return False
        return bool(head.get("done"))

    def loop_forever(self, on: bool) -> bool:
        try:
            head, _ = self._ask(op="forever", on=bool(on))
        except (OSError, Unsupported):
            return False
        return head.get("done") is True

    def channel_levels(self) -> list[float] | None:
        return self._levels

    def pattern_view(self, rows: int) -> tuple[int, list[str]] | None:
        try:
            head, _ = self._ask(WAIT_READ, op="pattern", rows=int(rows))
        except (OSError, Unsupported):
            return None
        view = head.get("view")
        if not (isinstance(view, list) and len(view) == 2 and isinstance(view[1], list)):
            return None
        return _int(view[0], 0), [_text(line, 400) for line in view[1][:200]]

    def details(self) -> Details:
        try:
            head, _ = self._ask(op="details")
        except (OSError, Unsupported):
            return super().details()
        d = Details()
        for pair in (head.get("facts") or [])[:100]:
            if isinstance(pair, list) and len(pair) == 2:
                d.facts.append((_text(pair[0], 100), _text(pair[1])))
        for block in (head.get("texts") or [])[:50]:
            if isinstance(block, list) and len(block) == 2 and isinstance(block[1], list):
                d.texts.append((_text(block[0], 100), [_text(x, 400) for x in block[1][:1000]]))
        return d

    def _kill(self) -> None:
        """The process is ended, and reaped: no zombie is left."""
        proc = self._proc
        if proc is not None and proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass

    def close(self) -> None:
        if self._proc is None:
            return
        try:
            if self._proc.poll() is None:
                self._proc.stdin.write(b'{"op": "close"}\n')
                self._proc.stdin.flush()
                self._proc.wait(timeout=2)
        except (OSError, subprocess.TimeoutExpired, ValueError):
            pass
        self._kill()
        for f in (self._proc.stdin, self._proc.stdout):
            try:
                f.close()
            except OSError:
                pass
        self._proc = None


def use_remote() -> bool:
    """The libraries run apart when the sandbox works, and not inside the process
    that runs them apart."""
    return sandbox.available() and not os.environ.get("BBMP_REMOTE")


# ---- the process's side ----

def _send(out, head: dict, body: bytes = b"") -> None:
    if body:
        head["bytes"] = len(body)
    data = json.dumps(head).encode()
    out.write(struct.pack("<I", len(data)) + data + body)
    out.flush()


def _info_dict(info: TrackInfo) -> dict:
    return {"title": info.title, "author": info.author, "format": info.format,
            "duration": info.duration, "subsongs": info.subsongs, "subsong": info.subsong,
            "extra": dict(info.extra)}


def serve(backend: str, path: str, lengths: list[float]) -> int:
    from . import LOCAL_OPENERS
    # The answers go out on a copy of the pipe; what a library prints, from Python or
    # from C, goes to stderr and can't break it.
    sys.stdout.flush()
    out = os.fdopen(os.dup(1), "wb")
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    try:
        decoder = LOCAL_OPENERS[backend](Path(path), lengths)
    except (Unsupported, OSError) as e:
        _send(out, {"ok": False, "error": str(e), "needs_download": isinstance(e, NeedsDownload)})
        return 1
    sent = _info_dict(decoder.info)
    _send(out, {"ok": True, "backend": decoder.backend, "info": sent})
    for line in sys.stdin.buffer:
        try:
            cmd = json.loads(line)
            op = cmd.get("op")
            head: dict = {}
            body = b""
            if op == "read":
                body = decoder.read(int(cmd["frames"]))
                head["levels"] = decoder.channel_levels()
                if decoder.never_ends:
                    head["never_ends"] = True
            elif op == "subsong":
                decoder.select_subsong(int(cmd["n"]))
            elif op == "seek":
                head["done"] = decoder.seek(float(cmd["seconds"]))
            elif op == "forever":
                head["done"] = decoder.loop_forever(bool(cmd["on"]))
            elif op == "pattern":
                view = decoder.pattern_view(int(cmd["rows"]))
                head["view"] = list(view) if view else None
            elif op == "details":
                d = decoder.details()
                head["facts"], head["texts"] = d.facts, d.texts
            elif op == "close":
                break
            now = _info_dict(decoder.info)
            if now != sent:
                head["info"] = sent = now
        except Unsupported as e:
            head, body = {"unsupported": True, "error": str(e)}, b""
        _send(out, head, body)
    decoder.close()
    return 0


if __name__ == "__main__":
    os.environ["BBMP_REMOTE"] = "1"
    sys.exit(serve(sys.argv[1], sys.argv[2], json.loads(sys.argv[3])))
