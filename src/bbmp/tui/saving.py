# SPDX-License-Identifier: GPL-3.0-or-later
"""Saving music on this computer (keep.py): what, where, and fetching it. A part of the app."""

from __future__ import annotations

import os
import shutil
import time
from pathlib import Path

from textual import work
from textual.app import App
from textual.css.query import NoMatches
from textual.widgets import OptionList, TabbedContent

from .. import collection as collections_, paths, units
from ..keep import Busy as KeepBusy, Keeper, Scope

from .dialogs import Ask, Confirm, Pick
from .guard import guarded
from .widgets import NEW, FilesTree, SongTable


NIGHT_BEGINS, NIGHT_ENDS = 0, 7      # "at night" is from midnight to seven
CHECK_EVERY = 3600      # seconds between looks at whether the kept files are all there


class SavingPart(metaclass=type(App)):
    """Saving music on this computer (keep.py): what, where, and fetching it. A part of ModlandApp."""

    def _open_keeper(self) -> None:
        if self.keeper is not None:
            self.keeper.close()
        self.keeper = Keeper(self.index, self.fetcher, library=self.library)
        self._kept = self.keeper.scopes()
        self.index.kept_dir = self.keeper.folder     # its files are the collections' songs
        self._kept_seen()
        SongTable.kept = staticmethod(self._song_is_kept)

    def _song_is_kept(self, key: str) -> bool:
        """Is the song's file in the kept music? Asked for every row of a list of songs:
        a look at the disk for each was 29% of filling a list (2 October 2026). With
        nothing kept there is nothing to look for; else each folder is read once, and
        read again once what is kept has changed (_kept_seen)."""
        if not self._kept:
            return False
        known = self.__dict__.setdefault("_kept_keys", {})
        if (kept := known.get(key)) is not None:
            return kept
        collection, path = collections_.split(key)
        if collection == collections_.LOCAL:
            return False
        # where Fetcher._places puts it, as a string: a Path for each row cost as
        # much as the look at the disk did
        folder, _, name = os.path.join(str(self.fetcher.kept_dir), collections_.folder(collection),
                                       path).rpartition("/")
        folders = self.__dict__.setdefault("_kept_folders", {})
        names = folders.get(folder)
        if names is None:
            try:
                names = frozenset(os.listdir(folder))
            except OSError:
                names = frozenset()
            if len(folders) >= 4000:
                folders.clear()
                known.clear()
            folders[folder] = names
        known[key] = kept = name in names
        return kept

    def _kept_seen(self) -> None:
        """Files have come to or gone from the kept music: look at its folders again."""
        self.__dict__.pop("_kept_folders", None)
        self.__dict__.pop("_kept_keys", None)

    def _shown_collections(self, all_: bool = False) -> list[str]:
        """The collections whose songs are on show: in a view, the one the format on
        show is listed under (`all_`: every collection the lists have)."""
        if self._fmt_in and not all_:
            return [self._fmt_in]
        return [self._collection] if self._collection else [c for c, _ in self._own]

    def _is_kept(self, fmt: str = "", artist: str = "", collection: str = "") -> bool:
        return any(kept.holds(Scope(c, fmt, artist))
                   for c in ([collection] if collection else self._shown_collections())
                   for kept in self._kept)

    def _scope_name(self, scope: Scope) -> str:
        """As Scope.name, without the collection where there is only the one."""
        name = scope.name
        if len(self._own) == 1 and scope.format:
            name = name.removesuffix(f" on {collections_.name(scope.collection)}")
        if scope.path:
            return f"The song {name}"
        return name[0].upper() + name[1:]

    def _keep_choices(self) -> list[tuple[Scope, int]]:
        """What can be kept from where the user is: the artist, the format, the
        collection, each with its bytes."""
        tabs = self.query_one("#tabs", TabbedContent)
        fmt = artist = ""
        collections = self._shown_collections()
        song = self._target_song() if self._focused_table() is not None else None
        if song is not None and song.local:
            song = None
        if song is not None:
            fmt, artist, collections = song.format, song.artist, [song.collection]
        elif tabs.active == "browse" and self._fmt and self._fmt != NEW:
            fmt, artist = self._fmt, self._artist or ""
        elif (playing := self.player.status().song) is not None and not playing.local:
            song = playing
            fmt, artist, collections = song.format, song.artist, [song.collection]
        out = []
        if tabs.active == "favorites":
            # the playlist on show, or the favorites, as they are and will be
            which = "favorites" if self._list is None else f"playlist:{self._list}"
            scope = Scope.playlist(which)
            out.append((scope, self.keeper.size_of(scope)[1]))
        if song is not None:
            out.append((Scope.of(song), self.index.size_with_companions(song)))
            if song.album:
                album = Scope.album_of(song)
                out.append((album, sum(f.size for f in self.keeper.wanted(album))))
        for c in collections:
            if fmt and artist and (size := self.index.artist_sizes(fmt, c).get(artist)):
                out.append((Scope(c, fmt, artist), size))
        for c in collections:
            if fmt and (size := self.index.format_sizes(c).get(fmt)):
                out.append((Scope(c, fmt), size))
        for c in collections:
            out.append((Scope(c), self.index.about(c).get("bytes") or 0))
        return out

    def action_keep(self) -> None:
        if self.keeper is None:
            return
        if isinstance(self.focused, (SongTable, FilesTree)):
            song = self._target_song()
            if song is not None and song.local:
                self.notify("That file is on this computer already.")
                return
        choices = self._keep_choices()
        rows = []
        for i, (scope, size) in enumerate(choices):
            inside = next((k for k in self._kept if k.holds(scope)), None)
            name = self._scope_name(scope)
            if inside == scope:
                label = f"✓ {name}  ·  {units.size(size)}  ·  saved"
            elif inside is not None:
                label = f"✓ {name}  ·  {units.size(size)}  ·  saved with {self._scope_name(inside)}"
            else:
                label = f"  {name}  ·  {units.size(size)}"
            rows.append((str(i), label))
        if self._kept or self._keeping:
            rows.append(("activity", "  What is saved, and how far fetching has come... (t)"))

        def chosen(id_: str | None) -> None:
            if id_ == "activity":
                self.action_activity()
            elif id_ is not None:
                scope, size = choices[int(id_)]
                inside = next((k for k in self._kept if k.holds(scope)), None)
                if inside == scope:
                    self._ask_drop(scope)
                elif inside is not None:
                    self.notify(f"It is saved with {inside.name}.")
                else:
                    self._keep(scope)
        self.push_screen(Pick(
            "Save on this computer, to play without the servers\n"
            + ("Enter on what is kept (✓) stops keeping it\n" if self._kept else "")
            + f"Free on the disk: {units.size(self.keeper.free())}  ({self.keeper.folder})",
            rows), chosen)

    @work(thread=True, group="keep-ask")
    @guarded("saving music on this computer")
    def _keep(self, scope: Scope) -> None:
        keeper = self.keeper
        fits, need, free = keeper.room(keeper.plan([scope]))
        if not fits:
            self.call_from_thread(
                self.notify, f"There is not room for {scope.name}: {units.size(need)} are "
                f"to be fetched, and the disk has {units.size(free) or 'nothing'} free.",
                severity="error", timeout=20)
            return
        keeper.add(scope)
        self.call_from_thread(self._keep_changed)
        self.call_from_thread(
            self.notify, f"Keeping {scope.name}: {units.size(need) or 'nothing'} to fetch. "
            + ("You can go on listening meanwhile." if self._keep_allowed()
               else self._keep_wait_text()), timeout=10)
        self.call_from_thread(self._keep_fetch)

    def _keep_changed(self) -> None:
        """What is kept has changed: show it."""
        self._kept = self.keeper.scopes()
        self._kept_seen()
        self._fill_formats()                # the lists show what is kept: again, where one is
        self._fill_settings()

    def _keep_allowed(self) -> bool:
        """May saved music be fetched now? At any time, or at night (00-07), as
        Settings says."""
        if self.settings.keep_when != "night":
            return True
        return NIGHT_BEGINS <= time.localtime().tm_hour < NIGHT_ENDS

    @staticmethod
    def _keep_wait_text() -> str:
        return f"Waiting for the night: saving starts at {NIGHT_BEGINS:02d}:00."

    def _keep_later(self) -> None:
        """Every ten minutes: fetch what is saved and not there, where it may be now
        (the night has come, a saved playlist has changed)."""
        if self._kept and not self._keeping and self._keep_allowed():
            self._keep_fetch()

    def _lists_changed(self) -> None:
        """A playlist or the favorites changed: a saved one follows."""
        if any(s.list for s in self._kept):
            self._keep_fetch()

    def _keep_fetch(self, asked: bool = False) -> None:
        """Fetch what is kept and not there, behind what the user does. `asked`: the
        user asked for it, and is told when it waits for the night."""
        if not self._keep_allowed():
            if asked:
                self.notify(self._keep_wait_text(), timeout=10)
            return          # the night comes (_keep_later)
        if self._keeping:
            self._keep_more = True      # the one at work goes on with it
            return
        self._keeping = True
        self._keep_stop.clear()
        self._keep_work()

    @work(thread=True, group="keep")
    def _keep_work(self) -> None:
        keeper, result = self.keeper, None
        try:
            if time.monotonic() - getattr(self, "_keep_checked", -CHECK_EVERY) >= CHECK_EVERY:
                keeper.check()      # files deleted by hand are fetched again
                self._keep_checked = time.monotonic()
            while True:
                self._keep_more = False
                plan = keeper.plan()
                if plan.done:
                    break
                result = keeper.run(plan, lambda p: self.call_from_thread(self._keep_said, p),
                                    self._keep_stop, speed=self.settings.keep_speed * 1024,
                                    allowed=self._keep_allowed)
                if not self._keep_more or self._keep_stop.is_set():
                    break
        except KeepBusy:
            result = None       # another player has it in hand
        except Exception as e:
            self.call_from_thread(self.notify, f"Kept music: {e}", severity="error", timeout=20)
        finally:
            self._keeping = False
        if not self._closing:
            self.call_from_thread(self._keep_done, result)

    def _remark_kept(self) -> None:
        """The song lists' ⤓ marks again, now that files have come."""
        self._kept_seen()
        for table in self.query(SongTable):
            table.remark_kept()

    def _keep_said(self, p) -> None:
        self._keep_progress = p
        self._kept_seen()
        # files came: the lists show it, once a second at the most
        if p.done != getattr(self, "_keep_marked", None) and \
                time.monotonic() - getattr(self, "_keep_marked_at", 0.0) >= 1.0:
            self._keep_marked, self._keep_marked_at = p.done, time.monotonic()
            self._remark_kept()
        try:
            self.query_one("#now").border_subtitle = f"Saving: {p.line}  (t)"
        except NoMatches:
            pass

    def _keep_done(self, result) -> None:
        self._remark_kept()
        if result is not None:
            self._keep_progress = result
        try:
            self.query_one("#now").border_subtitle = ""
        except NoMatches:
            return
        self._fill_settings()
        if result is None:
            return
        if result.waiting:
            self.notify(f"Kept music: {self._keep_wait_text()}", timeout=10)
        elif result.stopped:
            self.notify(f"Kept music: {result.stopped}", timeout=20,
                        severity="warning" if result.stopped.startswith("Stopped") else "error")
        elif result.done or result.moved or result.removed or result.failed:
            parts = [f"{result.done:,} files fetched ({units.size(result.done_bytes)})"
                     if result.done else "",
                     f"{result.moved:,} moved with the collection" if result.moved else "",
                     f"{result.removed:,} taken away that nothing kept wants any more"
                     if result.removed else "",
                     f"{len(result.failed):,} could not be fetched and are tried the next time"
                     if result.failed else ""]
            self.notify("Kept music: " + ", ".join(x for x in parts if x) + ".", timeout=15)

    def _kept_summary(self) -> str:
        if not self._kept:
            return "nothing"
        size = units.size(self.keeper.size()) or "nothing there yet"
        return f"{len(self._kept)} saved, {size}" + (", fetching" if self._keeping else "")

    def _ask_drop(self, scope: Scope) -> None:
        def answered(yes: bool | None) -> None:
            if yes:
                self._drop(scope)
        self.push_screen(Confirm(
            f"Stop keeping {self._scope_name(scope)}?\n\nIts files are taken away from this computer, "
            "but for those\nthat something else kept has too. The songs play from the "
            "servers\nas before."), answered)

    @work(thread=True, group="keep-ask")
    @guarded("taking saved music away")
    def _drop(self, scope: Scope) -> None:
        freed = self.keeper.drop(scope)
        self.call_from_thread(self._keep_changed)
        self.call_from_thread(self.notify, f"{scope.name} is no longer kept. "
                                           f"{units.size(freed) or 'Nothing'} freed.")

    def _pick_keep_dir(self) -> None:
        """Where kept music is: in the music folder, in the player's own, or anywhere."""
        if paths.env("KEPT"):
            self.notify(f"The folder is {self.keeper.folder}, as BBMP_KEPT says. Without "
                        "that, it is chosen here.", timeout=15)
            return
        now = self.keeper.folder.absolute()
        offered = [(paths.KEPT_DIR, "in your music folder"),
                   (paths.OWN_KEPT_DIR, "the player's own folder, out of sight")]
        rows = [(str(folder), f"{units.folder(folder)}  ·  {what}")
                for folder, what in offered if folder.absolute() != now]
        rows.append(("other", "Another folder..."))

        def chosen(id_: str | None) -> None:
            if id_ == "other":
                self.push_screen(Ask("The folder for kept music (a disk with room for it)",
                                     units.folder(now), replace=False), named)
            elif id_:
                named(id_)

        def named(name: str | None) -> None:
            if not name:
                return
            folder = Path(name).expanduser().absolute()
            if folder == now:
                return
            if Keeper.is_in(folder):
                self._use_keep_dir(folder, move=False)
            elif self._kept:
                def answered(yes: bool | None) -> None:
                    if yes:
                        self._use_keep_dir(folder, move=True)
                self.push_screen(Confirm(
                    f"Move the kept music ({units.size(self.keeper.size()) or 'nothing yet'}) to\n"
                    f"{units.folder(folder)}?\n\nOn the same disk that takes a moment. To "
                    "another disk it is copied,\nwhich takes its time. You can go on "
                    "listening meanwhile."), answered)
            else:
                self._use_keep_dir(folder, move=True)
        self.push_screen(Pick(f"Kept music is in {units.folder(now)}\n"
                              f"Free on its disk: {units.size(self.keeper.free())}", rows), chosen)

    @work(thread=True, exclusive=True, group="keep-move")
    def _use_keep_dir(self, folder: Path, move: bool) -> None:
        """Have the kept music in `folder` from now on: by moving what is kept there,
        or, for a folder that has kept music already, by taking that as it is."""
        was, moved = self.keeper.folder, 0
        self._keep_stop.set()               # what is being fetched goes on in the new place
        for _ in range(600):
            if not self._keeping:
                break
            time.sleep(0.1)
        try:
            if move:
                if self._kept:
                    self.call_from_thread(self.notify, "Moving the kept music...")
                moved = self.keeper.move_to(folder)
            else:
                folder.mkdir(parents=True, exist_ok=True)
        except (OSError, shutil.Error) as e:
            self.call_from_thread(
                self.notify, f"The kept music stays where it is: {e}.", severity="error",
                timeout=20)
            self.call_from_thread(self._keep_fetch)
            return
        # the files are there now, and the setting says so in one go; none where it
        # is the folder it would be without one
        self.fetcher.kept_dir = folder
        self.settings.update(keep_dir="" if paths.kept_dir() == folder else str(folder))
        self.call_from_thread(self._keep_dir_changed, was, moved, move)

    def _keep_dir_changed(self, was: Path, moved: int, move: bool) -> None:
        self._open_keeper()
        self._keep_changed()
        here = units.folder(self.keeper.folder)
        if not move:
            self.notify(f"{here} has kept music already, and that is what is kept now. "
                        f"What was kept in {units.folder(was)} is still there.", timeout=20)
        elif moved:
            self.notify(f"The kept music ({units.size(moved)}) is in {here} now.", timeout=10)
        else:
            self.notify(f"Kept music goes to {here}.", timeout=10)
        if self._kept:
            self._keep_fetch()
