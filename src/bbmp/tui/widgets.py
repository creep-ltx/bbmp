# SPDX-License-Identifier: GPL-3.0-or-later
"""The lists, the Now playing box, the visualiser and the files tree."""

from __future__ import annotations

from itertools import repeat

from rich.cells import cell_len
from rich.padding import Padding
from rich.segment import Segment
from rich.style import Style
from textual.renderables.styled import Styled
from rich.text import Text
from textual import events
from textual.binding import Binding
from textual.geometry import Region, Size, Spacing
from textual.message import Message
from textual.scroll_view import ScrollView
from textual.strip import Strip
from textual.widget import Widget
from textual.widgets import DirectoryTree, Footer, Input, OptionList, Static, Tabs

from .. import collection as collections_, theme, units, visuals
from ..backends import is_supported
from ..index import Song
from ..player import Status


BARS = " ▁▂▃▄▅▆▇█"

NEW = "\x00new"      # in the list of formats: what the collection has added or changed

KEPT = "\u00a0⤓"      # beside what is kept on this computer

LONGEST = 56           # letters of a title in a list of songs

REPEAT_LABELS = {"off": "off", "all": "the whole queue", "one": "the current song"}


def mmss(t: float | None) -> str:
    t = int(t or 0)
    return f"{t // 60:02d}:{t % 60:02d}"


# The keys of the lists, with the ids by which settings.json changes them as it changes the
# app's (a list's own key comes first, so it must carry the same id). → and ← go in and out
# of Browse's columns and a song's copies: a scrolling widget's own → and ← (scroll
# sideways) would come first. Esc goes out too, so that someone who gives ← and → to
# paging ("page_up": "left", "page_down": "right") goes in with Enter and out with Esc.
IN_AND_OUT = [Binding("right", "app.copies(True)", show=False, id="open_copies"),
              Binding("left,escape", "app.copies(False)", show=False, id="close_copies")]
PAGES = [Binding("pageup", "page_up", show=False, id="page_up"),
         Binding("pagedown", "page_down", show=False, id="page_down")]
LIST_KEYS = {"page_up", "page_down"}     # ids only the lists have


class SearchBox(Input):
    """The search box. It takes every key as text; Esc leaves it for the results."""

    BINDINGS = [Binding("escape", "leave", show=False),
                Binding("down", "leave", show=False)]

    def action_leave(self) -> None:
        # to the results, or with none to the tab bar, where the number keys work again:
        # the tabbed box itself takes no keys, and the box kept them (3 October 2026)
        table = self.screen.query_one("#search-songs")
        (table if table.row_count else self.screen.query_one("#tabs").query_one(Tabs)).focus()


class OneLineList(OptionList):
    """A list whose options are each one line high, a long one cut with "…". Textual's
    OptionList measures every option as it is added, wrapping its text: 7,606 artists
    took 100 ms, 30 ms without (3 October 2026). Options are measured only as drawn.
    This replaces a private method of Textual's (8.2): check it on an upgrade."""

    DEFAULT_CSS = "OneLineList { text-wrap: nowrap; text-overflow: ellipsis; }"
    node: list = []          # in Browse: the path of steps whose entries it lists
    waiting = False          # in Browse: "…" until the catalog's answer comes (browse.Waiting)
    BINDINGS = IN_AND_OUT + PAGES

    def _update_lines(self) -> None:
        if not self.scrollable_content_region:
            return
        cache = self._line_cache
        lines = cache.lines
        start = lines[-1][0] + 1 if lines else 0
        width = self.scrollable_content_region.width - self._get_left_gutter_width()
        new = self.options[start:]
        if not any(option._divider for option in new):
            # all one line high, as nearly every list is: counted in bulk (26k artists
            # 32 to ~8 ms, 13 November 2026)
            ids = range(start, start + len(new))
            cache.index_to_line.update(zip(ids, range(len(lines), len(lines) + len(new))))
            cache.heights.update(dict.fromkeys(ids, 1))
            lines.extend(zip(ids, repeat(0)))
            new = ()
        for index, option in enumerate(new, start):
            cache.index_to_line[index] = len(lines)
            height = 1 + option._divider
            cache.heights[index] = height
            lines.extend((index, n) for n in range(height))
        last_divider = self.options and self.options[-1]._divider
        size = Size(width, len(lines) - (1 if last_divider else 0))
        if size != self.virtual_size:
            self.virtual_size = size
            self._scroll_update(size)


class SongTable(ScrollView, can_focus=True):
    """A list of songs. Enter plays from the selected row onwards.

    Only the rows in sight are made and drawn. Textual's DataTable, which this was,
    made every row as it was added: 100,000 songs (Modland's unknown artist) took 11 s
    to show and each cursor move 0.1 s (measured 13 November 2026). It looks as the
    DataTable did, with its CSS and component classes, and answers the parts of its
    interface the app uses (cursor_row, row_count, move_cursor, get_row_at,
    get_cell_at, update_cell_at, RowHighlighted, RowSelected)."""

    BINDINGS = [
        Binding("enter", "select_cursor", "Select", show=False),
        Binding("up", "cursor_up", "Cursor up", show=False),
        Binding("down", "cursor_down", "Cursor down", show=False),
        Binding("ctrl+home", "scroll_top", "Top", show=False),
        Binding("ctrl+end", "scroll_bottom", "Bottom", show=False),
        Binding("home", "scroll_home", "Home", show=False),
        Binding("end", "scroll_end", "End", show=False),
        # → and ← open and close a song's copies, and ← goes back out of Browse's songs
        *IN_AND_OUT, *PAGES,
    ]

    COMPONENT_CLASSES = {
        "datatable--cursor", "datatable--hover", "datatable--fixed", "datatable--fixed-cursor",
        "datatable--header", "datatable--header-cursor", "datatable--header-hover",
        "datatable--odd-row", "datatable--even-row",
    }

    # DataTable's own CSS (Textual 8.2), for the same look
    DEFAULT_CSS = """
    SongTable {
        background: $surface;
        color: $foreground;
        height: auto;
        max-height: 100%;
        &:focus {
            background-tint: $foreground 5%;
            & > .datatable--cursor {
                background: $block-cursor-background;
                color: $block-cursor-foreground;
                text-style: $block-cursor-text-style;
            }
            & > .datatable--header { background-tint: $foreground 5%; }
        }
        &:dark { & > .datatable--even-row { background: $surface-darken-1 40%; } }
        & > .datatable--header { text-style: bold; background: $panel; color: $foreground; }
        &:ansi > .datatable--header { background: ansi_bright_blue; color: ansi_default; }
        & > .datatable--even-row { background: $surface-lighten-1 50%; }
        & > .datatable--cursor {
            background: $block-cursor-blurred-background;
            color: $block-cursor-blurred-foreground;
            text-style: $block-cursor-blurred-text-style;
        }
        & > .datatable--hover { background: $block-hover-background; }
    }
    """

    class RowHighlighted(Message):
        """The cursor came onto a row."""

        def __init__(self, data_table: "SongTable", cursor_row: int) -> None:
            self.data_table = data_table
            self.cursor_row = cursor_row
            super().__init__()

        @property
        def control(self) -> "SongTable":
            return self.data_table

    class RowSelected(Message):
        """Enter (or a click on the row the cursor is on) chose a row."""

        def __init__(self, data_table: "SongTable", cursor_row: int) -> None:
            self.data_table = data_table
            self.cursor_row = cursor_row
            super().__init__()

        @property
        def control(self) -> "SongTable":
            return self.data_table

    COLUMNS = {"title": "Title", "artist": "Artist", "album": "Album", "format": "Format",
               "size": "Size"}
    # With more than one collection in the catalog, the app puts their names here
    # (by id) and every song says which one it is from.
    collections: dict[str, str] = {}
    kept = staticmethod(lambda key: False)      # is the song kept on this computer?
    troubled = staticmethod(lambda key: False)  # is it known not to play?
    # the same song in the other collections that have it (Song.also), from the catalog
    copies = staticmethod(lambda song: [])

    # The columns are as wide as the longest of what they show in the first MEASURED
    # rows; a longer one further down widens its column when it comes into sight. A
    # column doesn't narrow again, so that the columns stay put from one list to the next.
    MEASURED = 1000

    def __init__(self, *, columns: tuple[str, ...] = ("title", "artist", "format"), **kw):
        super().__init__(**kw)
        self.songs: list[Song] = []        # as shown: a row that is opened has its copies
        self._depth: list[int] = []        # 0 for a song, 1 for a copy of the one above it
        self._base: list[Song] = []        # as given
        self._open: set[str] = set()       # the keys of the rows opened out
        self._shown_with: tuple = (set(), -1, None)
        self._columns = columns
        self._widths = {"mark": 1, **{c: len(self.COLUMNS[c]) for c in columns}}
        self._header = [Text(" ")] + [Text(self.COLUMNS[c], justify="right") if c == "size"
                                      else Text(self.COLUMNS[c]) for c in columns]
        self._cursor = 0
        self._hover = 0
        self._show_hover = False
        self._drawn_with: tuple = (set(), -1, None, LONGEST)   # favourites, ▶, albums, title length
        self._rows: dict[int, tuple] = {}     # a row's cells as strings, made as it is drawn
        self._set: dict[tuple[int, int], Text] = {}             # cells changed by update_cell_at
        self._lines: dict[tuple, Strip] = {}
        self._styles: dict[tuple, tuple[Style, Style]] = {}
        self._made = 0                     # bumped whenever what a row shows may have changed
        self._wider = False                # a row in sight widened a column
        self._pseudo = None

    # ---- the interface of Textual's DataTable that the app uses ----

    @property
    def cursor_row(self) -> int:
        return self._cursor

    @property
    def row_count(self) -> int:
        return len(self.songs)

    def move_cursor(self, *, row: int | None = None, column: int | None = None,
                    animate: bool = False, scroll: bool = True) -> None:
        if row is not None:
            self._move(row, scroll=False)
        if scroll:
            self._scroll_cursor_into_view(animate)

    def get_row_at(self, row: int) -> list[Text]:
        if not 0 <= row < len(self.songs):
            raise IndexError(row)
        return [self.get_cell_at((row, c)) for c in range(len(self._columns) + 1)]

    def get_cell_at(self, coordinate) -> Text:
        row, column = coordinate
        if not 0 <= row < len(self.songs):
            raise IndexError(row)
        return self._set.get((row, column)) or self._texts(row)[column]

    def update_cell_at(self, coordinate, value, *, update_width: bool = False) -> None:
        row, column = coordinate
        if not 0 <= row < len(self.songs):
            raise IndexError(row)
        self._set[(row, column)] = value if isinstance(value, Text) else Text(str(value))
        self._made += 1
        self.refresh_row(row)

    # ---- filling ----

    def set_songs(self, songs: list[Song], favorites: set[str], current: int = -1,
                  albums: dict[str, str] | None = None) -> None:
        """`albums` has, by a song's key, what is shown in place of its album."""
        if songs != self._base:
            self._open &= {s.key for s in songs}
        self._base = list(songs)
        self._shown_with = (favorites, current, albums)
        rows: list[Song] = []
        depth: list[int] = []
        shown = -1              # the row of `current`, which counts the songs as given
        for i, s in enumerate(songs):
            if i == current:
                shown = len(rows)
            rows.append(s)
            depth.append(0)
            if s.key in self._open:
                copies = self.copies(s)
                rows += copies
                depth += [1] * len(copies)
        self._draw(rows, depth, favorites, shown, albums)

    def toggle(self, open_: bool | None = None) -> bool:
        """Open the selected song out to its copies in the other collections, or close
        it (from it, or from one of its copies). Says whether anything changed."""
        row = self.cursor_row
        if not self.songs or not 0 <= row < len(self.songs):
            return False
        if self._depth[row] == 1:                   # a copy: close its song
            while row > 0 and self._depth[row] == 1:
                row -= 1
            open_ = False
        song = self.songs[row]
        if not song.also:
            return False
        want = song.key not in self._open if open_ is None else open_
        if want == (song.key in self._open):
            return False
        (self._open.add if want else self._open.discard)(song.key)
        self.set_songs(self._base, *self._shown_with)
        self.move_cursor(row=row + (1 if want else 0))
        return True

    def base_row(self, row: int) -> int:
        """Where a row's song is in the list as given (a copy's: its song's); -1 if none."""
        if not 0 <= row < len(self.songs):
            return -1
        return sum(1 for i in range(row + 1) if self._depth[i] == 0) - 1

    def base_song(self) -> Song | None:
        """The selected row's song as the list has it: for a copy, the song it is a copy of."""
        i = self.base_row(self.cursor_row)
        return self._base[i] if 0 <= i < len(self._base) else None

    def play_from(self, row: int) -> list[Song]:
        """The songs to play from a row on: that one, then the songs below it, without
        the copies of other rows."""
        if not 0 <= row < len(self.songs):
            return []
        return [self.songs[row]] + [s for i, s in enumerate(self.songs)
                                    if i > row and self._depth[i] == 0]

    def _draw(self, songs: list[Song], depth: list[int], favorites: set[str], current: int,
              albums: dict[str, str] | None) -> None:
        same = self.songs == songs
        keep = self._cursor if same else 0
        self.songs = songs
        self._depth = depth
        longest = LONGEST
        if "size" in self._columns and self.size.width:
            longest = max(16, min(LONGEST, self.size.width - 20))   # the size stays in sight
        self._drawn_with = (favorites, current, albums, longest)
        self._rows.clear()
        self._set.clear()
        self._forget()
        for i in range(min(len(songs), self.MEASURED)):
            self._row(i)
        self._wider = False
        self._resize()
        self._cursor = min(keep, len(songs) - 1) if songs else 0
        if not same:            # a new list starts at its top; the same one stays in view
            self.scroll_to(0, 0, animate=False, force=True, immediate=True)
        self.refresh()
        if songs:
            self.post_message(self.RowHighlighted(self, self._cursor))
            if keep:
                self.call_after_refresh(self._scroll_cursor_into_view)

    def _row(self, i: int) -> tuple:
        """Row i as strings: (mark, whether it plays, the columns' cells). Its columns
        widen to it."""
        made = self._rows.get(i)
        if made is not None:
            return made
        favorites, current, albums, longest = self._drawn_with
        s = self.songs[i]
        key = s.key
        ok = is_supported(s.format)
        bad = ok and self.troubled(key)
        ok = ok and not bad
        mark = ("▶" if i == current else "♥" if key in favorites
                else "!" if bad else "" if not ok else "⤓" if self.kept(key) else "")
        mark = mark or ("" if ok else "✗")
        album = s.album_name + (f" (with {s.partner})" if s.partner else "")
        where = self.collections.get(s.collection, "") or (
            collections_.name(s.collection) if self._depth[i] else "")
        if where and s.also:
            where = " + ".join([where, *(collections_.name(c) for c in s.also)])
        if albums and key in albums:
            album = albums[key]
        if where and "format" not in self._columns:
            # a list without the format column says the collection with the album
            album = f"{album.strip()} · {where}" if album.strip() else where
        # a title as long as a line would push what follows it out of sight
        title = s.title
        if len(title) > longest:
            title = title[:longest - 1] + "…"
        if self._depth[i]:
            title = f"  ↳ {title}"          # a copy, in the collection it says
        elif s.also:
            title = ("▾ " if key in self._open else "▸ ") + title
        cells = {"title": title, "artist": s.artist, "album": album.strip(),
                 "format": s.format + (f" · {where}" if where else ""),
                 "size": units.size(s.size)}
        made = (mark, ok, tuple(cells[c] for c in self._columns))
        widths = self._widths
        for c, text in zip(self._columns, made[2]):
            if (n := cell_len(text)) > widths[c]:
                widths[c] = n
                self._wider = True
        if len(self._rows) > 4 * self.MEASURED:
            self._rows.clear()
        self._rows[i] = made
        return made

    def _texts(self, i: int) -> list[Text]:
        mark, ok, cells = self._row(i)
        style = {"♥": f"bold {theme.current.bad}", "!": f"bold {theme.current.warn}"}
        # an empty cell plain, as DataTable drew it (a dim one would dim its padding)
        return [Text(mark, style=style.get(mark, "")) if mark else Text()] + [
            Text(text, style="" if ok else "dim", justify="right" if c == "size" else "left")
            if text else Text() for c, text in zip(self._columns, cells)]

    def remark_kept(self) -> None:
        """Files have come to the kept music: the ⤓ marks again, where a row has that
        mark or none (not ▶, ♥, ! or ✗). The list was not drawn again when a saved
        file landed, and its ⤓ was missing until one went away and back (7 October 2026).
        Rows are marked as they are drawn, so the rows made so far are made again."""
        for at in [at for at, text in self._set.items() if at[1] == 0 and str(text) in ("", "⤓")]:
            del self._set[at]
        self._rows.clear()
        self._forget()
        self.refresh()

    def selected(self) -> Song | None:
        if self.songs and 0 <= self.cursor_row < len(self.songs):
            return self.songs[self.cursor_row]
        return None

    # ---- the cursor, keys and the mouse, as DataTable's with a row cursor ----

    def _move(self, row: int, scroll: bool = True, animate: bool = False) -> None:
        row = max(0, min(row, len(self.songs) - 1))
        old, self._cursor = self._cursor, row
        if old != row:
            self.refresh_row(old)
            self.refresh_row(row)
            if self.songs:
                self.post_message(self.RowHighlighted(self, row))
        if scroll:
            self._scroll_cursor_into_view(animate)

    def _scroll_cursor_into_view(self, animate: bool = False) -> None:
        if not self.songs:
            return
        region = Region(int(self.scroll_x), self._cursor + 1, self._width(), 1)
        self.scroll_to_region(region, animate=animate, spacing=Spacing(1, 0, 0, 0), force=True)

    def _hover_off(self) -> None:
        if self._show_hover:
            self._show_hover = False
            self.refresh_row(self._hover)

    def _page(self) -> int:
        return self.scrollable_content_region.height - 1

    def action_page_down(self) -> None:
        self._hover_off()
        height = self._page()
        self.scroll_relative(y=height, animate=False, force=True)
        self._move(min(self._cursor + height, len(self.songs) - 1), scroll=False)

    def action_page_up(self) -> None:
        self._hover_off()
        height = self._page()
        self.scroll_relative(y=-height, animate=False)
        self._move(max(self._cursor - height, 0), scroll=False)

    def action_scroll_top(self) -> None:
        self._hover_off()
        self._move(0)

    def action_scroll_bottom(self) -> None:
        self._hover_off()
        self._move(len(self.songs) - 1)

    def action_scroll_home(self) -> None:
        self._hover_off()
        self.scroll_x = 0

    def action_scroll_end(self) -> None:
        self._hover_off()
        self.scroll_x = self.max_scroll_x

    def action_cursor_up(self) -> None:
        self._hover_off()
        self._move(self._cursor - 1)

    def action_cursor_down(self) -> None:
        self._hover_off()
        self._move(self._cursor + 1)

    def action_select_cursor(self) -> None:
        self._hover_off()
        if self.songs:
            self.post_message(self.RowSelected(self, self._cursor))

    def _on_mouse_move(self, event: events.MouseMove) -> None:
        row = event.style.meta.get("row") if event.style.meta else None
        if row is None or row < 0:
            self._hover_off()
            return
        if (row, True) != (self._hover, self._show_hover):
            self.refresh_row(self._hover)
            self._hover, self._show_hover = row, True
            self.refresh_row(row)

    def _on_leave(self, _: events.Leave) -> None:
        self._hover_off()

    async def _on_click(self, event: events.Click) -> None:
        self._show_hover = True
        row = event.style.meta.get("row") if event.style.meta else None
        if row is None or row < 0 or row >= len(self.songs):
            return
        again = row == self._cursor
        self._move(row, scroll=False)
        if again:
            self.post_message(self.RowSelected(self, row))
        self._scroll_cursor_into_view(animate=True)
        event.stop()

    # ---- drawing ----

    def _width(self) -> int:
        return sum(w + 2 for w in self._widths.values())

    def _resize(self) -> None:
        self.virtual_size = Size(self._width(), len(self.songs) + 1)

    def _forget(self) -> None:
        self._made += 1
        self._lines.clear()

    def refresh_row(self, row: int) -> None:
        y = row + 1 - int(self.scroll_y)
        if 0 <= row < len(self.songs) and 1 <= y < self.size.height:
            self.refresh(Region(0, y, self.size.width, 1))

    def notify_style_update(self) -> None:
        super().notify_style_update()
        self._styles.clear()
        self._forget()
        self.refresh()

    def _cell_styles(self, header: bool, hover: bool, cursor: bool) -> tuple[Style, Style]:
        """The cursor's and the hover's style on a cell, and what goes over the cell's
        own (the cursor's colour), as DataTable's _get_styles_to_render_cell."""
        key = (header, hover, cursor)
        found = self._styles.get(key)
        if found is None:
            get = self.get_component_rich_style
            style = Style()
            if hover:
                style += get("datatable--hover")
                if header:
                    style += get("datatable--header-hover")
            if cursor:
                style += get("datatable--cursor")
                if header:
                    style += get("datatable--header-cursor")
            found = self._styles[key] = (style, Style.from_color(color=style.color))
        return found

    def render_lines(self, crop: Region) -> list[Strip]:
        self._pseudo = self.get_pseudo_class_state()
        lines = super().render_lines(crop)
        if self._wider:          # a row just drawn widened a column: draw them all again
            self._wider = False
            self._forget()
            self._resize()
            self.refresh()
        return lines

    def render_line(self, y: int) -> Strip:
        width = self.size.width
        scroll_x, scroll_y = self.scroll_offset
        base = self.rich_style
        row = -1 if y == 0 else y - 1 + scroll_y
        if row >= len(self.songs):
            return Strip.blank(width, base)
        cursor = row == self._cursor and row >= 0
        hover = self._show_hover and row == self._hover and row >= 0
        key = (row, self._made, cursor, hover, self._pseudo, width, scroll_x, base)
        line = self._lines.get(key)
        if line is not None:
            return line
        if row == -1:
            row_style = self.get_component_styles("datatable--header").rich_style
            cells = self._header
        else:
            row_style = self.get_component_styles(
                "datatable--odd-row" if row % 2 else "datatable--even-row").rich_style
            cells = [self._set.get((row, c)) or text for c, text in enumerate(self._texts(row))]
        component, post = self._cell_styles(row == -1, hover, cursor)
        pre = row_style + Style.from_meta({"row": row}) + component
        options = self.app.console_options
        render = self.app.console.render_lines
        segments: list[Segment] = []
        for cell, w in zip(cells, self._widths.values()):
            lines = render(Styled(Padding(cell, (0, 1)), pre_style=pre, post_style=post),
                           options.update_dimensions(w + 2, 1).update(no_wrap=True))
            segments += lines[0]
        segments.append(Segment(" " * max(0, width - self._width()),
                                row_style + component + Style.from_meta({"row": row})))
        line = Strip(segments).crop(scroll_x, scroll_x + width).adjust_cell_length(width, base).simplify()
        if len(self._lines) > 2000:
            self._lines.clear()
        self._lines[key] = line
        return line


def _drawn_key(content):
    """What a Static shows, as something to compare: a Text by its letters and styles (a
    renderer may hand back the same Text changed)."""
    if isinstance(content, Text):
        return (content.plain, tuple(content.spans), content.style)
    return content


def draw_if_changed(widget: Static, content) -> None:
    """Update a Static twelve times a second only when what it shows changed: an unchanged
    visualiser and Now playing were sent to the terminal again and again (85 KB a second
    with nothing playing, 11 October 2026), which a slow terminal spends its time
    on while the keys wait."""
    key = _drawn_key(content)
    if getattr(widget, "_drawn", None) == key:
        return
    widget._drawn = key
    widget.update(content, layout=False)


class NowPlaying(Widget):
    """The strip at the bottom: the song, where it is, how loud. Four lines (three in a
    short terminal): title and artist; the format and the play modes; the time bar; the
    level meters, the channels and the queue.

    Each line is a widget of its own, drawn only when it changed: as one, all of the strip
    was sent to the terminal twelve times a second while a song played, though mostly
    only the meters change (40 KB/s with no visualiser; split, 19 KB/s; 11 October 2026)."""

    # its height is fixed (CSS), so an update needn't lay out the whole screen again:
    # 12 a second did, and took most of a CPU core (1 October 2026)
    DEFAULT_CSS = """
    NowPlaying { layout: vertical; }
    NowPlaying > .now-row { height: 1; width: 1fr; text-wrap: nowrap; text-overflow: ellipsis; }
    """
    ROWS = 4

    def compose(self):
        for i in range(self.ROWS):
            yield Static(classes=f"now-row now-row-{i}", markup=False)

    def on_mount(self) -> None:
        self._rows = list(self.query(".now-row").results(Static))

    # what is being saved or fetched is set as the subtitle; the strip has only its top
    # line, so it is shown there, on the right
    @property
    def border_subtitle(self):
        return self.border_title

    @border_subtitle.setter
    def border_subtitle(self, value) -> None:
        self.border_title = value

    def show(self, st: Status, favorite: bool, likes: tuple[int, bool] | None = None) -> None:
        width = max(20, self.content_size.width or self.size.width - 4)
        rows = self.content_size.height or 4
        pal = theme.current
        if st.song is None:
            t = Text()
            t.append("ByteBandit Music Player", style=f"bold {pal.accent}")
            t.append("\nNothing playing  ·  Enter plays a song, r starts radio, / searches",
                     style="dim")
            if st.error:
                t.append(f"\n{st.error}", style=pal.bad)
            self._draw(t.split("\n"))
            return
        icon = {"playing": "▶", "paused": "❚❚", "loading": "…", "stopped": "■"}[st.state]
        fmt = (st.info.format if st.info and st.info.format else st.song.format)
        about = fmt + (f" · {st.backend}" if st.backend else "")

        title = Text()
        title.append(f"{icon}  ", style=f"bold {pal.accent}")
        title.append(st.song.title, style="bold")
        title.append("  ")
        title.append(st.song.artist, style=pal.primary)
        if favorite:
            title.append("  ♥", style=f"bold {pal.bad}")
        if likes is not None:
            title.append(f"  {likes[0]:,} like{'' if likes[0] == 1 else 's'}", style="dim")

        modes = []
        if st.info and st.info.subsongs > 1:
            modes.append(f"subsong {st.info.subsong + 1}/{st.info.subsongs}")
        if st.forever:
            modes.append("∞ on and on")
        if st.repeat != "off":
            modes.append(f"repeat {st.repeat}")
        if st.shuffle:
            modes.append("shuffle")
        if st.radio:
            modes.append(f"radio: {st.radio}")

        dur = st.info.duration if st.info else None
        times = f"{mmss(st.elapsed)} / {mmss(dur)}" if dur else mmss(st.elapsed)
        bar = Text()
        bar_w = max(10, width - len(times) - 2)
        frac = min(1.0, st.elapsed / dur) if dur else 0.0
        filled = int(bar_w * frac)
        bar.append("━" * filled, style=f"bold {pal.accent}")
        bar.append("━" * (bar_w - filled), style="dim")
        bar.append("  " + times)

        meters = Text()
        vu_w = 12
        for label, level in zip("LR", st.levels):
            n = int(level * vu_w)
            meters.append(f"{label} ", style="dim")
            meters.append("■" * n, style=pal.bar(level * 0.85))
            meters.append("■" * (vu_w - n) + "  ", style="dim")
        if st.channels:
            meters.append("".join(BARS[min(8, int(c * 8.99))] for c in st.channels[:64]),
                          style=pal.accent)
        right = f"vol {st.volume}%"
        if st.queue_len:
            right = f"queue {st.queue_pos + 1}/{st.queue_len}  ·  " + right

        if st.error:
            info = Text(st.error, style=pal.bad)
        else:
            info = Text(about, style="dim")
            if modes:
                info.append("  ·  " + "  ·  ".join(modes), style=pal.primary)
        if rows < 4:                   # a short terminal: the format joins the title
            title.append("   ")
            title.append_text(info)
            lines = [title, bar, _spread(meters, Text(right, style="dim"), width)]
        else:
            lines = [title, info, bar, _spread(meters, Text(right, style="dim"), width)]
        for line in lines:
            line.no_wrap = True
            line.truncate(width, overflow="ellipsis")
        self._draw(lines)

    def _draw(self, lines) -> None:
        """Each line in its row; a row whose line didn't change isn't drawn again."""
        rows = self.__dict__.get("_rows") or []
        lines = list(lines)
        for i, row in enumerate(rows):
            draw_if_changed(row, lines[i] if i < len(lines) else Text())


def _spread(left: Text, right: Text, width: int) -> Text:
    """`left`, then `right` at the end of a line `width` wide (if there is room)."""
    gap = width - left.cell_len - right.cell_len
    if gap < 2:
        return left
    return Text.assemble(left, " " * gap, right)


class Visualizer(Static):
    """Cycles through the visualisers in visuals.MODES that the song playing has data for;
    hidden when off. The spectra, the spectrogram, the oscilloscope and the demo effects
    are drawn from the sound, so every song has them; channel bars and scopes and the
    pattern only where the replayer gives them (Status.shows). The visualiser chosen (`mode`) is kept: a song without it shows the
    spectrum instead, and the next song that has it shows it again."""

    FALLBACK = "spectrum"
    GRACE = 0.6         # seconds at a song's start in which its replayer may say what it has

    def __init__(self, **kw):
        super().__init__(**kw)
        self.mode: str | None = None
        self.available: frozenset = frozenset(visuals.MODES)    # all, until a song plays
        self._spectrum = visuals.Spectrum()
        self._scope = visuals.Scope()
        self._channels = visuals.Channels()
        self._bars = visuals.GradientSpectrum()
        self._mirror = visuals.GradientSpectrum(mirror=True)
        self._spectrogram = visuals.Spectrogram()
        self._scopes = visuals.ChannelScopes()
        self._demo = visuals.Demo()

    demo = False        # the demo effects in the cycle too (Settings); they are music-free eye candy

    def cycle(self) -> str | None:
        modes = [m for m in visuals.MODES if m in self.available
                 and (m != "demo" or self.demo or self.mode == "demo")] + [None]
        at = modes.index(self.mode) if self.mode in modes else -1
        self.set_mode(modes[(at + 1) % len(modes)])
        return self.mode

    @property
    def wants_pattern(self) -> bool:
        """Whether the visualiser chosen draws from the pattern (the player then reads it)."""
        return self.mode in visuals.WANTS_PATTERN

    @property
    def wants_waves(self) -> bool:
        """Whether it draws each channel's own sound (the replayer then plays a copy of
        the song for each channel)."""
        return self.mode in visuals.WANTS_WAVES

    def set_mode(self, mode: str | None) -> None:
        self.mode = mode if mode in visuals.MODES else None
        self.set_class(self.mode is not None, "on")
        self._title(self.shown())

    def shown(self, elapsed: float | None = None) -> str | None:
        """The visualiser drawn: the one chosen, or the spectrum where the song lacks it."""
        if self.mode is None or self.mode in self.available:
            return self.mode
        if elapsed is not None and elapsed < self.GRACE:
            return self.mode            # not known yet: its first data may still come
        return self.FALLBACK

    def _title(self, shown: str | None, scene: str = "") -> None:
        title = visuals.TITLES.get(shown, "") + (f" · {scene}" if scene else "")
        if shown and self.mode and shown != self.mode:
            title += f" · no {visuals.TITLES[self.mode].lower()} in this song"
        if getattr(self, "_shown_title", None) != title:    # asked twelve times a second
            self._shown_title = title
            self.border_title = title

    def show(self, st: Status) -> None:
        self.available = st.shows if st.song is not None else frozenset(visuals.MODES)
        mode = self.shown(st.elapsed)
        self._title(mode, self._demo.scene if mode == "demo" else "")
        rows, width = self.content_size.height, self.content_size.width
        if rows <= 0 or width <= 0:
            return
        if mode == "spectrum":
            draw_if_changed(self, self._spectrum.render(st.wave, rows, width))
        elif mode == "scope":
            draw_if_changed(self, self._scope.render(st.wave, rows, width))
        elif mode == "channels":
            draw_if_changed(self, self._channels.render(st.channels, st.levels, rows, width))
        elif mode == "pattern":
            draw_if_changed(self, visuals.pattern(st.pattern, rows))
        elif mode == "bars":
            draw_if_changed(self, self._bars.render(st.wave, rows, width))
        elif mode == "mirror":
            draw_if_changed(self, self._mirror.render(st.wave, rows, width))
        elif mode == "spectrogram":
            if st.state == "playing":       # it scrolls on each frame: paused, it stands still
                draw_if_changed(self, self._spectrogram.render(st.wave, rows, width))
        elif mode == "scopes":
            draw_if_changed(self, self._scopes.render(st.waves, st.channels, rows, width))
        elif mode == "demo":
            # paused, it stands still: it went on at 95 KB/s (11 October 2026)
            if st.state == "playing" or getattr(self, "_drawn_mode", None) != "demo":
                draw_if_changed(self, self._demo.render(st.wave, _scroll_text(st), rows, width))
        self._drawn_mode = mode


def _scroll_text(st: Status) -> str:
    """What the demo's sine scroller says: the song, its author and its format."""
    info, song = st.info, st.song
    title = (info.title if info and info.title else "") or (song.title if song else "")
    author = (info.author if info and info.author else "") or (song.artist if song else "")
    fmt = (info.format if info and info.format else "") or (song.format if song else "")
    parts = [title + (f" by {author}" if author else ""), fmt]
    return "   ·   ".join(p for p in parts if p)


class Note(Static):
    """Text in a box of a fixed height (CSS), such as the note under Browse's lists.
    Its height doesn't follow the text: one that did resized the lists beside it, and
    they were drawn again from nothing at every format moved to (35% of the drawing,
    2 October 2026). What doesn't fit ends in "…" instead of being cut off unseen."""

    def __init__(self, **kw):
        super().__init__(**kw)
        self._full = Text()
        self._shown: tuple | None = None

    def show(self, text: Text) -> None:
        self._full = text
        self._fit()

    def on_resize(self) -> None:
        self._fit()

    def _fit(self) -> None:
        width, height = self.content_size.width, self.content_size.height
        shown = (self._full, width, height)
        if shown == self._shown:
            return
        self._shown = shown
        text = self._full
        if width > 0 and height > 0:
            lines = text.wrap(self.app.console, width)
            if len(lines) > height:
                last = lines[height - 1]
                last.rstrip()
                if last.cell_len > width - 1:
                    last.truncate(width - 1)
                last.append("…")
                text = Text("\n").join(lines[:height])
        draw_if_changed(self, text)


class KeysFooter(Footer):
    """Textual's Footer is made anew (every key taken away and put back, and the screen
    laid out) whenever the screen's keys may have changed: twice at each move of the
    focus, ~17 ms a time (2 October 2026). This one is made anew only when the keys
    it shows have changed."""

    _keys_shown: tuple | None = None

    def bindings_changed(self, screen) -> None:
        self._bindings_ready = True
        if not screen.app.app_focus:
            return
        if self.is_attached and screen is self.screen:
            self.call_after_refresh(self._recompose_if_changed)

    def _shown_keys(self) -> tuple:
        """What Footer.compose would show, as a value to compare."""
        app = self.app
        active = self.screen.active_bindings
        keys = [(b.key, app.get_key_display(b), b.description, b.action, enabled, tooltip,
                 (b.group.description, b.group.compact) if b.group else None)
                for _, b, enabled, tooltip in active.values() if b.show]
        palette = None
        if self.show_command_palette and app.ENABLE_COMMAND_PALETTE:
            if (entry := active.get(app.COMMAND_PALETTE_BINDING)) is not None:
                _, b, enabled, tooltip = entry
                palette = (b.key, app.get_key_display(b), b.description, enabled, tooltip)
        return tuple(keys), palette, self.combine_groups

    async def _recompose_if_changed(self) -> None:
        if not self.is_attached:
            return
        keys = self._shown_keys()
        if keys == self._keys_shown:
            return
        self._keys_shown = keys
        await self.recompose()


class FilesTree(DirectoryTree):
    """The music folder on this computer: folders, and the files a replayer knows.
    → opens a folder (on an open one, goes to what is in it), ← closes it (on a file or
    a closed folder, goes to the folder it is in); Enter opens and closes, and plays."""

    BINDINGS = [Binding("right", "open_or_in", show=False, id="open_copies"),
                Binding("left,escape", "close_or_out", show=False, id="close_copies"), *PAGES]

    accept = staticmethod(lambda path: True)   # set by the app once the catalog is open

    def action_open_or_in(self) -> None:
        node = self.cursor_node
        if node is None or not node.allow_expand:
            return
        if not node.is_expanded:
            node.expand()
        elif node.children:
            self.move_cursor(node.children[0])

    def action_close_or_out(self) -> None:
        node = self.cursor_node
        if node is None:
            return
        if node.allow_expand and node.is_expanded and node is not self.root:
            node.collapse()
        elif node.parent is not None and node.parent is not self.root:
            self.move_cursor(node.parent)
        elif node.parent is self.root:
            self.move_cursor(self.root)

    def filter_paths(self, paths_):
        out = []
        for p in paths_:
            if p.name.startswith("."):
                continue
            try:
                if p.is_dir() or self.accept(p):
                    out.append(p)
            except OSError:
                pass
        return out
