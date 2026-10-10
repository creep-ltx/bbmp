# SPDX-License-Identifier: GPL-3.0-or-later
"""The Artist and Album pages, and the Info tab. A part of the app."""

from __future__ import annotations

import subprocess

from rich.text import Text
from textual import on
from textual.app import App
from textual.widgets import OptionList, Static, TabbedContent
from textual.widgets.option_list import Option

from .. import collection as collections_, pages
from ..index import Song

from .widgets import SongTable


class ArtistPart(metaclass=type(App)):
    """The Artist and Album pages, and the Info tab. A part of ModlandApp."""

    def action_artist(self, name: str | None = None) -> None:
        song = self._target_song()
        name = name or (song.artist if song else None)
        if not name:
            return
        page = pages.artist_page(self.index, self.library, self._favs, name)
        if page is None and song is not None and song.partner:
            page = pages.artist_page(self.index, self.library, self._favs, song.partner)
        if page is None:
            self.notify(f"{collections_.name(song.collection)} doesn't name an artist for this song."
                        " Press A for its album or game."
                        if song and not song.local else "Files on this computer have no artist page.")
            return
        self._open_page(page, select=song)

    def action_album(self) -> None:
        song = self._target_song()
        if song is None:
            return
        page = pages.album_page(self.index, song)
        if page is None:
            self.notify("This song isn't filed under an album or game.")
            return
        self._open_page(page, select=song)

    def _open_page(self, page: pages.Page, select: Song | None = None) -> None:
        self._page = page
        self.query_one("#artist-facts", Static).update(page.facts)
        self.query_one("#artist-scroll").border_title = page.title
        self.query_one("#artist-scroll").scroll_home(animate=False)
        partners = self.query_one("#partners", OptionList)
        partners.clear_options()
        partners.add_options([Option(Text.assemble(name, (f"\u00a0\u00a0{n}", "dim")), id=name)
                              for name, n in page.partners])
        partners.display = bool(page.partners)
        if page.partners:
            partners.highlighted = 0
        table = self.query_one("#artist-songs", SongTable)
        table.set_songs(page.songs, self._favs)
        table.border_title = f"{page.title} ({len(page.songs):,})"
        if select is not None:
            row = next((i for i, s in enumerate(page.songs) if s.key == select.key), 0)
            table.move_cursor(row=row)
        self.query_one("#tabs", TabbedContent).active = "artist"
        table.focus()

    @on(OptionList.OptionSelected, "#partners")
    def _partner_selected(self, event: OptionList.OptionSelected) -> None:
        self.action_artist(event.option_id)

    def action_lookup(self, site: str) -> None:
        tabs = self.query_one("#tabs", TabbedContent)
        if tabs.active != "artist" or self._page is None or self._page.kind != "artist":
            return
        self._open_url(pages.lookup_url(site, self._page.title))
        where = "Demozoo" if site == "demozoo" else "the web"
        self.notify(f"Searching {where} for {self._page.title} in your browser")

    def _open_url(self, url: str) -> None:
        try:
            subprocess.Popen(["xdg-open", url], stdin=subprocess.DEVNULL,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            self.notify(f"Can't open a browser: {e}", severity="error")

    def _show_info(self) -> None:
        song, details, file = self.player.details()
        st = self.player.status()
        body = self.query_one("#info-body", Static)
        if song is None or details is None or st.info is None:
            if self._info_shown == "nothing":
                return              # asked twelve times a second: drawn once
            self._info_shown = "nothing"
            body.update(Text("Nothing is playing. Play a song to see what its file says.", style="dim"))
            return
        key = (song.key, st.info.subsong, id(details))
        if key == self._info_shown:
            return
        self._info_shown = key
        width = self.query_one("#info-scroll").size.width
        body.update(pages.info_page(song, details, file, st.backend,
                                    (st.info.subsong, st.info.subsongs), width,
                                    level=(st.loudness, st.level_db) if self.settings.level else None,
                                    kept=not song.local and self.fetcher.is_kept(song.key),
                                    trouble=self.player.trouble.get(song.key)
                                    if self.player.trouble else None,
                                    note="" if song.local else self.index.note(song)))
