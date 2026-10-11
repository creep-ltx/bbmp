# SPDX-License-Identifier: GPL-3.0-or-later
"""What makes the screen answer sooner (the speed audit of 11 October 2026): the look
ahead filled at once, no sliding tab underline, Now playing redrawn by the row, the slow
lists asked in the background, lists kept."""

from test_tui import browse_ready, make_app, run, settle
from bbmp.tui.browse import id_step


def test_the_tab_underline_jumps_instead_of_sliding(catalog, fetcher, tmp_path):
    """The slide took ~0.3 s of frames at every tab switch. (Textual's private method is
    wrapped: an upgrade that renames it shows here.)"""
    from textual.widgets import Tabs
    from textual.widgets._tabs import Underline

    assert getattr(Tabs._highlight_active, "_no_slide", False)

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await browse_ready(pilot, app)
            tabs = app.query_one("#tabs").query_one(Tabs)
            line = tabs.query_one(Underline)
            for key in "394":
                await pilot.press(key)
                seen = set()
                for _ in range(20):                     # 0.4 s: longer than a slide
                    seen.add(line.highlight_start)
                    await pilot.pause(0.02)
                tab = tabs.active_tab
                start, end = tab.virtual_region.shrink(tab.styles.gutter).column_span
                assert (line.highlight_start, line.highlight_end) == (start, end)
                assert len(seen) <= 2                   # where it was, where it is: no steps between
    run(go())

def test_the_look_ahead_is_filled_at_once_after_a_lone_key(catalog, fetcher, tmp_path):
    """It waited 0.12 s after every key; now only while keys come fast."""
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await browse_ready(pilot, app)
            formats = app._levels[-1]
            await pilot.pause(0.3)

            def ahead():
                return app._path + [id_step(formats.highlighted_option.id)]
            await pilot.press("down")
            await pilot.pause()
            assert app._preview_of == ahead()           # at once, not 0.12 s later
            await pilot.pause(0.3)
            await pilot.press("down", "down")           # fast: the second one waits
            await pilot.pause()
            assert app._preview_of != ahead()
            assert await settle(pilot, lambda: app._preview_of == ahead(), 1)
    run(go())


def test_now_playing_redraws_only_the_rows_that_changed(catalog, fetcher, tmp_path):
    """The meters change twelve times a second, the time bar once a second, the title
    with the song: as one widget, all four rows were sent each time."""
    from dataclasses import replace
    from bbmp.backends.base import TrackInfo
    from bbmp.player import Status

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await browse_ready(pilot, app)
            app._terminal_behind = lambda: True       # the tick draws nothing: only what this test shows
            now = app.query_one("#now")
            rows = now._rows
            drawn = []
            for i, row in enumerate(rows):
                row.update = (lambda i, real: lambda c, layout=True: (
                    drawn.append(i), real(c, layout=layout)))(i, row.update)
            song = catalog.by_path("Protracker/Tester/beep.mod")
            st = Status(state="playing", song=song, info=TrackInfo(duration=60.0),
                        elapsed=1.0, levels=(0.5, 0.5), queue_pos=0, queue_len=1)
            now.show(st, False)
            assert sorted(drawn) == [0, 1, 2, 3]
            drawn.clear()
            now.show(replace(st, levels=(0.9, 0.2)), False)
            assert drawn == [3]                       # the meters only
            drawn.clear()
            now.show(replace(st, levels=(0.9, 0.2), elapsed=2.0), False)
            assert drawn == [2]                       # the time bar only
            await pilot.pause()
            assert "beep" in str(rows[0].render()) and "00:02" in str(rows[2].render())
            # the short terminal: three rows, the format beside the title
            await pilot.resize_terminal(120, 24)
            await pilot.pause(0.2)
            assert not rows[3].display or rows[3].region.height == 0
            now.show(replace(st, elapsed=3.0), False)
            await pilot.pause()
            assert "Protracker" in str(rows[0].render()) and "00:03" in str(rows[1].render())
            now.border_subtitle = "Saving: 3 of 9"
            assert now.border_title == "Saving: 3 of 9"
    run(go())


def _gated(monkeypatch, owner, name):
    """`owner.name` made to wait until the returned Event is set (at most 10 s)."""
    import threading
    gate = threading.Event()
    real = getattr(owner, name)

    def slow(*args, **kwargs):
        gate.wait(10)
        return real(*args, **kwargs)
    monkeypatch.setattr(owner, name, slow)
    return gate


def test_platforms_are_counted_in_the_background(catalog, fetcher, tmp_path, monkeypatch):
    """The first look at Platforms froze the screen 720 ms: now the look says "…" and
    is filled when the counts come; going in meanwhile fills the list one is in."""
    from test_tui import put
    gate = _gated(monkeypatch, type(catalog), "view_counts")

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await browse_ready(pilot, app)
            await pilot.press("left")                   # out to the ways
            await pilot.pause(0.2)
            await pilot.press("down")                   # onto Platforms
            await pilot.pause(0.1)
            preview = app._preview_list()
            assert preview.waiting and str(preview.get_option_at_index(0).prompt) == "…"
            await pilot.press("right")                  # in, while it is counted
            await pilot.pause(0.1)
            ways = app._levels[-2]
            assert app._levels[-1].waiting and app.focused is app._levels[-1]
            gate.set()
            platforms = await browse_ready(pilot, app)
            assert platforms.highlighted == 0 and platforms.highlighted_option.id
            assert await settle(pilot, lambda: app._preview_of == app._path + [
                id_step(platforms.highlighted_option.id)])
            # kept: out and onto it again shows the very same list at once
            options = app._built(app._path)[1]
            await pilot.press("left")
            await pilot.pause(0.1)
            assert app._preview_list().get_option_at_index(0) is options[0]
            assert app.focused is ways
            put(ways, ("menu", "chips"))                # Chips: counted in the background too
            await pilot.pause(0.05)
            assert await settle(pilot, lambda: not app._preview_list().waiting
                                and app._preview_list().option_count > 0)
    run(go())


def test_history_is_looked_up_in_the_background(catalog, fetcher, tmp_path, monkeypatch):
    """The History tab looked its 300 songs up on the screen's thread (310 ms)."""
    from bbmp.library import Library
    lib = Library(tmp_path / "user.db")
    for path in ("Protracker/Tester/beep.mod", "Protracker/Tester/beep two.mod"):
        lib.finish_play(lib.start_play(catalog.by_path(path)), 60)
    gate = _gated(monkeypatch, Library, "history")

    async def go():
        app = make_app(catalog, fetcher, tmp_path, library=lib)
        async with app.run_test(size=(120, 40)) as pilot:
            await browse_ready(pilot, app)
            await pilot.press("6")
            await pilot.pause(0.1)
            table = app.query_one("#hist-songs")
            assert app.query_one("#tabs").active == "history"      # the screen goes on
            assert table.row_count == 0 and table.border_title == "…"
            gate.set()
            assert await settle(pilot, lambda: table.row_count == 2)
            assert not table.border_title
    run(go())


def test_an_answer_asked_for_again_meanwhile_is_not_shown(catalog, fetcher, tmp_path):
    """History: the answer to an earlier ask that comes late is left out; Stats: only the
    period asked last is shown."""
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await browse_ready(pilot, app)
            song = catalog.by_path("Protracker/Tester/beep.mod")
            app._history_asked = (5, id(app.index))
            app._history_answered((4, id(app.index)), [song])     # late: not shown
            assert app.query_one("#hist-songs").row_count == 0
            app._history_answered((5, id(app.index)), [song])
            assert app.query_one("#hist-songs").row_count == 1
            shown = []
            app.query_one("#stats-body").update = shown.append
            app._stats_asked = ("month", 7)
            app._stats_answered(("week", 6), {})
            assert shown == []
    run(go())


def test_the_artist_page_is_made_in_the_background(catalog, fetcher, tmp_path, monkeypatch):
    """m went to the artist's page after 290 ms of a frozen screen; now the tab comes at
    once with the name and "…", and the page when it is made."""
    from bbmp import pages
    gate = _gated(monkeypatch, pages, "artist_page")

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            await browse_ready(pilot, app)
            app.player.play_list([catalog.by_path("Protracker/Other Guy/coop-Tester/duet.mod")])
            assert await settle(pilot, lambda: app.player.status().state == "playing")
            await pilot.press("m")
            await pilot.pause(0.1)
            assert app.query_one("#tabs").active == "artist"
            assert app.query_one("#artist-scroll").border_title == "Other Guy …"
            gate.set()
            assert await settle(pilot, lambda: app._page is not None
                                and app._page.title == "Other Guy")
            assert app.query_one("#artist-songs").row_count > 0
    run(go())


def test_an_artists_songs_are_looked_up_in_the_background(catalog, fetcher, tmp_path, monkeypatch):
    """Among every artist, each one the cursor stopped on was looked up on the screen's
    thread (40 ms); now in the background, and going in meanwhile lands on the first."""
    from test_tui import put
    gate = _gated(monkeypatch, type(catalog), "artist_songs")

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await browse_ready(pilot, app)
            await pilot.press("left")
            put(app._levels[-1], ("menu", "artists"))
            await pilot.press("right")
            artists = await browse_ready(pilot, app)
            put(artists, ("artist", "Tester"))
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: table.border_title == "Tester …", 1)
            assert table.row_count == 0
            await pilot.press("right")                  # in, before the songs came
            gate.set()
            assert await settle(pilot, lambda: table.row_count > 0)
            assert table.cursor_row == 0 and app.focused is table
            assert "Tester (" in table.border_title
            assert "Song" == app.query_one("#song-info").border_title
    run(go())


def test_a_terminal_that_is_behind_gets_fewer_pictures(catalog, fetcher, tmp_path):
    """While the terminal was behind in the last seconds the visualiser is drawn six
    times a second instead of twelve and the search box's cursor doesn't blink; the
    demo stands still while paused."""
    from bbmp.player import Status

    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            await browse_ready(pilot, app)
            vis = app.query_one("#visual")
            vis.set_mode("spectrum")
            shown = []
            vis.show = shown.append
            behind = [False]
            app._terminal_behind = lambda: behind[0]
            for _ in range(6):
                app._tick()
            assert len(shown) == 6 and app.query_one("#q").cursor_blink
            behind[0] = True
            app._tick()                                 # behind: nothing drawn
            behind[0] = False
            shown.clear()
            for _ in range(6):
                app._tick()
            assert len(shown) == 3                      # caught up, but spared a while
            assert not app.query_one("#q").cursor_blink
            app._behind_at -= app.BEHIND_FOR
            shown.clear()
            for _ in range(4):
                app._tick()
            assert len(shown) == 4 and app.query_one("#q").cursor_blink
            del vis.show
            # the demo, paused: drawn once, then it stands still
            behind[0] = True                            # the tick leaves it to this test
            vis.set_mode("demo")
            await pilot.pause(0.1)
            song = catalog.by_path("Protracker/Tester/beep.mod")
            drawn = []
            vis.update = lambda content, layout=True: drawn.append(content)
            for elapsed in (1.0, 1.1, 1.2):
                vis.show(Status(state="paused", song=song, elapsed=elapsed, wave=bytes(4096)))
                await pilot.pause(0.1)                  # the demo moves with time
            assert len(drawn) == 1
            vis.show(Status(state="playing", song=song, elapsed=1.3, wave=bytes(4096)))
            assert len(drawn) == 2
    run(go())
