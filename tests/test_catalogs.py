# SPDX-License-Identifier: GPL-3.0-or-later
"""A catalog per collection: choosing them, HVSC on its own, and updating."""

import time

import pytest

from conftest import HVSC_COUNT, MODLAND_COUNT, RAW_LIST, SIZES, modland_listing
from bbmp import catalogs, index as index_mod, paths
from bbmp.collection import hvsc, modland
from bbmp.index import Index
from bbmp.settings import Settings

SONGLENGTHS = (
    "[Database]\n"
    "; /MUSICIANS/H/Hubbard_Rob/Commando.sid\n"
    "00000000000000000000000000000c=3:55.594 1:01.288 0:06\n"
    "; /GAMES/A-F/Bionic.sid\n"
    "00000000000000000000000000000d=2:00\n")


def files_in(folder):
    """The files in a catalog folder, but for the one that updates hold (catalogs.py)."""
    return sorted(f.name for f in folder.iterdir() if f.name not in (".updating", "change-days.json"))


@pytest.fixture
def lists(monkeypatch, tmp_path):
    """Modland's lists and HVSC's own, as the servers would give them, counted."""
    asked = {"modland": 0, "hvsc": 0}

    def modland_listing_():
        asked["modland"] += 1
        return modland_listing()

    def own(progress=lambda s: None, folder=None):
        asked["hvsc"] += 1
        tunes = hvsc.read_songlengths(SONGLENGTHS)
        return index_mod.Listing([hvsc.parse(p, m, 0) for p, m, _ in tunes],
                                 [(p, l) for p, _, l in tunes], [], 7)
    monkeypatch.setattr(modland.Modland, "listing", lambda self, progress=None: modland_listing_())
    monkeypatch.setattr(hvsc, "own_listing", own)
    monkeypatch.setattr(paths, "INDEX_DB", tmp_path / "old-index.db")
    from bbmp import snapshot
    monkeypatch.setattr(snapshot, "where", lambda: tmp_path / "no snapshot")
    return asked


def test_songlengths_is_read():
    tunes = hvsc.read_songlengths(SONGLENGTHS)
    assert tunes[0] == ("MUSICIANS/H/Hubbard_Rob/Commando.sid", "00000000000000000000000000000c",
                        "3:55.594 1:01.288 0:06")
    assert len(tunes) == 2


def test_the_first_start_asks_and_the_choice_is_kept(tmp_path, lists):
    folder = tmp_path / "catalogs"
    s = Settings()
    assert catalogs.first_start(s, folder) and catalogs.chosen(s, folder) == ["modland", "hvsc"]
    catalogs.choose(s, ["modland", "hvsc"], folder=folder)
    assert files_in(folder) == ["hvsc.db", "modland.db"]
    assert s.collections == ["modland", "hvsc"] and not catalogs.first_start(s, folder)
    assert lists == {"modland": 1, "hvsc": 0}      # HVSC came from Modland's list
    ix = Index(folder)
    assert ix.song_count() == MODLAND_COUNT and ix.song_count("hvsc") == HVSC_COUNT


def test_hvsc_alone_comes_from_its_own_list(tmp_path, lists):
    folder = tmp_path / "catalogs"
    s = Settings()
    catalogs.choose(s, ["hvsc"], folder=folder)
    assert files_in(folder) == ["hvsc.db"]
    assert lists == {"modland": 0, "hvsc": 1}      # Modland's list was not downloaded
    ix = Index(folder)
    assert ix.song_count() == 2                     # on its own, it is every collection
    key = "modland:HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid"
    song = ix.resolve(key)                          # a tune is still known by Modland's key
    assert song.collection == "hvsc" and song.key == key
    assert ix.lengths(key) == [235.594, 61.288, 6.0]
    assert [s.title for s in ix.search("commando")] == ["Commando"]


def test_turning_a_collection_off_and_on(tmp_path, lists):
    folder = tmp_path / "catalogs"
    s = Settings()
    catalogs.choose(s, ["modland", "hvsc"], folder=folder)
    off = catalogs.choose(s, ["hvsc"], folder=folder)
    assert off == ["modland"] and files_in(folder) == ["hvsc.db"]
    assert lists["hvsc"] == 1                       # HVSC made anew from its own list
    catalogs.choose(s, ["modland", "hvsc"], folder=folder)
    assert files_in(folder) == ["hvsc.db", "modland.db"]
    assert Index(folder).song_count("hvsc") == HVSC_COUNT     # from Modland's list again


def test_updates_come_when_they_are_due(tmp_path, lists):
    folder = tmp_path / "catalogs"
    s = Settings(update_every="week")
    catalogs.choose(s, ["modland"], folder=folder)
    assert not catalogs.due(s, folder)
    s.last_update = 0
    index_mod.build({"modland": modland_listing()}, folder,
                    built={"modland": int(time.time()) - 8 * 86400})
    assert catalogs.due(s, folder)
    s.update_every = "month"
    assert not catalogs.due(s, folder)
    s.update_every = "never"
    assert not catalogs.due(s, folder)
    s.update_every = "day"
    before = lists["modland"]
    catalogs.update(s, folder=folder)
    assert lists["modland"] == before + 1 and not catalogs.due(s, folder)


def test_updating_modland_brings_hvsc_along(tmp_path, lists):
    folder = tmp_path / "catalogs"
    s = Settings()
    catalogs.choose(s, ["modland", "hvsc"], folder=folder)
    old = (folder / "hvsc.db").stat().st_mtime_ns
    time.sleep(0.01)
    catalogs.update(s, only=["modland"], folder=folder)
    assert (folder / "hvsc.db").stat().st_mtime_ns != old
    assert lists["hvsc"] == 0


def test_the_player_asks_at_its_first_start(tmp_path, lists, monkeypatch, fetcher):
    import asyncio

    from bbmp.audio import NullOutput
    from bbmp.library import Library
    from bbmp.tui import ModlandApp
    monkeypatch.setattr(paths, "CATALOG_DIR", tmp_path / "catalogs")
    settings = Settings(resume=False)

    async def go():
        app = ModlandApp(None, fetcher, Library(tmp_path / "user.db"), NullOutput(realtime=True),
                         settings=settings, session_path=tmp_path / "s.json")
        async with app.run_test(size=(130, 44)) as pilot:
            for _ in range(100):
                if app.screen.query("#choose"):
                    break
                await pilot.pause(0.05)
            choose = app.screen.query_one("#choose")
            rows = [str(choose.get_option_at_index(i).prompt) for i in range(choose.option_count)]
            assert rows[0].startswith("✓ Modland") and rows[1].startswith("✓ HVSC")
            assert rows[-1] == "Done"
            await pilot.press("down", "enter")          # HVSC off
            assert str(choose.get_option_at_index(1).prompt).startswith("  HVSC")
            choose.highlighted = choose.option_count - 1
            await pilot.press("enter")                  # Done
            for _ in range(200):
                if app.player is not None and getattr(app, "_placed", False) and app._levels[-1].option_count > 3:
                    break
                await pilot.pause(0.05)
            assert settings.collections == ["modland"]
            assert files_in(tmp_path / "catalogs") == ["modland.db"]
            assert app.index.song_count() == MODLAND_COUNT

            await pilot.press("0")                      # Settings: the Collections group
            rows = app.query_one("#settings-list")
            ids = [rows.get_option_at_index(i).id for i in range(rows.option_count)]
            rows.highlighted = ids.index("update_every")
            await pilot.press("enter")
            assert settings.update_every == "2weeks"
            rows.highlighted = ids.index("collections")
            await pilot.press("enter")
            assert await _until(pilot, lambda: bool(app.screen.query("#choose")))
            choose = app.screen.query_one("#choose")
            await pilot.press("down", "enter")          # HVSC on
            choose.highlighted = choose.option_count - 1
            await pilot.press("enter")
            assert await _until(pilot, lambda: settings.collections == ["modland", "hvsc"]
                                and app.index.song_count("hvsc") == HVSC_COUNT)
    asyncio.run(go())


async def _until(pilot, cond, timeout=10.0):
    for _ in range(int(timeout / 0.05)):
        if cond():
            return True
        await pilot.pause(0.05)
    return False


LIB = ('[{"st":"0BiT - Fax Spider 2.1 kg","rg":"0BiT","sn":"Fax Spider 2.1 kg","n":1,'
       '"mdt":"caramel condition","path":"/kgm/!Others/0BiT - Fax Spider 2.1 kg.xm"},'
       '{"rg":"TSRh","sn":"Opus Creator 9.01 kg","path":"/kgm/TSRh/TSRh - Opus Creator 9.01 kg.xm"},'
       '{"rg":"RED","sn":"Old one","path":"/kgm/RED/RED - old.mod"},'
       '{"path":"/kgm/../etc/passwd"},{"path":"/elsewhere/x.xm"}]').encode()


def test_keygen_music_is_read_from_the_sites_list():
    from bbmp.collection import keygenmusic
    listing = keygenmusic.read_list(LIB, 1716600311)
    entries = list(listing.entries)
    assert [(e.path, e.format, e.artist, e.name) for e in entries] == [
        ("!Others/0BiT - Fax Spider 2.1 kg.xm", "Fasttracker 2", "0BiT", "Fax Spider 2.1 kg.xm"),
        ("TSRh/TSRh - Opus Creator 9.01 kg.xm", "Fasttracker 2", "TSRh", "Opus Creator 9.01 kg.xm"),
        ("RED/RED - old.mod", "Protracker", "RED", "Old one.mod")]
    assert listing.changes == [] and listing.dated == 1716600311
    later = keygenmusic.read_list(LIB, 2, before=["RED/RED - old.mod"])
    assert [p for _, kind, p in later.changes if kind == "added"] == [
        "!Others/0BiT - Fax Spider 2.1 kg.xm", "TSRh/TSRh - Opus Creator 9.01 kg.xm"]
    kgm = keygenmusic.KeygenMusic()
    url = kgm.url("TSRh/TSRh - Opus Creator 9.01 kg.xm")
    assert url == "https://keygenmusic.tk/kgm/TSRh/TSRh%20-%20Opus%20Creator%209.01%20kg.xm"
    assert kgm.path_in(url) == "TSRh/TSRh - Opus Creator 9.01 kg.xm"
    assert kgm.path_in("https://keygenmusic.tk/#track=RED/RED%20-%20old.mod") == "RED/RED - old.mod"
    assert kgm.path_in("https://example.com/x.xm") is None


def test_keygen_music_is_a_collection_to_choose(tmp_path, lists, monkeypatch):
    from bbmp.collection import keygenmusic
    from bbmp import collection as registry
    registry.register(keygenmusic.KeygenMusic())
    monkeypatch.setattr(keygenmusic.KeygenMusic, "listing",
                        lambda self, progress=None: keygenmusic.read_list(LIB, 5))
    folder = tmp_path / "catalogs"
    s = Settings()
    catalogs.choose(s, ["modland", "keygenmusic"], folder=folder)
    ix = Index(folder)
    assert ix.song_count("keygenmusic") == 3
    assert [s.key for s in ix.songs("Fasttracker 2", "TSRh", "keygenmusic")] == [
        "keygenmusic:TSRh/TSRh - Opus Creator 9.01 kg.xm"]
    assert any(s.collection == "keygenmusic" for s in ix.search("fax spider"))
    # in every collection's lists, Modland's formats and Keygen Music's are one
    assert dict((f, n) for f, n, _ in ix.formats())["Fasttracker 2"] == (
        ix.formats("modland") and dict((f, n) for f, n, _ in ix.formats("modland"))
        .get("Fasttracker 2", 0) + 2)


def test_a_song_a_collection_has_twice_shows_twice(tmp_path):
    """Only a file two collections share is shown once; a collection's own copies, in
    different places, all show."""
    same = "0123456789abcdef0123456789abcdef"
    raw = (f"{same} Protracker/Tester/twice.mod\n{same} Protracker/Other/twice.mod\n"
           f"{same} Protracker/Tester/Album/twice.mod\n").encode()
    folder = tmp_path / "catalogs"
    index_mod.build({"modland": modland.read_list(raw)}, folder)
    ix = Index(folder)
    assert len(ix.search("twice")) == 3
    assert len(ix.songs("Protracker", "Tester")) == 2
    assert len(ix.random_song(fmt="Protracker") and ix.search("twice", collection="modland")) == 3


def test_only_chosen_collections_are_updated(tmp_path, lists, monkeypatch, capsys):
    folder = tmp_path / "catalogs"
    s = Settings()
    catalogs.choose(s, ["hvsc"], folder=folder)
    asked = dict(lists)
    # Modland is not chosen: naming it doesn't make its catalog
    assert catalogs.update(s, only=["modland"], folder=folder).songs == 0
    assert files_in(folder) == ["hvsc.db"] and lists == asked
    # nothing chosen at all: nothing is made
    s.collections = []
    assert catalogs.update(s, folder=folder) == catalogs.Updated()
    assert files_in(folder) == ["hvsc.db"] and lists == asked

    # and the command line says why, and how to choose it
    import argparse
    from bbmp import cli
    s.collections = ["hvsc"]
    monkeypatch.setattr(Settings, "load", classmethod(lambda cls, path=None: s))
    monkeypatch.setattr(paths, "CATALOG_DIR", folder)
    assert cli.cmd_update(argparse.Namespace(collections=["modland"])) == 1
    said = capsys.readouterr().err
    assert "Modland is not one of the collections you have" in said
    assert "bbmp collections --use hvsc modland" in said
    assert files_in(folder) == ["hvsc.db"] and lists == asked


def test_one_update_at_a_time(tmp_path, lists):
    import fcntl
    import os
    import threading

    folder = tmp_path / "catalogs"
    s = Settings()
    catalogs.choose(s, ["modland"], folder=folder)
    # another thread of this player is updating
    holding, done = threading.Event(), threading.Event()

    def other():
        with catalogs.updating(folder):
            holding.set()
            done.wait(5)
    t = threading.Thread(target=other)
    t.start()
    holding.wait(5)
    try:
        with pytest.raises(catalogs.Busy):
            catalogs.update(s, folder=folder)
        with pytest.raises(catalogs.Busy):
            catalogs.choose(s, ["modland", "hvsc"], folder=folder)
    finally:
        done.set()
        t.join()
    assert s.collections == ["modland"]
    # another bbmp is updating: it holds the file
    fd = os.open(folder / ".updating", os.O_RDWR)
    fcntl.flock(fd, fcntl.LOCK_EX)
    try:
        with pytest.raises(catalogs.Busy, match="another bbmp"):
            catalogs.update(s, folder=folder)
    finally:
        os.close(fd)
    before = lists["modland"]
    catalogs.update(s, folder=folder)           # and after them, it can
    assert lists["modland"] == before + 1


def test_a_collection_that_fails_is_said_and_tried_again(tmp_path, lists, monkeypatch):
    from bbmp import collection as registry
    from bbmp.collection import keygenmusic

    registry.register(keygenmusic.KeygenMusic())
    monkeypatch.setattr(keygenmusic.KeygenMusic, "listing", lambda self, progress=None:
                        keygenmusic.read_list(b'[{"path": "/kgm/G/G - x.xm"}]'))
    folder = tmp_path / "catalogs"
    s = Settings()
    catalogs.choose(s, ["modland", "keygenmusic"], folder=folder)

    def down(self, progress=None):
        raise OSError("the site doesn't answer")
    monkeypatch.setattr(keygenmusic.KeygenMusic, "listing", down)
    s.last_update = 0
    done = catalogs.update(s, folder=folder)
    assert done.songs == MODLAND_COUNT + 1          # Modland was updated, Keygen Music kept
    assert done.failed == {"keygenmusic": "the site doesn't answer"}
    assert done.why() == "Keygen Music: the site doesn't answer"
    assert s.last_update == 0                       # so it is due again
    monkeypatch.setattr(keygenmusic.KeygenMusic, "listing", lambda self, progress=None:
                        keygenmusic.read_list(b'[{"path": "/kgm/G/G - y.xm"}]'))
    assert not catalogs.update(s, folder=folder).failed and s.last_update > 0
    assert [p for p, *_ in Index(folder).files_in("keygenmusic")] == ["G/G - y.xm"]


def test_a_build_that_fails_leaves_no_file_behind(tmp_path):
    folder = tmp_path / "catalogs"
    index_mod.build({"modland": modland_listing()}, folder)
    before = (folder / "modland.db").read_bytes()

    def broken():
        yield from modland_listing().entries
        raise OSError("the list broke off")
    with pytest.raises(OSError):
        index_mod.build({"modland": index_mod.Listing(broken())}, folder)
    assert files_in(folder) == ["modland.db"]
    assert (folder / "modland.db").read_bytes() == before


def test_hvsc_alone_says_what_changed_since_its_catalog(tmp_path, monkeypatch, hosted):
    """HVSC's own list says nothing of what changed: what differs from the catalog there
    was is what changed. Without one, nothing did (the first time, or HVSC came from
    Modland's list until now: every tune would be new)."""
    import io
    from bbmp import snapshot

    text = [SONGLENGTHS]

    class Response(io.BytesIO):
        headers = {}
    monkeypatch.setattr(hvsc, "_ask_own", lambda head=False, timeout=60: Response(
        text[0].encode("latin-1")))
    folder = tmp_path / "catalogs"
    first = hvsc.own_listing(folder=folder)
    assert len(first.entries) == 2 and first.changes == []
    # the catalog that comes with the player gives sizes, and no changes
    packed = tmp_path / "catalog.xz"
    snapshot.write(packed, hosted.path)
    monkeypatch.setattr(snapshot, "where", lambda: packed)
    again = hvsc.own_listing(folder=folder)
    assert again.changes == [] and all(e.size for e in again.entries)
    # HVSC's next release: a tune changed and one new
    index_mod.build({"hvsc": again}, folder)
    text[0] = (SONGLENGTHS.replace("0c=", "0e=")
               + "; /GAMES/G-L/New.sid\n00000000000000000000000000000f=1:00\n")
    today = time.strftime("%Y-%m-%d")
    assert sorted(hvsc.own_listing(folder=folder).changes) == [
        (today, "added", "GAMES/G-L/New.sid"),
        (today, "updated", "MUSICIANS/H/Hubbard_Rob/Commando.sid")]


def test_catalogs_in_a_folder_with_any_name(tmp_path, lists):
    folder = tmp_path / "a #1? 100%" / "catalogs"
    s = Settings()
    catalogs.choose(s, ["modland", "hvsc"], folder=folder)
    ix = Index(folder)
    assert ix.ids == ["modland", "hvsc"] and ix.song_count() == MODLAND_COUNT
    assert catalogs.chosen(s, folder) == ["modland", "hvsc"] and not catalogs.due(s, folder)
    ix.close()
    ix.close()                                  # twice does nothing


def test_songs_say_their_sid_and_the_catalogs_keep_it(tmp_path):
    from dataclasses import replace
    from bbmp import snapshot
    from conftest import modland_listing
    commando = "HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid"
    bionic = "HVSC/GAMES/A-F/Bionic.sid"
    listing = replace(modland_listing(), chips=[(commando, "sid6581"), (bionic, "sid8580"),
                                                (bionic, "sid6581")])
    folder = tmp_path / "catalogs"
    index_mod.build({"modland": listing}, folder)
    ix = index_mod.Index(folder)
    assert {k: n for k, n in ix.view_counts("modland", "chip").items()
            if k.startswith("sid")} == {"sid": 2, "sid6581": 2, "sid8580": 1}
    fmt = ix.by_path(commando).format
    assert ix.formats_on("chip", "sid8580") == {"modland": [fmt]}
    assert [s.path for s in ix.search("chip:8580")] == [bionic]
    assert {s.path for s in ix.search("chip:6581")} == {commando, bionic}
    assert [s.path for s in ix.search("-chip:8580 chip:sid6581")] == [commando]
    artist = ix.by_path(bionic).artist
    assert [s.path for s in ix.songs(fmt, artist, "modland", ("chip", "sid8580"))] == [bionic]
    ix.close()
    # an update's list says nothing of chips: what the songs said is kept
    index_mod.build({"modland": modland_listing()}, folder)
    ix = index_mod.Index(folder)
    assert {k: n for k, n in ix.view_counts("modland", "chip").items()
            if k.startswith("sid")} == {"sid": 2, "sid6581": 2, "sid8580": 1}
    # and the catalog that comes with the player carries it
    packed = tmp_path / "catalog.xz"
    snapshot.write(packed, folder)
    listings, _ = snapshot.read(packed)
    assert sorted(listings["modland"].chips) == sorted(listing.chips)
    ix.close()


ASMADB = ('const asma = {"composerInfos": [{ "folderName" : "Aki", "handles" : "Aki"}],\n'
          '"fileInfos": [\n'
          '{"filePath": "Composers/Aki/Atari_Style.sap", "fileSize": 7291, "title": "[pong] '
          'Atari Style", "author": "<?> (Aki)", "date": "2013", "channels": 2},\n'
          '{"filePath": "Composers/Berger_Dominique/Atari_Land.sap", "fileSize": 100, "title": '
          '"Atari Land \\"live\\" [x]", "author": "Dominique Berger (StaxX)"},\n'
          '{"filePath": "Games/Altazar.ttt", "fileSize": 50, "title": "Altazar", "author": '
          '"Jeremie Bernard & <?> (Tobi)"},\n'
          '{"filePath": "../../etc/passwd.sap", "title": "evil", "author": "<?>"},\n'
          '{"filePath": "Unknown/Nobody.sap", "fileSize": 9, "title": "", "author": "<?>"}\n'
          '], "demozoo": {"productions": []}}\n').encode()


def test_asma_is_read_from_its_web_players_list():
    from bbmp.collection import asma
    assert asma.artist_of("<?> (MusicMarius) <?>") == "MusicMarius"
    listing = asma.read_list(ASMADB, 1745652327)
    got = [(e.path, e.format, e.artist, e.name, e.size) for e in listing.entries]
    assert got == [
        ("Composers/Aki/Atari_Style.sap", "Slight Atari Player", "Aki",
         "[pong] Atari Style.sap", 7291),
        ("Composers/Berger_Dominique/Atari_Land.sap", "Slight Atari Player", "StaxX",
         'Atari Land "live" [x].sap', 100),
        ("Games/Altazar.ttt", "TIATracker", "Jeremie Bernard & Tobi", "Altazar.ttt", 50),
        ("Unknown/Nobody.sap", "Slight Atari Player", "- unknown", "Nobody.sap", 9)]
    assert listing.dated == 1745652327 and not listing.changes
    later = asma.read_list(ASMADB, 2, before=["Composers/Aki/Atari_Style.sap"])
    assert len([c for c in later.changes if c[1] == "added"]) == 3
    a = asma.Asma()
    url = a.url("Composers/Aki/Atari_Style.sap")
    assert url == "https://asma.atari.org/asma/Composers/Aki/Atari_Style.sap"
    assert a.path_in(url) == "Composers/Aki/Atari_Style.sap"


def test_asma_is_atari_8bit_music_in_the_views(tmp_path):
    from bbmp import collection as registry, platforms
    from bbmp.collection import asma
    registry.register(asma.Asma())          # the tests leave it out, not to ask its site
    index_mod.build({"asma": asma.read_list(ASMADB)}, tmp_path / "catalogs")
    ix = index_mod.Index(tmp_path / "catalogs")
    assert ix.formats_on("platform", "atari8") == {"asma": ["Slight Atari Player"]}
    assert ix.formats_on("platform", "atari2600") == {"asma": ["TIATracker"]}
    assert platforms.of("Slight Atari Player", "asma") == (("atari8",), ("pokey",))
    assert [s.title for s in ix.search("staxx")] == ['Atari Land "live" [x]']
    ix.close()


def test_deflemask_and_vgm_songs_say_what_they_play_on(tmp_path):
    import gzip
    import zlib
    from dataclasses import replace
    from bbmp import song_chips
    from bbmp.collection import Entry
    # a Deflemask song's system byte, and a VGM file's clocks
    genesis = zlib.compress(b".DelekDefleMask." + bytes([0x18, 0x02]) + bytes(100))
    game_boy = zlib.compress(b".DelekDefleMask." + bytes([0x18, 0x04]) + bytes(100))
    assert song_chips.read("Deflemask", genesis) == ("platform:megadrive", "ym2612", "sn76489")
    assert song_chips.read("Deflemask", game_boy) == ("platform:gb", "gbapu")
    header = bytearray(0x100)
    header[0:4] = b"Vgm "
    header[8:12] = (0x161).to_bytes(4, "little")
    header[0x34:0x38] = (0xCC).to_bytes(4, "little")       # the data after the header
    header[0x0C:0x10] = (3579545).to_bytes(4, "little")    # SN76489
    header[0x10:0x14] = (3579545).to_bytes(4, "little")    # YM2413: the FM unit
    assert song_chips.read("Video Game Music - Sega Master System",
                           gzip.compress(bytes(header))) == ("sn76489", "opll")
    # in the catalog: the songs decide, not the format's list of every system
    songs = [Entry("Deflemask/A/genesis.dmf", "Deflemask", "A", "", "genesis.dmf"),
             Entry("Deflemask/A/gameboy.dmf", "Deflemask", "A", "", "gameboy.dmf")]
    from bbmp.collection import Listing
    listing = Listing(songs, chips=[("Deflemask/A/genesis.dmf", c) for c in
                                    ("platform:megadrive", "ym2612", "sn76489")]
                      + [("Deflemask/A/gameboy.dmf", c) for c in ("platform:gb", "gbapu")])
    index_mod.build({"modland": listing}, tmp_path / "catalogs")
    ix = index_mod.Index(tmp_path / "catalogs")
    assert ix.view_formats("modland", "platform", "megadrive") == [("Deflemask", 1)]
    assert ix.view_formats("modland", "platform", "nes") == []         # none of its songs
    assert [s.title for s in ix.songs("Deflemask", "A", "modland", ("platform", "gb"))] == [
        "gameboy"]
    assert [s.title for s in ix.search("platform:megadrive")] == ["genesis"]
    assert [s.title for s in ix.search("-platform:megadrive a")] == ["gameboy"]
    assert ix.view_counts("modland", "platform") == {"megadrive": 1, "gb": 1}
    ix.close()


ZX_TUNES = [
    {"id": 19636, "title": "Beyond Time", "authorIds": [7744], "type": "PT3", "time": "3:14.88",
     "year": 2013, "originalUrl": "https://zxart.ee/file/id:19636/filename:x.mt3",
     "originalFileName": "MmcM_-_Beyond_Time_%282013%29.mt3", "dateModified": 1772312799},
    {"id": 5, "title": "", "authorIds": [1, 2], "type": "STC", "time": "",
     "originalUrl": "https://zxart.ee/file/id:5/filename:a.stc", "originalFileName": "a.stc"},
    {"id": 6, "title": "A recording", "authorIds": [1], "type": "MP3",
     "originalUrl": "https://zxart.ee/file/id:6/filename:b.mp3", "originalFileName": "b.mp3"},
    {"id": 7, "title": "Odd", "authorIds": [9], "type": "QQQ",
     "originalUrl": "https://zxart.ee/file/id:7/filename:..%2F..%2Fx", "originalFileName": "..%2F..%2Fx"},
]
ZX_PEOPLE = [{"id": 7744, "title": "MmcM", "realName": "Sergey"},
             {"id": 1, "title": "Nik-O"}, {"id": 2, "title": "Karbofos"}]


def test_zxart_is_read_from_its_api():
    from bbmp.collection import zxart
    # a tune whose kind ZX-Art doesn't name goes by its file's extension
    assert zxart.entry({"id": 8, "originalUrl": "x", "originalFileName": "Feud.fxm"},
                       {}).format == "Spectrum - Fuxoft AY Language"
    assert zxart.entry({"id": 9, "originalUrl": "x", "originalFileName": "Thriller.etc"},
                       {}).format == "Spectrum"
    listing = zxart.read_list(ZX_TUNES, ZX_PEOPLE, 1772312799)
    got = [(e.path, e.format, e.artist, e.name) for e in listing.entries]
    assert got == [
        ("19636/MmcM_-_Beyond_Time_(2013).mt3", "Spectrum - Pro Tracker 3", "MmcM",
         "Beyond Time.mt3"),
        ("5/a.stc", "Spectrum - ST Song Compiler", "Nik-O & Karbofos", "a.stc")]
    assert list(listing.lengths) == [("19636/MmcM_-_Beyond_Time_(2013).mt3", "3:14.880")]
    z = zxart.ZxArt()
    url = z.url("19636/MmcM - Beyond Time (2013).mt3")
    assert url == "https://zxart.ee/file/id:19636/filename:MmcM%20-%20Beyond%20Time%20%282013%29.mt3"
    assert z.path_in(url) == "19636/MmcM - Beyond Time (2013).mt3"
    assert zxart._Files().url("5/a b.stc") == "https://zxart.ee/file/id:5/filename:a%20b.stc"


def test_sndh_is_read_from_its_archives_list_and_shares_modlands_checksums():
    from bbmp.collection import sndh
    files = [("sndh_lf/", 0), ("sndh_lf/Mad_Max/", 0),
             ("sndh_lf/Mad_Max/Games/Wings_Of_Death.sndh", 2000),
             ("sndh_lf/Mad_Max/Games/Lethal_Xcess.sndh", 3000),
             ("sndh_lf/Loony/Loony.sndh", 1776), ("sndh_lf/readme.txt", 10),
             ("sndh_lf/../../etc/passwd.sndh", 5)]
    modland = {"Mad_Max/Games/Wings_Of_Death.sndh": ("a" * 32, 2000),
               "Mad_Max/Games/Lethal_Xcess.sndh": ("b" * 32, 2999)}     # another size
    listing = sndh.read_list(files, 1767260259, modland=modland)
    got = [(e.path, e.format, e.artist, e.album, e.md5, e.size) for e in listing.entries]
    assert got == [
        ("Mad_Max/Games/Wings_Of_Death.sndh", "SNDH", "Mad_Max", "Games", "a" * 32, 2000),
        ("Mad_Max/Games/Lethal_Xcess.sndh", "SNDH", "Mad_Max", "Games", "", 3000),
        ("Loony/Loony.sndh", "SNDH", "Loony", "", "", 1776)]
    s = sndh.Sndh()
    url = s.url("Mad_Max/Games/Wings_Of_Death.sndh")
    assert url == "https://sndh.atari.org/sndh/sndh_lf/Mad_Max/Games/Wings_Of_Death.sndh"
    assert s.path_in(url) == "Mad_Max/Games/Wings_Of_Death.sndh"


STIL_TEXT = """#  STIL v85

### /MUSICIANS/H/ ###

/MUSICIANS/H/Hubbard_Rob/
COMMENT: a note on the whole folder, left out

/MUSICIANS/H/Hubbard_Rob/Commando.sid
COMMENT: "I worked on it through the night." (RH)
(#2)
  TITLE: Base \\\\ [from the arcade game]

/MUSICIANS/../../etc/passwd.sid
COMMENT: never
"""


def test_hvscs_notes_are_kept_and_shown_for_its_tunes(hosted, tmp_path):
    from dataclasses import replace
    from bbmp import snapshot
    from bbmp.collection import hvsc
    from conftest import modland_listing
    notes = hvsc.read_stil(STIL_TEXT)
    assert notes == [("MUSICIANS/H/Hubbard_Rob/Commando.sid",
                      '''COMMENT: "I worked on it through the night." (RH)\n(#2)\n'''
                      '''  TITLE: Base \\\\ [from the arcade game]''')]
    raw = RAW_LIST
    folder = tmp_path / "catalogs"
    index_mod.build({"modland": modland_listing(),
                     "hvsc": replace(hvsc.read_list(raw), notes=notes)}, folder)
    ix = index_mod.Index(folder)
    commando = ix.by_path("HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid")
    assert "through the night" in ix.note(commando)          # as Modland's song too
    assert ix.note(ix.by_path("MUSICIANS/H/Hubbard_Rob/Commando.sid", "hvsc")) == notes[0][1]
    ix.close()
    # an update that couldn't have them keeps the ones there were
    index_mod.build({"hvsc": hvsc.read_list(raw)}, folder)
    ix = index_mod.Index(folder)
    assert "through the night" in ix.note(ix.by_path("MUSICIANS/H/Hubbard_Rob/Commando.sid",
                                                     "hvsc"))
    # and the catalog that comes with the player carries them, line breaks and all
    packed = tmp_path / "catalog.xz"
    snapshot.write(packed, folder)
    listings, _ = snapshot.read(packed)
    assert listings["hvsc"].notes == notes
    ix.close()
