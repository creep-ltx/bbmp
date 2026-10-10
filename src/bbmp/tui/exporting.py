# SPDX-License-Identifier: GPL-3.0-or-later
"""Exporting songs and lists as FLAC, MP3, Ogg, Opus or WAV. A part of the app."""

from __future__ import annotations

from textual import work
from textual.app import App
from textual.css.query import NoMatches
from textual.widgets import TabbedContent

from .. import formats_info, pages, units
from ..backends import is_supported
from ..index import Song

from .activity import ExportJob
from .dialogs import Pick
from .widgets import NEW


class ExportPart(metaclass=type(App)):
    """Exporting songs and lists as FLAC, MP3, Ogg, Opus or WAV. A part of ModlandApp."""

    def _export_kind(self) -> str:
        from ..export import KINDS
        name, what, _ = KINDS.get(self.settings.export_format, KINDS["flac"])
        return f"{name}  ({what})"

    def _pick_export_kind(self) -> None:
        from ..export import KINDS, kinds
        can = kinds()
        rows = [(k, f"{name}  ·  {what}" + ("" if k in can else "  ·  needs ffmpeg"))
                for k, (name, what, _) in KINDS.items()]

        def chosen(kind: str | None) -> None:
            if kind in can:
                self.settings.update(export_format=kind)
                self._fill_settings()
            elif kind:
                self.notify(f"{KINDS[kind][0]} needs the ffmpeg program, which is not on "
                            "this computer.", severity="warning", timeout=10)
        pick = Pick(f"e writes the song as a file, to {units.folder(self.export_dir)}", rows)
        pick.start = self.settings.export_format
        self.push_screen(pick, chosen)

    def _export_choices(self) -> list[tuple[str, str, list[Song]]]:
        """What can be exported from where the user is: (name for the folder, label,
        songs). The song first, then the lists it is in."""
        out: list[tuple[str, str, list[Song]]] = []
        tabs = self.query_one("#tabs", TabbedContent).active
        song = self._target_song()
        if song is not None:
            out.append(("", f"The song {song.title}", [song]))
        table = self._focused_table()
        if tabs == "favorites" and table is not None and table.songs:
            title = (table.border_title or "Playlist").rsplit(" (", 1)[0]
            out.append((title, f"The playlist {title}", list(table.songs)))
        artist = fmt = collection = None
        if tabs == "browse" and self._fmt and self._fmt != NEW and self._artist:
            artist, fmt = self._artist, self._fmt
        elif song is not None and not song.local and song.artist not in pages.NO_ARTIST:
            artist, fmt, collection = song.artist, song.format, song.collection
        if artist:
            songs = self.index.songs(fmt, artist, collection or self._here())
            if len(songs) > 1:
                out.append((f"{artist} - {fmt}", f"{artist}'s songs in {fmt}", songs))
            everything = self.index.artist_songs(artist)
            if len(everything) > len(songs):
                out.append((artist, f"Everything by {artist}", everything))
        queue, _ = self.player.queue()
        if len(queue) > 1:
            out.append(("Queue", "The queue", queue))
        return out

    def action_export(self) -> None:
        from ..export import KINDS
        choices = self._export_choices()
        if not choices:
            return
        rows = []
        for i, (_, label, songs) in enumerate(choices):
            playable = sum(1 for s in songs if is_supported(s.format)
                           and not formats_info.needed(s.format))
            count = "" if len(songs) == 1 else f"  ·  {playable:,} songs" + (
                f" ({len(songs) - playable:,} can't be played)" if playable < len(songs) else "")
            rows.append((str(i), label + count))
        if self._exporting:
            rows.append(("stop", "Stop exporting"))
        kind = KINDS.get(self.settings.export_format, KINDS["flac"])[0]
        level = ", at the same loudness" if self.settings.level else ""

        def chosen(id_: str | None) -> None:
            if id_ == "stop":
                self._export_stop.set()
            elif id_ is not None:
                name, _, songs = choices[int(id_)]
                self._start_export(name, songs)
        self.push_screen(Pick(f"Export as {kind}{level}, to {units.folder(self.export_dir)}\n"
                              "Settings has the kind of file", rows), chosen)

    def _start_export(self, name: str, songs: list[Song]) -> None:
        playable = [s for s in songs if is_supported(s.format) and not formats_info.needed(s.format)]
        if not playable:
            self.notify(formats_info.why_not(songs[0].format) if len(songs) == 1 else
                        "None of them can be played.", severity="warning", timeout=15)
            return
        songs = playable
        if self._exporting:
            self.notify("Songs are being exported. e has \"Stop exporting\".", timeout=10)
            return
        self._exporting = True
        self._export_stop.clear()
        self._export_job = ExportJob(name, len(songs))
        if name:
            self.notify(f"Exporting {len(songs):,} songs to {name}. You can go on listening.")
        else:
            self.notify(f"Exporting {songs[0].title}...")
        self._export(name, songs)

    @work(thread=True, group="export")
    def _export(self, name: str, songs: list[Song]) -> None:
        from ..export import ExportError, Stopped, export_song, export_songs
        try:
            job = self._export_job
            if not name:
                job.song = songs[0].title
                try:
                    files = export_song(self.index, self.fetcher, songs[0], self.settings,
                                        self.export_dir, stop=self._export_stop)
                except Stopped:
                    job.stopped = True
                    self.call_from_thread(self.notify, "The export was stopped.")
                    return
                except ExportError as e:
                    job.failed.append((songs[0], str(e)))
                    self.call_from_thread(self.notify, f"Export failed: {e}", severity="error")
                    return
                job.done, job.written = 1, len(files)
                job.folder = units.folder(files[0].parent)
                what = (files[0].name if len(files) == 1
                        else f"{len(files)} files ({songs[0].title})")
                self.call_from_thread(self.notify, f"Exported {what} to "
                                                   f"{units.folder(files[0].parent)}")
                return

            def said(done: int, of: int, song: Song | None, written: int, failed) -> None:
                job.done, job.song, job.written, job.failed = done, song.title if song else "", \
                    written, list(failed)
                text = f"Exporting {done + 1} of {of}  (t)" if song else ""
                self.call_from_thread(self._export_said, text)
            folder, files, failed = export_songs(
                self.index, self.fetcher, songs, self.settings, name, self.export_dir,
                progress=said, stop=self._export_stop)
            job.folder = units.folder(folder)
            job.stopped = self._export_stop.is_set()
            stopped = " Stopped." if job.stopped else ""
            text = f"Exported {len(files):,} files to {units.folder(folder)}.{stopped}"
            if failed:
                text += (f" {len(failed):,} songs could not be: "
                         + "; ".join(f"{s.title} ({why})" for s, why in failed[:3]))
            self.call_from_thread(self.notify, text, timeout=20,
                                  severity="warning" if failed else "information")
        finally:
            self._exporting = False
            if self._export_job is not None:
                self._export_job.running = False
            self.call_from_thread(self._export_said, "")

    def _export_said(self, text: str) -> None:
        try:
            now = self.query_one("#now")
        except NoMatches:
            return
        keeping = now.border_subtitle if (now.border_subtitle or "").startswith("Saving") else ""
        now.border_subtitle = text or keeping
