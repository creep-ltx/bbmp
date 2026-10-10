# SPDX-License-Identifier: GPL-3.0-or-later
"""Playing: pause, next, seek, volume, radio, repeat, shuffle, favorites, and files on
this computer. A part of the app."""

from __future__ import annotations

from pathlib import Path

from rich.text import Text
from textual import on
from textual.app import App
from textual.widgets import DirectoryTree, TabbedContent

from .. import archives, platforms, playlists, theme, wrong
from ..backends import is_supported
from ..index import Song

from .dialogs import Ask
from .widgets import NEW, REPEAT_LABELS, SongTable, Visualizer


class PlayingPart(metaclass=type(App)):
    """Playing: pause, next, seek, volume, radio, repeat, shuffle, favorites, files on this computer. A part of ModlandApp."""

    def action_enqueue(self, next_: bool = False) -> None:
        """Add the selected song, or the artist's or the playlist's songs, to the queue:
        at its end, or (`next_`) right after the song that plays."""
        add = self.player.play_next if next_ else self.player.enqueue
        where = "next" if next_ else ""
        if self.focused is self.query_one("#lists") and self.query_one("#fav-songs", SongTable).songs:
            songs = [s for s in self.query_one("#fav-songs", SongTable).songs if is_supported(s.format)]
            add(songs)
            self.notify(f"Queued {len(songs)} songs {where}".strip())
            return
        if (table := self._focused_table()) and (song := table.selected()):
            if table.id == "queue-songs":
                return
            add([song])
            self.notify(f"Plays next: {song.title}" if next_ else f"Queued: {song.title}")
        elif self._in_browse_list() and (found := self._browse_songs_under_cursor()) is not None:
            # an artist, or a day of what is new: all its songs
            songs = [s for s in found if is_supported(s.format)]
            add(songs)
            by = f"from {self._artist}" if self._fmt == NEW else f"by {self._artist}"
            self.notify(f"Queued {len(songs)} songs {by} {where}".strip())

    def action_pause(self) -> None:
        self.player.toggle_pause()

    def action_stop(self) -> None:
        self.player.stop()

    def action_next(self) -> None:
        self.player.next()

    def action_prev(self) -> None:
        self.player.prev()

    def action_subsong(self, delta: int) -> None:
        self.player.subsong(delta)

    def action_seek(self, delta: int) -> None:
        self.player.seek(delta)

    def action_volume(self, delta: int) -> None:
        self.player.set_volume(self.player.status().volume + delta)

    def action_radio(self) -> None:
        tabs = self.query_one("#tabs", TabbedContent)
        if tabs.active == "browse" and self._fmt and self._fmt != NEW:
            artist = self._artist
            self.player.radio(self._fmt, artist, self._here(), view=self._view())
            self.notify(f"Radio: {self._fmt}" + (f" / {artist}" if artist else ""))
        elif tabs.active == "browse" and (step := self._highlighted_step()) is not None \
                and step[0] in ("collection", "platform", "chip"):
            # the platform, chip or collection under the cursor: random Sharp X68000 songs
            ctx = self._place(self._path + [step])
            self.player.radio(collection=ctx["collection"], view=ctx["view"])
            where = dict(self._collections).get(ctx["collection"], "")
            what = platforms.name(*ctx["view"]) if ctx["view"] else (
                f"all of {where}" if where else "every collection")
            self.notify(f"Radio: {what}" + (f" on {where}" if where and ctx["view"] else ""))
        else:
            self.action_radio_all()

    def action_forever(self) -> None:
        """The song that plays goes on and on, until another song starts."""
        st = self.player.status()
        if st.song is None:
            self.notify("Nothing is playing.")
            return
        self.player.forever()
        if st.forever:
            self.notify(f"{st.song.title} ends as songs do.")
        else:
            self.notify(f"{st.song.title} goes on and on, until another song starts "
                        "(o again: it ends as songs do).")

    def action_radio_all(self) -> None:
        """Random songs from everything on show: a collection or all, and in a view, its
        platform or chip ("random Amiga songs")."""
        view = self._view()
        self.player.radio(collection=self._collection, view=view)
        where = self._collection_name()
        if view:
            self.notify(f"Radio: {platforms.name(*view)}" + (f" on {where}" if where else ""))
        else:
            self.notify(f"Radio: all of {where}" if where else "Radio: every collection")

    def _play_local(self, paths_: list[str]) -> None:
        songs: list[Song] = []
        for p in paths_:
            path = Path(p).expanduser()
            if archives.is_archive(path) and path.is_file():
                songs += self._archive_songs(path)
            elif path.is_dir():
                songs += self.index.local_songs(path, recursive=True)
            elif song := self.index.local_song(path):
                songs.append(song)
        if songs:
            self.player.play_list(songs)
        else:
            self.notify("No music files found there.", severity="warning")

    def _archive_songs(self, archive: Path) -> list[Song]:
        try:
            songs = self.index.archive_songs(archive)
        except OSError as e:
            self.notify(str(e), severity="warning", timeout=10)
            return []
        if not songs:
            self.notify(f"{archive.name} has no music a replayer here knows.", severity="warning")
        return songs

    @on(DirectoryTree.FileSelected, "#files-tree")
    def _file_selected(self, event: DirectoryTree.FileSelected) -> None:
        if playlists.is_playlist_file(event.path):
            self._import_file(str(event.path))
            return
        if archives.is_archive(event.path):
            songs = self._archive_songs(event.path)
            if songs:
                self.player.play_list(songs)
                self.notify(f"{len(songs)} songs in {event.path.name}")
            return
        songs = self.index.local_songs(event.path.parent)
        start = next((i for i, s in enumerate(songs) if Path(s.path) == event.path.resolve()), None)
        if start is None:
            self.notify("No replayer knows this kind of file.", severity="warning")
            return
        self.player.play_list(songs, start)

    def action_favorite(self) -> None:
        song = self._target_song()
        if song is None:
            return
        on_ = self.library.toggle_favorite(song.key)
        self._favs = self.library.favorite_keys()
        self._lists_changed()
        self.notify(("♥ Added " if on_ else "Removed ") + song.title)
        # the mark goes on the row only when the row is that song: with an empty list
        # (a search that found nothing) the song is the one playing, and there is no
        # row to mark (that closed the player, 7 October 2026)
        if (table := self._focused_table()) and table.selected() is song:
            table.update_cell_at((table.cursor_row, 0),
                                 Text("♥" if on_ else "", style=f"bold {theme.current.bad}"))

    def action_sounds_wrong(self) -> None:
        """Note the song that plays as sounding wrong, with where in it, and a word or two."""
        st = self.player.status()
        if st.song is None or st.state not in ("playing", "paused"):
            self.notify("Nothing is playing: ! notes the song that plays as sounding wrong.",
                        severity="warning")
            return
        song, at = st.song, st.elapsed
        subsong, subsongs = (st.info.subsong, st.info.subsongs) if st.info else (0, 1)

        def done(words: str | None) -> None:
            if words is None:
                return
            try:
                wrong.note(song, words, at=at, subsong=subsong, subsongs=subsongs,
                           replayer=st.backend, file=self.wrong_file)
            except OSError as e:
                self.notify(f"Could not write the list: {e}", severity="error")
                return
            n = wrong.count(self.wrong_file)
            self.notify(f"Noted: {song.title} sounds wrong at {wrong.clock(at)} "
                        f"({n} on the list; bbmp wrong shows it)")

        self.push_screen(Ask(f"What sounds wrong in {song.title} at {wrong.clock(at)}?\n"
                             "A word or two, or nothing; Enter notes it.", empty=True), done)

    def action_visual(self) -> None:
        vis = self.query_one("#visual", Visualizer)
        mode = vis.cycle()
        self.player.want_pattern(vis.wants_pattern)
        self.settings.update(visualizer=mode)

    def action_cycle_repeat(self) -> None:
        order = ("off", "all", "one")
        mode = order[(order.index(self.settings.repeat) + 1) % len(order)]
        self.settings.update(repeat=mode)
        self.notify(f"Repeat: {REPEAT_LABELS[mode]}")
        self._fill_settings()

    def action_toggle_shuffle(self) -> None:
        self.settings.update(shuffle=not self.settings.shuffle)
        self.notify("Shuffle on" if self.settings.shuffle else "Shuffle off")
        self._fill_settings()
