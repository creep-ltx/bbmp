# SPDX-License-Identifier: GPL-3.0-or-later
"""Browse, in columns as ranger and yazi have them, and Search. A part of the app.

Browse shows three columns: the list one came from, the list one is in, and a look at
what the entry under the cursor holds. → or Enter goes into an entry, ← goes back. The
first list is the ways to browse: by collection, format, platform, chip, artist, or
what is new. Where one is is a path of steps, ("format", "Protracker") and so on.

Each list is a widget of its own that stays as it is while one is deeper in: going
into an entry makes the look at it the list one is in, going back makes the list one
was in the look again. Nothing is filled anew either way."""

from __future__ import annotations

from rich.text import Text
from textual import on, work
from textual.app import App
from textual.css.query import NoMatches
from textual.widgets import DataTable, Input, OptionList, Static, TabbedContent, Tabs
from textual.widgets.option_list import Option

from .. import collection as collections_, formats_info, index as index_mod, platforms, theme, units
from ..backends import is_supported

from .guard import guarded
from .widgets import KEPT, NEW, Note, OneLineList, SongTable

IN = "\x1f"            # between a collection and a format, in a view's format ids
SEP = "\x1e"           # between a step's kind and its value, in an option's id

SHOWN = 2000        # songs a list shows at the most: more take long to draw, and memory

# The ways to browse, in the first list and in a collection's
MENU = {
    "formats": ("Formats", "The music by the program or player it was made with: "
                           "Protracker, SID, AHX..."),
    "platforms": ("Platforms", "The music by the computer or console it was made for: "
                               "Amiga, C64, Mega Drive..."),
    "chips": ("Chips", "The music by the sound chip it plays on: SID, Paula, YM2612, OPL..."),
    "artists": ("Artists", "Everyone, A to Z, with everything they made in every format."),
    "new": ("New", "What the collections have added or changed, by day."),
}
WAYS = ("formats", "platforms", "chips", "artists", "new")

Step = tuple[str, str]


def step_id(step: Step) -> str:
    return f"{step[0]}{SEP}{step[1]}"


def id_step(id_: str) -> Step:
    kind, _, value = id_.partition(SEP)
    return kind, value


def holds_songs(path: list[Step]) -> bool:
    """Does the entry at the end of the path hold songs (rather than a list)?"""
    return bool(path) and path[-1][0] in ("artist", "day")


class BrowsePart(metaclass=type(App)):
    """Browse (in columns) and Search. A part of ModlandApp."""

    _path: list[Step]                   # where Browse is: the steps taken from the first list
    _levels: list                       # a list for each step, the last the one one is in
    _preview_of: list | None = None     # what the look on the right shows
    _songs_of: list | None = None       # what the song table shows

    # ---- where one is ----

    def _place(self, path: list[Step] | None = None) -> dict:
        """The collection, platform or chip, format and artist the path is in."""
        ctx = {"collection": "", "view": None, "fmt": None, "fmt_in": "", "artist": None,
               "day": None, "menu": None}
        for kind, value in (getattr(self, "_path", []) if path is None else path):
            if kind == "collection":
                ctx["collection"] = value
            elif kind in ("platform", "chip"):
                ctx["view"] = (kind, value)
            elif kind == "format":
                ctx["fmt_in"], _, ctx["fmt"] = value.rpartition(IN)
            elif kind == "artist":
                ctx["artist"] = value
            elif kind == "day":
                ctx["day"] = value
            elif kind == "menu":
                ctx["menu"] = value
        return ctx

    def _view(self) -> tuple[str, str] | None:
        """(kind, id) of the platform or chip Browse is in, or None."""
        return self._place()["view"]

    def _here(self) -> str:
        """The collection of the format on show: in a view, the one it is listed under."""
        return self._fmt_in or self._collection

    def _change_days(self, collection: str | None = None) -> list[tuple[str, int, int]]:
        """The days with changes. Counting them takes a quarter of a second, and they
        change only with the catalog."""
        collection = self._collection if collection is None else collection
        cache = self.__dict__.setdefault("_days_cache", {})
        if any(k[0] != id(self.index) for k in cache):
            cache.clear()
        key = (id(self.index), collection)
        if key not in cache:
            cache[key] = self.index.change_days(collection)
        return cache[key]

    def _view_counts(self, kind: str, collection: str = "") -> dict[str, int]:
        """Songs on each platform, or on each chip, in a collection or all of them."""
        cache = self.__dict__.setdefault("_view_cache", {})
        if any(k[0] != id(self.index) for k in cache):
            cache.clear()
        key = (id(self.index), kind, collection)
        if key not in cache:
            out: dict[str, int] = {}
            for c in ([collection] if collection else [c for c, _ in self._own]):
                for id_, songs in self.index.view_counts(c, kind).items():
                    out[id_] = out.get(id_, 0) + songs
            cache[key] = out
        return cache[key]

    def _several(self) -> bool:
        """More than one collection: the first list is All and each of them."""
        return len(getattr(self, "_collections", [])) > 1

    def _root(self, collection: str = "") -> list[Step]:
        """The steps to a collection (or All), where there is more than one."""
        return [("collection", collection)] if self._several() else []

    def _where_name(self, collection: str) -> str:
        if collection:
            return collections_.name(collection)
        return self._own[0][1] if len(self._own) == 1 else "every collection"

    # ---- what a list holds ----

    def _entries(self, path: list[Step]) -> tuple[str, list[Option]]:
        """The title and the options of the list at the end of a path."""
        ctx = self._place(path)
        c = ctx["collection"]
        last = path[-1] if path else ("", "")
        dim = "dim"
        if not path and self._several():
            # where, then how: All and each collection, each with the ways to browse it
            options = [Option(Text.assemble(("All", "bold"),
                                            (f"\u00a0\u00a0{self.index.song_count():,}", dim)),
                              id=step_id(("collection", "")))]
            options += [Option(Text.assemble(
                name, (f"\u00a0\u00a0{self.index.song_count(id_):,}", dim),
                (KEPT if self._is_kept(collection=id_) else "", theme.current.accent)),
                id=step_id(("collection", id_))) for id_, name in self._collections]
            return "Browse", options
        if not path or last[0] == "collection":
            options = []
            for key in WAYS:
                if key == "new" and not self._change_days(c):
                    continue
                label = Text(MENU[key][0])
                if key == "formats":
                    label.append(f"\u00a0\u00a0{len(self.index.formats(c)):,}", dim)
                options.append(Option(label, id=step_id(("menu", key))))
            return ("Browse by" if not path else collections_.name(c) if c else "All"), options
        if last == ("menu", "formats"):
            return self._format_entries(c)
        if last in (("menu", "platforms"), ("menu", "chips")):
            kind = last[1][:-1]
            counts = self._view_counts(kind, c)
            options = [Option(Text.assemble(name, (f"  {counts[id_]:,}", dim)),
                              id=step_id((kind, id_)))
                       for id_, name in sorted(platforms.KINDS[kind].items(),
                                               key=lambda x: x[1].lower())
                       if counts.get(id_)]
            if not options:
                options = [Option(Text("No music here is known to be on any.", style=dim),
                                  disabled=True)]
            return f"{MENU[last[1]][0]} · {self._where_name(c)}", options
        if last[0] in ("platform", "chip"):
            return self._view_entries(c, last[0], last[1])
        if last[0] == "format":
            fmt, here = ctx["fmt"], ctx["fmt_in"] or c
            artists = self.index.artists(fmt, here, view=ctx["view"])
            options = [Option(Text.assemble(a, (f"  {n}", dim),
                                            (KEPT if self._is_kept(fmt, a, collection=here) else "",
                                             theme.current.accent)),
                              id=step_id(("artist", a)))
                       for a, n in artists]
            size = 0 if ctx["view"] else self.index.format_sizes(here).get(fmt, 0)
            return " · ".join(x for x in (f"{fmt} ({len(artists):,})",
                                          units.size(size) if size else "") if x), options
        if last == ("menu", "artists"):
            artists = self.index.all_artists(c)
            return (f"Artists ({len(artists):,}) · {self._where_name(c)}",
                    [Option(Text.assemble(a, (f"  {n:,}", dim)), id=step_id(("artist", a)))
                     for a, n in artists])
        if last == ("menu", "new"):
            days = self._change_days(c)
            return f"New · {self._where_name(c)} ({len(days)} days)", [
                Option(Text.assemble(units.day(day), (f"  {added} new" if added else "", dim),
                                     (f"  {updated} changed" if updated else "", dim)),
                       id=step_id(("day", day)))
                for day, added, updated in days]
        return "", []

    def _format_label(self, fmt: str, songs: int, collection: str, shown: str) -> Text:
        ok = is_supported(fmt)
        info = formats_info.about(fmt)
        # ✓ verified against the original, ! plays with faults or gaps,
        # + needs a file: a download, or one from the user
        mark = ((f" {info.mark}", f"bold {theme.current.good}" if info.group == "verified"
                 else f"bold {theme.current.warn}") if ok and info.mark else ("", ""))
        return Text.assemble((shown, "" if ok else "dim strike"), mark,
                             (f"  {songs:,}", "dim"),
                             (KEPT if self._is_kept(fmt, collection=collection) else "",
                              theme.current.accent))

    def _format_entries(self, c: str) -> tuple[str, list[Option]]:
        self._sizes = self.index.format_sizes(c)
        name = collections_.name(c) if c else ""
        options = []
        for fmt, songs, _ in self.index.formats(c):
            # in HVSC's own list, "HVSC - Musicians" is "Musicians"
            shown = fmt.removeprefix(f"{name} - ") if c else fmt
            options.append(Option(self._format_label(fmt, songs, c, shown),
                                  id=step_id(("format", fmt))))
        title = " · ".join(x for x in (f"Formats ({len(options)})", self._where_name(c),
                                       units.size(self.index.size(c))) if x)
        return title + (KEPT if self._is_kept(collection=c) else ""), options

    def _view_entries(self, c: str, kind: str, id_: str) -> tuple[str, list[Option]]:
        """The formats on a platform or chip, under the collection each is in: the
        collections keep their own layout, and only what is on it is shown."""
        shown = [c] if c else [x for x, _ in self._own]
        on = {x: rows for x in shown if (rows := self.index.view_formats(x, kind, id_))}
        heads = len(on) > 1
        options = []
        for x in shown:
            if x not in on:
                continue
            rows = on[x]
            if heads:
                options.append(Option(Text.assemble(
                    (collections_.name(x), "bold underline"),
                    (f"  {sum(n for _, n in rows):,}", "dim")), disabled=True))
            for fmt, songs in rows:
                options.append(Option(self._format_label(
                    fmt, songs, x, fmt.removeprefix(f"{collections_.name(x)} - ")),
                    id=step_id(("format", f"{x}{IN}{fmt}"))))
        if not options:
            options.append(Option(Text(f"Nothing on {platforms.name(kind, id_)} here.",
                                       style="dim"), disabled=True))
        return f"{platforms.name(kind, id_)} · {self._where_name(c)}", options

    def _songs(self, path: list[Step]) -> tuple[str, list, dict | None]:
        """The title, songs and (for days) what is shown in place of the album, of an
        entry that holds songs."""
        ctx = self._place(path)
        c = ctx["collection"]
        kind, value = path[-1]
        if kind == "day":
            changed = self.index.changed(value, c)
            songs = [s for _, s in changed[:SHOWN]]
            albums = {s.key: f"{s.format} · {s.artist}" + ("" if what == "added" else f" ({what})")
                      for what, s in changed[:SHOWN]}
            return (f"{units.day(value)} ({len(changed):,}" + (
                f", the first {SHOWN:,} shown)" if len(changed) > SHOWN else ")"), songs, albums)
        if ctx["fmt"] is not None:
            songs = self.index.songs(ctx["fmt"], value, ctx["fmt_in"] or c, view=ctx["view"])
        else:
            songs = [s for s in self.index.artist_songs(value) if not c or s.collection == c]
        return " · ".join(x for x in (f"{value} ({len(songs)})",
                                      units.size(sum(s.size for s in songs))) if x), songs, None

    # ---- the columns ----

    def _miller(self):
        return self.query_one("#miller")

    def _preview_list(self) -> OneLineList:
        return self._miller().query_one(".preview-list", OneLineList)

    def _current(self):
        """The list or table one is in (none before a catalog is open)."""
        if not hasattr(self, "_path"):
            return None
        if holds_songs(self._path):
            return self.query_one("#browse-songs", SongTable)
        return self._levels[-1] if self._levels else None

    def _in_browse_list(self) -> bool:
        """Has a list of Browse's columns the keys?"""
        return bool(getattr(self, "_levels", None)) and self.focused is self._levels[-1]

    def _new_list(self, title: str, options: list[Option], classes: str) -> OneLineList:
        ol = OneLineList(*options, markup=False, classes=f"pane {classes}")
        ol.border_title = title
        return ol

    def _start_browse(self) -> None:
        """Browse as it was left: the path kept in the settings, or the formats."""
        path = [tuple(s) for s in self.settings.browse
                if isinstance(s, (list, tuple)) and len(s) == 2]
        if path[:1] == [("menu", "collections")]:
            path = path[1:]                 # a Collections list of 3 October 2026
        if not path:
            # the settings of before October 2026: a collection, and a platform or chip
            kind, _, id_ = self.settings.view.partition(":")
            if kind in platforms.KINDS:
                path = [("menu", f"{kind}s"), (kind, id_)]
            else:
                path = [("menu", "formats")]
            path = self._root(self.settings.collection) + path
        elif self._several() and path[0][0] != "collection":
            path = self._root() + path      # the ways were the first list, before All
        elif not self._several() and path[0][0] == "collection":
            path = path[1:]
        self._go(path)

    def _fill_formats(self) -> None:
        """What the lists show may have changed (a catalog, kept music, downloads):
        show it again, where one is."""
        if not hasattr(self, "_path"):
            self._start_browse()
            return
        cursors = [ol.highlighted_option.id if ol.highlighted_option else None
                   for ol in self._levels]
        table = self.query_one("#browse-songs", SongTable)
        row = table.cursor_row if holds_songs(self._path) else None
        self._go(list(self._path), cursors, row)

    def _go(self, path: list[Step], cursors: list | None = None, row: int | None = None) -> None:
        """Show the columns of a path, made anew. `cursors`: the option id each list's
        cursor is on (the next step's, where there is one); `row`: the song's row."""
        try:
            miller = self._miller()
        except NoMatches:
            return
        good: list[Step] = []       # the steps that are still there
        levels = []
        for i in range(len(path) + 1):
            here = path[:i]
            title, options = self._entries(here)
            ids = [o.id for o in options]
            want = path[i] if i < len(path) else None
            if want is not None and not holds_songs(here + [want]) and step_id(want) not in ids:
                want = None         # a format a catalog no longer has, say
            if want is not None and holds_songs(here + [want]) and step_id(want) not in ids:
                want = None
            cursor = (step_id(want) if want else
                      cursors[i] if cursors and i < len(cursors) else None)
            ol = self._new_list(title, options, "level")
            ol.node = list(here)
            ol.start_at = ids.index(cursor) if cursor in ids else next(
                (k for k, o in enumerate(options) if not o.disabled), None)
            levels.append(ol)
            if want is None:
                break
            good.append(want)
            if holds_songs(good):
                break
        for old in getattr(self, "_levels", []):
            old.remove()
        self._path = good
        self._levels = levels
        self._placed = False                # until the cursors are in place, after a refresh
        self._preview_of = None
        self._songs_of = None
        miller.mount(*levels, before=self._preview_list())

        def placed() -> None:
            for ol in levels:
                if ol.start_at is not None and ol.start_at < ol.option_count:
                    ol.highlighted = ol.start_at
            if holds_songs(self._path):
                self._fill_songs(self._path)
                table = self.query_one("#browse-songs", SongTable)
                if row is not None and table.row_count:
                    table.move_cursor(row=min(row, table.row_count - 1))
                self._show_song_info()
                self._show_note(None)
            else:
                step = self._highlighted_step()
                if step is not None:
                    self._fill_preview(self._path + [step])
                self._show_note(step)
            self._arrange()
            self._focus_current()
            self._placed = True
        self.call_after_refresh(placed)

    def _focus_current(self) -> None:
        try:
            if self.query_one("#tabs", TabbedContent).active == "browse":
                current = self._current()
                if current is not None:
                    current.focus()
        except NoMatches:
            pass

    def _arrange(self) -> None:
        """Which widget is the parent, the current list and the look on the right."""
        in_songs = holds_songs(self._path)
        current = len(self._levels) if in_songs else len(self._levels) - 1
        # only the list one is in takes the keys: Tab doesn't wander into the others
        for i, ol in enumerate(self._levels):
            ol.set_class(i == current - 1, "parent")
            ol.set_class(i == current, "current")
            ol.can_focus = i == current
        table = self.query_one("#browse-songs", SongTable)
        step = self._highlighted_step()
        songs_ahead = step is not None and holds_songs(self._path + [step])
        table.set_class(in_songs, "current")
        table.set_class(songs_ahead, "preview")
        table.can_focus = in_songs
        preview = self._preview_list()
        preview.set_class(not in_songs and not songs_ahead, "preview")
        preview.can_focus = False
        self.query_one("#song-info", Static).set_class(in_songs, "preview")
        self._sync()

    def _highlighted_step(self) -> Step | None:
        if holds_songs(self._path) or not self._levels:
            return None
        option = self._levels[-1].highlighted_option
        return id_step(option.id) if option is not None and option.id else None

    def _sync(self) -> None:
        """The rest of the app asks where Browse is: the collection, the format and
        artist (or "new" and the day), the collection a view's format is listed under."""
        ctx = self._place()
        step = self._highlighted_step()
        if step is not None and step[0] in ("format", "artist", "day"):
            ctx = self._place(self._path + [step])
        self._collection = ctx["collection"]
        self._fmt_in = ctx["fmt_in"]
        if ctx["day"] is not None or ctx["menu"] == "new":
            self._fmt, self._artist = NEW, ctx["day"]
        else:
            self._fmt, self._artist = ctx["fmt"], ctx["artist"]
        if self.settings.collection != self._collection:
            self.settings.update(collection=self._collection)

    def _keep_path(self) -> None:
        browse = [list(s) for s in self._path]
        if self.settings.browse != browse:
            self.settings.update(browse=browse)

    @on(OptionList.OptionHighlighted, ".level")
    def _level_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        if (not getattr(self, "_levels", None) or event.option_list is not self._levels[-1]
                or holds_songs(self._path)):
            return
        step = id_step(event.option_id) if event.option_id else None
        self._sync()
        self._show_note(step)
        if step is not None:
            self._later("preview", 0.12, lambda: self._fill_preview(self._path + [step]))

    @on(OptionList.OptionSelected, ".level")
    def _level_selected(self, event: OptionList.OptionSelected) -> None:
        if self._levels and event.option_list is self._levels[-1]:
            self.action_browse_in()

    def _fill_preview(self, path: list[Step]) -> None:
        """The look on the right at what the entry under the cursor holds."""
        if path != self._preview_of:
            self._preview_of = list(path)
            if holds_songs(path):
                self._fill_songs(path)
            else:
                preview = self._preview_list()
                title, options = self._entries(path)
                preview.clear_options()
                preview.add_options(options)
                preview.border_title = title
                preview.node = list(path)
        self._arrange()

    def _fill_songs(self, path: list[Step]) -> None:
        if path == self._songs_of:
            return
        self._songs_of = list(path)
        title, songs, albums = self._songs(path)
        table = self.query_one("#browse-songs", SongTable)
        table.set_songs(songs, self._favs, albums=albums)
        table.border_title = title

    def _stop_preview(self) -> None:
        if (timer := self._debounce.get("preview")) is not None:
            timer.stop()

    def action_browse_in(self) -> None:
        """Into the entry under the cursor: its list becomes the one one is in."""
        step = self._highlighted_step()
        if step is None:
            return
        self._stop_preview()
        path = self._path + [step]
        if holds_songs(path):
            self._fill_songs(path)
            self._path = path
            table = self.query_one("#browse-songs", SongTable)
            if table.row_count:
                table.move_cursor(row=0)
            self._arrange()
            table.focus()
            self._show_song_info()
            self._show_note(None)
        else:
            preview = self._preview_list()
            if self._preview_of != path:
                title, options = self._entries(path)
                preview.clear_options()
                preview.add_options(options)
                preview.border_title = title
            # the look becomes the list one is in, and a new look is made beside it
            preview.remove_class("preview-list", "preview")
            preview.add_class("level")
            preview.node = list(path)
            self._levels.append(preview)
            self._path = path
            self._preview_of = None
            self._miller().mount(self._new_list("", [], "preview-list"), after=preview)
            first = next((k for k, o in enumerate(preview.options) if not o.disabled), None)
            preview.highlighted = None
            self._arrange()
            preview.focus()
            if first is not None:
                preview.highlighted = first
        self._keep_path()

    def action_browse_out(self) -> None:
        """Back to the list one came from; the one one was in becomes the look again."""
        if not self._path:
            return
        self._stop_preview()
        if holds_songs(self._path):
            self._preview_of = list(self._path)
            self._path = self._path[:-1]
        else:
            current = self._levels.pop()
            self._preview_list().remove()
            current.remove_class("level", "current", "parent")
            current.add_class("preview-list")
            self._preview_of = list(self._path)
            self._path = self._path[:-1]
        self._arrange()
        self._levels[-1].focus()
        self._show_note(self._highlighted_step())
        self._keep_path()

    @on(DataTable.RowHighlighted, "#browse-songs")
    def _browse_row(self, event: DataTable.RowHighlighted) -> None:
        if holds_songs(getattr(self, "_path", [])):
            self._later("song-info", 0.1, self._show_song_info)

    def _show_song_info(self) -> None:
        """The look on the right when one is among the songs: the song under the cursor."""
        song = self.query_one("#browse-songs", SongTable).selected()
        info = self.query_one("#song-info", Static)
        info.border_title = "Song"
        if song is None:
            info.update("")
            return
        t = Text()
        t.append(song.title + "\n", style=f"bold {theme.current.accent}")
        t.append(song.artist + "\n", style=theme.current.primary)
        if song.album_name:
            t.append(song.album_name + "\n")
        t.append("\n")
        rows = [("Format", song.format), ("Size", units.size(song.size)),
                ("Collection", collections_.name(song.collection))]
        if song.also:
            rows.append(("Also on", ", ".join(collections_.name(c) for c in song.also)))
        if song.partner:
            rows.append(("With", song.partner))
        for label, value in rows:
            t.append(f"{label:<11}", style="dim")
            t.append(f"{value}\n")
        about = formats_info.about(song.format)
        if about.title:
            t.append(f"\n{about.title}\n", style="dim")
        t.append("\nEnter plays · a queues", style="dim")
        info.update(t)

    def _show_note(self, step: Step | None) -> None:
        """Under the columns: what the entry under the cursor is, or what is known
        about how well its format plays."""
        notes = self.query("#format-note")
        if not notes:       # a highlight can come before the note is mounted
            return
        note = notes.first(Note)
        if step is not None and step[0] == "menu":
            note.show(Text.assemble((MENU[step[1]][0], "bold"), "  ", MENU[step[1]][1]))
            return
        ctx = self._place(self._path + ([step] if step else []))
        if ctx["fmt"]:
            self._show_format_note(ctx["fmt"])
        elif ctx["menu"] == "new" or ctx["day"]:
            self._show_format_note(NEW)
        else:
            note.show(Text("→ or Enter goes in, ← goes back. "
                           "c: All and the collections, b: the platforms.", style="dim"))

    def _show_format_note(self, fmt: str) -> None:
        """Under the lists: what is known about how well the format plays."""
        if not self.query("#format-note"):
            return
        if fmt == NEW:
            where = self._collection_name() or "every collection"
            shown = self._collection or (self._own[0][0] if len(self._own) == 1 else "")
            about = self.index.about(shown) if shown else {}
            text = Text.assemble(("New", "bold"), f"  What {where} has added or changed, "
                                 "by day. Enter on a day goes to its songs.")
            if about.get("dated"):
                text.append(f"\nThe list of {where}'s files is from "
                            f"{units.moment(about['dated'])}. u asks for a newer one.")
            self.query_one("#format-note", Note).show(text)
            return
        info = formats_info.about(fmt)
        colour = {"verified": theme.current.good, "faults": theme.current.warn,
                  "needs": theme.current.warn,
                  "off": theme.current.bad, "no": theme.current.bad}.get(info.group, "")
        line = Text.assemble((fmt, "bold"), "  ", (info.title, colour))
        if info.counted:
            line.append(f"  ·  {info.counted}", style="dim")
        sizes = self.__dict__.get("_sizes") or {}
        if sizes.get(fmt):
            line.append(f"  ·  {units.size(sizes[fmt])}", style="dim")
        for text in (info.text, info.files_short):
            if text:
                line.append(f"\n{text}")
        self.query_one("#format-note", Note).show(line)

    # ---- going places ----

    def _browse_to(self, path: list[Step], cursor: Step | None = None) -> None:
        self.query_one("#tabs", TabbedContent).active = "browse"
        self._go(path, [None] * len(path) + [step_id(cursor) if cursor else None])
        self._keep_path()

    def action_view(self) -> None:
        """b: browse by platform (or, from there, by chip)."""
        base = self._root(self._place()["collection"])
        to = ("chips" if self._path[len(base):len(base) + 1] == [("menu", "platforms")]
              else "platforms")
        self._browse_to(base + [("menu", to)])

    def action_collection(self) -> None:
        """c: in Browse, the collections, the cursor on the one Browse is in; elsewhere
        (Search) the next collection to search, and Browse goes along."""
        if len(self._collections) < 2:
            self.notify("There is only the one collection.")
            return
        here = self._place()["collection"]
        if self.query_one("#tabs", TabbedContent).active == "browse":
            self._browse_to([], ("collection", here))
            return
        order = [""] + [c for c, _ in self._collections
                        if len(self._own) > 1 or (c, _) not in self._own]
        self._collection = order[(order.index(self._collection) + 1) % len(order)]
        self.settings.update(collection=self._collection)
        self._go(self._root(self._collection) + [("menu", "formats")])
        self._keep_path()
        self._fill_settings()
        query = self.query_one("#q", Input).value.strip()
        if query:
            self._search(query)
        self.notify(f"Showing {self._collection_name() or 'every collection'}")

    def action_go_to_playing(self) -> None:
        """Browse to the song that plays: its collection, format, artist, and the song."""
        song = self.player.status().song
        if song is None:
            self.notify("Nothing is playing.")
            return
        if song.local:
            self.notify(f"{song.title} is a file on this computer: {song.path}", timeout=10)
            return
        path = self._root(song.collection) + [("menu", "formats"), ("format", song.format), ("artist", song.artist)]
        self.query_one("#tabs", TabbedContent).active = "browse"
        self._go(path)
        self._keep_path()

        def find() -> None:
            table = self.query_one("#browse-songs", SongTable)
            row = next((i for i, s in enumerate(table.songs) if s.key == song.key), None)
            if row is not None:
                table.move_cursor(row=row)
        self.call_after_refresh(lambda: self.call_after_refresh(find))

    def _browse_songs_under_cursor(self) -> list | None:
        """The songs of the entry under the cursor, where it holds songs (an artist, a day)."""
        step = self._highlighted_step()
        if step is None or not holds_songs(self._path + [step]):
            return None
        return self._songs(self._path + [step])[1]

    # ---- Search ----

    @on(Input.Changed, "#q")
    def _query_changed(self, event: Input.Changed) -> None:
        text = event.value.strip()
        if len(text) >= 2:
            self._later("search", 0.3, lambda: self._search(text))

    @on(Input.Submitted, "#q")
    def _query_submitted(self, event: Input.Submitted) -> None:
        self._search(event.value.strip())
        # to the results; when the search finds none, on to the tab bar (_show_found),
        # as Down and Esc do, so that the number keys work again
        self._leave_if_none = True
        self.query_one("#search-songs").focus()

    def _search(self, text: str) -> None:
        """Search in the background: a word as short as "a" takes a moment. In a view
        (a platform or chip, b), a search that names none searches in the view."""
        view = self._view()
        if text and view and not index_mod.view_terms(text)[1]:
            text = f"{text} {view[0]}:{view[1]}"
        self._searching = (text, self._collection)
        self._search_in_background(text, self._collection)

    @work(thread=True, exclusive=True, group="search")
    @guarded("searching")
    def _search_in_background(self, text: str, collection: str) -> None:
        songs = self.index.search(text, limit=SHOWN, collection=collection) if text else []
        found = len(songs)
        if found >= SHOWN:
            found = self.index.search_count(text, collection)
        self.call_from_thread(self._show_found, text, collection, songs, found)

    def _show_found(self, text: str, collection: str, songs: list, found: int) -> None:
        if getattr(self, "_searching", None) != (text, collection):
            return                          # a later search is on its way
        table = self.query_one("#search-songs", SongTable)
        table.set_songs(songs, self._favs)
        if getattr(self, "_leave_if_none", False):
            self._leave_if_none = False
            if not songs and table.has_focus:
                self.query_one("#tabs").query_one(Tabs).focus()
        noun = "result" if found == 1 else "results"
        title = f"{found:,} {noun}"
        if found > len(songs):
            title += f", the first {len(songs):,} shown"
        if self._collection:
            title += f" on {self._collection_name()}"
        view = self._view()
        if view and f"{view[0]}:{view[1]}" in text:
            title += f", {platforms.name(*view)}, as Browse is"
        if not songs and text and not index_mod.search_understood(text):
            _, terms = index_mod.view_terms(text)
            unknown = [k for k, id_, _ in terms if id_ is None]
            title = (f"No such {unknown[0]}: b lists them" if unknown else
                     "Nothing to search for: a word, or artist:, format:, title:, album:, "
                     "platform:, chip:")
        table.border_title = title

    def action_search(self) -> None:
        self.query_one("#tabs", TabbedContent).active = "search"
        self.query_one("#q", Input).focus()
