# SPDX-License-Identifier: GPL-3.0-or-later
"""The Settings tab, with downloads, extra files and the cache. A part of the app."""

from __future__ import annotations

from rich.text import Text
from textual import on, work
from textual.app import App
from textual.css.query import NoMatches
from textual.widgets import OptionList, Static
from textual.widgets.option_list import Option

from .. import (catalogs, downloads, extras, paths,
                theme, units, visuals)
from ..settings import CACHE_LIMITS_MB

from .dialogs import Ask, Confirm, Pick
from .guard import guarded
from .widgets import REPEAT_LABELS


KEEP_SPEEDS = (0, 256, 512, 1024, 2048, 5120)     # KB a second; 0: no limit


class SettingsPart(metaclass=type(App)):
    """The Settings tab, with downloads, extra files and the cache. A part of ModlandApp."""

    def _downloads_summary(self) -> str:
        there = sum(1 for d in downloads.DOWNLOADS if downloads.installed(d))
        return f"{there} of {len(downloads.DOWNLOADS)} there"

    def _offer_downloads(self) -> None:
        """At the first start: what the player may fetch and doesn't come with."""
        wanted = [d for d in downloads.DOWNLOADS if not downloads.installed(d)]
        self.settings.update(asked_downloads=True)
        roms = ("A few other formats need a file that only you can add, such as a C64 ROM. "
                "Settings \u2192 Extra files lists them and tells you where to put each one.")
        if not wanted:
            self.notify(roms, timeout=15)
            return
        lines = "\n".join(f"  \u2022 {d.name} ({d.size / 1e6:.1f} MB), for {downloads.used_for(d)}"
                          for d in wanted)

        def answered(yes: bool | None) -> None:
            if yes:
                self._download([d.id for d in wanted])
            else:
                self.notify("You can download them any time in Settings \u2192 Downloads.",
                            timeout=10)
        self.push_screen(Confirm(
            "Some songs need one more file to play\n\n"
            "A few formats can only be played with a file from the program their songs were "
            "made with. The player can't include these files, because their makers don't "
            "allow it, but it can download them for you from the makers' own sites:\n\n"
            f"{lines}\n\n"
            "Download them now? You can also do it later in Settings \u2192 Downloads.\n\n"
            f"{roms}"), answered)

    def _show_downloads(self) -> None:
        rows = [(d.id, f"{'there    ' if downloads.installed(d) else 'not there'}  {d.name}"
                       f"  ({d.size / 1e6:.1f} MB)  for {downloads.used_for(d)}")
                for d in downloads.DOWNLOADS]

        def chosen(id_: str | None) -> None:
            if not id_:
                return
            d = downloads.get(id_)
            if downloads.installed(d):
                self.notify(f"{d.name} is there ({downloads.folder(d)}). It is {d.what}; "
                            f"{d.terms}.", timeout=30)
            else:
                self._download([id_])
        self.push_screen(Pick("Downloads: Enter fetches what is not there", rows), chosen)

    @work(thread=True, group="downloads")
    def _download(self, ids: list[str]) -> None:
        for id_ in ids:
            d = downloads.get(id_)
            self.call_from_thread(self.notify, f"Downloading {d.name}...")
            try:
                downloads.install(d)
            except (OSError, ValueError) as e:
                self.call_from_thread(self.notify, f"{d.name}: {e}", severity="error", timeout=20)
                continue
            self.call_from_thread(self.notify, f"{d.name} is there: {downloads.used_for(d)} "
                                               "can be played.")
        self.call_from_thread(self._downloads_changed)

    def _downloads_changed(self) -> None:
        self._fill_settings()
        self._fmt = self._fmt_id = None
        self._fill_formats()

    def _extras_summary(self) -> str:
        there = sum(1 for e in extras.EXTRAS if extras.state(e)[0] is not None)
        return f"{there} of {len(extras.EXTRAS)} there"

    def _show_extras(self) -> None:
        """What each extra file is, and whether it is there."""
        try:
            extras.folder().mkdir(parents=True, exist_ok=True)
        except OSError:
            pass
        rows = []
        for e in extras.EXTRAS:
            path, note = extras.state(e)
            mark = "there    " if path is not None else "not there"
            rows.append((e.id, f"{mark}  {e.name}  ({e.size:,} bytes)  for {', '.join(e.formats)}"))

        def chosen(id_: str | None) -> None:
            if id_:
                self.notify(extras.describe(extras.get(id_)) + " It is: "
                            + extras.get(id_).source + ".", timeout=30)
        self.push_screen(Pick(f"Extra files go into {extras.folder()}", rows), chosen)

    def _setting_rows(self) -> list[tuple[str, str, str | None]]:
        """(id, label, value) in groups; a group's heading has no id and no value."""
        s = self.settings
        limit = f"{s.cache_limit_mb / 1000:g} GB" if s.cache_limit_mb else "no limit"
        return [
            ("", "Playing", None),
            ("subsongs", "Songs with subsongs",
             "play them all, like an album" if s.subsongs == "all" else "play the first one only"),
            ("repeat", "Repeat", REPEAT_LABELS[s.repeat]),
            ("shuffle", "Shuffle the queue", "on" if s.shuffle else "off"),
            ("level", "Same loudness for all songs", "on" if s.level else "off"),
            ("resume", "Reopen where I left off", "yes" if s.resume else "no"),
            *([("collection", "Browse and search", self._collection_name() or "every collection")]
              if len(self._collections) > 1 else []),
            ("", "Collections", None),
            ("collections", "Collections I have", catalogs.summary(catalogs.chosen(s))),
            ("update_now", "Update the catalogs now", self._catalog_age()),
            ("update_every", "Update automatically",
             catalogs.EVERY.get(s.update_every, catalogs.EVERY["week"])[1]),
            ("mirror", "Home mirror", self._mirror_summary()),
            ("", "Looks", None),
            ("theme", "Theme", self.theme),
            ("visualizer", "Visualizer", visuals.TITLES.get(s.visualizer, "off")),
            ("demo_effects", "Demo effects with v", "on" if s.demo_effects else "off"),
            ("", "Saving and exporting", None),
            ("kept", "Saved on this computer", self._kept_summary()),
            ("keep_dir", "Folder for saved music", units.folder(self.keeper.folder)),
            ("keep_when", "Fetch saved music",
             "at night (00-07)" if s.keep_when == "night" else "at any time"),
            ("keep_speed", "Fetching speed",
             units.size(s.keep_speed * 1024) + "/s at the most" if s.keep_speed else "no limit"),
            ("export_format", "Export songs as", self._export_kind()),
            ("", "Files some formats need", None),
            ("downloads", "Downloads", self._downloads_summary()),
            ("extras", "Extra files", self._extras_summary()),
            ("", "Cache of played songs", None),
            ("cache_limit_mb", "Cache size limit", limit),
            ("clear_cache", "Clear the cache now", ""),
        ]

    def _mirror_summary(self) -> str:
        if paths.env("MIRROR") is not None:
            return (paths.env("MIRROR") or "none") + " (BBMP_MIRROR)"
        return self.settings.mirror or "none"

    def _ask_mirror(self) -> None:
        """The address of the user's own copy of the collections, asked first."""
        def chosen(value: str | None) -> None:
            if value is None:
                return
            self.settings.update(mirror=value)
            self.fetcher.close()        # the next song asks it (or the servers) anew
            self._fill_settings()
            if paths.env("MIRROR") is not None:
                self.notify("BBMP_MIRROR is set, and counts instead of this setting.")
        self.push_screen(Ask(
            "The address of your own copy of the collections (a home mirror), for example "
            "http://192.168.1.10:8088. It is asked before the collections' servers, and "
            "has each collection in a folder of its id (modland/pub/modules/..., asma/asma/..., "
            "sndh/sndh/sndh_lf/..., keygenmusic/kgm/...). Empty: none.",
            self.settings.mirror, empty=True), chosen)

    def _catalog_age(self) -> str:
        """How old the oldest catalog is, as shown."""
        days = self.index.age_days() if self.index is not None else 1e9
        if days > 1e8:
            return "no catalog yet"
        if days < 1:
            return "they were updated today"
        return f"the oldest is {days:.0f} day{'s' if round(days) != 1 else ''} old"

    def _fill_settings(self) -> None:
        try:
            ol = self.query_one("#settings-list", OptionList)
        except NoMatches:
            return
        at = ol.highlighted
        keep = ol.get_option_at_index(at).id if at is not None and at < ol.option_count else None
        # built first and put in at once: the list's height follows its options, and
        # each one added by itself had it measured and laid out again
        options: list[Option | None] = []
        for key, label, value in self._setting_rows():
            if value is None:           # a group's heading
                if options:
                    options.append(None)
                options.append(Option(Text(label, style=f"bold {theme.current.accent}"),
                                      disabled=True))
            else:
                options.append(Option(Text.assemble(
                    (f"  {label:<28}", ""), (value, f"bold {theme.current.primary}")), id=key))
        ol.clear_options()
        ol.add_options(options)
        ids = [ol.get_option_at_index(i).id for i in range(ol.option_count)]
        at = (ids.index(keep) if keep and keep in ids
              else next(i for i, key in enumerate(ids) if key))
        ol.highlighted = at
        self._settings_note()

    @work(thread=True, exclusive=True, group="cache-size")
    @guarded("measuring the cache")
    def _settings_note(self) -> None:
        size = self.fetcher.cache_size() / 1e6
        text = (f"Cache: {size:,.0f} MB in {paths.FILES_DIR}\n"
                f"Settings save as soon as you change them ({paths.SETTINGS}).\n"
                f"The Files tab shows {self._music_dir()} (music_dir in that file).\n"
                "Enter changes the selected setting. Favorites and the queue are never "
                "removed from the cache, and kept music is not in it.")
        self.call_from_thread(self._set_note, text)

    def _set_note(self, text: str) -> None:
        try:
            self.query_one("#settings-note", Static).update(text)
        except NoMatches:
            pass

    @on(OptionList.OptionHighlighted, "#settings-list")
    def _setting_highlighted(self, event: OptionList.OptionHighlighted) -> None:
        if event.option_id is None:     # a group's heading: the setting under it
            event.option_list.action_cursor_down()

    @on(OptionList.OptionSelected, "#settings-list")
    def _setting_selected(self, event: OptionList.OptionSelected) -> None:
        s, key = self.settings, event.option_id
        if key == "subsongs":
            s.update(subsongs="one" if s.subsongs == "all" else "all")
        elif key == "repeat":
            self.action_cycle_repeat()
            return
        elif key == "shuffle":
            self.action_toggle_shuffle()
            return
        elif key == "level":
            s.update(level=not s.level)
            self.notify("Songs play at the same loudness from the next song on." if s.level
                        else "Songs play as loud as they are from the next song on.")
        elif key == "resume":
            s.update(resume=not s.resume)
        elif key == "visualizer":
            self.action_visual()
        elif key == "demo_effects":
            s.update(demo_effects=not s.demo_effects)
            self.query_one("#visual").demo = s.demo_effects
            self.notify("v shows the demo effects too: copper bars, starfield, plasma, a scroller."
                        if s.demo_effects else "v leaves the demo effects out.")
        elif key == "theme":
            self._pick_theme()
            return
        elif key == "collection":
            self.action_collection()
            return
        elif key == "downloads":
            self._show_downloads()
            return
        elif key == "extras":
            self._show_extras()
            return
        elif key == "keep_when":
            s.update(keep_when="any" if s.keep_when == "night" else "night")
            self._keep_later()
        elif key == "keep_speed":
            speeds = KEEP_SPEEDS
            i = speeds.index(s.keep_speed) if s.keep_speed in speeds else -1
            s.update(keep_speed=speeds[(i + 1) % len(speeds)])
        elif key == "collections":
            self._ask_collections()
            return
        elif key == "update_now":
            self.action_update_catalog()
            return
        elif key == "update_every":
            order = list(catalogs.EVERY)
            now = s.update_every if s.update_every in order else "week"
            s.update(update_every=order[(order.index(now) + 1) % len(order)])
        elif key == "kept":
            self.action_activity()
            return
        elif key == "keep_dir":
            self._pick_keep_dir()
            return
        elif key == "mirror":
            self._ask_mirror()
            return
        elif key == "export_format":
            self._pick_export_kind()
            return
        elif key == "cache_limit_mb":
            limits = CACHE_LIMITS_MB
            i = limits.index(s.cache_limit_mb) if s.cache_limit_mb in limits else -1
            s.update(cache_limit_mb=limits[(i + 1) % len(limits)])
            self._trim_cache()
        elif key == "clear_cache":
            self._clear_cache()
        self._fill_settings()

    @work(thread=True, exclusive=True, group="cache-trim")
    @guarded("making the cache smaller")
    def _trim_cache(self) -> None:
        keep = self._protected()
        freed = self.fetcher.trim(self.settings.cache_limit_mb * 1_000_000, keep)
        if freed:
            self.call_from_thread(self.notify, f"Removed {freed / 1e6:,.0f} MB of old songs from the cache")
        self.call_from_thread(self._settings_note)

    @work(thread=True, exclusive=True, group="cache-trim")
    @guarded("clearing the cache")
    def _clear_cache(self) -> None:
        freed = self.fetcher.trim(1, self._protected())
        self.call_from_thread(self.notify, f"Cleared {freed / 1e6:,.0f} MB (kept favorites and the queue)")
        self.call_from_thread(self._settings_note)

    def _protected(self) -> set[str]:
        songs, _ = self.player.queue()
        keep = self.library.protected_keys() | {s.key for s in songs if not s.local}
        for p in list(keep):
            keep.update(self.index.companions(p))
        return keep
