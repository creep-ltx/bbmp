# SPDX-License-Identifier: GPL-3.0-or-later
"""The terminal user interface (Textual): the app, made of its parts."""

from __future__ import annotations

import contextlib
import json
import os
import sys
import threading
from pathlib import Path

from rich.text import Text
from textual import on, work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.css.query import NoMatches
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.timer import Timer
from textual.widgets import (DataTable, Footer, Input, OptionList, Static,
                             TabbedContent, TabPane)
from textual.widgets.option_list import Option

from .. import (__version__, amiga_themes, archives, catalogs, collection as collections_, command, formats_info,
               pages, paths, playlists, sandbox, stats_view, theme, units, wrong)
from ..keep import Keeper, Scope
from ..settings import Settings
from ..backends import is_supported
from ..fetch import Fetcher
from ..index import Index, Song
from ..library import Library
from ..player import Player
from ..trouble import Trouble

from .activity import Activity
from .dialogs import Busy, ChooseCollections, Confirm, Pick
from .guard import guarded
from .help import HELP
from .widgets import LIST_KEYS, FilesTree, KeysFooter, Note, NowPlaying, OneLineList, SearchBox, SongTable, Visualizer
from .playing import PlayingPart
from .browse import BrowsePart
from .lists import ListsPart
from .artist import ArtistPart
from .saving import SavingPart
from .exporting import ExportPart
from .settings_tab import SettingsPart


class ModlandApp(PlayingPart, BrowsePart, ListsPart, ArtistPart, SavingPart, ExportPart, SettingsPart, App):
    TITLE = "ByteBandit Music Player"
    SUB_TITLE = f"v{__version__}"
    CSS = """
    /* Quiet frames: a list is a line with its name on top, in the theme's faint border
       colour; the one with the keys has its line and name in the theme's border colour. */
    OptionList, DataTable, DirectoryTree, VerticalScroll { scrollbar-size-vertical: 1; }
    .pane { border: none; border-top: solid $border-blurred; border-title-color: $foreground 60%;
            height: 1fr; }
    VerticalScroll.pane, #visual { padding: 0 1; }
    .pane:focus, .pane:focus-within { border-top: heavy $border; border-title-color: $border;
                                      border-title-style: bold; }
    #tabs { height: 1fr; }
    #tabs > ContentSwitcher { height: 1fr; }
    TabPane { height: 1fr; padding: 0; }
    #miller { height: 1fr; }
    /* ranger's columns: the list one came from, the one one is in, a look ahead */
    #miller > * { display: none; }
    #miller > .parent { display: block; width: 2fr; margin-right: 1; }
    #miller > .current { display: block; width: 3fr; margin-right: 1; }
    #miller > .preview { display: block; width: 4fr; }
    #miller > SongTable.current { width: 6fr; }
    #miller > #song-info.preview { width: 3fr; padding: 0 1; }
    #miller > .preview-list.preview { opacity: 85%; }
    /* the format note: a fixed height, one that followed the note resized the
       columns above it (widgets.Note) */
    #format-note { height: 5; padding: 0 2; color: $text-muted; background: $surface; }
    /* A narrow terminal: the list one came from is left out */
    #miller.narrow > .parent { display: none; }
    #lists.narrow { width: 24; }
    #artist-side.narrow { width: 32; }
    /* A short one: a smaller Now playing strip and note. */
    Screen.short #now { height: 4; }
    Screen.short #format-note { height: 2; }
    Screen.short #visual { height: 7; }
    #visual { height: 13; display: none; }
    #visual.on { display: block; }
    #now { height: 5; border: none; border-top: solid $border-blurred; padding: 0 2;
           background: $surface; border-title-align: right; border-title-color: $foreground 60%; }
    #help { padding: 1 2; }
    #stats-row { height: 1fr; }
    #periods { width: 18; margin-right: 1; }
    #settings-list { height: auto; max-height: 1fr; }
    #settings-note { padding: 1 2; color: $text-muted; }
    #artist-row { height: 1fr; }
    #artist-side { width: 48; margin-right: 1; }
    #partners { height: auto; max-height: 12; }
    #lists-row { height: 1fr; }
    #lists { width: 38; margin-right: 1; }
    Tabs { background: $surface; }
    """
    # Every binding has an id, by which "keys" in settings.json gives it other keys:
    # {"keys": {"save": "d", "move_up": "ctrl+up"}}. Shift gives the bigger or related
    # thing: r radio, R repeat; l add to a playlist, L the queue as one.
    BINDINGS = [
        Binding("space", "pause", "Pause", id="pause"),
        Binding("shift+space,x", "stop", "Stop", show=False, id="stop"),
        Binding("n", "next", "Next", id="next"),
        Binding("p", "prev", "Prev", show=False, id="prev"),
        Binding("right_square_bracket", "subsong(1)", "Subsong+", show=False, id="subsong_next"),
        Binding("left_square_bracket", "subsong(-1)", "Subsong-", show=False, id="subsong_prev"),
        Binding("full_stop", "seek(10)", "Seek+", show=False, id="seek_forward"),
        Binding("comma", "seek(-10)", "Seek-", show=False, id="seek_back"),
        Binding("plus,equals_sign", "volume(5)", "Vol+", show=False, id="volume_up"),
        Binding("minus", "volume(-5)", "Vol-", show=False, id="volume_down"),
        Binding("r", "radio", "Radio", id="radio"),
        Binding("slash", "search", "Search", id="search"),
        Binding("b", "view", "By", show=False, id="view"),
        Binding("s", "keep", "Save", id="save"),
        Binding("f", "favorite", "Fav", id="favorite"),
        Binding("exclamation_mark", "sounds_wrong", show=False, id="sounds_wrong"),
        # Help and Quit before the rest of the footer, so that a narrow terminal shows them
        Binding("question_mark", "help", "Help", id="help"),
        Binding("q", "quit", "Quit", id="quit"),
        Binding("ctrl+r", "radio_all", "Radio all", show=False, id="radio_all"),
        Binding("R", "cycle_repeat", "Repeat", show=False, id="repeat"),
        Binding("S", "toggle_shuffle", "Shuffle", show=False, id="shuffle"),
        Binding("o", "forever", "On and on", show=False, id="forever"),
        Binding("c", "collection", "Collection", id="collection"),
        Binding("m", "artist", "Artist", id="artist"),
        Binding("g", "go_to_playing", "Go to playing", show=False, id="go_to_playing"),
        Binding("A", "album", "Album", show=False, id="album"),
        Binding("i", "tab('info')", "Info", id="info"),
        Binding("w", "lookup('web')", show=False, id="web"),
        Binding("d", "lookup('demozoo')", show=False, id="demozoo"),
        Binding("u", "update_catalog", "Update", show=False, id="update"),
        Binding("a", "enqueue", "Queue", show=False, id="enqueue"),
        Binding("N", "enqueue(True)", "Play next", show=False, id="play_next"),
        Binding("l", "add_to_playlist", "Playlist", show=False, id="add_to_playlist"),
        Binding("L", "save_queue", show=False, id="queue_to_playlist"),
        Binding("e", "export", "Export", show=False, id="export"),
        Binding("delete,backspace", "remove", show=False, id="remove"),
        Binding("f2", "rename_playlist", show=False, id="rename"),
        Binding("shift+up,K", "move_song(-1)", show=False, id="move_up"),
        Binding("shift+down,J", "move_song(1)", show=False, id="move_down"),
        Binding("v", "visual", "Visuals", id="visuals"),
        Binding("right", "copies(True)", show=False, id="open_copies"),
        Binding("left,escape", "copies(False)", show=False, id="close_copies"),
        Binding("t", "activity", "Activity", show=False, id="activity"),
        *(Binding(str((n + 1) % 10), f"tab('{tab}')", show=False, id=f"tab_{tab}")
          for n, tab in enumerate(("browse", "search", "queue", "favorites", "files",
                                   "history", "info", "artist", "stats", "settings"))),
    ]

    def __init__(self, index: Index | None = None, fetcher: Fetcher | None = None,
                 library: Library | None = None, output=None, settings: Settings | None = None,
                 session_path=paths.SESSION, play_paths: list[str] = (), mpris: bool = False,
                 ask: bool = False):
        super().__init__()
        self.index = index
        self.fetcher = fetcher or Fetcher()
        self.library = library or Library()
        self.settings = settings or Settings.load()
        self._session_path = session_path
        self._output = output
        self._play_id: int | None = None
        self._stats_period = "week"
        self._play_paths = list(play_paths)
        self.export_dir = paths.EXPORT_DIR
        self.wrong_file = wrong.FILE          # the "sounds wrong" list (the ! key)
        self._use_mpris = mpris
        self._ask = ask          # offer the downloads at the first start
        self._mpris = None
        self._page: pages.Page | None = None
        self._list: int | None = None        # the playlist on show; None is the favorites
        self._info_shown: tuple | None = None
        self.player: Player | None = None
        self._favs: set[str] = self.library.favorite_keys()
        self._collections: list[tuple[str, str]] = []    # (id, name) of the ones in the catalog
        self._own: list[tuple[str, str]] = []            # those that no other collection hosts
        self._collection = ""                            # the one on show; "" is all of them
        self._debounce: dict[str, Timer] = {}
        self._last_song: str | None = None
        self._last_queue: tuple[int, int] = (-1, -1)
        self._fmt: str | None = None
        self._fmt_id: str | None = None                  # the format's option: in a view, with its collection
        self._fmt_in = ""                                # in a view, the collection of the format on show
        self._artist: str | None = None
        self._sizes: dict[str, int] = {}             # of the formats on show
        self.keeper: Keeper | None = None
        self._kept: list[Scope] = []                 # what is kept on this computer
        self._keep_stop = threading.Event()
        self._keeping = False                        # kept music is being fetched
        self._keep_more = False                      # and more was asked for meanwhile
        self._closing = False
        self._keep_progress = None                   # the last word from saving (keep.Progress)
        self._export_job = None                      # what is or was exported (ExportJob)
        self._exporting = False                      # songs are being exported
        self._export_stop = threading.Event()

    def notify(self, message: str, *, markup: bool = False, **kwargs) -> None:
        """Notes are plain text: they name songs and playlists, and a name like
        "mix [/] 2" is no markup."""
        super().notify(message, markup=markup, **kwargs)

    def compose(self) -> ComposeResult:
        with TabbedContent(initial="browse", id="tabs"):
            with TabPane("1 Browse", id="browse"):
                with Vertical():
                    # the columns: a list for each step (browse.py mounts them), the
                    # look at what the entry under the cursor holds, the songs, a song
                    with Horizontal(id="miller"):
                        yield OneLineList(markup=False, classes="pane preview-list")
                        yield SongTable(columns=("title", "size", "album"), id="browse-songs", classes="pane")
                        yield Static(id="song-info", classes="pane", markup=False)
                    yield Note(id="format-note", markup=False)
            with TabPane("2 Search", id="search"):
                with Vertical():
                    yield SearchBox(placeholder="Search: words, artist:jester, format:ahx, "
                                            "title:, album:, platform:amiga, chip:sid, \"two words\", -leave-out", id="q")
                    yield SongTable(id="search-songs", classes="pane")
            with TabPane("3 Queue", id="queue"):
                yield SongTable(id="queue-songs", classes="pane")
            with TabPane("4 Playlists", id="favorites"):
                with Horizontal(id="lists-row"):
                    yield OptionList(id="lists", markup=False, classes="pane")
                    yield SongTable(id="fav-songs", classes="pane")
            with TabPane("5 Files", id="files"):
                yield FilesTree(self._music_dir(), id="files-tree", classes="pane")
            with TabPane("6 History", id="history"):
                yield SongTable(id="hist-songs", classes="pane")
            with TabPane("7 Info", id="info"):
                with VerticalScroll(id="info-scroll", classes="pane"):
                    yield Static(id="info-body", markup=False)
            with TabPane("8 Artist", id="artist"):
                with Horizontal(id="artist-row"):
                    with Vertical(id="artist-side"):
                        with VerticalScroll(id="artist-scroll", classes="pane"):
                            yield Static(id="artist-facts", markup=False)
                        yield OptionList(id="partners", markup=False, classes="pane")
                    yield SongTable(columns=("title", "album", "format"), id="artist-songs", classes="pane")
            with TabPane("9 Stats", id="stats"):
                with Horizontal(id="stats-row"):
                    yield OptionList(*(Option(label, id=key) for key, (label, _) in
                                       stats_view.PERIODS.items()), id="periods", classes="pane")
                    with VerticalScroll(id="stats-scroll", classes="pane"):
                        yield Static(id="stats-body", markup=False)
            with TabPane("0 Settings", id="settings"):
                with Vertical(id="settings-col"):
                    yield OptionList(id="settings-list", markup=False, classes="pane")
                    yield Static(id="settings-note", markup=False)
            with TabPane("? Help", id="helptab"):
                # in a box that takes the keys: a tab with nothing to focus was left at
                # once for the one that had the focus (3 October 2026)
                with VerticalScroll(id="help-scroll"):
                    yield Static(HELP, id="help")
        yield Visualizer(id="visual", classes="pane")
        yield NowPlaying(id="now")
        yield KeysFooter(show_command_palette=False)

    # ---- the terminal's size ----

    NARROW = 120        # columns: below this, Browse shows two columns at a time
    TABS_NARROW = 100   # below this, the tabs other than the one on show are only numbers
    SHORT = 32          # rows

    TAB_NAMES = {"browse": "1 Browse", "search": "2 Search", "queue": "3 Queue",
                 "favorites": "4 Playlists", "files": "5 Files", "history": "6 History",
                 "info": "7 Info", "artist": "8 Artist", "stats": "9 Stats",
                 "settings": "0 Settings", "helptab": "? Help"}

    def on_resize(self, event) -> None:
        self._fit(event.size)

    def _fit(self, size=None) -> None:
        """Lay the screen out for the terminal's size."""
        width, height = size or self.size
        self._width = width
        try:
            self.screen_stack[0].set_class(height < self.SHORT, "short")
            self.query_one("#miller").set_class(width < self.NARROW, "narrow")
            self.query_one("#lists").set_class(width < self.NARROW, "narrow")
            self.query_one("#artist-side").set_class(width < self.NARROW, "narrow")
            self.query_one(Footer).compact = width < self.NARROW
        except NoMatches:
            return
        self._label_tabs()

    def _label_tabs(self) -> None:
        try:
            tabs = self.query_one("#tabs", TabbedContent)
        except NoMatches:
            return
        short = getattr(self, "_width", self.size.width) < self.TABS_NARROW
        for pane, name in self.TAB_NAMES.items():
            label = name if not short or pane == tabs.active else name.split(" ", 1)[0]
            tab = tabs.get_tab(pane)
            if str(tab.label) != label:
                tab.label = label

    def action_activity(self) -> None:
        """The page of what is being saved and exported."""
        if not isinstance(self.screen, Activity):
            self.push_screen(Activity())

    def action_copies(self, open_: bool) -> None:
        """→ opens a song that other collections have too out to one row for each of
        them, ← closes it; Enter on a row plays that collection's copy."""
        if self._in_browse_list():
            (self.action_browse_in if open_ else self.action_browse_out)()
            return
        table = self._focused_table()
        if table is not None:
            changed = table.toggle(open_)
            if not changed and not open_ and table.id == "browse-songs":
                self.action_browse_out()        # ← among Browse's songs: back to the list

    def _use_keys(self, keys: dict) -> None:
        """The user's own keys, by the bindings' ids. One that is not known is told."""
        known = {b.id for b in self.BINDINGS if b.id} | LIST_KEYS
        wrong = sorted(k for k in keys if k not in known)
        self.set_keymap({k: str(v) for k, v in keys.items() if k in known})
        if wrong:
            self.notify("settings.json names keys for what isn't there: " + ", ".join(wrong)
                        + ". What there is: " + ", ".join(sorted(known)),
                        severity="warning", timeout=20)

    def _music_dir(self) -> Path:
        folder = Path(self.settings.music_dir).expanduser()
        return folder if folder.is_dir() else Path.home()

    def on_mount(self) -> None:
        if self.settings.keys:
            self._use_keys(self.settings.keys)
        for t in (theme.BYTEBANDIT, *amiga_themes.THEMES):
            self.register_theme(t)
        self.theme = (self.settings.theme if self.settings.theme in self.available_themes
                      else theme.DEFAULT_THEME)
        theme.use(self.theme_variables)
        self.theme_changed_signal.subscribe(self, self._theme_changed)
        self.query_one("#files-tree").border_title = str(self._music_dir())
        self.query_one("#partners").border_title = "Worked with"
        self.query_one("#partners").display = False
        self.query_one("#artist-facts", Static).update(Text(
            "Press m on a song to see everything by its artist,\n"
            "or A for the album or game it belongs to.", style="dim"))
        self._warn_unprotected()
        if self.index is not None:
            self._start()
        elif catalogs.first_start(self.settings):
            self._ask_collections(first=True)
        elif catalogs.needs_work(self.settings):
            self.push_screen(Busy("Getting the catalogs ready..."))
            self._prepare()
        else:
            self._start()

    @work(thread=True, group="sandbox")
    @guarded("checking the sandbox")
    def _warn_unprotected(self) -> None:
        """Once at start: when the replayers can't be shut in (no bubblewrap, and no
        Landlock), the user is told plainly, and how to mend it. (Finding out runs
        bubblewrap once, so not in the way of the start.)"""
        text = sandbox.warning()
        if text:
            self.call_from_thread(self.notify, text, severity="warning", timeout=30)

    # ---- the collections the user has ----

    def _offered(self) -> list[tuple[str, str, str]]:
        return [(c.id, c.name, c.about) for c in collections_.known() if c.about]

    def _ask_collections(self, first: bool = False) -> None:
        """Which collections to have: at the first start, and from Settings."""
        now = catalogs.chosen(self.settings)

        def answered(ids: list[str] | None) -> None:
            if first:
                ids = ids or list(catalogs.DEFAULT)
                self.settings.update(collections=ids)
                self.push_screen(Busy("Getting the catalogs ready (first start only)..."))
                self._prepare()
            elif ids and ids != now:
                gone = [collections_.name(c) for c in now if c not in ids]
                if gone:
                    def sure(yes: bool | None) -> None:
                        if yes:
                            self._change_collections(ids)
                    self.push_screen(Confirm(
                        f"Stop having {', '.join(gone)}?\n\nIts catalog is taken away. Your "
                        "saved music, favorites and playlists stay,\nand come back when you "
                        "choose it again."), sure)
                else:
                    self._change_collections(ids)
        title = ("Welcome to ByteBandit Music Player. Which collections do you want?\n"
                 "You can change it later in Settings." if first else
                 "Which collections do you want?")
        self.push_screen(ChooseCollections(
            title, self._offered(), now,
            esc=f"take {catalogs.summary(catalogs.DEFAULT)}" if first else "cancel"), answered)

    def _change_collections(self, ids: list[str]) -> None:
        self.push_screen(Busy("Getting the catalogs ready..."))
        self._prepare(ids)

    @work(thread=True, exclusive=True, group="catalog")
    def _prepare(self, ids: list[str] | None = None) -> None:
        """Get the chosen collections' catalogs ready (and change the choice to `ids`)."""
        def say(msg: str) -> None:
            screen = self.screen
            if isinstance(screen, Busy):
                self.call_from_thread(screen.say, msg)
        ok = True
        try:
            if ids is not None:
                catalogs.choose(self.settings, ids, say)
            else:
                catalogs.prepare(self.settings, say)
        except Exception as e:
            ok = False
            self.call_from_thread(self.notify, _update_failed(e) if _busy(e) else
                                  f"The catalogs couldn't be made: {e}",
                                  severity="error", timeout=20)
        self.call_from_thread(self._catalog_done, ok)

    def _start(self) -> None:
        if self.index is None:
            self.index = Index()
        self.player = Player(self.index, self.fetcher, self._output, on_song=self._played,
                             on_done=self._done, settings=self.settings,
                             keep=self.library.protected_keys, levels=self.library,
                             trouble=Trouble(self.library))
        SongTable.troubled = staticmethod(lambda key: self.player.trouble.get(key) is not None)
        SongTable.copies = staticmethod(lambda song: self.index.copies(song))
        vis = self.query_one("#visual", Visualizer)
        vis.demo = self.settings.demo_effects
        vis.set_mode(self.settings.visualizer)
        self.player.want_pattern(vis.wants_pattern)
        self._read_collections()
        self._open_keeper()
        self._fill_formats()
        self._fill_settings()
        tree = self.query_one("#files-tree", FilesTree)
        tree.accept = lambda path: (playlists.is_playlist_file(path) or archives.is_archive(path)
                                    or self.index.local_song(path) is not None)
        tree.reload()
        if self._play_paths:
            self._play_local(self._play_paths)
        elif self.settings.resume:
            self._resume()
        if self._use_mpris:
            from ..mpris import Mpris
            self._mpris = Mpris(self.player, self.settings)
            self._mpris.start()
        self.set_interval(1 / 12, self._tick)
        if self._ask and not self.settings.asked_downloads:
            self._offer_downloads()
        if self._ask and command.should_offer(self.settings.asked_command):
            self._offer_command()
        if self._ask:
            self._look_for_newer_lists()
            self._auto_update()
            self.set_interval(3600, self._auto_update)          # for a player left running
            self.set_interval(600, self._keep_later)            # the night, a saved list
        self._relocate()
        if self._kept:
            self._keep_fetch()      # what was not fetched the last time

    def _offer_command(self) -> None:
        """Once at the first start: in an AppImage, make the bbmp command; in a Flatpak,
        which may not, say the line that makes it (command.py)."""
        if command.in_flatpak() and not command.appimage():
            self.settings.update(asked_command=True)
            self.notify(command.make() + f" ({command.how_to_start()} command says it again.)",
                        timeout=60)
            return

        def answered(yes: bool | None) -> None:
            self.settings.update(asked_command=True)
            if yes:
                self.notify(command.make(), timeout=20)
            else:
                self.notify(f"Later: bbmp command, or {command.how_to_start()} command.",
                            timeout=12)
        self.push_screen(Confirm(
            "Start the player by typing bbmp in a terminal?\n\n"
            f"This makes a two-line script,\n"
            f"{command.target()}, that starts it."), answered)

    @work(thread=True, group="relocate")
    def _relocate(self) -> None:
        """Favorites and playlist songs the collections have moved are found again."""
        try:
            moved = self.library.relocate(self.index)
        except Exception:
            return
        if moved:
            def done() -> None:
                self._favs = self.library.favorite_keys()
                self.notify(f"{len(moved)} of your favorites and playlist songs moved in "
                            "their collection, and were found again.")
                if self._kept:
                    self._keep_fetch()
            self.call_from_thread(done)

    @work(thread=True, exclusive=True, group="catalog")
    def download_catalog(self) -> None:
        """Update the chosen collections' catalogs from their servers, behind what the
        user does; what it is doing shows under the song that plays."""
        def say(msg: str) -> None:
            self.call_from_thread(self._update_said, msg)
        try:
            done = catalogs.update(self.settings, say)
        except Exception as e:
            self.call_from_thread(self._update_said, "")
            self.call_from_thread(self.notify, _update_failed(e),
                                  severity="warning" if _busy(e) else "error", timeout=15)
            return
        finally:
            self._updating = False
        self.call_from_thread(self._update_said, "")
        self._say_failed(done)
        self.call_from_thread(self._catalog_done, not getattr(done, "failed", None))

    def _update_said(self, text: str) -> None:
        try:
            now = self.query_one("#now")
        except NoMatches:
            return
        if text:
            now.border_subtitle = f"Catalogs: {text}"
        elif (now.border_subtitle or "").startswith("Catalogs:"):
            now.border_subtitle = ""

    def _catalog_done(self, ok: bool = True) -> None:
        if isinstance(self.screen, Busy):
            self.pop_screen()
        if self.player is None:
            self._start()
        else:
            old = self.index
            self.index = Index()
            self.index.kept_dir = old.kept_dir
            self.player.index = self.index
            self.fetcher.checksum = self.index.md5     # downloads are checked against the new
            self._retire(old)
            self._read_collections()
            self._open_keeper()
            self._fmt = self._fmt_id = None
            self._fill_formats()
            if ok:
                self.notify(f"The catalogs are up to date: {self.index.song_count():,} songs.")
            self._relocate()
            if self._kept:
                self._keep_fetch()      # what the collections added and changed

    def _retire(self, old: Index) -> None:
        """An Index the player no longer uses is closed once nothing can be using it
        still (a search, saving or an export that began with it), so the catalog files
        an update replaced are let go."""
        retired = self.__dict__.setdefault("_retired", [])
        retired.append(old)

        def close_them() -> None:
            if self._keeping or self._exporting:
                self.set_timer(60, close_them)
                return
            while retired:
                close = getattr(retired.pop(), "close", None)
                if close is not None:
                    try:
                        close()
                    except Exception:
                        pass
        self.set_timer(60, close_them)

    def action_update_catalog(self) -> None:
        if getattr(self, "_updating", False):
            self.notify("The catalogs are being updated already.")
            return
        self._updating = True
        self.notify("Updating the catalogs. You can go on listening.")
        self.download_catalog()

    @work(thread=True, exclusive=True, group="catalog")
    def _auto_update(self) -> None:
        """The catalogs update by themselves when they are due (Settings), behind what
        the user does."""
        if not catalogs.due(self.settings):
            return
        self.call_from_thread(self.notify, "Updating the catalogs, as Settings says ("
                              + catalogs.EVERY.get(self.settings.update_every,
                                                   catalogs.EVERY["week"])[1] + ")...")
        try:
            done = catalogs.update(self.settings)
        except Exception as e:
            if not _busy(e):
                self.call_from_thread(self.notify, f"{_update_failed(e)} It is tried again "
                                      "later.", severity="warning", timeout=15)
            return
        self._say_failed(done)
        self.call_from_thread(self._catalog_done, not getattr(done, "failed", None))

    def _say_failed(self, done) -> None:
        """The collections an update couldn't do (the others were updated)."""
        if getattr(done, "failed", None):
            self.call_from_thread(self.notify, f"Not updated: {done.why()}. It is tried "
                                  "again later.", severity="warning", timeout=20)

    @work(thread=True, group="newer")
    def _look_for_newer_lists(self) -> None:
        """Ask the collections whether they have a newer list of their files than the
        catalog was made from."""
        hosted = self.index._hosted
        for c, name in self._collections:
            source = collections_.get(c)
            have = self.index.about(c).get("dated") or 0
            if source is None or c in hosted or not have:
                continue
            try:
                newest = source.latest()
            except (OSError, EOFError):
                continue
            if newest > have + 3600:
                self.call_from_thread(
                    self.notify, f"{name} has a newer list of its files, from "
                    f"{units.moment(newest)}. Yours is from {units.moment(have)}. "
                    "u updates the catalog.", timeout=20)

    def _read_collections(self) -> None:
        self._collections = [(c, name) for c, name, _, _ in self.index.collections()]
        hosted = self.index._hosted
        # the ones whose songs are their own; the others' songs are their hosts'
        self._own = [(c, name) for c, name in self._collections if c not in hosted]
        several = len(self._collections) > 1
        # lists of songs from more than one collection say which one a song is from
        SongTable.collections = dict(self._own) if len(self._own) > 1 else {}
        ids = [c for c, _ in self._collections]
        self._collection = self.settings.collection if several and self.settings.collection in ids else ""
        if len(self._own) == 1 and self._collection == self._own[0][0]:
            self._collection = ""       # the only one of its own is what "" shows
        self.refresh_bindings()

    def check_action(self, action: str, parameters: tuple) -> bool | None:
        if action == "collection":
            return len(self._collections) > 1     # with one collection there is nothing to choose
        if action == "copies":                    # the arrows are the song lists' and Browse's
            return isinstance(self.focused, SongTable) or self._in_browse_list()
        if action == "lookup":                    # w and d are the artist page's
            try:
                return self.query_one("#tabs", TabbedContent).active == "artist"
            except NoMatches:
                return False
        return True

    def _collection_name(self) -> str:
        """The collection on show; "" when it is all of them. With one collection that
        has songs of its own, all of them is that one."""
        if not self._collection and len(self._own) == 1:
            return self._own[0][1]
        return dict(self._collections).get(self._collection, "")

    def _later(self, key: str, delay: float, fn) -> None:
        if (t := self._debounce.get(key)) is not None:
            t.stop()

        def run() -> None:
            try:
                fn()
            except NoMatches:
                pass        # the app is closing and its lists are gone

        self._debounce[key] = self.set_timer(delay, run)

    def action_tab(self, tab: str) -> None:
        self.query_one("#tabs", TabbedContent).active = tab

    @on(TabbedContent.TabActivated)
    def _tab_activated(self, event: TabbedContent.TabActivated) -> None:
        pane = event.pane.id
        self._label_tabs()
        if pane == "favorites":
            self._fill_lists()
            self._show_list()       # now; the list's highlight then finds it shown
            self.query_one("#fav-songs").focus()
        elif pane == "history":
            self._show_history()
            self.query_one("#hist-songs", SongTable).focus()
        elif pane == "stats":
            self._show_stats()
            self.query_one("#periods").focus()
        elif pane == "settings":
            self._fill_settings()
            self.query_one("#settings-list").focus()
        elif pane == "files":
            self.query_one("#files-tree").focus()
        elif pane == "artist":
            self.query_one("#artist-songs").focus()
        elif pane == "info":
            self._info_shown = None
            self._show_info()
            self.query_one("#info-scroll").focus()
        elif pane == "queue":
            self._refresh_queue(force=True)
            self.query_one("#queue-songs").focus()
        elif pane == "search":
            self.query_one("#q").focus()
        elif pane == "browse":
            self._focus_current()
        elif pane == "helptab":
            self.query_one("#help-scroll").focus()

    def _show_history(self) -> None:
        """The songs played, newest first. Asked of the library only when something has
        been played since it was last shown (or the catalog has changed)."""
        t = self.query_one("#hist-songs", SongTable)
        asked = (self.__dict__.get("_plays_started", 0), id(self.index))
        if asked != self.__dict__.get("_history_asked"):
            self._history_asked = asked
            self._history = self.library.history(self.index)
        if self._history != t._base or self._favs != t._shown_with[0]:
            t.set_songs(self._history, self._favs)

    def _refresh_queue(self, force: bool = False) -> None:
        songs, pos = self.player.queue()
        if force or (len(songs), pos) != self._last_queue:
            self._last_queue = (len(songs), pos)
            t = self.query_one("#queue-songs", SongTable)
            t.set_songs(songs, self._favs, current=pos)
            t.border_title = f"Queue ({len(songs)})"

    def _focused_table(self) -> SongTable | None:
        f = self.focused
        return f if isinstance(f, SongTable) else None

    @on(DataTable.RowHighlighted)
    def _row_highlighted(self, event: DataTable.RowHighlighted) -> None:
        """The cursor resting on a song for a moment fetches it, so that it starts at
        once if it is chosen (Player.prefetch)."""
        table = event.data_table
        if (isinstance(table, SongTable) and table.has_focus and self.player
                and 0 <= event.cursor_row < len(table.songs)):
            song = table.songs[event.cursor_row]
            self._later("prefetch", 0.3, lambda: self.player.prefetch(song))

    @on(DataTable.RowSelected)
    def _row_selected(self, event: DataTable.RowSelected) -> None:
        table = event.data_table
        if not isinstance(table, SongTable) or not table.songs:
            return
        row = event.cursor_row
        if table.id == "queue-songs":
            pos = table.base_row(row)
            if table._depth[row]:
                # a copy of a queued song: it plays in that song's place
                songs, _ = self.player.queue()
                if 0 <= pos < len(songs):
                    songs[pos] = table.songs[row]
                    self.player.play_list(songs, pos)
            else:
                self.player.jump(pos)
            return
        song = table.songs[row]
        if not is_supported(song.format) or formats_info.needed(song.format):
            self.notify(formats_info.why_not(song.format), severity="warning", timeout=15)
            return
        rest = [s for s in table.play_from(row) if is_supported(s.format)]
        self.player.play_list(rest)

    def _target_song(self) -> Song | None:
        if (table := self._focused_table()) and (song := table.selected()):
            return song
        if isinstance(self.focused, FilesTree) and (node := self.focused.cursor_node) and node.data:
            if song := self.index.local_song(node.data.path):
                return song
        return self.player.status().song

    def _theme_changed(self, *_) -> None:
        """Whichever way the theme was changed (Settings, or ctrl+p), draw with it and keep it."""
        theme.use(self.theme_variables)
        if self.settings.theme != self.theme:
            self.settings.update(theme=self.theme)
        if self.player is None:
            return
        self._fill_settings()
        active = self.query_one("#tabs", TabbedContent).active
        if active == "stats":
            self._show_stats()
        elif active == "favorites":
            self._fill_lists()
            self._show_list()
        elif active == "info":
            self._info_shown = None
        elif active == "artist" and self._page is not None:
            name, kind = self._page.title, self._page.kind
            if kind == "artist":
                self.action_artist(name)

    def _pick_theme(self) -> None:
        names = sorted(self.available_themes,
                       key=lambda n: (n != theme.DEFAULT_THEME, not n.startswith("amiga-"), n))
        before = self.theme

        def chosen(name: str | None) -> None:
            self.theme = name or before       # Esc puts the old one back

        pick = Pick("Theme", [(n, n + ("  (default)" if n == theme.DEFAULT_THEME else "")) for n in names])
        pick.preview = lambda name: setattr(self, "theme", name)
        pick.start = before
        self.push_screen(pick, chosen)

    @on(OptionList.OptionHighlighted, "#periods")
    def _period_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        self._stats_period = event.option_id
        self._show_stats()

    def _show_stats(self) -> None:
        stats = self.library.stats(since=stats_view.since(self._stats_period))
        self.query_one("#stats-body", Static).update(stats_view.render(stats, self._stats_period))

    def _resume(self) -> None:
        try:
            data = json.loads(self._session_path.read_text())
        except (OSError, ValueError):
            return
        if not isinstance(data, dict):
            return
        keys, pos = data.get("queue", []), data.get("pos", -1)
        if not (isinstance(keys, list) and isinstance(pos, int) and 0 <= pos < len(keys)
                and all(isinstance(k, str) for k in keys)):
            return
        subsong, seconds = data.get("subsong", 0), data.get("seconds", 0.0)
        subsong = subsong if isinstance(subsong, int) and not isinstance(subsong, bool) else 0
        seconds = (float(seconds) if isinstance(seconds, (int, float))
                   and not isinstance(seconds, bool) else 0.0)
        # the song that was playing is found at once and put back; the rest of the queue
        # (a long one takes seconds to look up) is found in the background and put
        # round it when it is, without interrupting it
        song = self.index.resolve(keys[pos])
        if song is not None:
            self.player.restore([song], 0, subsong, seconds)
            self.notify(f"Resuming {song.title}; press space to play")
        if len(keys) < 2:
            return
        index, player = self.index, self.player
        # until it is, the saved queue is what is saved again at quitting
        self._resuming = (data, song.key if song is not None else None)

        def rest() -> None:
            songs, at = [], -1
            for i, key in enumerate(keys):
                if (found := index.resolve(key)) is not None:
                    if i == pos:
                        at = len(songs)
                    songs.append(found)
            if song is not None and at >= 0:
                player.set_queue(songs, at, [song.key])
            elif song is None and songs:          # it is gone: the queue, where it was
                player.restore(songs, min(pos, len(songs) - 1))
            self._resuming = None
        threading.Thread(target=rest, name="resume-queue", daemon=True).start()

    def _save_session(self) -> None:
        try:
            self._session_path.parent.mkdir(parents=True, exist_ok=True)
            session = self.player.session()
            resuming = getattr(self, "_resuming", None)
            if resuming is not None and session["queue"] in ([resuming[1]], []):
                # quit before the saved queue was all found again: it is kept as it was
                session = {**resuming[0], **({"subsong": session["subsong"],
                                              "seconds": session["seconds"]}
                                             if session["queue"] else {})}
            # written beside it, then put in its place: a player that stops halfway
            # (the power, a crash) leaves the last one whole
            tmp = self._session_path.with_name(
                f"{self._session_path.name}.{os.getpid()}.tmp")
            try:
                tmp.write_text(json.dumps(session))
                os.replace(tmp, self._session_path)
            finally:
                tmp.unlink(missing_ok=True)
        except OSError:
            pass

    def action_help(self) -> None:
        self.query_one("#tabs", TabbedContent).active = "helptab"

    def _played(self, song: Song) -> None:
        # a song of a collection that another one hosts is put down as the host's
        self._play_id = self.library.start_play(self.index.resolve(song.key) or song)
        self._plays_started = self.__dict__.get("_plays_started", 0) + 1     # History asks again

    def _done(self, song: Song, seconds: float) -> None:
        if self._play_id is not None:
            self.library.finish_play(self._play_id, seconds)
            self._play_id = None

    def _tick(self) -> None:
        try:
            now = self.query_one("#now", NowPlaying)
            vis = self.query_one("#visual", Visualizer)
        except NoMatches:   # shutting down, or a modal screen is on top
            return
        st = self.player.status()
        now.show(st, bool(st.song and st.song.key in self._favs))
        if vis.mode:
            vis.show(st)
        active = self.query_one("#tabs", TabbedContent).active
        if active == "queue":
            self._refresh_queue()
        elif active == "info":
            self._show_info()
        key = st.song.key if st.song else None
        if key != self._last_song:
            self._last_song = key
            self.sub_title = f"{st.song.artist} - {st.song.title}" if st.song else f"v{__version__}"

    def on_unmount(self) -> None:
        for timer in self._debounce.values():
            timer.stop()
        self._closing = True
        self._keep_stop.set()
        self._export_stop.set()
        if self._mpris:
            self._mpris.stop()
        if self.player:
            if self.settings.resume:
                self._save_session()
            self.player.shutdown()
        self.fetcher.close()


def _busy(e: Exception) -> bool:
    """Is it that another update runs (here, or `bbmp update` in a terminal)?"""
    return isinstance(e, getattr(catalogs, "Busy", ()))


def _update_failed(e: Exception) -> str:
    if _busy(e):
        return "The catalogs are being updated already."
    return f"The update failed: {e}."


LOG_LIMIT = 5_000_000        # bytes the log may grow to; then only its end is kept


def log_path() -> Path:
    return paths.CACHE_DIR / "bbmp.log"


@contextlib.contextmanager
def quiet_stderr():
    """UADE's emulator process and some libraries print to stderr, which would
    scribble over the screen. Send it to a log file meanwhile. Gives where the log
    was when this began, so that what was written since can be read."""
    paths.CACHE_DIR.mkdir(parents=True, exist_ok=True)
    log = log_path()
    old = paths.CACHE_DIR / "modland.log"           # its name before
    if old.is_file():
        old.unlink(missing_ok=True)
    try:
        if log.stat().st_size > LOG_LIMIT:
            with open(log, "rb") as f:
                f.seek(-LOG_LIMIT // 5, os.SEEK_END)
                end = f.read()
            log.write_bytes(end)
    except OSError:
        pass
    with open(log, "ab", buffering=0) as f:
        start = f.tell()
        saved = os.dup(2)
        # Textual draws the screen on sys.__stderr__: it keeps a copy of the real
        # terminal, and only what others write to descriptor 2 goes to the log
        screen = os.fdopen(os.dup(saved), "w", buffering=1, encoding="utf-8",
                           errors="replace")
        real = sys.__stderr__
        sys.__stderr__ = screen
        os.dup2(f.fileno(), 2)
        try:
            yield start
        finally:
            os.dup2(saved, 2)
            os.close(saved)
            sys.__stderr__ = real
            screen.close()


def _freeze_startup() -> None:
    """What starting up made stays: the collector needn't look at it again."""
    import gc
    gc.collect()
    gc.freeze()


def run(play_paths: list[str] = ()) -> int:
    app = ModlandApp(play_paths=play_paths, mpris=True, ask=True)
    # The player's thread lets go of Python's lock at every read from a replayer and
    # waited the whole switch interval (5 ms) to get it back while the screen was
    # drawn; and a full garbage collection of what starting up made took 100-235 ms.
    # Either empties the sound's buffer (measured 1 October 2026).
    sys.setswitchinterval(0.0005)
    freeze = threading.Timer(6, _freeze_startup)
    freeze.daemon = True
    freeze.start()
    with quiet_stderr() as start:
        app.run()
    freeze.cancel()
    code = app.return_code or 0
    if code:
        # Textual has shown what went wrong on the screen; what the helpers and
        # libraries wrote meanwhile is in the log: the end of it is said here
        try:
            with open(log_path(), "rb") as f:
                f.seek(start)
                said = f.read().decode("utf-8", "replace")
        except OSError:
            said = ""
        tail = "\n".join(said.strip().splitlines()[-40:])
        print(f"{tail}\n\nByteBandit Music Player stopped because of an error (above). "
              f"All of it is in {log_path()}.", file=sys.stderr)
    return code
