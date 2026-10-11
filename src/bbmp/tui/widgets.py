# SPDX-License-Identifier: GPL-3.0-or-later
"""The lists, the Now playing box, the visualiser and the files tree."""

from __future__ import annotations

from rich.cells import cell_len
from rich.text import Text
from textual.binding import Binding
from textual.widget import Widget
from textual.widgets import DataTable, DirectoryTree, Footer, Input, Static, Tabs
from textual.geometry import Size
from textual.widgets import OptionList
from textual.widgets.data_table import ColumnKey

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
        for index, option in enumerate(self.options[start:], start):
            cache.index_to_line[index] = len(lines)
            height = 1 + option._divider
            cache.heights[index] = height
            lines.extend((index, n) for n in range(height))
        last_divider = self.options and self.options[-1]._divider
        size = Size(width, len(lines) - (1 if last_divider else 0))
        if size != self.virtual_size:
            self.virtual_size = size
            self._scroll_update(size)


class SongTable(DataTable):
    """A list of songs. Enter plays from the selected row onwards."""

    # → and ← open and close a song's copies, and ← goes back out of Browse's songs
    BINDINGS = IN_AND_OUT + PAGES

    COLUMNS = {"title": "Title", "artist": "Artist", "album": "Album", "format": "Format",
               "size": "Size"}
    # With more than one collection in the catalog, the app puts their names here
    # (by id) and every song says which one it is from.
    collections: dict[str, str] = {}
    kept = staticmethod(lambda key: False)      # is the song kept on this computer?
    troubled = staticmethod(lambda key: False)  # is it known not to play?
    # the same song in the other collections that have it (Song.also), from the catalog
    copies = staticmethod(lambda song: [])

    def __init__(self, *, columns: tuple[str, ...] = ("title", "artist", "format"), **kw):
        super().__init__(cursor_type="row", zebra_stripes=True, **kw)
        self.songs: list[Song] = []        # as shown: a row that is opened has its copies
        self._depth: list[int] = []        # 0 for a song, 1 for a copy of the one above it
        self._base: list[Song] = []        # as given
        self._open: set[str] = set()       # the keys of the rows opened out
        self._shown_with: tuple = (set(), -1, None)
        self._columns = columns

    def on_mount(self) -> None:
        self.add_column(" ", width=1, key="mark")
        for c in self._columns:
            # a width of its own (set at each fill, _draw): DataTable measures no cells then
            self.add_column(Text(self.COLUMNS[c], justify="right") if c == "size"
                            else self.COLUMNS[c], key=c, width=len(self.COLUMNS[c]))

    # Every column has a width that _draw works out from the strings it shows: Textual's
    # DataTable would otherwise measure every cell of every new row with Rich on the next
    # idle (2,000 rows: 34 ms), and the whole column again when a cell gets narrower (the
    # mark when a favourite is taken away). Rows are one line high, without labels.

    def _fixed(self) -> bool:
        return not any(c.auto_width for c in self.columns.values())

    def _update_dimensions(self, new_rows) -> None:
        super()._update_dimensions(() if self._fixed() else new_rows)

    def _update_column_widths(self, updated_cells) -> None:
        if not self._fixed():
            super()._update_column_widths(updated_cells)
            return
        self._require_update_dimensions = True
        self.hover_coordinate = self.hover_coordinate
        self._update_count += 1
        self.refresh(layout=True)

    def set_songs(self, songs: list[Song], favorites: set[str], current: int = -1,
                  albums: dict[str, str] | None = None) -> None:
        """`albums` has, by a song's key, what is shown in place of its album."""
        if songs != self._base:
            self._open &= {s.key for s in songs}
        self._base = list(songs)
        self._shown_with = (favorites, current, albums)
        rows: list[tuple[Song, int]] = []
        shown = -1              # the row of `current`, which counts the songs as given
        for i, s in enumerate(songs):
            if i == current:
                shown = len(rows)
            rows.append((s, 0))
            if s.key in self._open:
                rows += [(c, 1) for c in self.copies(s)]
        self._draw([s for s, _ in rows], [d for _, d in rows], favorites, shown, albums)

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
        keep = self.cursor_row if self.songs == songs else 0
        self.songs = songs
        self._depth = depth
        self.clear()
        longest = LONGEST
        if "size" in self._columns and self.size.width:
            longest = max(16, min(LONGEST, self.size.width - 20))   # the size stays in sight
        # as Textual's own measuring did: a column grows to what it has shown, and
        # doesn't shrink back, so that the columns stay put from one list to the next
        widths = {c: self.columns[ColumnKey(c)].width for c in self._columns}
        kept = self.kept
        for i, s in enumerate(songs):
            key = s.key
            ok = is_supported(s.format)
            bad = ok and self.troubled(key)
            ok = ok and not bad
            mark = ("▶" if i == current else "♥" if key in favorites
                    else "!" if bad else "" if not ok else "⤓" if kept(key) else "")
            mark = mark or ("" if ok else "✗")
            album = s.album_name + (f" (with {s.partner})" if s.partner else "")
            where = self.collections.get(s.collection, "") or (
                collections_.name(s.collection) if depth[i] else "")
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
            if depth[i]:
                title = f"  ↳ {title}"          # a copy, in the collection it says
            elif s.also:
                title = ("▾ " if key in self._open else "▸ ") + title
            cells = {"title": title, "artist": s.artist, "album": album.strip(),
                     "format": s.format + (f" · {where}" if where else ""),
                     "size": units.size(s.size)}
            style = {"♥": f"bold {theme.current.bad}", "!": f"bold {theme.current.warn}"}
            row = []
            for c in self._columns:
                text = cells[c]
                if (n := cell_len(text)) > widths[c]:
                    widths[c] = n
                row.append(Text(text, style="" if ok else "dim",
                                justify="right" if c == "size" else "left"))
            self.add_row(Text(mark, style=style.get(mark, "")), *row, key=str(i))
        for c, width in widths.items():
            self.columns[ColumnKey(c)].width = width
        if songs:
            self.move_cursor(row=min(keep, len(songs) - 1))

    def remark_kept(self) -> None:
        """Files have come to the kept music: the ⤓ marks again, where a row has that
        mark or none (not ▶, ♥, ! or ✗). The list was not drawn again when a saved
        file landed, and its ⤓ was missing until one went away and back (7 October 2026)."""
        kept = self.kept
        for i, s in enumerate(self.songs):
            try:
                now = str(self.get_cell_at((i, 0)))
            except Exception:
                return          # the rows are being drawn anew
            if now not in ("", "⤓") or not is_supported(s.format) or self.troubled(s.key):
                continue
            want = "⤓" if kept(s.key) else ""
            if want != now:
                self.update_cell_at((i, 0), Text(want))

    def selected(self) -> Song | None:
        if self.songs and 0 <= self.cursor_row < len(self.songs):
            return self.songs[self.cursor_row]
        return None


def _drawn_key(content):
    """What a Static shows, as something to compare: a Text by its letters and styles (a
    renderer may hand back the same Text changed)."""
    if isinstance(content, Text):
        return (content.plain, tuple(content.spans), content.style)
    return content


def draw_if_changed(widget: Static, content) -> None:
    """Update a Static twelve times a second only when what it shows changed: an unchanged
    visualiser and Now playing were sent to the terminal again and again (85 KB a second
    with nothing playing, 11 October 2026), which a slow terminal (macOS's) spends its time
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

    def show(self, st: Status, favorite: bool) -> None:
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
            draw_if_changed(self, self._scopes.render(st.channels, st.pattern, rows, width))
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
