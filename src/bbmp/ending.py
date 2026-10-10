# SPDX-License-Identifier: GPL-3.0-or-later
"""When a song is over: one rule, for playing and for exporting alike.

1. A song that ends by itself ends there. Most replayers know: the song's own end, or
   the point where it would start again (it is played once through; where the
   replayer can, it fades into the repeat).
2. Only the replayers whose songs never end by themselves (NEVER_ENDS: SID tunes, SNDH,
   ZXTune's formats, VGM and S98) are stopped by the player: at the song's known
   length (HVSC's list, the file's own), else after DEFAULT_LENGTH, with a FADE.
3. Silence is never played at a song's end: once a song has made a sound, silence is
   held back; it is played when the song sounds again, and let go when the song ends,
   which a silence of SILENT_END seconds also means.
4. Silence at a song's start isn't played either, and a (sub)song that stays silent
   for SILENT_SUBSONG (a file with one song: SILENT_SONG) seconds is passed over.
5. Nothing plays longer than LONGEST; a song whose replayer said its length is stopped
   well after that length (it went on where it should have ended), with a FADE.
"""

from __future__ import annotations

import array

NEVER_ENDS = frozenset({"sidplayfp", "sndh", "zxtune", "vgm"})
DEFAULT_LENGTH = 180.0      # seconds a song that never ends plays, when its length isn't known
FADE = 4.0                  # seconds of fade where the player stops a song
LONGEST = 3600.0            # no song or subsong plays longer
SILENT_SUBSONG = 3.0        # a subsong still silent after this many seconds is skipped
SILENT_SONG = 60.0          # and a file with a single song after this many (Coconizer's
                            # DREAM starts 45 s in, Oktalyzer's second halves 20-25 s)
SILENT_END = 12.0           # silence this long after a song has sounded: it is over
SILENT_LEVEL = 0.01         # peak level that counts as silence


def peak(samples: array.array) -> float:
    return max(max(samples, default=0), -min(samples, default=0)) / 32768 if samples else 0.0


def limit(backend: str, duration: float | None, never_ends: bool = False) -> float:
    """Where the player stops a song (with a FADE before it), from its replayer and the
    length the replayer said. `never_ends`: the decoder knows this song doesn't end
    though its replayer's songs do (Decoder.never_ends)."""
    if backend in NEVER_ENDS or never_ends:
        return duration or DEFAULT_LENGTH
    if duration:
        return min(LONGEST, duration * 1.5 + 30)    # only where it doesn't end as it said
    return LONGEST


def patience(subsongs: int) -> float:
    """Seconds a (sub)song may stay silent before its first sound."""
    return SILENT_SUBSONG if subsongs > 1 else SILENT_SONG


def fade(elapsed: float, stop: float) -> float:
    """The gain at `elapsed` seconds of a song stopped at `stop`: 1, then down to 0 over
    the last FADE seconds."""
    if elapsed <= stop - FADE:
        return 1.0
    return max(0.0, (stop - elapsed) / FADE)


class Silence:
    """Silence after a song has made its sound, held back: given out again when the
    song sounds again, let go when it ends. take() gives what is to be played now."""

    def __init__(self, rate: int, frame_bytes: int):
        self._held: list[bytes] = []
        self._bytes = 0
        self._most = int(SILENT_END * rate) * frame_bytes

    def take(self, pcm: bytes, silent: bool) -> list[bytes] | None:
        """The pieces to play now: nothing while it is silent; what was held and `pcm`
        when it sounds again; None when the silence has lasted SILENT_END (the song is
        over, and what was held is let go)."""
        if silent:
            self._held.append(pcm)
            self._bytes += len(pcm)
            if self._bytes >= self._most:
                self.clear()
                return None
            return []
        out = [*self._held, pcm]
        self.clear()
        return out

    def clear(self) -> None:
        self._held, self._bytes = [], 0
