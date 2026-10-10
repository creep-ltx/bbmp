# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive the TUI with Textual's test pilot against the small test catalog."""

import asyncio

from conftest import SONGS
from bbmp.audio import NullOutput
from bbmp.library import Library
from bbmp.settings import Settings
from bbmp.tui import ModlandApp


def make_app(catalog, fetcher, tmp_path, settings=None, library=None):
    """An app that keeps its settings, library, session and exports inside tmp_path."""
    app = ModlandApp(catalog, fetcher, library or Library(tmp_path / "user.db"),
                     NullOutput(realtime=True), settings=settings or Settings(),
                     session_path=tmp_path / "session.json")
    app.export_dir = tmp_path / "exports"
    return app


async def settle(pilot, cond, timeout=5.0):
    for _ in range(int(timeout / 0.05)):
        if cond():
            return True
        await pilot.pause(0.05)
    return False


def _lists(app):
    """Browse's lists: the columns one came through and is in, and the look ahead."""
    from bbmp.tui.widgets import OneLineList
    return [w for w in app.query_one("#miller").children if isinstance(w, OneLineList)]


def formats_list(app):
    """The list of formats (or of a platform's formats) Browse shows, or None."""
    return next((ol for ol in _lists(app) if ol.node and (
        ol.node[-1] == ("menu", "formats") or ol.node[-1][0] in ("platform", "chip"))), None)


def artists_list(app):
    """The list of a format's artists (or of days) Browse shows, or None."""
    return next((ol for ol in _lists(app) if ol.node and (
        ol.node[-1][0] == "format" or ol.node[-1] == ("menu", "new"))
        and ol.option_count), None)


def option_ids(ol):
    return [ol.get_option_at_index(i).id for i in range(ol.option_count)]


async def select_format(pilot, app, fmt):
    """Wait for Browse's formats, and put the cursor on one (by name, or in a view by
    the collection it is listed under and its name)."""
    from bbmp.tui.browse import IN, step_id
    assert await settle(pilot, lambda: getattr(app, "_placed", False)
                        and formats_list(app) is not None and formats_list(app).option_count > 0)
    ol = formats_list(app)
    ids = option_ids(ol)
    want = next(i for i, id_ in enumerate(ids) if id_ and (
        id_ == step_id(("format", fmt)) or id_.endswith(f"{IN}{fmt}")))
    ol.highlighted = want
    await pilot.pause(0.25)                     # the look at its artists comes 0.12 s later


async def browse_ready(pilot, app):
    """Wait until Browse's columns are in place; the list one is in."""
    assert await settle(pilot, lambda: getattr(app, "_placed", False) and app._levels
                        and app._levels[-1].option_count > 0)
    return app._levels[-1]


def put(ol, step):
    """The cursor on a list's entry for a step, ("format", "TFMX") and so on."""
    from bbmp.tui.browse import step_id
    ol.highlighted = option_ids(ol).index(step_id(step))


async def go_in(pilot, app, *steps):
    """Into each entry in turn, from the list one is in."""
    for step in steps:
        put(app._levels[-1], step)
        await pilot.pause(0.05)
        await pilot.press("right")
        await pilot.pause(0.1)


def run(coro, timeout=120):
    """A test that hangs fails after `timeout` seconds instead of stopping the run."""
    asyncio.run(asyncio.wait_for(coro, timeout))


def test_browse_play_favorite_queue(catalog, fetcher, tmp_path):
    async def go():
        lib = Library(tmp_path / "user.db")
        app = make_app(catalog, fetcher, tmp_path, library=lib)
        async with app.run_test(size=(120, 40)) as pilot:
            await select_format(pilot, app, "Protracker")
            await pilot.press("enter")                      # into the format: its artists
            assert app.focused is artists_list(app)
            assert await settle(pilot, lambda: artists_list(app).option_count == 2)
            await pilot.press("down")
            await pilot.press("enter")                      # into the artist: the songs
            assert app.focused.id == "browse-songs"
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: table.row_count == 2)
            await pilot.press("enter")
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            assert app.player.status().song.path == "Protracker/Tester/beep.mod"
            await pilot.press("f")
            assert lib.favorite_keys() == {"modland:Protracker/Tester/beep.mod"}
            app.wrong_file = tmp_path / "sounds-wrong.txt"
            await pilot.press("exclamation_mark", *"hiss", "enter")
            assert await settle(pilot, lambda: app.wrong_file.exists())
            assert "Protracker/Tester/beep.mod" in app.wrong_file.read_text()
            assert '"hiss"' in app.wrong_file.read_text()
            await pilot.press("exclamation_mark", "escape")     # Esc notes nothing
            await pilot.pause(0.1)
            assert app.wrong_file.read_text().count("beep.mod") == 1
            await pilot.press("space")
            assert await settle(pilot, lambda: app.player.status().state == "paused")
            await pilot.press("space")
            await pilot.press("n")
            assert await settle(pilot, lambda: app.player.status().song.title == "beep two")
            await pilot.press("4")
            assert await settle(pilot, lambda: app.query_one("#fav-songs").row_count == 1)
            await pilot.press("3")
            assert await settle(pilot, lambda: app.query_one("#queue-songs").row_count == 2)
            await pilot.press("6")
            assert await settle(pilot, lambda: app.query_one("#hist-songs").row_count == 2)
            await pilot.press("q")
    run(go())


def test_browse_goes_in_and_out_as_ranger_does(catalog, fetcher, tmp_path):
    """→ goes into the entry under the cursor, ← back; the list one was in becomes the
    look ahead again, and where one is is kept for the next start."""
    async def go():
        settings = Settings.load(tmp_path / "settings.json")
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(140, 44)) as pilot:
            formats = await browse_ready(pilot, app)
            assert app._path == [("menu", "formats")]
            assert app._levels[0].border_title == "Browse by"     # the ways, on the left
            put(formats, ("format", "TFMX"))
            assert await settle(pilot, lambda: app._preview_list().node == app._path + [("format", "TFMX")])
            preview = app._preview_list()
            await pilot.press("right")
            assert app._levels[-1] is preview                    # the look became the list
            assert app._path[-1] == ("format", "TFMX") and app.focused is preview
            await pilot.press("right")                           # into Tester: the songs
            assert app.focused.id == "browse-songs" and app._artist == "Tester"
            assert await settle(pilot, lambda: "level1" in str(app.query_one("#song-info").render()))
            await pilot.press("left")                            # back to the artists
            assert app.focused is preview
            await pilot.press("left")                            # back to the formats
            assert app.focused is formats and app._preview_list() is preview
            await pilot.press("left")                            # the ways to browse
            assert app._path == []
            put(app._levels[-1], ("menu", "platforms"))
            await pilot.press("enter")
            assert app._path == [("menu", "platforms")]
            await go_in(pilot, app, ("platform", "amiga"))
            assert app._view() == ("platform", "amiga")
            assert any(id_ and id_.endswith("TFMX") for id_ in option_ids(app._levels[-1]))
        assert Settings.load(tmp_path / "settings.json").browse == [
            ["menu", "platforms"], ["platform", "amiga"]]
        again = make_app(catalog, fetcher, tmp_path,
                         settings=Settings.load(tmp_path / "settings.json"))
        async with again.run_test(size=(140, 44)) as pilot:
            await browse_ready(pilot, again)
            assert again._path == [("menu", "platforms"), ("platform", "amiga")]
    run(go())


def test_left_goes_back_from_songs_too_wide_to_show(catalog, fetcher, tmp_path):
    """← among Browse's songs goes back even when the table is wider than its place
    (a long title), where a table's own ← would scroll it sideways."""
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(36, 30)) as pilot:
            formats = await browse_ready(pilot, app)
            put(formats, ("format", "TFMX"))
            await pilot.pause(0.2)
            await pilot.press("right", "right")
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: app.focused is table and table.row_count)
            await pilot.pause(0.1)
            assert table.virtual_size.width > table.scrollable_content_region.width
            await pilot.press("left")
            assert await settle(pilot, lambda: app.focused is not table)
            assert app._path[-1] == ("format", "TFMX")
    run(go())


def test_radio_from_the_platform_under_the_cursor(catalog, fetcher, tmp_path):
    """r on a platform in the list plays from that platform, not from everything."""
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(140, 44)) as pilot:
            await browse_ready(pilot, app)
            await pilot.press("left")
            put(app._levels[-1], ("menu", "platforms"))
            await pilot.press("enter")
            await browse_ready(pilot, app)
            put(app._levels[-1], ("platform", "amiga"))
            await pilot.pause(0.1)
            assert app._view() is None                   # not in it, only on it
            await pilot.press("r")
            assert await settle(pilot, lambda: app.player.status().radio == "Amiga")
    run(go())


def test_search_and_unsupported(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await pilot.press("slash")
            assert app.focused.id == "q"
            await pilot.press(*"duet", "enter")
            table = app.query_one("#search-songs")
            assert await settle(pilot, lambda: table.row_count == 1)
            assert app.focused is table
            await pilot.press("enter")
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            # typing in the search box must not trigger player keys
            # (/ selects the old query, so typing replaces it)
            await pilot.press("slash", "q", "n", "space")
            assert app.query_one("#q").value == "qn "
            assert app.is_running
            # an FMP song is marked unplayable and doesn't play
            app.query_one("#q").value = "opening"
            await pilot.press("enter")
            assert await settle(pilot, lambda: table.row_count == 1)
            await pilot.press("enter")
            await pilot.pause(0.3)
            assert app.player.status().song.path == "Protracker/Other Guy/coop-Tester/duet.mod"
    run(go())


def test_radio_and_pattern(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await select_format(pilot, app, "Protracker")
            await pilot.pause(0.3)
            await pilot.press("r")
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            assert app.player.status().radio == "Protracker"
            vis = app.query_one("#visual")
            from bbmp import visuals
            app.query_one("#visual").demo = True        # Settings: the demo effects too
            for mode in visuals.MODES:
                await pilot.press("v")
                assert vis.mode == mode
                await pilot.pause(0.3)
                assert str(vis.render()).strip(), mode
                if mode == "pattern":
                    assert await settle(pilot, lambda: app.player.status().pattern is not None)
            await pilot.press("v")
            assert vis.mode is None
            await pilot.press("plus")
            assert app.player.status().volume == 85
            assert app.settings.volume == 85
            assert app.settings.visualizer is None   # cycled through them all and back to off
    run(go())


def test_stats_and_settings_tabs(catalog, fetcher, tmp_path):
    async def go():
        lib = Library(tmp_path / "user.db")
        song = catalog.by_path("Protracker/Tester/beep.mod")
        for _ in range(3):
            lib.finish_play(lib.start_play(song), 60)
        settings = Settings.load(tmp_path / "settings.json")
        app = make_app(catalog, fetcher, tmp_path, settings=settings, library=lib)
        async with app.run_test(size=(120, 40)) as pilot:
            await pilot.press("9")
            await pilot.pause(0.3)
            screen = app.export_screenshot()
            assert "beep" in screen and "Tester" in screen and "Protracker" in screen
            await pilot.press("0")
            assert app.focused.id == "settings-list"
            await pilot.press("enter")          # first row: subsongs, all -> one
            assert settings.subsongs == "one"
            await pilot.press("R")              # repeat off -> all
            assert settings.repeat == "all"
            await pilot.pause(0.2)
        # everything was saved as it changed
        again = Settings.load(tmp_path / "settings.json")
        assert (again.subsongs, again.repeat) == ("one", "all")
    run(go())


def test_resume_where_we_left_off(catalog, fetcher, tmp_path):
    async def go():
        settings = Settings()
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(120, 40)) as pilot:
            app.player.play_list(catalog.songs("Protracker", "Tester"), 1)
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            await pilot.pause(1.2)
            await pilot.press("q")
        assert (tmp_path / "session.json").exists()

        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(120, 40)) as pilot:
            # the song comes back first, and the rest of the queue round it soon after
            assert await settle(pilot, lambda: app.player.status().state == "paused"
                                and app.player.status().queue_len == 2)
            st = app.player.status()
            assert st.song.title == "beep two" and st.queue_pos == 1 and st.elapsed > 0.5
            await pilot.press("space")
            assert await settle(pilot, lambda: app.player.status().state == "playing")
    run(go())


def test_files_tab_plays_local_files(catalog, fetcher, tmp_path, mod_bytes):
    music = tmp_path / "music" / "My Mods"
    music.mkdir(parents=True)
    for name in ("alpha.mod", "beta.mod", "cover.jpg", "notes.txt"):
        (music / name).write_bytes(mod_bytes if name.endswith(".mod") else b"x")

    async def go():
        settings = Settings(music_dir=str(tmp_path / "music"))
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(120, 40)) as pilot:
            await pilot.press("5")
            tree = app.query_one("#files-tree")
            assert app.focused is tree
            await pilot.press("down", "enter")          # open "My Mods"
            assert await settle(pilot, lambda: len(tree.root.children) == 1
                                and len(tree.root.children[0].children) == 2)   # no jpg, no txt
            await pilot.press("down", "down", "enter")  # beta.mod
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            st = app.player.status()
            assert st.song.local and st.song.title == "beta" and st.song.format == "Protracker"
            assert st.queue_len == 2 and st.queue_pos == 1
            assert fetcher.downloads == []               # nothing was fetched from Modland
            await pilot.press("f")                       # local files can be favorites too
            await pilot.press("4")
            assert await settle(pilot, lambda: app.query_one("#fav-songs").row_count == 1)
            await pilot.press("s")                       # it is on this computer already
            await pilot.pause(0.2)
            assert app.is_running and len(app.screen.query("#pick")) == 0
    run(go())


def test_artist_album_and_info_pages(catalog, fetcher, tmp_path):
    async def go():
        opened = []
        app = make_app(catalog, fetcher, tmp_path)
        app._open_url = opened.append
        async with app.run_test(size=(130, 44)) as pilot:
            app.player.play_list([catalog.by_path("Protracker/Other Guy/coop-Tester/duet.mod")])
            assert await settle(pilot, lambda: app.player.status().state == "playing")

            await pilot.press("m")                       # more by the artist of the playing song
            tabs = app.query_one("#tabs")
            assert tabs.active == "artist" and app._page.title == "Other Guy"
            assert app._page.partners == [("Tester", 1)]
            await pilot.press("w")
            assert opened and "Other+Guy" in opened[0]

            app.query_one("#partners").focus()           # jump to the collaborator
            await pilot.press("enter")
            assert await settle(pilot, lambda: app._page.title == "Tester")
            titles = [s.title for s in app._page.songs]
            assert "beep" in titles and "duet" in titles     # own songs and the co-written one
            assert {s.format for s in app._page.songs} >= {"Protracker", "TFMX"}

            table = app.query_one("#artist-songs")
            table.move_cursor(row=next(i for i, s in enumerate(table.songs) if s.album == "Album"))
            await pilot.press("A")                       # the album of the selected song
            assert await settle(pilot, lambda: app._page.kind == "album" and app._page.title == "Album")
            assert [s.name for s in app._page.songs] == ["song.smus"]

            await pilot.press("i")
            assert tabs.active == "info"
            await pilot.pause(0.3)
            screen = app.export_screenshot()
            assert "square" in screen and "Sample" in screen    # the test module's sample name
    run(go())


def test_export_key(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(export_format="wav"))
        async with app.run_test(size=(120, 40)) as pilot:
            app.player.play_list(catalog.songs("Protracker", "Tester"))
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            await pilot.press("e")
            assert await settle(pilot, lambda: len(app.screen.query("#pick")) == 1)
            pick = app.screen.query_one("#pick")
            rows = [str(pick.get_option_at_index(i).prompt) for i in range(pick.option_count)]
            assert rows[0] == "The song beep"
            assert rows[1].startswith("Tester's songs in Protracker  ·  2 songs")
            assert rows[2].startswith("Everything by Tester")
            assert rows[-1].startswith("The queue  ·  2 songs")
            await pilot.press("enter")
            out = tmp_path / "exports" / "Tester - beep.wav"
            assert await settle(pilot, lambda: out.exists() and out.stat().st_size > 100000)
            assert app.player.status().state == "playing"     # playback carried on meanwhile

            await pilot.press("e")                            # the queue, in one folder
            assert await settle(pilot, lambda: len(app.screen.query("#pick")) == 1)
            pick = app.screen.query_one("#pick")
            pick.highlighted = pick.option_count - 1
            await pilot.press("enter")
            folder = tmp_path / "exports" / "Queue"
            assert await settle(pilot, lambda: not app._exporting and folder.is_dir())
            assert sorted(f.name for f in folder.iterdir()) == [
                "01 - Tester - beep.wav", "02 - Tester - beep two.wav"]
    run(go())


def test_playlists(catalog, fetcher, tmp_path):
    async def go():
        lib = Library(tmp_path / "user.db")
        app = make_app(catalog, fetcher, tmp_path, library=lib)
        async with app.run_test(size=(130, 44)) as pilot:
            app.player.play_list(catalog.songs("Protracker", "Tester"))
            assert await settle(pilot, lambda: app.player.status().state == "playing")

            await pilot.press("l")                        # add the playing song to a new playlist
            await pilot.press("enter")                    # the only choice: + New playlist
            await pilot.press(*"Chips", "enter")
            assert await settle(pilot, lambda: lib.playlists() == [(1, "Chips", 1)])

            await pilot.press("n")
            assert await settle(pilot, lambda: app.player.status().song.title == "beep two")
            await pilot.press("l", "enter")               # add the next one to Chips
            assert await settle(pilot, lambda: lib.playlists() == [(1, "Chips", 2)])
            await pilot.press("l", "enter")               # twice is once
            await pilot.pause(0.2)
            assert lib.playlists() == [(1, "Chips", 2)]

            await pilot.press("L")                        # the queue as a playlist
            await pilot.press(*"Tonight", "q", "enter")   # q types a letter here, it doesn't quit
            assert await settle(pilot, lambda: len(lib.playlists()) == 2)
            assert app.is_running and lib.playlists()[1][1:] == ("Tonightq", 2)

            await pilot.press("4")
            table = app.query_one("#fav-songs")
            lists = app.query_one("#lists")
            lists.focus()
            assert lists.highlighted == 2                 # it remembers the playlist just made
            await pilot.press("up")                       # Tonightq -> Chips
            assert await settle(pilot, lambda: [s.title for s in table.songs] == ["beep", "beep two"])
            await pilot.press("enter")                    # into the songs
            assert app.focused is table
            await pilot.press("delete")                   # take "beep" out
            assert await settle(pilot, lambda: [s.title for s in table.songs] == ["beep two"])
            await pilot.press("enter")                    # play from the playlist
            assert await settle(pilot, lambda: app.player.status().queue_len == 1)

            lists.focus()
            await pilot.press("delete")                   # delete Chips: asks first
            await pilot.press("escape")
            assert len(lib.playlists()) == 2
            await pilot.press("delete", "enter")
            assert await settle(pilot, lambda: [n for _, n, _ in lib.playlists()] == ["Tonightq"])
            assert lib.protected_keys() == {"modland:Protracker/Tester/beep.mod",
                                            "modland:Protracker/Tester/beep two.mod"}
    run(go())


def test_rename_reorder_and_import_playlists(catalog, fetcher, tmp_path, mod_bytes):
    music = tmp_path / "music"
    music.mkdir()
    (music / "local.mod").write_bytes(mod_bytes)
    (music / "party.m3u").write_text(
        "local.mod\nftp://ftp.modland.com/pub/modules/Protracker/Tester/beep.mod\nmissing.mod\n")

    async def go():
        lib = Library(tmp_path / "user.db")
        pid = lib.create_playlist("Mix", ["modland:Protracker/Tester/beep.mod",
                                          "modland:Protracker/Tester/beep two.mod",
                                          "modland:Protracker/Other Guy/coop-Tester/duet.mod"])
        app = make_app(catalog, fetcher, tmp_path, library=lib,
                       settings=Settings(music_dir=str(music)))
        async with app.run_test(size=(130, 44)) as pilot:
            await pilot.press("4")
            lists, table = app.query_one("#lists"), app.query_one("#fav-songs")
            lists.focus()
            await pilot.press("down")                     # Favorites -> Mix
            assert await settle(pilot, lambda: len(table.songs) == 3)

            await pilot.press("f2")                       # rename
            await pilot.press(*"Best", "enter")           # the old name is selected: typing replaces it
            assert await settle(pilot, lambda: lib.playlist_name(pid) == "Best")
            assert "Best" in str(lists.get_option_at_index(1).prompt)

            await pilot.press("enter")                    # into the songs, cursor on "beep"
            assert app.focused is table
            await pilot.press("shift+down")
            assert await settle(pilot, lambda: [s.title for s in table.songs] ==
                                ["beep two", "beep", "duet"])
            assert table.cursor_row == 1                  # the cursor went along with the song
            await pilot.press("J")                        # the same without a shift+arrow key
            assert await settle(pilot, lambda: [s.title for s in table.songs] ==
                                ["beep two", "duet", "beep"])
            await pilot.press("J")                        # already last: nothing happens
            await pilot.press("K", "K", "K")
            assert await settle(pilot, lambda: [s.title for s in table.songs] ==
                                ["beep", "beep two", "duet"])
            assert lib.playlist_keys(pid)[0] == "modland:Protracker/Tester/beep.mod"

            await pilot.press("5")                        # Files tab: the .m3u shows up
            tree = app.query_one("#files-tree")
            assert await settle(pilot, lambda: sorted(n.data.path.name for n in tree.root.children)
                                == ["local.mod", "party.m3u"])
            tree.move_cursor(next(n for n in tree.root.children if n.data.path.name == "party.m3u"))
            await pilot.press("enter")
            assert await settle(pilot, lambda: lib.playlist_id("Party") is not None)
            assert app.query_one("#tabs").active == "favorites"
            assert [s.title for s in table.songs] == ["local", "beep"]    # missing.mod was left out

            lists.focus()                                 # and by typing the file's path
            lists.highlighted = lists.option_count - 1
            await pilot.press("enter")
            await pilot.press(*"party.m3u", "enter")      # the music folder is already filled in
            assert await settle(pilot, lambda: lib.playlist_id("Party 2") is not None)
            lists.highlighted = lists.option_count - 1
            await pilot.press("enter")
            await pilot.press(*"no-such-file.m3u", "enter")
            await pilot.pause(0.3)
            assert app.is_running and len(lib.playlists()) == 3
    run(go())


def test_theme_is_chosen_in_settings_and_kept(catalog, fetcher, tmp_path):
    from bbmp import theme

    async def go():
        settings = Settings.load(tmp_path / "settings.json")
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(120, 40)) as pilot:
            assert app.theme == theme.DEFAULT_THEME
            assert {"amiga-workbench-1.3", "amiga-copper"} <= set(app.available_themes)
            await pilot.press("0")
            rows = app.query_one("#settings-list")
            rows.highlighted = next(i for i in range(rows.option_count)
                                    if rows.get_option_at_index(i).id == "theme")
            await pilot.press("enter")                    # the theme picker
            await pilot.press("down")                     # the first Amiga theme, shown at once
            assert await settle(pilot, lambda: app.theme.startswith("amiga-"))
            await pilot.press("escape")                   # no: back to what it was
            assert await settle(pilot, lambda: app.theme == theme.DEFAULT_THEME)
            await pilot.press("enter", "down", "enter")
            assert await settle(pilot, lambda: app.theme.startswith("amiga-"))
            chosen = app.theme
            assert settings.theme == chosen
            assert theme.current.bars                     # the bars took the theme's colours
            app.theme = "nord"                            # as ctrl+p would change it
            assert await settle(pilot, lambda: settings.theme == "nord")
        assert Settings.load(tmp_path / "settings.json").theme == "nord"

        app = make_app(catalog, fetcher, tmp_path, settings=Settings(theme="amiga-workbench-1.3"))
        async with app.run_test(size=(120, 40)) as pilot:
            assert app.theme == "amiga-workbench-1.3"     # it comes back on the next start
            assert theme.current.accent.lower() == "#ff8800"
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(theme="no-such-theme"))
        async with app.run_test(size=(120, 40)) as pilot:
            assert app.theme == theme.DEFAULT_THEME
    run(go())


def test_formats_say_how_well_they_play(catalog, fetcher, tmp_path, monkeypatch):
    from bbmp import formats_info
    from bbmp.tui.browse import id_step
    monkeypatch.setattr(formats_info, "NOTES", {
        "Protracker": ("verified", "40 of 40 tried", "Compared with the original."),
        "TFMX": ("faults", "38 of 40 tried", "2 of 40 tried are silent."),
        "Renoise Old": ("no", "0", "Renoise's effect devices were never published."),
    })

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            formats = await browse_ready(pilot, app)
            labels = {id_step(formats.get_option_at_index(i).id)[1]:
                      str(formats.get_option_at_index(i).prompt) for i in range(formats.option_count)}
            assert "✓" in labels["Protracker"] and "!" in labels["TFMX"]
            assert "✓" not in labels["Fasttracker 2"] and "!" not in labels["Fasttracker 2"]
            note = app.query_one("#format-note")

            def shows(fmt, *words):
                put(formats, ("format", fmt))
                return settle(pilot, lambda: all(w in str(note.render()) for w in words))
            assert await shows("TFMX", "Plays with faults or gaps", "38 of 40", "are silent")
            assert await shows("Protracker", "Verified against the original")
            assert await shows("Renoise Old", "Doesn't play", "never published")

            # a song that can't be played says why
            said = []
            app.notify = lambda message, **kw: said.append(message)
            await pilot.press("enter", "enter", "enter")
            assert await settle(pilot, lambda: bool(said))
            assert said[-1] == ("Can't play Renoise Old files. Renoise's effect devices were"
                                " never published.")
    run(go())


def test_a_song_that_needs_an_extra_file_says_which_and_where(catalog, fetcher, tmp_path, monkeypatch):
    import sys

    from bbmp import extras, formats_info
    monkeypatch.setattr(formats_info, "is_supported", lambda fmt: True)
    for name, module in list(sys.modules.items()):          # every part of the screen code
        if name.startswith("bbmp.tui.") and hasattr(module, "is_supported"):
            monkeypatch.setattr(module, "is_supported", lambda fmt: True)

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            from bbmp.tui.browse import step_id
            formats = await browse_ready(pilot, app)
            shroom = option_ids(formats).index(step_id(("format", "Shroom")))
            assert "+" in str(formats.get_option_at_index(shroom).prompt)
            formats.highlighted = shroom
            note = app.query_one("#format-note")
            assert await settle(pilot, lambda: "Needs a file" in str(note.render())
                                and "mario-paint-notes.wav" in str(note.render()))
            said = []
            app.notify = lambda message, **kw: said.append(message)
            await pilot.press("enter", "enter", "enter")
            assert await settle(pilot, lambda: bool(said))
            assert "mario-paint-notes.wav" in said[-1] and str(extras.folder()) in said[-1]
            assert app.player.status().song is None             # and nothing was tried

            await pilot.press("0")                              # Settings lists the files
            rows = app.query_one("#settings-list")
            row = next(rows.get_option_at_index(i) for i in range(rows.option_count)
                       if rows.get_option_at_index(i).id == "extras")
            assert "0 of 7 there" in str(row.prompt)
            assert extras.folder().is_dir() is False
            rows.highlighted = next(i for i in range(rows.option_count)
                                    if rows.get_option_at_index(i).id == "extras")
            await pilot.press("enter")
            assert await settle(pilot, lambda: extras.folder().is_dir())   # made for the user
            await pilot.press("escape")
    run(go())


def test_downloads_are_offered_once_and_are_in_settings(catalog, fetcher, tmp_path, monkeypatch):
    from bbmp import downloads
    fetched = []
    monkeypatch.setattr(downloads, "install", lambda d, progress=None: fetched.append(d.id))

    async def go():
        settings = Settings.load(tmp_path / "settings.json")
        settings.resume = False
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        app._ask = True                                  # as when the player is started for real
        async with app.run_test(size=(130, 44)) as pilot:
            assert await settle(pilot, lambda: type(app.screen).__name__ == "Confirm")
            shown = app.export_screenshot().replace("&#160;", " ")
            assert "CyberTracker 1.01" in shown and "Download them now?" in shown
            await pilot.press("escape")                  # not now
            assert await settle(pilot, lambda: type(app.screen).__name__ != "Confirm")
            assert settings.asked_downloads and fetched == []

            await pilot.press("0")                       # they are in Settings
            rows = app.query_one("#settings-list")
            row = next(i for i in range(rows.option_count)
                       if rows.get_option_at_index(i).id == "downloads")
            assert f"0 of {len(downloads.DOWNLOADS)} there" in str(rows.get_option_at_index(row).prompt)
            rows.highlighted = row
            await pilot.press("enter")
            assert await settle(pilot, lambda: type(app.screen).__name__ == "Pick")
            await pilot.press("enter")                   # the first one
            assert await settle(pilot, lambda: fetched == ["soundclub"])

        again = make_app(catalog, fetcher, tmp_path, settings=Settings.load(tmp_path / "settings.json"))
        again._ask = True
        async with again.run_test(size=(130, 44)) as pilot:
            await pilot.pause(0.5)
            assert type(again.screen).__name__ != "Confirm"      # asked once
    run(go())


def test_sizes_and_what_is_new_are_shown(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            formats = await browse_ready(pilot, app)
            assert "KB" in formats.border_title                      # the whole collection
            table, note = app.query_one("#browse-songs"), app.query_one("#format-note")
            # New: days, and a day's songs
            await pilot.press("left")
            await go_in(pilot, app, ("menu", "new"))
            days = app._levels[-1]
            assert days.option_count == 2 and "(2 days)" in days.border_title
            assert "27 Sep 2026" in str(days.get_option_at_index(0).prompt)
            assert "1 new" in str(days.get_option_at_index(0).prompt)
            assert "1 changed" in str(days.get_option_at_index(0).prompt)
            assert await settle(pilot, lambda: "is from" in str(note.render()))
            assert await settle(pilot, lambda: [s.title for s in table.songs] == ["beep two", "level1"])
            assert "Protracker · Tester" in str(table.get_row_at(0)[-1])
            assert "(updated)" in str(table.get_row_at(1)[-1])
            # a format, an artist and a song say how large they are
            await pilot.press("left")
            await go_in(pilot, app, ("menu", "formats"), ("format", "TFMX"))
            artists = app._levels[-1]
            assert "KB" in artists.border_title and "TFMX (1)" in artists.border_title
            assert await settle(pilot, lambda: "Tester (2)" in (table.border_title or "")
                                and "KB" in table.border_title)
            assert "KB" in str(table.get_row_at(0)[2])
            assert "KB" in str(note.render())
    run(go())


def test_an_artist_is_kept_and_let_go(catalog, fetcher, tmp_path):
    """k keeps the artist, the format or the collection on this computer."""
    kept = tmp_path / "kept/Modland/TFMX/Tester"
    beep = next(iter(fetcher.server.values()))
    fetcher.server.update({p: beep for p in SONGS if p.startswith("TFMX/")})

    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        async with app.run_test(size=(130, 44)) as pilot:
            await select_format(pilot, app, "TFMX")
            await pilot.press("right")                       # into TFMX: Tester under the cursor
            assert await settle(pilot, lambda: app._artist == "Tester")
            await pilot.press("s")
            assert await settle(pilot, lambda: len(app.screen.query("#pick")) == 1)
            pick = app.screen.query_one("#pick")
            rows = [str(pick.get_option_at_index(i).prompt) for i in range(pick.option_count)]
            assert [r.split("  ·  ")[0].strip() for r in rows] == [
                "Tester in TFMX", "TFMX", "All of Modland"]      # no song: the lists are in focus
            assert all("KB" in r for r in rows)
            await pilot.press("enter")                       # the artist
            assert await settle(pilot, lambda: (kept / "smpl.level2").is_file()
                                and not app._keeping)
            assert sorted(f.name for f in kept.iterdir()) == [
                "mdat.level1", "mdat.level2", "smpl.level1", "smpl.level2"]
            # the lists are made again with the marks (new widgets)
            assert await settle(pilot, lambda: app._placed and
                                "⤓" in str(artists_list(app).get_option_at_index(0).prompt))
            formats = formats_list(app)
            assert "⤓" not in str(formats.get_option_at_index(formats.highlighted).prompt)
            assert app._kept_summary().startswith("1 saved")

            # it plays from there, and no server is asked
            fetcher.downloads.clear()
            await pilot.press("right")                       # into Tester's songs
            await pilot.press("enter")
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            assert fetcher.downloads == []
            await pilot.press("x")

            await pilot.press("s")
            assert await settle(pilot, lambda: len(app.screen.query("#pick")) == 1)
            pick = app.screen.query_one("#pick")
            # the songs are in focus now: the song is the first choice, kept with its artist
            assert "saved with Tester in TFMX" in str(pick.get_option_at_index(0).prompt)
            assert str(pick.get_option_at_index(1).prompt).startswith("✓ Tester in TFMX")
            assert "What is saved" in str(pick.get_option_at_index(4).prompt)
            await pilot.press("down", "enter")
            assert await settle(pilot, lambda: "Stop keeping Tester in TFMX" in
                                app.export_screenshot().replace("&#160;", " "))
            await pilot.press("enter")
            assert await settle(pilot, lambda: not kept.exists() and app._kept == [])
            assert await settle(pilot, lambda: app._placed and
                                "⤓" not in str(artists_list(app).get_option_at_index(0).prompt))
    run(go())


def test_what_is_kept_is_brought_up_to_date_at_the_start(catalog, fetcher, tmp_path):
    from bbmp.keep import Keeper, Scope
    keeper = Keeper(catalog, fetcher)
    keeper.add(Scope("modland", "Protracker"))
    keeper.close()

    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        async with app.run_test(size=(130, 44)) as pilot:
            assert await settle(pilot, lambda: (
                tmp_path / "kept/Modland/Protracker/Tester/beep.mod").is_file()
                and not app._keeping)
            from bbmp.tui.browse import step_id
            assert await settle(pilot, lambda: app._placed)
            formats = formats_list(app)
            row = option_ids(formats).index(step_id(("format", "Protracker")))
            assert "⤓" in str(formats.get_option_at_index(row).prompt)
            await pilot.press("0")
            settings = app.query_one("#settings-list")
            rows = [str(settings.get_option_at_index(i).prompt)
                    for i in range(settings.option_count)]
            assert await settle(pilot, lambda: any(
                "Saved on this computer" in str(settings.get_option_at_index(i).prompt)
                and "1 saved" in str(settings.get_option_at_index(i).prompt)
                for i in range(settings.option_count))), rows
    run(go())


def test_the_folder_for_kept_music_is_chosen_in_settings(catalog, fetcher, tmp_path, monkeypatch):
    from bbmp import paths
    from bbmp.keep import Keeper, Scope
    monkeypatch.delenv("BBMP_KEPT")
    monkeypatch.setattr(paths, "KEPT_DIR", tmp_path / "Music/ByteBandit Music Player")
    monkeypatch.setattr(paths, "OWN_KEPT_DIR", tmp_path / "data/kept")
    fetcher.kept_dir = tmp_path / "kept"
    keeper = Keeper(catalog, fetcher)
    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper.run()
    keeper.close()
    music = tmp_path / "Music"
    music.mkdir()
    settings = Settings.load(tmp_path / "settings.json")
    settings.update(resume=False, music_dir=str(music), keep_dir=str(tmp_path / "kept"))

    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(130, 44)) as pilot:
            await pilot.press("0")
            rows = app.query_one("#settings-list")
            assert await settle(pilot, lambda: rows.option_count > 5)
            rows.highlighted = next(i for i in range(rows.option_count)
                                    if rows.get_option_at_index(i).id == "keep_dir")
            await pilot.press("enter")
            assert await settle(pilot, lambda: len(app.screen.query("#pick")) == 1)
            pick = app.screen.query_one("#pick")
            offered = [str(pick.get_option_at_index(i).prompt) for i in range(pick.option_count)]
            assert "Music/ByteBandit Music Player" in offered[0] and "Another folder" in offered[-1]
            await pilot.press("enter")                       # the one in the music folder
            assert await settle(pilot, lambda: "Move the kept music" in
                                app.export_screenshot().replace("&#160;", " "))
            await pilot.press("enter")
            there = music / "ByteBandit Music Player"
            assert await settle(pilot, lambda: app.keeper.folder == there
                                and (there / "Modland/Protracker/Tester/beep.mod").is_file())
            assert not (tmp_path / "kept").exists()
            assert app._kept == [Scope("modland", "Protracker", "Tester")]
            assert await settle(pilot, lambda: any(
                "Music/ByteBandit Music Player" in str(rows.get_option_at_index(i).prompt)
                for i in range(rows.option_count)))
            # and it is played from there
            fetcher.downloads.clear()
            assert fetcher.get("modland:Protracker/Tester/beep.mod").is_relative_to(there)
            assert fetcher.downloads == []
    run(go())
    # it is where kept music is when nothing is said, so nothing needs to be said
    assert Settings.load(tmp_path / "settings.json").keep_dir == ""


def test_a_song_is_kept_with_the_same_key(catalog, fetcher, tmp_path):
    """One key for a song, an artist, a format and a collection."""
    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        async with app.run_test(size=(130, 44)) as pilot:
            await select_format(pilot, app, "Protracker")
            assert await settle(pilot, lambda: app._fmt == "Protracker")
            await pilot.press("right")
            put(app._levels[-1], ("artist", "Tester"))
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: [s.title for s in table.songs] == ["beep", "beep two"])
            keys = {b.key for _, b, _, _ in app.screen.active_bindings.values()}
            assert "k" not in keys and "s" in keys               # one key for saving
            await pilot.press("right")                       # among the songs
            assert app.focused is table
            await pilot.press("s")
            assert await settle(pilot, lambda: len(app.screen.query("#pick")) == 1)
            pick = app.screen.query_one("#pick")
            rows = [str(pick.get_option_at_index(i).prompt) for i in range(pick.option_count)]
            assert [r.split("  ·  ")[0].strip() for r in rows] == [
                "The song beep by Tester", "Tester in Protracker", "Protracker", "All of Modland"]
            await pilot.press("enter")
            kept = tmp_path / "kept/Modland/Protracker/Tester"
            assert await settle(pilot, lambda: (kept / "beep.mod").is_file() and not app._keeping)
            assert [f.name for f in kept.iterdir()] == ["beep.mod"]
            assert await settle(pilot, lambda: str(table.get_row_at(0)[0]) == "⤓")
            assert str(table.get_row_at(1)[0]) == ""
    run(go())



def test_the_kept_mark_comes_when_the_file_lands(catalog, fetcher, tmp_path):
    """The lists were drawn when a song was saved, before its file came, and not again
    when it came: the ⤓ was missing (7 October 2026)."""
    from bbmp.keep import Scope

    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        async with app.run_test(size=(130, 44)) as pilot:
            await select_format(pilot, app, "Protracker")
            assert await settle(pilot, lambda: app._fmt == "Protracker")
            await pilot.press("right")
            put(app._levels[-1], ("artist", "Tester"))
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: [s.title for s in table.songs] == ["beep", "beep two"])
            app.keeper.add(Scope("modland", "Protracker", "Tester"))
            app._keep_changed()                         # drawn: nothing there yet
            await pilot.pause(0.2)
            assert str(table.get_row_at(0)[0]) == ""
            there = app.fetcher.kept_dir / "Modland/Protracker/Tester/beep.mod"
            there.parent.mkdir(parents=True, exist_ok=True)
            there.write_bytes(b"x")
            app._keep_done(None)                        # the fetching is done
            await pilot.pause(0.1)
            assert str(table.get_row_at(0)[0]) == "⤓"
            assert str(table.get_row_at(1)[0]) == ""
    run(go())


def test_an_error_in_a_background_job_is_told_and_the_player_goes_on(catalog, fetcher,
                                                                    tmp_path):
    """An error nobody caught in a background job closed the player (7 October 2026):
    now it is written to the error log, and told."""
    def broken(*a, **k):
        raise RuntimeError("the disk said no")

    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        app.error_log = tmp_path / "errors.log"
        async with app.run_test(size=(130, 44), notifications=True) as pilot:
            await browse_ready(pilot, app)
            app.fetcher.trim = broken
            app._clear_cache()
            assert await settle(pilot, lambda: any("clearing the cache" in n.message
                                                   for n in app._notifications))
            app.index.search = broken
            await pilot.press("slash", *"beep", "enter")
            assert await settle(pilot, lambda: any("searching" in n.message
                                                   for n in app._notifications))
            assert app.is_running
            assert "the disk said no" in app.error_log.read_text()
    run(go())


def test_keys_can_be_changed_in_the_settings(catalog, fetcher, tmp_path):
    settings = Settings(resume=False, keys={"save": "g", "shuffle": "z", "nothing": "y"})

    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(130, 44), notifications=True) as pilot:
            formats = formats_list(app)
            assert await settle(pilot, lambda: formats.option_count > 3)
            await pilot.press("z")
            assert settings.shuffle
            await pilot.press("S")                        # the old key does nothing now
            assert settings.shuffle
            await pilot.press("g")
            assert await settle(pilot, lambda: len(app.screen.query("#pick")) == 1)
            await pilot.press("escape")
            assert any("nothing" in n.message for n in app._notifications)
            # w and d are only the artist page's; elsewhere they do nothing
            opened = []
            app._open_url = opened.append
            await pilot.press("w", "d")
            assert opened == []
    run(go())


def test_a_small_terminal_shows_what_fits(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        async with app.run_test(size=(80, 24)) as pilot:
            formats = await browse_ready(pilot, app)
            row = app.query_one("#miller")
            songs = app.query_one("#browse-songs")
            assert row.has_class("narrow") and app.screen.has_class("short")
            assert app._levels[0].region.width == 0           # no room for the list one came from
            assert formats.region.width > 20 and not songs.region.width
            await select_format(pilot, app, "Protracker")
            await pilot.press("enter")                        # to the artists: the songs show
            assert await settle(pilot, lambda: formats.region.width == 0 and songs.region.width > 30)
            tabs = app.query_one("#tabs")
            assert str(tabs.get_tab("browse").label) == "1 Browse"
            assert str(tabs.get_tab("search").label) == "2"
            footer = app.export_screenshot().replace("&#160;", " ")
            assert "Help" in footer and "Quit" in footer
            await pilot.press("2")
            assert await settle(pilot, lambda: str(tabs.get_tab("search").label) == "2 Search"
                                and str(tabs.get_tab("browse").label) == "1")
            await pilot.resize_terminal(140, 44)                 # and back to a large one
            assert await settle(pilot, lambda: not row.has_class("narrow")
                                and not app.screen.has_class("short"))
            await pilot.press("escape", "1")                  # out of the search box, to Browse
            assert await settle(pilot, lambda: formats.region.width > 0)   # three columns again
            assert str(tabs.get_tab("browse").label) == "1 Browse"
    run(go())


def test_n_plays_the_selected_song_next(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        async with app.run_test(size=(130, 44)) as pilot:
            app.player.play_list(catalog.songs("Protracker", "Tester"))
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            await select_format(pilot, app, "TFMX")
            await pilot.press("right")
            assert await settle(pilot, lambda: app._artist == "Tester")
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: len(table.songs) == 2)
            await pilot.press("right")
            assert app.focused is table
            await pilot.press("N")
            assert await settle(pilot, lambda: [s.title for s in app.player.queue()[0]]
                                == ["beep", "level1", "beep two"])
    run(go())


def test_g_goes_to_the_song_that_plays(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        async with app.run_test(size=(130, 44)) as pilot:
            beep_two = catalog.by_path("Protracker/Tester/beep two.mod")
            app.player.play_list([beep_two])
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            await pilot.press("3")                            # somewhere else
            await pilot.press("g")
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: app.query_one("#tabs").active == "browse"
                                and app.focused is table and table.selected() is not None
                                and table.selected().key == beep_two.key)
            assert app._fmt == "Protracker" and app._artist == "Tester"
    run(go())


def test_g_shows_the_collection_that_has_the_song(two, fetcher, tmp_path):
    from bbmp.settings import Settings as S
    async def go():
        app = make_app(two, fetcher, tmp_path, settings=S(resume=False, collection="chipland"))
        async with app.run_test(size=(130, 44)) as pilot:
            assert await settle(pilot, lambda: formats_list(app).option_count > 0)
            beep = two.by_path("Protracker/Tester/beep.mod")        # Modland's
            app.player.play_list([beep])
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            await pilot.press("g")
            table = app.query_one("#browse-songs")
            ok = await settle(pilot, lambda: table.selected() is not None
                              and table.selected().key == beep.key)
            assert ok, (app._collection, app._fmt, app._artist, [s.key for s in table.songs],
                        [n.message for n in app._notifications])
            assert app._collection == "modland"
    run(go())



def test_t_shows_what_is_saved_and_exported(catalog, fetcher, tmp_path):
    from bbmp.keep import Scope

    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False, export_format="wav"))
        async with app.run_test(size=(130, 44)) as pilot:
            assert await settle(pilot, lambda: app.keeper is not None)
            await pilot.press("t")
            assert await settle(pilot, lambda: "Nothing is saved yet" in
                                app.export_screenshot().replace("&#160;", " "))
            await pilot.press("escape")
            app._keep(Scope("modland", "Protracker", "Tester"))
            assert await settle(pilot, lambda: not app._keeping and app._keep_progress is not None
                                and app._keep_progress.done == 2)   # the list is not on the test server
            app._start_export("Queue", catalog.songs("Protracker", "Tester"))
            assert await settle(pilot, lambda: app._export_job is not None
                                and not app._export_job.running)
            await pilot.press("t")
            shown = lambda: app.export_screenshot().replace("&#160;", " ")
            ok = await settle(pilot, lambda: "Tester in Protracker" in shown())
            if not ok:
                import re
                raise AssertionError(" ".join(re.findall(r">([^<>]+)</text>", shown())))
            assert "2 files fetched" in shown() and "Queue: done, 2 files" in shown()
            assert "could not be fetched" in shown()
            actions = app.screen.query_one("#activity-actions")
            # the list that failed can be fetched again; then what is saved
            assert str(actions.get_option_at_index(0).prompt) == "Fetch what is not there yet"
            assert str(actions.get_option_at_index(1).prompt).startswith("Stop keeping Tester")
            await pilot.press("down", "enter")                # asks first
            assert await settle(pilot, lambda: "Stop keeping Tester in Protracker?" in shown())
            await pilot.press("enter")
            assert await settle(pilot, lambda: app._kept == [])
    run(go())


def test_a_song_known_to_fail_is_marked(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        async with app.run_test(size=(130, 44)) as pilot:
            assert await settle(pilot, lambda: app.player is not None)
            app.player.trouble.note(catalog.by_path("Protracker/Tester/beep two.mod").key,
                                    "failed", "the replayer refuses it")
            await select_format(pilot, app, "Protracker")
            await pilot.press("right")
            assert await settle(pilot, lambda: app._artist is not None)
            put(app._levels[-1], ("artist", "Tester"))
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: [s.title for s in table.songs] == ["beep", "beep two"])
            assert str(table.get_row_at(0)[0]) == "" and str(table.get_row_at(1)[0]) == "!"
    run(go())


def test_help_and_names_with_brackets_show_as_they_are(catalog, fetcher, tmp_path):
    # Textual reads [..] as markup: the help's "] / [" crashed the player, and a
    # playlist called "mix [/] 2" crashed it wherever the name was shown
    async def go():
        lib = Library(tmp_path / "user.db")
        lib.create_playlist("mix [/] 2")
        app = make_app(catalog, fetcher, tmp_path, library=lib)
        async with app.run_test(size=(130, 44)) as pilot:
            await pilot.press("question_mark")
            await pilot.pause(0.3)
            assert app.is_running
            await pilot.press("4")
            lists = app.query_one("#lists")
            assert await settle(pilot, lambda: lists.option_count >= 2)
            app.notify("Added to the playlist mix [/] 2 [b]")
            await pilot.pause(0.3)
            assert app.is_running
            assert any("mix [/] 2" in str(lists.get_option_at_index(i).prompt)
                       for i in range(lists.option_count))
    run(go())


def test_a_crash_is_told_when_the_window_closes(tmp_path, monkeypatch, capfd):
    # stderr goes to the log while the player runs; Textual's report of a crash went
    # there too, and the window just vanished
    import os
    from bbmp import paths
    from bbmp.tui import app as app_mod
    monkeypatch.setattr(paths, "CACHE_DIR", tmp_path)

    class Crashes:
        return_code = 1

        def __init__(self, **kwargs):
            pass

        def run(self):
            os.write(2, b"Traceback (most recent call last):\nKeyError: 'boom'\n")
    monkeypatch.setattr(app_mod, "ModlandApp", Crashes)
    assert app_mod.run() == 1
    err = capfd.readouterr().err
    assert "KeyError: 'boom'" in err and "stopped because of an error" in err
    assert "boom" in (tmp_path / "bbmp.log").read_text()


def test_esc_leaves_the_search_box(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            await pilot.press("slash", *"beep")
            assert app.focused.id == "q"
            table = app.query_one("#search-songs")
            assert await settle(pilot, lambda: table.row_count >= 1)
            await pilot.press("escape")
            assert app.focused is table
            await pilot.press("space")                  # a key for the player again
            assert app.query_one("#q").value == "beep"
    run(go())



async def _play_a_song(pilot, app):
    await select_format(pilot, app, "Protracker")
    await pilot.press("enter")
    await pilot.pause(0.3)
    await pilot.press("enter")
    await pilot.pause(0.5)
    await pilot.press("enter")
    assert await settle(pilot, lambda: app.player.status().state == "playing")


def test_favorite_on_an_empty_list_marks_the_playing_song(catalog, fetcher, tmp_path):
    """f in a search that found nothing (or an empty Favorites list) takes the song that
    plays; it closed the player, there being no row to mark (7 October 2026)."""
    from textual.widgets import Tabs

    async def go():
        lib = Library(tmp_path / "user.db")
        app = make_app(catalog, fetcher, tmp_path, library=lib,
                       settings=Settings(resume=False))
        async with app.run_test(size=(130, 40)) as pilot:
            await _play_a_song(pilot, app)
            playing = app.player.status().song
            await pilot.press("2", *"zzzz", "enter")
            table = app.query_one("#search-songs")
            # Enter with no results goes on to the tab bar, as the README says
            assert await settle(pilot, lambda: isinstance(app.focused, Tabs))
            assert table.row_count == 0
            table.focus()                       # an empty list focused, as Tab can do
            await pilot.pause(0.1)
            await pilot.press("f")
            await pilot.pause(0.3)
            assert app.is_running
            assert lib.favorite_keys() == {playing.key}
            await pilot.press("4", "right", "f")    # Favorites, into an empty list
            await pilot.pause(0.3)
            assert app.is_running
    run(go())


def test_enter_in_the_search_box_goes_to_the_results(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 40)) as pilot:
            await pilot.press("slash", *"beep", "enter")
            table = app.query_one("#search-songs")
            assert await settle(pilot, lambda: table.row_count >= 1)
            await pilot.pause(0.2)
            assert app.focused is table
    run(go())

def test_updating_the_catalogs_leaves_the_player_to_use(catalog, fetcher, tmp_path, monkeypatch):
    import threading
    import time as time_
    from bbmp import catalogs
    from bbmp.tui import app as app_mod
    release = threading.Event()
    calls = []

    def slow_update(settings, progress=lambda s: None, **kw):
        calls.append(1)
        progress("Downloading Modland's list...")
        release.wait(5)
        return 1
    monkeypatch.setattr(catalogs, "update", slow_update)
    monkeypatch.setattr(app_mod, "Index", lambda *a, **k: catalog)

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            await pilot.press("u")
            now = app.query_one("#now")
            assert await settle(pilot, lambda: "Modland's list" in str(now.border_subtitle))
            await pilot.press("3")                      # keys still work meanwhile
            assert app.query_one("#tabs").active == "queue"
            await pilot.press("u")                      # once is enough
            await pilot.pause(0.2)
            assert len(calls) == 1
            release.set()
            assert await settle(pilot, lambda: not str(now.border_subtitle or ""), 20)
            assert await settle(pilot, lambda: any("up to date" in str(n.message)
                                                   for n in app._notifications), 20)
    run(go())


def test_in_a_flatpak_the_line_for_the_bbmp_command_is_shown_once(catalog, fetcher, tmp_path,
                                                                 monkeypatch):
    """A Flatpak may not write to ~/.local/bin: the player shows the line that makes the
    command, once, and makes nothing itself."""
    from bbmp import command
    home = tmp_path / "home"
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("FLATPAK_ID", command.APP_ID)
    monkeypatch.delenv("APPIMAGE", raising=False)

    async def go():
        settings = Settings.load(tmp_path / "settings.json")
        settings.resume = False
        settings.asked_downloads = True                  # only this question
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        app._ask = True                                  # as when the player is started for real
        async with app.run_test(size=(130, 44), notifications=True) as pilot:
            assert await settle(pilot, lambda: any(command.line() in n.message
                                                   for n in app._notifications))
            assert settings.asked_command
            assert not (home / ".local/bin/bbmp").exists()

        again = make_app(catalog, fetcher, tmp_path, settings=Settings.load(tmp_path / "settings.json"))
        again._ask = True
        async with again.run_test(size=(130, 44), notifications=True) as pilot:
            await pilot.pause(0.5)
            assert not any(command.line() in n.message for n in again._notifications)
    run(go())


def test_in_an_appimage_the_bbmp_command_is_offered_once(catalog, fetcher, tmp_path, monkeypatch):
    from bbmp import command
    home = tmp_path / "home"
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.delenv("FLATPAK_ID", raising=False)
    monkeypatch.setattr(command, "in_flatpak", lambda: False)
    monkeypatch.setenv("APPIMAGE", str(tmp_path / "bbmp.AppImage"))

    async def go():
        settings = Settings.load(tmp_path / "settings.json")
        settings.resume = False
        settings.asked_downloads = True
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        app._ask = True
        async with app.run_test(size=(130, 44)) as pilot:
            assert await settle(pilot, lambda: type(app.screen).__name__ == "Confirm")
            shown = app.export_screenshot().replace("&#160;", " ")
            assert "typing bbmp in a terminal" in shown
            await pilot.press("enter")                   # yes
            assert await settle(pilot, lambda: (home / ".local/bin/bbmp").is_file())
            assert settings.asked_command

        again = make_app(catalog, fetcher, tmp_path, settings=Settings.load(tmp_path / "settings.json"))
        again._ask = True
        async with again.run_test(size=(130, 44)) as pilot:
            await pilot.pause(0.5)
            assert type(again.screen).__name__ != "Confirm"      # asked once
    run(go())


def test_the_look_ahead_follows_the_cursor(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await select_format(pilot, app, "Protracker")
            await pilot.press("right")
            artists, table = app._levels[-1], app.query_one("#browse-songs")
            assert await settle(pilot, lambda: artists.option_count == 2)
            first = artists.get_option_at_index(0).id.split("\x1e")[1]
            second = artists.get_option_at_index(1).id.split("\x1e")[1]
            assert await settle(pilot, lambda: {s.artist for s in table.songs} == {first})
            artists.highlighted = 1
            assert await settle(pilot, lambda: {s.artist for s in table.songs} == {second})
            artists.highlighted = 0
            assert await settle(pilot, lambda: {s.artist for s in table.songs} == {first})
    run(go())


def test_the_format_note_keeps_its_height_and_says_when_it_is_cut(catalog, fetcher, tmp_path, monkeypatch):
    """The note under Browse's lists is as high whatever it says (a note that grew
    resized the lists, which were then drawn from nothing), and a note too long ends
    in "…"."""
    from bbmp import formats_info
    monkeypatch.setitem(formats_info.NOTES, "TFMX", ("faults", "1 of 2", "words " * 400))

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            note = app.query_one("#format-note")
            heights = set()
            for fmt in ("Protracker", "TFMX", "Fasttracker 2"):
                await select_format(pilot, app, fmt)
                assert await settle(pilot, lambda: app._fmt == fmt)
                await pilot.pause(0.05)
                heights.add(note.region.height)
                if fmt == "TFMX":
                    shown = str(note.render())
                    assert shown.rstrip().endswith("…") and len(shown.splitlines()) == 5
            assert heights == {5}
    run(go())


def test_the_footer_is_made_again_only_when_its_keys_change(catalog, fetcher, tmp_path):
    from textual.widgets import Footer
    from bbmp.tui.widgets import KeysFooter

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            footer = app.query_one(KeysFooter)
            assert await settle(pilot, lambda: len(footer.children) > 3)
            await pilot.pause(0.1)
            made = []
            orig = Footer.recompose

            async def counted(self):
                made.append(1)
                return await orig(self)
            footer.recompose = counted.__get__(footer)
            formats = await browse_ready(pilot, app)
            made.clear()
            for key in ("right", "right", "left", "left"):    # formats, artists, songs, back
                await pilot.press(key)
                await pilot.pause(0.05)
            await pilot.pause(0.1)
            assert app.focused is formats and made == []
            # a key that comes and goes: "c" changes the collection with more than one
            assert "Collection" not in app.export_screenshot()
            app._collections = [("modland", "Modland"), ("hvsc", "HVSC")]
            app.refresh_bindings()
            assert await settle(pilot, lambda: made and "Collection" in app.export_screenshot())
    run(go())


def test_the_playlists_tab_fills_its_songs_once(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            await pilot.pause(0.2)
            table = app.query_one("#fav-songs")
            fills = []
            orig = table.set_songs
            table.set_songs = lambda *a, **k: (fills.append(1), orig(*a, **k))
            await pilot.press("4")
            await pilot.pause(0.4)
            assert len(fills) == 1
            # history: asked of the library again only once something has played
            asked = []
            history = app.library.history
            app.library.history = lambda *a, **k: (asked.append(1), history(*a, **k))[1]
            for key in ("6", "1", "6"):
                await pilot.press(key)
                await pilot.pause(0.1)
            assert len(asked) == 1
            app._played(catalog.by_path("Protracker/Tester/beep.mod"))
            await pilot.press("1", "6")
            assert await settle(pilot, lambda: app.query_one("#hist-songs").row_count == 1)
            assert len(asked) == 2
    run(go())


def test_a_kept_song_is_marked_from_its_folder_read_once(catalog, fetcher, tmp_path):
    from bbmp.tui.widgets import SongTable

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            await pilot.pause(0.2)
            app.fetcher.kept_dir = tmp_path / "kept"
            song = catalog.by_path("Protracker/Tester/beep.mod")
            other = catalog.by_path("Protracker/Tester/beep two.mod")
            place = app.fetcher._places(song.key)[0]
            place.parent.mkdir(parents=True)
            place.write_bytes(b"x")
            table = app.query_one("#browse-songs", SongTable)

            def marks():
                table.set_songs([song, other], set())
                return [str(table.get_row_at(i)[0]) for i in range(2)]
            assert marks() == ["", ""]              # nothing is kept: not looked for
            app._kept = ["something"]
            app._kept_seen()
            assert marks() == ["⤓", ""]
            place.unlink()
            assert marks() == ["⤓", ""]             # as read, until what is kept changes
            app._kept_seen()
            assert marks() == ["", ""]
            app._kept = []
    run(go())


def test_the_home_mirror_is_set_in_settings(catalog, fetcher, tmp_path, monkeypatch):
    monkeypatch.delenv("BBMP_MIRROR")

    async def go():
        settings = Settings.load(tmp_path / "settings.json")
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(120, 40)) as pilot:
            await pilot.press("0")
            rows = app.query_one("#settings-list")
            rows.highlighted = next(i for i in range(rows.option_count)
                                    if rows.get_option_at_index(i).id == "mirror")
            await pilot.press("enter")
            await pilot.press(*"192.168.1.10:8088", "enter")
            assert await settle(pilot, lambda: settings.mirror == "192.168.1.10:8088")
            await pilot.press("enter", "backspace", "enter")     # and off again
            assert await settle(pilot, lambda: settings.mirror == "")
        assert Settings.load(tmp_path / "settings.json").mirror == ""
    run(go())


def test_help_stays_shown_and_escape_leaves_an_empty_search(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            tabs = app.query_one("#tabs")
            await pilot.press("?")
            await pilot.pause(0.3)
            assert tabs.active == "helptab"             # it went straight back to Browse
            await pilot.press("2")
            assert await settle(pilot, lambda: app.focused is app.query_one("#q"))
            await pilot.press("escape")                 # no results: to the tab bar
            await pilot.pause(0.1)
            assert app.focused is not app.query_one("#q")
            await pilot.press("3")
            assert await settle(pilot, lambda: tabs.active == "queue")
            assert app.query_one("#q").value == ""
    run(go())


def test_the_files_tree_opens_and_closes_with_the_arrows(catalog, fetcher, tmp_path):
    music = tmp_path / "Music"
    (music / "AHX/AceMan").mkdir(parents=True)
    (music / "AHX/Other").mkdir()
    settings = Settings(resume=False, music_dir=str(music))

    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(120, 40)) as pilot:
            await pilot.press("5")
            tree = app.query_one("#files-tree")
            assert await settle(pilot, lambda: app.focused is tree and tree.root.children)
            tree.move_cursor(tree.root.children[0])            # AHX, closed
            ahx = tree.cursor_node
            await pilot.press("right")                         # opens it
            assert await settle(pilot, lambda: ahx.is_expanded and len(ahx.children) == 2)
            await pilot.press("right")                         # into it: AceMan
            assert tree.cursor_node is ahx.children[0]
            await pilot.press("left")                          # out: AHX
            assert tree.cursor_node is ahx and ahx.is_expanded
            await pilot.press("left")                          # closes it
            assert not ahx.is_expanded
            await pilot.press("enter")                         # Enter opens it again
            assert await settle(pilot, lambda: ahx.is_expanded)
            await pilot.press("enter")                         # and closes it
            assert await settle(pilot, lambda: not ahx.is_expanded)
    run(go())
