# SPDX-License-Identifier: GPL-3.0-or-later
"""The Playlists tab: favorites, playlists, importing them. A part of the app."""

from __future__ import annotations

from pathlib import Path

from rich.text import Text
from textual import on, work
from textual.app import App
from textual.widgets import OptionList, TabbedContent
from textual.widgets.option_list import Option

from .. import collection as collections_, playlists, theme

from .dialogs import Ask, Confirm, Pick
from .guard import guarded
from .widgets import SongTable


class ListsPart(metaclass=type(App)):
    """The Playlists tab: favorites, playlists, importing them. A part of ModlandApp."""

    def _fill_lists(self) -> None:
        ol = self.query_one("#lists", OptionList)
        ol.border_title = "Playlists"
        ol.clear_options()
        ol.add_option(Option(Text.assemble(("♥ ", f"bold {theme.current.bad}"), "Favorites",
                                           (f"\u00a0\u00a0{len(self._favs)}", "dim")), id="fav"))
        current = 0
        for i, (pid, name, count) in enumerate(self.library.playlists(), 1):
            ol.add_option(Option(Text.assemble(name, (f"\u00a0\u00a0{count}", "dim")), id=f"pl:{pid}"))
            if pid == self._list:
                current = i
        ol.add_option(None)
        ol.add_option(Option(Text("+ New playlist", style=theme.current.primary), id="new"))
        for c in collections_.known():
            if c.has_lists and any(c.id == have for have, _ in self._collections):
                ol.add_option(Option(Text(f"+ Import one of {c.name}'s lists",
                                          style=theme.current.primary), id=f"import:{c.id}"))
        ol.add_option(Option(Text("+ Import a playlist file (.m3u)", style=theme.current.primary), id="file"))
        ol.highlighted = current

    def _show_list(self) -> None:
        table = self.query_one("#fav-songs", SongTable)
        if self._list is None:
            songs, title = self.library.favorites(self.index), "Favorites"
        else:
            songs = self.library.playlist_songs(self._list, self.index)
            title = next((n for pid, n, _ in self.library.playlists() if pid == self._list), "Playlist")
        table.set_songs(songs, self._favs)
        table.border_title = f"{title} ({len(songs):,})"
        self._list_shown = self._list

    @on(OptionList.OptionHighlighted, "#lists")
    def _list_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        key = event.option_id or ""
        if key == "fav" or key.startswith("pl:"):
            self._list = None if key == "fav" else int(key[3:])
            # _fill_lists highlights the list on show, and its caller has shown it
            # already; a list moved to is shown once the cursor rests
            if self._list != getattr(self, "_list_shown", ...):
                self._later("lists", 0.12, self._show_list)

    @on(OptionList.OptionSelected, "#lists")
    def _list_selected(self, event: OptionList.OptionSelected) -> None:
        key = event.option_id
        if key == "new":
            self.push_screen(Ask("Name of the new playlist"), self._new_playlist)
        elif key.startswith("import:"):
            self.notify(f"Asking {collections_.name(key[7:])} for its lists...")
            self._load_lists(key[7:])
        elif key == "file":
            self.push_screen(Ask("The playlist file to import (.m3u or .m3u8)",
                                 str(self._music_dir()) + "/", replace=False), self._import_file)
        else:
            self.query_one("#fav-songs").focus()

    def _new_playlist(self, name: str | None, keys: list[str] = ()) -> None:
        if not name:
            return
        self._list = self.library.create_playlist(name, list(keys))
        self._fill_lists()
        self._show_list()
        self.notify(f"Created the playlist {name}" + (f" with {len(keys)} songs" if keys else ""))

    def _import_file(self, name: str | None) -> None:
        if not name:
            return
        path = Path(name).expanduser()
        try:
            pid, title, found, missing = playlists.import_file(path, self.index, self.library)
        except OSError as e:
            self.notify(f"Can't read {path}: {e.strerror or e}", severity="error")
            return
        except ValueError as e:
            self.notify(f"{path.name}: {e}", severity="warning")
            return
        self._list = pid
        self.query_one("#tabs", TabbedContent).active = "favorites"
        self._fill_lists()
        self._show_list()
        note = f"Imported {title}: {found} songs"
        if missing:
            note += f" ({missing} could not be found)"
        self.notify(note)

    def action_rename_playlist(self) -> None:
        if self.query_one("#tabs", TabbedContent).active != "favorites" or self._list is None:
            return
        pid, old = self._list, self.library.playlist_name(self._list)

        def renamed(name: str | None) -> None:
            if not name or name == old:
                return
            if self.library.rename_playlist(pid, name):
                self._fill_lists()
                self._show_list()
            else:
                self.notify(f"There already is a playlist called {name}.", severity="warning")
        self.push_screen(Ask(f"New name for {old}", old), renamed)

    def action_move_song(self, delta: int) -> None:
        table = self.query_one("#fav-songs", SongTable)
        if self.focused is not table or self._list is None or not (song := table.base_song()):
            return
        new = self.library.move_in_playlist(self._list, song.key, delta)
        if new is not None:
            self._show_list()
            table.move_cursor(row=new)

    def action_add_to_playlist(self) -> None:
        song = self._target_song()
        if song is None:
            return
        options = [(f"pl:{pid}", f"{name}  ({count})") for pid, name, count in self.library.playlists()]
        options.append(("new", "+ New playlist"))

        def chosen(key: str | None) -> None:
            if key == "new":
                self.push_screen(Ask("Name of the new playlist"),
                                 lambda name: self._new_playlist(name, [song.key]))
            elif key:
                added = self.library.add_to_playlist(int(key[3:]), [song.key])
                self.notify(f"Added {song.title}" if added else f"{song.title} is already in it")
                self._refresh_lists()
        self.push_screen(Pick(f"Add {song.title} to", options), chosen)

    def action_save_queue(self) -> None:
        songs, _ = self.player.queue()
        if not songs:
            self.notify("The queue is empty.")
            return
        self.push_screen(Ask(f"Save the {len(songs)} songs in the queue as"),
                         lambda name: self._new_playlist(name, [s.key for s in songs]))

    def _refresh_lists(self) -> None:
        self._lists_changed()
        if self.query_one("#tabs", TabbedContent).active == "favorites":
            self._fill_lists()
            self._show_list()

    def action_remove(self) -> None:
        lists = self.query_one("#lists", OptionList)
        table = self.query_one("#fav-songs", SongTable)
        if self.focused is table and (song := table.base_song()):
            # a copy's row stands for its song: the list has the song, not the copy
            if self._list is None:
                self.library.toggle_favorite(song.key)
                self._favs = self.library.favorite_keys()
            else:
                self.library.remove_from_playlist(self._list, song.key)
            row = table.cursor_row
            self._fill_lists()
            self._show_list()
            if table.row_count:
                table.move_cursor(row=min(row, table.row_count - 1))
        elif self.focused is lists and self._list is not None:
            pid = self._list
            name = next((n for p_, n, _ in self.library.playlists() if p_ == pid), "")

            def answered(yes: bool | None) -> None:
                if yes:
                    self.library.delete_playlist(pid)
                    self._list = None
                    self._fill_lists()
                    self._show_list()
                    self.notify(f"Deleted the playlist {name}")
            self.push_screen(Confirm(f"Delete the playlist {name}?\n"
                                     "The songs themselves stay where they are."), answered)

    @work(thread=True, exclusive=True, group="lists")
    @guarded("reading the collection's lists")
    def _load_lists(self, collection: str) -> None:
        source = collections_.get(collection)
        try:
            with self.fetcher.transport(collection) as transport:
                found = source.published_lists(transport)
        except (OSError, EOFError) as e:
            self.call_from_thread(self.notify, f"Can't reach {source.name}: {e}", severity="error")
            return
        self.call_from_thread(self.push_screen, Pick(f"{source.name}'s lists", found),
                              lambda ref: self._import(collection, ref) if ref else None)

    @work(thread=True, group="lists")
    @guarded("importing the list")
    def _import(self, collection: str, ref: str) -> None:
        source = collections_.get(collection)
        try:
            with self.fetcher.transport(collection) as transport:
                text = source.read_list(transport, ref)
        except (OSError, EOFError) as e:
            self.call_from_thread(self.notify, f"Import failed: {e}", severity="error")
            return
        found, missing = playlists.resolve(playlists.parse_m3u(text), self.index)
        if not found:
            self.call_from_thread(self.notify,
                                  f"None of that list's songs are on {source.name} any more.",
                                  severity="warning")
            return
        name = playlists.list_name(ref.rsplit("/", 1)[-1])
        pid = self.library.create_playlist(name, found)
        note = f"Imported {name}: {len(found)} songs"
        if missing:
            note += f" ({missing} are no longer on {source.name} under a name that can be matched)"

        def done() -> None:
            self._list = pid
            self._refresh_lists()
            self.notify(note)
        self.call_from_thread(done)
