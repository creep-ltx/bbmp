# SPDX-License-Identifier: GPL-3.0-or-later
"""Export songs as FLAC, MP3, Ogg, Opus or WAV files, for phones, car stereos and
other players.

A song is rendered the way the player would play it: silence before the first note
is left out, subsongs that stay silent are skipped, and songs that loop forever end
at their stated length with a fade. With "Same loudness for all songs" on, every file
is made as loud as the player plays it, measured on the whole song. Rendering runs as
fast as the replayer can go.

Several songs (a playlist, an artist, the queue) go into a folder of their own, with
their number in front so that they sort in their order.
"""

from __future__ import annotations

import os
import re
import threading
import shutil
import subprocess
import wave
from pathlib import Path
from typing import Callable

import numpy as np

from . import archives, collection as collections_, ending, loudness, paths
from .backends import Decoder, Unsupported, open_song
from .backends.base import FRAME_BYTES, RATE
from .fetch import Fetcher
from .index import Index, Song
from .settings import Settings

CHUNK = 4800
MAX_SECONDS = ending.LONGEST    # as the player: no song or subsong is longer


class ExportError(Exception):
    pass


class Stopped(ExportError):
    """The user stopped the export."""


# What a song can be exported as: its name as shown, what it is, and how ffmpeg is
# told to write it. FLAC and WAV are the song as it is; the others are smaller and
# lose a little, at settings where that is not heard.
KINDS = {
    "flac": ("FLAC", "as it is, about half the size of WAV", ["-c:a", "flac"]),
    "mp3": ("MP3", "plays everywhere, about 190 kbit/s", ["-c:a", "libmp3lame", "-q:a", "2"]),
    "ogg": ("Ogg Vorbis", "about 190 kbit/s", ["-c:a", "libvorbis", "-q:a", "6"]),
    "opus": ("Opus", "the smallest, about 128 kbit/s", ["-c:a", "libopus", "-b:a", "128k"]),
    "wav": ("WAV", "as it is, and large", []),
}
_encoders: set[str] | None = None


def encoder() -> str | None:
    """The program that writes FLAC files, if there is one."""
    return next((tool for tool in ("flac", "ffmpeg") if shutil.which(tool)), None)


def can_write(kind: str) -> bool:
    """Is there a program on this computer that writes files of this kind?"""
    global _encoders
    if kind == "wav":
        return True
    if kind not in KINDS:
        return False
    if kind == "flac" and shutil.which("flac"):
        return True
    if not shutil.which("ffmpeg"):
        return False
    if _encoders is None:
        try:
            listed = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"],
                                    capture_output=True, text=True, timeout=20).stdout
        except (OSError, subprocess.SubprocessError):
            listed = ""
        _encoders = {line.split()[1] for line in listed.splitlines() if len(line.split()) > 1}
    return KINDS[kind][2][1] in _encoders


def kinds() -> list[str]:
    """The kinds this computer can write."""
    return [k for k in KINDS if can_write(k)]


def safe_name(text: str) -> str:
    text = re.sub(r'[/\\:*?"<>|\x00-\x1f]', "_", text).strip(" .")
    return text[:120] or "song"


def file_stem(song: Song, subsong: int, subsongs: int, number: str = "") -> str:
    artist = song.artist if song.artist not in ("", "- unknown") else ""
    stem = f"{artist} - {song.title}" if artist else song.title
    if number:
        stem = f"{number} - {stem}"
    if subsongs > 1:
        stem += f" ({subsong + 1:02d} of {subsongs})"
    return safe_name(stem)


def _render(decoder: Decoder, out: wave.Wave_write,
            stop: threading.Event | None = None) -> float:
    """Write the decoder's current subsong to `out` by the one rule of how songs end
    (ending.py), as the player plays it; returns the seconds written."""
    limit = min(ending.limit(decoder.backend, decoder.info.duration,
                                    decoder.never_ends), MAX_SECONDS)
    patience = ending.patience(decoder.info.subsongs)
    silence = ending.Silence(RATE, FRAME_BYTES)
    frames = written = 0
    heard = False
    while frames / RATE < limit:
        if stop is not None and stop.is_set():
            raise Stopped("stopped")
        pcm = decoder.read(CHUNK)
        if not pcm:
            break
        elapsed = frames / RATE
        frames += len(pcm) // FRAME_BYTES
        silent = _peak(pcm) <= ending.SILENT_LEVEL
        if not heard:
            if silent:
                if elapsed >= patience:
                    break
                continue
            heard = True
        pieces = silence.take(pcm, silent)
        if pieces is None:
            break                   # silent for SILENT_END: the song is over
        for piece in pieces:
            gain = ending.fade(written / RATE, limit)
            if gain < 0.999:
                # each sample times the gain, cut toward zero as int() does
                piece = (np.frombuffer(piece, np.int16) * gain).astype(np.int16).tobytes()
            out.writeframes(piece)
            written += len(piece) // FRAME_BYTES
    return written / RATE


def _peak(pcm: bytes) -> float:
    """ending.peak, by numpy: in Python, it and the fade took 0.18 s of a 0.71 s export
    (11 October 2026)."""
    samples = np.frombuffer(pcm, np.int16)
    if not samples.size:
        return 0.0
    return max(int(samples.max()), -int(samples.min())) / 32768


def level(wav: Path) -> float | None:
    """Make a WAV file as loud as the player plays songs: measured on all of it, turned
    up or down at once, and kept from clipping. Gives the song's loudness (None for
    silence, which is left as it is)."""
    meter = loudness.Meter()
    with wave.open(str(wav), "rb") as f:
        while pcm := f.readframes(RATE):
            meter.feed(pcm)
    lufs = meter.loudness()
    if lufs is None or abs(loudness.gain_db(lufs)) < 0.05:
        return lufs
    leveller = loudness.Leveller(known=lufs)
    tmp = wav.with_name(wav.name + ".level")
    try:
        with wave.open(str(wav), "rb") as f, wave.open(str(tmp), "wb") as out:
            out.setparams(f.getparams())
            while pcm := f.readframes(RATE // 10):
                out.writeframes(leveller.process(pcm))
        os.replace(tmp, wav)
    finally:
        tmp.unlink(missing_ok=True)
    return lufs


def _encode(wav: Path, out: Path, kind: str, tags: dict[str, str]) -> None:
    tool = "flac" if kind == "flac" and shutil.which("flac") else "ffmpeg"
    if tool == "flac":
        cmd = ["flac", "--silent", "--force", "--best", "-o", str(out)]
        cmd += [f"--tag={k}={v}" for k, v in tags.items() if v]
        cmd.append(str(wav))
    else:
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav)]
        for k, v in tags.items():
            if v:
                cmd += ["-metadata", f"{k}={v}"]
        cmd += [*KINDS[kind][2], str(out)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0 or not out.exists():
        out.unlink(missing_ok=True)
        raise ExportError(f"{tool} failed: {done.stderr.strip()[:200]}")


def export_song(index: Index, fetcher: Fetcher, song: Song, settings: Settings,
                dest: Path = paths.EXPORT_DIR, kind: str | None = None,
                progress: Callable[[str], None] = lambda s: None,
                number: str = "", stop: threading.Event | None = None) -> list[Path]:
    """Export a song (every subsong, or one, as in the settings). Returns the files
    written. `number`: put in front of the file's name, for songs in a list. `stop`,
    when set, ends it at once (Stopped); a file half written is not left behind."""
    kind = kind or settings.export_format
    if kind not in KINDS:
        raise ExportError(f"{kind} is not a kind of file the player writes: "
                          + ", ".join(KINDS))
    if not can_write(kind):
        raise ExportError(f"{KINDS[kind][0]} needs the ffmpeg program with its encoder; "
                          "WAV works without")
    if getattr(fetcher, "checksum", True) is None:
        fetcher.checksum = index.md5
    try:
        if song.local:
            file, lengths = archives.file_of(song.path), []
        else:
            file = fetcher.get(song.key, index.companions(song.key))
            lengths = index.lengths(song.key)
        decoder = open_song(file, song.format, lengths, local=song.local)
    except (Unsupported, OSError, EOFError) as e:
        raise ExportError(f"can't play {song.title}: {e}") from None

    dest.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    try:
        total = decoder.info.subsongs
        first = decoder.info.subsong
        order = [first] if settings.subsongs != "all" else [(first + i) % total for i in range(total)]
        for n in order:
            if n != decoder.info.subsong:
                try:
                    decoder.select_subsong(n)
                except Unsupported:
                    continue
            stem = file_stem(song, n, total, number)
            progress(stem)
            wav = dest / (stem + ".wav")
            try:
                with wave.open(str(wav), "wb") as out:
                    out.setnchannels(2)
                    out.setsampwidth(2)
                    out.setframerate(RATE)
                    seconds = _render(decoder, out, stop)
                if seconds >= 0.5 and settings.level:
                    level(wav)
            except BaseException as e:
                wav.unlink(missing_ok=True)
                if isinstance(e, Exception) and not isinstance(e, ExportError):
                    raise ExportError(f"{song.title}: {e}") from e
                raise
            if seconds < 0.5:        # an empty subsong
                wav.unlink()
                continue
            if kind != "wav":
                flac = wav.with_suffix("." + kind)
                title = decoder.info.title or song.title
                try:
                    _encode(wav, flac, kind, {
                        "TITLE": title if total == 1 else f"{title} ({n + 1}/{total})",
                        "ARTIST": song.artist if song.artist != "- unknown" else decoder.info.author,
                        "ALBUM": song.album_name, "GENRE": song.format,
                        "TRACKNUMBER": str(n + 1) if total > 1 else "",
                        "COMMENT": song.path if song.local
                        else f"{collections_.name(song.collection)}: {song.path}"})
                finally:
                    wav.unlink(missing_ok=True)
                written.append(flac)
            else:
                written.append(wav)
    finally:
        decoder.close()
    if not written:
        raise ExportError(f"{song.title} produced no sound")
    return written


def export_songs(index: Index, fetcher: Fetcher, songs: list[Song], settings: Settings,
                 name: str, dest: Path = paths.EXPORT_DIR, kind: str | None = None,
                 progress: Callable[..., None] = lambda done, of, song, written, failed: None,
                 stop: threading.Event | None = None,
                 ) -> tuple[Path, list[Path], list[tuple[Song, str]]]:
    """Export several songs into a folder `name` below `dest`, numbered in their order.
    A song that can't be exported is passed over and told. `progress` is told before
    each song: how many are done, of how many, the song, the files written so far, and
    the songs passed over so far. Gives (the folder, the files
    written, (song, why) for those passed over)."""
    folder = dest / safe_name(name)
    written: list[Path] = []
    failed: list[tuple[Song, str]] = []
    width = max(2, len(str(len(songs))))
    for i, song in enumerate(songs):
        if stop is not None and stop.is_set():
            break
        progress(i, len(songs), song, len(written), failed)
        try:
            written += export_song(index, fetcher, song, settings, folder, kind,
                                   number=f"{i + 1:0{width}d}", stop=stop)
        except Stopped:
            break
        except ExportError as e:
            failed.append((song, str(e)))
    progress(len(songs), len(songs), None, len(written), failed)
    return folder, written, failed
