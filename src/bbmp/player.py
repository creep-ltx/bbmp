# SPDX-License-Identifier: GPL-3.0-or-later
"""The playback engine: a queue, a playback thread, radio, repeat and shuffle.

The UI never touches decoders or the sound device. It sends commands and reads
`Player.status()`, a snapshot that is safe to read from any thread.

Songs follow each other without a gap: while one plays, the player decides what
comes next and opens it in the background, so the change takes no time. A song that
ends by itself plays out to its last sample; only skipping cuts the sound off.

Subsongs work the same for every replayer: when one ends, the next one plays
(settings.subsongs == "all") and subsongs that stay silent are skipped; with
"one", a file plays one subsong and the queue moves on.
"""

from __future__ import annotations

import queue
import random
import threading
import time
from dataclasses import dataclass, field
from typing import Callable

from .audio import NullOutput, open_output
from pathlib import Path

import numpy as np

from .backends import (Decoder, Details, TrackInfo, Unsupported,
                       backend_for, is_supported, open_song)
from .backends.base import FRAME_BYTES, RATE
from .backends.remote import use_remote
from .fetch import Fetcher
from . import archives, collection as collections_, ending, loudness, platforms, visuals
from .index import Index, Song
from .settings import Settings

CHUNK = 2400          # frames per write, 50 ms
# how a song ends: ending.py (the same rule for exports)
MAX_SUBSONG_SKIPS = 256
WAVE_FRAMES = 4096    # audio kept for the spectrum and oscilloscope
KEPT_FRAMES = WAVE_FRAMES + RATE // 2   # and half a second more: the output's latency

# Status fields that reset whenever nothing is playing.
# drawn from the sound itself, so every song has them; channel bars and scopes and the
# pattern need the replayer to say them, and are added as it does
ALWAYS_SHOWN = visuals.FROM_SOUND

_IDLE = dict(elapsed=0.0, levels=(0.0, 0.0), channels=[], pattern=None, wave=b"",
             shows=frozenset(ALWAYS_SHOWN))


@dataclass
class Status:
    state: str = "stopped"            # stopped | loading | playing | paused
    song: Song | None = None
    info: TrackInfo | None = None
    backend: str = ""
    elapsed: float = 0.0
    error: str = ""
    volume: int = 80
    loudness: float | None = None     # of the song, in LUFS, as far as it is known
    level_db: float = 0.0             # by how much it is played louder (or softer) for that
    levels: tuple[float, float] = (0.0, 0.0)
    channels: list[float] = field(default_factory=list)
    pattern: tuple[int, list[str]] | None = None
    wave: bytes = b""                 # the most recent audio (int16 stereo), for visualisers
    shows: frozenset = frozenset(ALWAYS_SHOWN)   # the visualisers the song that plays has data for
    queue_pos: int = -1
    queue_len: int = 0
    radio: str = ""                   # description of the radio scope, "" when off
    repeat: str = "off"
    shuffle: bool = False
    forever: bool = False     # the song that plays goes on and on ("o"), until another starts
    subsongs: str = "all"


@dataclass
class Radio:
    fmt: str | None = None
    artist: str | None = None
    collection: str = ""      # "" plays from every collection
    only: bool = False        # the catalog has no other collection, so there is no need to name it
    view: tuple[str, str] | None = None     # a platform or chip it keeps to (platforms.py)

    def describe(self) -> str:
        where = collections_.name(self.collection) if self.collection else ""
        on = platforms.name(*self.view) if self.view else ""
        if not self.fmt:
            if on:
                return f"{on} ({where})" if where and not self.only else on
            return f"all of {where}" if where else "every collection"
        what = f"{self.fmt} / {self.artist}" if self.artist else self.fmt
        if on:
            what = f"{what}, {on}"
        return what if self.only or not where else f"{what} ({where})"


@dataclass
class _Ready:
    """The next song, fetched and opened ahead of time."""
    song: Song
    file: Path | None = None
    decoder: Decoder | None = None
    details: Details | None = None
    ahead: bytes | None = None        # the song's beginning, read to measure its loudness
    marks: list = field(default_factory=list)   # and what the visualisers showed then
    preview: float | None = None      # and what it measured


class _Preparing:
    """A song being made ready in the background: whoever wants it when it is the one
    to play waits for it to be done, rather than starting again."""

    def __init__(self, song: Song):
        self.song = song
        self.done = threading.Event()
        self.ready: _Ready | None = None
        self.dropped = False              # nobody wants it: what it opens is closed


class Player:
    def __init__(self, index: Index, fetcher: Fetcher, output=None,
                 on_song: Callable[[Song], None] | None = None,
                 on_done: Callable[[Song, float], None] | None = None,
                 settings: Settings | None = None,
                 keep: Callable[[], set[str]] | None = None, levels=None, trouble=None):
        """on_song(song) when a song starts, on_done(song, seconds listened) when it ends;
        keep() returns the keys of songs the cache must not delete (favorites and such);
        levels keeps the loudness of songs (loudness(key, subsong) and set_loudness);
        trouble (trouble.Trouble) is told of songs that fail or stay silent."""
        self.index = index
        self.fetcher = fetcher
        if getattr(index, "kept_dir", True) is None:
            index.kept_dir = getattr(fetcher, "kept_dir", None)   # its files are the collections' songs
        if getattr(fetcher, "checksum", True) is None:
            fetcher.checksum = index.md5       # what a server gives is checked against the catalog
        self.settings = settings or Settings()
        self._output = output
        self._on_song = on_song
        self._on_done = on_done
        self._keep = keep
        self._levels = levels
        self.trouble = trouble
        self._leveller: loudness.Leveller | None = None
        self._level_of: tuple[str, int] | None = None     # the song and subsong it is for
        self._ahead = b""                   # sound that was read ahead and is played first
        # what the channel bars and the pattern showed as it was read: (where in it that
        # was, channel levels, pattern), and how far into it the song is
        self._marks: list[tuple[int, list | None, tuple | None]] = []
        self._shows: set[str] = set(ALWAYS_SHOWN)    # what the song has: Status.shows
        self._probed = False                         # its replayer asked for a pattern once
        self._ahead_at = 0
        self._shown: tuple[int, list | None, tuple | None] | None = None
        self._cmds: queue.Queue = queue.Queue()
        self._lock = threading.Lock()
        self._status = Status()
        self._queue: list[Song] = []
        self._pos = -1
        self._radio: Radio | None = None
        self._decoder: Decoder | None = None
        self._paused = False
        self._frames = 0                    # frames of the subsong read from its replayer
        self._played = 0                    # and given to the output (silence held back)
        self._silence = ending.Silence(RATE, FRAME_BYTES)
        self._want_pattern = False
        self._recent = b""
        self._song: Song | None = None      # the song the listened time belongs to
        self._listened = 0                  # frames played of the current song, all subsongs
        self._heard = False                 # has the current subsong made a sound yet
        self._skips = 0                     # silent subsongs skipped in a row
        self._failures = 0                  # errors in a row, in the player thread
        self._forever: str | None = None    # the key of the song that goes on and on
        # shuffle: the queue positions played this time round, and in the order played
        self._shuffled: set[int] = set()
        self._history: list[int] = []
        self._subsong = 0
        self._first_subsong = 0             # where the current file started
        self._details: Details | None = None
        self._file: Path | None = None
        self._planned: tuple | None = None  # (signature, what plays next)
        self._preparing: _Preparing | None = None   # the next song, being made ready
        # what is done when a song has started, after its first sound (_after_start)
        self._started: Song | None = None
        self._pattern_at = -RATE            # when the pattern view was last asked for
        self._pattern: tuple | None = None
        # fetchers of their own, with connections of their own: getting the next song
        # ready, and fetching the song the cursor is on, never wait for a download of
        # the song that is to play now (made when first needed)
        self._prep_fetcher: Fetcher | None = None
        self._prefetcher: Fetcher | None = None
        self._prefetch_want: Song | None = None
        self._prefetch_wake = threading.Event()
        self._prefetch_thread: threading.Thread | None = None
        self._status.volume = self.settings.volume
        self._thread = threading.Thread(target=self._run, name="player", daemon=True)
        self._thread.start()

    # ---- commands (any thread) ----

    def play_list(self, songs: list[Song], start: int = 0) -> None:
        self._cmds.put(("play_list", list(songs), start))

    def enqueue(self, songs: list[Song]) -> None:
        self._cmds.put(("enqueue", list(songs)))

    def play_next(self, songs: list[Song]) -> None:
        """Put songs right after the one that plays, in their order."""
        self._cmds.put(("play_next", list(songs)))

    def radio(self, fmt: str | None = None, artist: str | None = None,
              collection: str = "", view: tuple[str, str] | None = None) -> None:
        """Endless random songs, optionally from one format, artist or collection, and
        from a view's platform or chip."""
        hosted = collections_.hosted()
        own = [c for c, *_ in self.index.collections() if c not in hosted]
        if len(own) == 1 and not collection:
            collection = own[0]         # every collection is that one
        only = len(own) == 1 and collection == own[0]
        self._cmds.put(("radio", Radio(fmt, artist, collection, only=only, view=view)))

    def radio_off(self) -> None:
        self._cmds.put(("radio", None))

    def forever(self) -> None:
        """The song that plays goes on and on, until another song starts (or this is
        pressed again): a SID tune loops, and a song that ends starts again."""
        self._cmds.put(("forever",))

    def toggle_pause(self) -> None:
        self._cmds.put(("pause",))

    def stop(self) -> None:
        self._cmds.put(("stop",))

    def next(self) -> None:
        self._cmds.put(("next",))

    def prev(self) -> None:
        self._cmds.put(("prev",))

    def jump(self, pos: int) -> None:
        self._cmds.put(("jump", pos))

    def subsong(self, delta: int) -> None:
        self._cmds.put(("subsong", delta))

    def seek(self, delta: float) -> None:
        self._cmds.put(("seek", delta))

    def set_volume(self, volume: int) -> None:
        volume = max(0, min(100, volume))
        with self._lock:
            self._status.volume = volume
        self.settings.update(volume=volume)

    def restore(self, songs: list[Song], pos: int, subsong: int = 0, seconds: float = 0.0) -> None:
        """Load a saved queue, paused at the given song, subsong and position."""
        self._cmds.put(("restore", list(songs), pos, subsong, seconds))

    def set_queue(self, songs: list[Song], pos: int, expect: list[str]) -> None:
        """Put a whole queue in place of the one there is, the song at `pos` being the
        one that plays (or is paused) now: nothing is interrupted. Done only if the
        queue is still `expect` (keys) by then; otherwise the user has moved on."""
        self._cmds.put(("set_queue", list(songs), pos, list(expect)))

    def session(self) -> dict:
        """What restore() needs to pick up where playback is now."""
        with self._lock:
            st = self._status
            return {"queue": [s.key for s in self._queue], "pos": self._pos,
                    "subsong": st.info.subsong if st.info else 0,
                    "seconds": st.elapsed if st.state in ("playing", "paused") else 0.0}

    def want_pattern(self, on: bool) -> None:
        self._want_pattern = on

    def prefetch(self, song: Song | None) -> None:
        """Download a song (and its companion files) into the cache in the background,
        so that it starts at once if it is chosen: for the song the cursor rests on.
        A newer wish replaces one that hasn't started; one being downloaded is finished.
        Never waits, and doesn't hold up the player or its downloads."""
        if song is None or song.local or self.fetcher_of("prefetch") is None:
            return
        with self._lock:
            self._prefetch_want = song
            if self._prefetch_thread is None:
                self._prefetch_thread = threading.Thread(
                    target=self._prefetch_loop, name="prefetch", daemon=True)
                self._prefetch_thread.start()
        self._prefetch_wake.set()

    def _prefetch_loop(self) -> None:
        while True:
            self._prefetch_wake.wait()
            self._prefetch_wake.clear()
            with self._lock:
                song, self._prefetch_want = self._prefetch_want, None
            if song is None:
                continue
            if song is _QUIT:
                return
            try:
                fetcher = self.fetcher_of("prefetch")
                fetcher.get(song.key, self.index.companions(song.key))
            except Exception:       # it is fetched again, and the error told, if it is played
                pass

    def fetcher_of(self, use: str) -> Fetcher | None:
        """A fetcher of its own for work in the background ("prepare", "prefetch"), or
        None where the player's fetcher can't make one (then the player's is used)."""
        attr = "_prep_fetcher" if use == "prepare" else "_prefetcher"
        got = getattr(self, attr)
        if got is not None:
            return got
        clone = getattr(self.fetcher, "clone", None)
        if clone is None:
            return None
        try:
            made = clone()
        except Exception:
            return None
        with self._lock:
            got = getattr(self, attr)
            if got is None:
                got = made
                setattr(self, attr, made)
        if got is not made:
            made.close()            # another thread made one first
        return got

    def shutdown(self) -> None:
        self._cmds.put(("quit",))
        self._thread.join(timeout=3)
        with self._lock:
            self._prefetch_want = _QUIT
        self._prefetch_wake.set()
        for fetcher in (self._prep_fetcher, self._prefetcher):
            if fetcher is not None:
                try:
                    fetcher.close()
                except Exception:
                    pass

    # ---- state (any thread) ----

    def status(self) -> Status:
        with self._lock:
            s = self._status
            return Status(**{k: getattr(s, k) for k in s.__dataclass_fields__})

    def queue(self) -> tuple[list[Song], int]:
        with self._lock:
            return list(self._queue), self._pos

    def details(self) -> tuple[Song | None, Details | None, Path | None]:
        """What the playing file says about itself, and where the file is."""
        with self._lock:
            return self._status.song, self._details, self._file

    # ---- playback thread ----

    def _set(self, **kw) -> None:
        with self._lock:
            for k, v in kw.items():
                setattr(self._status, k, v)
            self._status.queue_pos = self._pos
            self._status.queue_len = len(self._queue)
            self._status.radio = self._radio.describe() if self._radio else ""
            self._status.repeat = self.settings.repeat
            self._status.shuffle = self.settings.shuffle
            self._status.forever = self._song is not None and self._forever == self._song.key
            self._status.subsongs = self.settings.subsongs

    def _out(self):
        if self._output is None:
            try:
                self._output = open_output()
            except Exception as e:  # no sound server: keep going silently
                self._output = NullOutput(realtime=True)
                self._set(error=f"No sound output: {e}")
        return self._output

    def _run(self) -> None:
        if self._output is None:
            self._out()     # the sound device is opened now, not as the first song starts
        while True:
            if self._decoder is not None and not self._paused:
                try:
                    cmd = self._cmds.get_nowait()
                except queue.Empty:
                    self._safely(self._render_chunk)
                    if self._started is not None and self._frames >= RATE:
                        self._safely(self._after_start)     # a second without a sound
                    continue
            else:
                if self._started is not None:
                    self._safely(self._after_start)
                cmd = self._cmds.get()
            if cmd[0] == "quit":
                for step in (self._finish_song, self._close_decoder, self._discard_ready):
                    try:
                        step()
                    except Exception:
                        pass
                if self._output:
                    self._output.close()
                return
            self._safely(self._handle, cmd)

    def _safely(self, work, *args) -> None:
        """Do `work`; whatever goes wrong in it is the song's failure, never the end of
        the player (which would go on saying "playing" and hear no key)."""
        try:
            work(*args)
            self._failures = 0
        except Exception as e:
            self._failures += 1
            song = self._song
            self._song = None           # what was heard of it is not reported
            self._started = None        # nor that it started, if it failed before a sound
            try:
                self._close_decoder()
            except Exception:
                self._decoder = None
            self._leveller = self._level_of = None
            self._clear_ahead()
            what = f"{song.title}: {e}" if song is not None else str(e)
            if self.trouble is not None and song is not None and not song.local:
                try:
                    self.trouble.note(song.key, "failed", str(e).split("\n")[0][:200])
                except Exception:
                    pass
            if self._failures >= 3:
                self._set(state="stopped", error=what, **_IDLE)
                return
            self._set(error=what)
            try:
                self._advance()
            except Exception as e2:
                self._set(state="stopped", error=str(e2), **_IDLE)

    def _handle(self, cmd: tuple) -> None:
        name = cmd[0]
        if name == "play_list":
            with self._lock:
                self._queue, self._pos = cmd[1], cmd[2]
            self._shuffled, self._history = set(), []
            self._play_current()
        elif name == "enqueue":
            with self._lock:
                self._queue.extend(cmd[1])
            if self._decoder is None and self._status.state != "paused":
                with self._lock:
                    self._pos = len(self._queue) - len(cmd[1])
                self._play_current()
            else:
                self._set()
                self._prepare_next()
        elif name == "play_next":
            if self._decoder is None and self._status.state != "paused":
                self._handle(("enqueue", cmd[1]))       # nothing plays: they play now
                return
            with self._lock:
                self._queue[self._pos + 1:self._pos + 1] = cmd[1]
            n = len(cmd[1])             # the positions after it move on
            self._shuffled = {i + n if i > self._pos else i for i in self._shuffled}
            self._history = [i + n if i > self._pos else i for i in self._history]
            self._set()
            self._prepare_next()                        # what was ready is not next any more
        elif name == "radio":
            # Radio takes over right away: drop what was queued after the current song.
            self._radio = cmd[1]
            if self._radio:
                with self._lock:
                    del self._queue[self._pos + 1:]
                self._advance()
            else:
                self._set()
                if self._decoder is not None:
                    self._prepare_next()
        elif name == "restore":
            _, songs, pos, subsong, seconds = cmd
            with self._lock:
                self._queue, self._pos = songs, pos
            self._shuffled, self._history = set(), []
            if self._start_current():
                d = self._decoder
                if 0 < subsong < d.info.subsongs:
                    try:
                        d.select_subsong(subsong)
                        self._subsong = subsong
                        self._level_begin()
                    except Unsupported:
                        pass
                if seconds > 1 and d.seek(seconds):
                    self._frames = int(seconds * RATE)
                    self._played = self._frames
                    self._silence.clear()
                    self._clear_ahead()    # what was read for measuring is the song's start
                self._paused = True
                self._set(state="paused", info=d.info, elapsed=self._frames / RATE)
        elif name == "set_queue":
            _, songs, pos, expect = cmd
            if ([s.key for s in self._queue] == expect and 0 <= pos < len(songs)
                    and 0 <= self._pos < len(self._queue)
                    and songs[pos].key == self._queue[self._pos].key):
                with self._lock:
                    self._queue, self._pos = songs, pos
                self._shuffled, self._history = set(), []
                self._set()
                if self._decoder is not None and self._started is None:
                    self._prepare_next()        # what follows it is another song now
        elif name == "forever":
            if self._song is not None:
                on = self._forever != self._song.key
                self._forever = self._song.key if on else None
                if self._decoder is not None:
                    self._decoder.loop_forever(on)
                self._prepare_next()        # what follows is decided anew
            self._set()
        elif name == "pause":
            if self._decoder:
                self._paused = not self._paused
                if self._paused and self._output:
                    self._output.flush()
                self._set(state="paused" if self._paused else "playing")
        elif name == "stop":
            self._finish_song()
            self._close_decoder()
            self._discard_ready()
            self._radio = None
            self._set(state="stopped", **_IDLE)
        elif name == "next":
            # "next" pressed many times in a row goes that many songs on at once, and
            # opens only the last of them
            more = 0
            with self._cmds.mutex:
                while self._cmds.queue and self._cmds.queue[0] == ("next",):
                    self._cmds.queue.popleft()
                    more += 1
            for _ in range(more):
                if not self._step():
                    return
            self._advance()
        elif name == "prev":
            if self._status.elapsed <= 3:
                if self.settings.shuffle and not self._radio and len(self._history) > 1:
                    self._history.pop()             # the song that plays
                    back = self._history.pop()      # is put back when it starts
                    self._shuffled.discard(self._pos)
                    with self._lock:
                        self._pos = back
                elif self._pos > 0:
                    with self._lock:
                        self._pos -= 1
            self._play_current()
        elif name == "jump":
            if 0 <= cmd[1] < len(self._queue):
                with self._lock:
                    self._pos = cmd[1]
                self._play_current()
        elif name == "subsong" and self._decoder:
            info = self._decoder.info
            n = info.subsong + cmd[1]
            if 0 <= n < info.subsongs:
                try:
                    self._select_subsong(n)
                    if self._output:
                        self._output.flush()
                except Unsupported as e:
                    self._set(error=str(e))
        elif name == "seek" and self._decoder:
            target = max(0.0, self._frames / RATE + cmd[1])
            try:
                moved = self._decoder.seek(target)
            except Unsupported as e:            # the replayer couldn't start again there
                self._set(error=f"Can't go there: {e}")
                self._close_decoder()
                self._song_ended()
                return
            if moved:
                self._clear_ahead()
                self._frames = int(target * RATE)
                self._played = self._frames
                self._silence.clear()
                if self._output:
                    self._output.flush()

    def _play_current(self) -> None:
        """Start the song at the queue position, skipping forward past songs that fail."""
        if not self._start_current() and self._cmds.empty():
            self._advance()

    def _advance(self, natural: bool = False) -> None:
        """Go to the next playable song. `natural`: the current one ended by itself."""
        for attempt in range(50):
            first = attempt == 0
            if not self._step(natural and first):
                return
            if self._start_current(flush=not (natural and first)) or not self._cmds.empty():
                return
        self._set(state="stopped", error="Gave up after 50 songs in a row failed to play")

    def _signature(self) -> tuple:
        """Everything the choice of the next song depends on."""
        return (len(self._queue), self._pos, self._radio.describe() if self._radio else None,
                self.settings.shuffle, self.settings.repeat, len(self._shuffled))

    def _choose(self, natural: bool) -> tuple[int, Song, bool] | None:
        """What follows the current song: (queue position, song, new to the queue)."""
        n = len(self._queue)
        if natural and self.settings.repeat == "one" and not self._radio and 0 <= self._pos < n:
            return self._pos, self._queue[self._pos], False
        if self.settings.shuffle and n > 1 and not self._radio:
            # every song once, in an order of chance; then again with repeat, else the end
            left = [i for i in range(n) if i != self._pos and i not in self._shuffled]
            if not left and self.settings.repeat == "all":
                self._shuffled = {self._pos}        # round again
                left = [i for i in range(n) if i != self._pos]
            if not left:
                return None
            pos = random.choice(left)
            return pos, self._queue[pos], False
        if self._pos + 1 < n:
            return self._pos + 1, self._queue[self._pos + 1], False
        if self._radio:
            song = self.index.random_song(fmt=self._radio.fmt, artist=self._radio.artist,
                                          view=self._radio.view,
                                          collection=self._radio.collection,
                                          playable=self._radio_plays)
            return (n, song, True) if song is not None else None
        if self.settings.repeat == "all" and n:
            return 0, self._queue[0], False
        return None

    def _step(self, natural: bool = False) -> bool:
        """Move the queue position on. False (and stopped) when there is nothing next."""
        planned, self._planned = self._planned, None
        repeating = self.settings.repeat == "one" and not self._radio
        if planned and planned[0] == self._signature() and (natural or not repeating):
            choice = planned[1]     # what was decided, and prepared, when this song started
        else:
            choice = self._choose(natural)
        if choice is None:
            self._close_decoder(flush=not natural)
            if self._radio:
                self._radio = None
                self._set(state="stopped", error="Radio: nothing playable here")
            else:
                self._set(state="stopped", **_IDLE)
            return False
        pos, song, new = choice
        with self._lock:
            if new:
                self._queue.append(song)
            self._pos = pos
        return True

    def _song_ended(self) -> None:
        """The current song is over (all the subsongs we meant to play)."""
        self._advance(natural=True)

    def _track_ended(self) -> None:
        """The current subsong is over: on to the next subsong, or the next song; or,
        going on and on, the same one from its start."""
        self._level_end(ended=self._heard)
        info = self._decoder.info
        if self._song is not None and self._forever == self._song.key:
            try:
                self._select_subsong(info.subsong)
                self._decoder.loop_forever(True)
                return
            except Unsupported:         # it has no subsongs to go back to: open it again
                if self._start_current(flush=False):
                    return
        # Start with the file's own start song, then go round through the others.
        nxt = (info.subsong + 1) % max(1, info.subsongs)
        if (self.settings.subsongs == "all" and info.subsongs > 1 and nxt != self._first_subsong
                and self._skips < MAX_SUBSONG_SKIPS):
            try:
                self._select_subsong(nxt)
                return
            except Unsupported:
                pass
        self._song_ended()

    def _select_subsong(self, n: int) -> None:
        self._decoder.select_subsong(n)
        details = self._decoder.details()   # title and comment can differ per subsong
        with self._lock:
            self._details = details
        self._subsong = self._decoder.info.subsong
        self._frames = 0
        self._played = self._frames
        self._silence.clear()
        self._heard = False
        self._set(info=self._decoder.info, elapsed=0.0)
        self._level_begin()

    def _finish_song(self) -> None:
        """Report the listening time of the song that is ending."""
        if self._started is not None:
            self._after_start(report_only=True)     # it is told that it started first
        self._level_end()
        if self._song is not None and self._on_done:
            self._on_done(self._song, self._listened / RATE)
        self._song = None
        self._listened = 0

    # ---- the same loudness for every song ----

    def _known_loudness(self, key: str, subsong: int) -> float | None:
        try:
            return self._levels.loudness(key, subsong) if self._levels else None
        except Exception:       # what keeps them is a convenience, and never stops a song
            return None

    @staticmethod
    def _read_ahead(decoder: Decoder) -> tuple[bytes, list]:
        """The song's beginning, for measuring, and what the channel bars and the pattern
        view showed as it was read: the decoder is that far on when the song starts, so
        those are shown with the sound, not what the decoder says then."""
        want, got, marks, at = int(loudness.PREVIEW * RATE) * FRAME_BYTES, [], [], 0
        pattern = True
        while want > 0:
            pcm = decoder.read(min(CHUNK * 2, want // FRAME_BYTES))
            if not pcm:
                break
            got.append(pcm)
            want -= len(pcm)
            at += len(pcm)
            view = decoder.pattern_view(11) if pattern else None
            pattern = view is not None          # a replayer without one is not asked again
            marks.append((at, decoder.channel_levels(), view))
        return b"".join(got), marks

    def _level_begin(self, ready: _Ready | None = None) -> None:
        """Find the level the subsong that starts is played at."""
        self._level_end()
        self._leveller, self._level_of = None, None
        # what was read ahead has left the decoder already: it is played, whatever else
        self._ahead = ready.ahead if ready is not None and ready.ahead else b""
        self._marks = ready.marks if self._ahead else []
        self._ahead_at, self._shown = 0, None
        self._shows, self._probed = set(ALWAYS_SHOWN), False
        for _, channels, view in self._marks:
            self._shows |= set(visuals.FROM_CHANNELS) if channels else set()
            self._shows |= {"pattern"} if view is not None else set()
        self._set(shows=frozenset(self._shows))
        song = self._song or (self._queue[self._pos] if 0 <= self._pos < len(self._queue) else None)
        if not self.settings.level or self._decoder is None or song is None:
            self._set(loudness=None, level_db=0.0)
            return
        self._level_of = (song.key, self._decoder.info.subsong)
        known = self._known_loudness(*self._level_of)
        preview = None
        if known is None:
            if self._ahead:
                preview = ready.preview
            else:
                self._ahead, self._marks = self._read_ahead(self._decoder)
                preview = loudness.measure(self._ahead)
        self._leveller = loudness.Leveller(known, preview)
        self._set(loudness=known if known is not None else preview, level_db=self._leveller.db)

    def _level_end(self, ended: bool = False) -> None:
        """Keep the loudness of the subsong that ends, if enough of it was heard."""
        leveller, of = self._leveller, self._level_of
        self._leveller = self._level_of = None
        if leveller is None or of is None or self._levels is None:
            return
        measured = leveller.result(ended)
        if measured is not None:
            try:
                self._levels.set_loudness(of[0], of[1], measured)
            except Exception:
                pass

    def _read(self, frames: int) -> bytes:
        """The next sound of the song: what was read ahead first."""
        if self._ahead:
            want = frames * FRAME_BYTES
            pcm, self._ahead = self._ahead[:want], self._ahead[want:]
            self._ahead_at += len(pcm)
            while self._marks and self._marks[0][0] <= self._ahead_at:
                self._shown = self._marks.pop(0)
            return pcm
        self._shown = None
        self._marks = []
        return self._decoder.read(frames)

    def _clear_ahead(self) -> None:
        self._ahead, self._marks, self._shown, self._ahead_at = b"", [], None, 0

    def _trim_cache(self) -> None:
        limit = self.settings.cache_limit_mb * 1_000_000
        if self._song is not None and self._song.local and archives.split(self._song.path):
            # archives unpacked long ago go, beyond 2 GB; not the one that plays
            playing = archives.folder_of(archives.split(self._song.path)[0])
            threading.Thread(target=archives.trim, kwargs={"keep": playing},
                             name="unpacked-trim", daemon=True).start()
        queued = [s.key for s in self._queue if not s.local]

        def work():
            # the first look at the cache's size goes through every file in it: not in
            # the thread that plays
            try:
                if not self.fetcher.over_limit(limit):
                    return
                keep = set(queued)
                if self._keep:
                    keep |= self._keep()
                for p in list(keep):
                    keep.update(self.index.companions(p))
                self.fetcher.trim(limit, keep)
            except OSError:
                pass
        threading.Thread(target=work, name="cache-trim", daemon=True).start()

    def _file_of(self, song: Song, fetcher: Fetcher | None = None) -> tuple[Path, list[float]]:
        """The song as a file on disk, downloaded if need be, and its subsongs' lengths."""
        if song.local:
            file = archives.file_of(song.path)      # itself, or unpacked from its archive
            if not file.is_file():
                raise FileNotFoundError("the file is gone")
            return file, []
        file = (fetcher or self.fetcher).get(song.key, self.index.companions(song.key))
        return file, self.index.lengths(song.key)

    def _start_current(self, flush: bool = True) -> bool:
        self._finish_song()
        self._close_decoder(flush)
        if not 0 <= self._pos < len(self._queue):
            self._discard_ready()
            self._set(state="stopped")
            return False
        song = self._queue[self._pos]
        self._set(state="loading", song=song, info=None, backend="", error="", **_IDLE)
        ready = self._take_ready(song)      # waits for it if it is being made ready
        try:
            if ready is not None:
                local, details, self._decoder = ready.file, ready.details, ready.decoder
            else:
                local, lengths = self._file_of(song)
            if ready is None:
                self._decoder = open_song(local, song.format, lengths, local=song.local)
                details = self._decoder.details()
        except (Unsupported, OSError, EOFError) as e:
            self._set(error=f"{song.title}: {e}")
            # A replayer that refuses the file, or a file no server has, is the song's
            # trouble; a server that doesn't answer is not.
            if self.trouble is not None and not song.local and isinstance(
                    e, (Unsupported, FileNotFoundError, EOFError)):
                self.trouble.note(song.key, "failed", str(e).split("\n")[0][:200])
            time.sleep(0.3)
            return False
        self._paused = False
        self._frames = 0
        self._played = self._frames
        self._silence.clear()
        if self.settings.shuffle:
            self._shuffled.add(self._pos)
            if not self._history or self._history[-1] != self._pos:
                self._history.append(self._pos)
        self._subsong = self._first_subsong = self._decoder.info.subsong
        self._heard = False
        self._skips = 0
        self._song = song
        with self._lock:
            self._details, self._file = details, local
        if self._forever is not None and self._forever != song.key:
            self._forever = None            # on and on was for the song that played
        if self._forever == song.key:
            self._decoder.loop_forever(True)
        self._level_begin(ready)       # reads the song's beginning, which takes a moment
        self._set(state="playing", info=self._decoder.info, backend=self._decoder.backend)
        self._pattern_at, self._pattern = -RATE, None
        # telling of the song, choosing the next and tidying the cache wait until the
        # song is heard (or paused, or a second has gone by in silence): _after_start
        self._started = song
        return True

    def _after_start(self, report_only: bool = False) -> None:
        """What is done once a song has started, after its first sound is on its way."""
        song, self._started = self._started, None
        if song is None:
            return
        if self._on_song:
            self._on_song(song)
        if report_only or self._song is not song:
            return          # another song starts: it is prepared for when it does
        if not (self._preparing is not None and self._planned
                and self._planned[0] == self._signature()):
            self._prepare_next()        # (unless a change of the queue did it meanwhile)
        self._trim_cache()

    # ---- the next song, opened ahead of time ----

    def _radio_plays(self, song: Song) -> bool:
        """Radio passes over what can't be played and what is known to fail."""
        return is_supported(song.format) and not (self.trouble and self.trouble.get(song.key))

    def _prepare_next(self) -> None:
        """Decide what plays after this song and get it ready in the background."""
        choice = self._choose(natural=True)
        self._planned = (self._signature(), choice)
        self._discard_ready()
        if choice is None:
            return
        song = choice[1]
        prep = _Preparing(song)
        with self._lock:
            self._preparing = prep

        def work():
            ready = _Ready(song)
            try:
                ready.file, lengths = self._file_of(song, self.fetcher_of("prepare"))
                # SunVox's library plays one project at a time, so in the player's own
                # process its songs can't be opened early; in a process of their own they can
                if backend_for(song.format) != "sunvox" or use_remote():
                    ready.decoder = open_song(ready.file, song.format, lengths, local=song.local)
                    ready.details = ready.decoder.details()
                    if self.settings.level and self._known_loudness(
                            song.key, ready.decoder.info.subsong) is None:
                        ready.ahead, ready.marks = self._read_ahead(ready.decoder)
                        ready.preview = loudness.measure(ready.ahead)
            except Exception:
                if ready.decoder is not None:
                    try:
                        ready.decoder.close()
                    except Exception:
                        pass
                ready.decoder = None
            with self._lock:
                dropped = prep.dropped
                if not dropped:
                    prep.ready = ready
            prep.done.set()
            if dropped and ready.decoder is not None:
                ready.decoder.close()
        threading.Thread(target=work, name="prepare-next", daemon=True).start()

    def _take_ready(self, song: Song) -> _Ready | None:
        """The prepared song, if it is the one that is about to play. If it is still
        being made ready, that is waited for: it is that much on its way already."""
        with self._lock:
            prep, self._preparing = self._preparing, None
        if prep is None:
            return None
        if prep.song.key != song.key:
            self._drop(prep)
            return None
        while not prep.done.wait(0.1):
            if not self._cmds.empty():          # something else was asked for meanwhile
                self._drop(prep)
                return None
        ready = prep.ready
        if ready is None or ready.decoder is None:
            return None     # it failed there: tried again here (the file may be there now)
        return ready

    def _drop(self, prep: _Preparing) -> None:
        with self._lock:
            prep.dropped = True
            ready, prep.ready = prep.ready, None
        if ready is not None and ready.decoder is not None:
            ready.decoder.close()

    def _discard_ready(self) -> None:
        with self._lock:
            prep, self._preparing = self._preparing, None
        if prep is not None:
            self._drop(prep)

    def _close_decoder(self, flush: bool = True) -> None:
        if self._decoder:
            self._decoder.close()
            self._decoder = None
        self._paused = False
        if flush and self._output:
            self._output.flush()

    def _render_chunk(self) -> None:
        """The next piece of the song, played by the one rule of how songs end (ending.py)."""
        d = self._decoder
        pcm = self._read(CHUNK)
        elapsed = self._frames / RATE
        stop = ending.limit(d.backend, d.info.duration, d.never_ends)
        if self._song is not None and self._forever == self._song.key:
            stop = float("inf")             # on and on: the player doesn't stop it
        gave_up = not self._heard and elapsed >= ending.patience(d.info.subsongs)
        if not pcm or elapsed >= stop or gave_up:
            self._silence.clear()           # silence at the end is not played
            self._skips = self._skips + 1 if not self._heard else 0
            if (gave_up and d.info.subsongs == 1 and self.trouble is not None
                    and self._song is not None and not self._song.local):
                self.trouble.note(self._song.key, "silent",
                                  f"stayed silent for {ending.SILENT_SONG:.0f} seconds "
                                  f"({d.backend})")
            self._track_ended()
            return
        self._frames += len(pcm) // FRAME_BYTES
        silent = _peak(np.frombuffer(pcm, dtype="<i2")) <= ending.SILENT_LEVEL
        if not self._heard:
            if silent:
                # Silence before the first sound isn't played: the song starts on its
                # first note, and empty subsongs are passed over at once.
                self._played = self._frames
                return
            self._heard = True
            if self.trouble is not None and self._song is not None:
                self.trouble.played(self._song.key)
        pieces = self._silence.take(pcm, silent)
        if pieces is None:                  # silent for SILENT_END: the song is over
            self._track_ended()
            return
        for piece in pieces:
            self._play(piece, d, stop)

    def _play(self, pcm: bytes, d: Decoder, stop: float) -> None:
        """Give a piece of the song to the output: at the volume, the loudness evened
        out, faded where the player stops the song."""
        gain = self._status.volume / 100 * ending.fade(self._played / RATE, stop)
        if self._leveller is not None:
            out_pcm = self._leveller.process(pcm, gain)
            if abs(self._leveller.db - self._status.level_db) >= 0.05:
                self._set(level_db=self._leveller.db)
        elif gain < 0.999:
            out_pcm = (np.frombuffer(pcm, dtype="<i2") * gain).astype("<i2").tobytes()
        else:
            out_pcm = pcm
        samples = np.frombuffer(out_pcm, dtype="<i2")
        self._played += len(pcm) // FRAME_BYTES
        self._listened += len(pcm) // FRAME_BYTES
        levels = (_peak(samples[0::2]), _peak(samples[1::2]))
        mark = self._shown or (self._marks[0] if self._marks else None)
        if mark is not None:            # the sound read ahead: what was shown with it
            channels, pattern = mark[1] or [], mark[2] if self._want_pattern else None
        else:
            channels = d.channel_levels() or []
            pattern = None
            if self._want_pattern:
                # the screen shows it about twelve times a second: asked for at most
                # ten times a second, not for every 50 ms written
                if self._played - self._pattern_at >= RATE // 10:
                    self._pattern_at, self._pattern = self._played, d.pattern_view(11)
                pattern = self._pattern
        shows = set(self._shows)
        if channels:
            shows.update(visuals.FROM_CHANNELS)
        if pattern is not None or (not self._probed and d.pattern_view(1) is not None):
            shows.add("pattern")
        self._probed = True
        out = self._out()
        # what is heard now is the output's latency behind what was written: the
        # spectrum and scope show that, not the newest sound (up to 0.2 s ahead)
        self._recent = (self._recent + out_pcm)[-KEPT_FRAMES * FRAME_BYTES:]
        behind = min(int(out.latency() * RATE), KEPT_FRAMES - WAVE_FRAMES) * FRAME_BYTES
        heard = self._recent[:len(self._recent) - behind] if behind else self._recent
        if shows != self._shows:
            self._shows = shows
            self._set(shows=frozenset(shows))
        self._set(elapsed=max(0.0, self._played / RATE - out.latency()), levels=levels,
                  channels=channels, pattern=pattern, wave=heard[-WAVE_FRAMES * FRAME_BYTES:])
        out.write(out_pcm)
        if self._started is not None:
            self._after_start()         # the first sound is on its way: now the rest


_QUIT = object()        # tells the prefetch thread to end


def _peak(a) -> float:
    """The peak of int16 samples (an array or numpy), as a fraction of full scale."""
    if not len(a):
        return 0.0
    a = np.asarray(a, dtype=np.int16)
    return max(int(a.max()), -int(a.min())) / 32768
