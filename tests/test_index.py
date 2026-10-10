# SPDX-License-Identifier: GPL-3.0-or-later
from conftest import HVSC_COUNT, SONGS
from bbmp import index as index_mod
from bbmp import companions
from bbmp.collection.modland import parse_path
from bbmp.index import display_name


def is_companion(path: str) -> bool:
    parts = path.split("/")
    return companions.is_companion(parts[0], parts[-1], parts[2:-1])


def test_parse_default_layout():
    assert parse_path("Protracker/Cybarite/blues jive.mod") == (
        "Protracker", "Cybarite", "", "blues jive.mod", False)
    fmt, artist, album, name, hidden = parse_path("Nintendo SPC/Greg Turner/Big Sky Trooper/09.spc")
    assert (artist, album) == ("Greg Turner", "Big Sky Trooper")


def test_parse_hvsc_and_vgm():
    assert parse_path("HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid")[:4] == (
        "HVSC", "Rob Hubbard", "", "Commando.sid")
    assert parse_path("HVSC/GAMES/A-F/Bionic_Commando.sid")[1:4] == ("Games", "", "Bionic Commando.sid")
    fmt, artist, album, *_ = parse_path("Video Game Music/Sega Megadrive/Shaka/Curse/11 boss.vgz")
    assert (fmt, artist, album) == ("Video Game Music - Sega Megadrive", "Shaka", "Curse")
    fmt, artist, album, *_ = parse_path("Ad Lib/Creative Music File/Dan Froelich/jill.cmf")
    assert (fmt, artist, album) == ("Ad Lib - Creative Music File", "Dan Froelich", "")


def test_companions_are_hidden():
    assert is_companion("TFMX/Chris Huelsbeck/smpl.titlesong")
    assert not is_companion("TFMX/Chris Huelsbeck/mdat.titlesong")
    assert is_companion("IFF-SMUS/- unknown/instruments/Bassdrum3.instr")
    assert is_companion("Richard Joseph/Richard Joseph/aquatic games.ins")
    assert not is_companion("Richard Joseph/Richard Joseph/aquatic games.sng")
    assert is_companion("Super Nintendo Sound Format/x/y/noahsark.snsflib")


def test_display_name():
    assert display_name("mdat.turrican 2") == "turrican 2"
    assert display_name("blues jive.mod") == "blues jive"
    assert display_name("t.o.s.main 1.mod") == "t.o.s.main 1"
    assert display_name("The Other Side (Aerosmith).W") == "The Other Side (Aerosmith)"


def test_browse(catalog):
    formats = {f for f, _, _ in catalog.formats()}
    assert {"Protracker", "TFMX", "HVSC", "Video Game Music - Sega Megadrive"} <= formats
    assert [a for a, _ in catalog.artists("Protracker")] == ["Other Guy", "Tester"]
    assert [s.title for s in catalog.songs("Protracker", "Tester")] == ["beep", "beep two"]
    # companions never show up as songs
    assert [s.name for s in catalog.songs("TFMX", "Tester")] == ["mdat.level1", "mdat.level2"]


def test_search(catalog):
    assert [s.path for s in catalog.search("beep two")] == ["Protracker/Tester/beep two.mod"]
    assert {s.path for s in catalog.search("bee")} >= {"Protracker/Tester/beep.mod"}
    assert catalog.search("hubbard")[0].artist == "Rob Hubbard"
    assert catalog.search("level1") == [catalog.by_path("TFMX/Tester/mdat.level1")]
    assert catalog.search("") == []
    assert catalog.search('"*') == []


def test_companions(catalog):
    def companions_of(path):
        return catalog.companions("modland:" + path)
    assert companions_of("TFMX/Tester/mdat.level1") == ["modland:TFMX/Tester/smpl.level1"]
    assert companions_of("Richard Joseph/Tester/game.sng") == ["modland:Richard Joseph/Tester/game.ins"]
    assert companions_of("IFF-SMUS/Tester/Album/song.smus") == [
        "modland:IFF-SMUS/Tester/Album/Instruments/bass.instr"]
    assert companions_of("Playstation Sound Format/Someone/Game/01.minipsf") == [
        "modland:Playstation Sound Format/Someone/Game/game.psflib"]
    assert companions_of("Protracker/Tester/beep.mod") == []
    assert companions_of("MDX/Someone/Game/track1.mdx") == ["modland:MDX/Someone/Game/drums.pdx"]
    assert companions_of("Ad Lib/Visual Composer/Someone/song.rol") == [
        "modland:Ad Lib/Visual Composer/Someone/standard.bnk"]
    # a game's track lists belong to its SGC file; other .m3u files belong to nothing
    assert companions_of("SGC/Someone/Game/Game.sgc") == [
        "modland:SGC/Someone/Game/01 Title Screen.m3u", "modland:SGC/Someone/Game/02 Stage 1.m3u"]
    assert companions_of("SGC/Someone/Other Game [SMS].sgc") == []
    # FAC SoundTracker's drum kits are for every song in the folder
    assert companions_of("FAC SoundTracker/Someone/Disk/Second.MUS") == [
        "modland:FAC SoundTracker/Someone/Disk/DRUMS1.SM1",
        "modland:FAC SoundTracker/Someone/Disk/DRUMS1.SM2",
        "modland:FAC SoundTracker/Someone/Disk/First.SM1"]
    assert companions_of("Ad Lib/Ken's AdLib Music/Someone/song.ksm") == [
        "modland:Ad Lib/Ken's AdLib Music/Someone/insts.dat"]
    assert companions_of("Ken's Digital Music/Someone/song.kdm") == [
        "modland:Ken's Digital Music/Someone/waves.kwv"]
    assert [s.name for s in catalog.songs("Ken's Digital Music", "Someone")] == ["song.kdm"]
    # samples all songs of a format share; a game's songs, which share instruments
    assert companions_of("Zoundmonitor/Someone/tune.sng") == [
        "modland:Zoundmonitor/Samples/perco", "modland:Zoundmonitor/Samples/tom4"]
    assert [a for a, _ in catalog.artists("Zoundmonitor")] == ["Someone"]
    assert companions_of("MaxTrax/Someone/game (level 1).mxtx") == [
        "modland:MaxTrax/Someone/game (intro).mxtx"]
    assert len(catalog.songs("MaxTrax", "Someone")) == 2
    assert companions_of("PokeyNoise/Someone/pn.tetris") == ["modland:PokeyNoise/Someone/pn.tetris.info"]
    assert companions_of("Synth Pack/Someone/Game/title.osp") == ["modland:Synth Pack/Someone/Game/smp.set"]
    # a SoundSmith song and its wave bank, also when the song's name has a dot in it
    assert [s.name for s in catalog.songs("SoundSmith", "Someone")] == [
        "Breathless", "FTA.Song (stripped)", "Lonely.W"]
    assert companions_of("SoundSmith/Someone/Breathless") == ["modland:SoundSmith/Someone/Breathless.W"]
    assert companions_of("SoundSmith/Someone/FTA.Song (stripped)") == [
        "modland:SoundSmith/Someone/FTA.Song (stripped).W"]
    assert catalog.by_path("Protracker/Tester/List/a list.m3u") is None or \
        catalog.search("a list") == []


def test_song_lengths(catalog):
    assert catalog.lengths("modland:HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid") == [235.594, 61.288, 6.0]
    assert catalog.lengths("modland:HVSC/GAMES/A-F/Bionic.sid") == []


def test_random_song(catalog):
    for _ in range(20):
        s = catalog.random_song(fmt="Protracker", artist="Tester")
        assert s.path in ("Protracker/Tester/beep.mod", "Protracker/Tester/beep two.mod")
    assert catalog.random_song(fmt="Nope") is None
    only = catalog.random_song(playable=lambda s: s.format == "HVSC")
    assert only.format == "HVSC"


def test_local_files_get_a_format_from_their_name(catalog, tmp_path, mod_bytes):
    (tmp_path / "tune.MOD").write_bytes(mod_bytes)
    (tmp_path / "mdat.level9").write_bytes(b"x")
    (tmp_path / "smpl.level9").write_bytes(b"x")
    (tmp_path / "readme.txt").write_bytes(b"x")
    song = catalog.local_song(tmp_path / "tune.MOD")
    assert song.local and song.format == "Protracker" and song.title == "tune"
    assert song.album == tmp_path.name
    names = [s.name for s in catalog.local_songs(tmp_path)]
    assert names == ["mdat.level9", "tune.MOD"]          # no sample file, no text file
    assert song.key == song.path and catalog.resolve(song.key) == song
    assert catalog.resolve("/nowhere/gone.mod") is None
    assert catalog.resolve("modland:Protracker/Tester/beep.mod").artist == "Tester"
    # as songs were saved before there were collections
    assert catalog.resolve("Protracker/Tester/beep.mod").key == "modland:Protracker/Tester/beep.mod"


def test_artist_songs_include_collaborations(catalog):
    paths = {s.path for s in catalog.artist_songs("Tester")}
    assert "Protracker/Other Guy/coop-Tester/duet.mod" in paths
    assert "Protracker/Tester/beep.mod" in paths
    duet = catalog.by_path("Protracker/Other Guy/coop-Tester/duet.mod")
    assert (duet.partner, duet.album_name) == ("Tester", "")
    assert catalog.artist_songs("Nobody") == []


def test_sizes(catalog):
    from conftest import SONGS
    size = {p: 1000 + 10 * i for i, p in enumerate(SONGS)}
    beep = catalog.by_path("Protracker/Tester/beep.mod")
    assert beep.size == size["Protracker/Tester/beep.mod"]
    # a song's download is the song and the files it needs
    level1 = catalog.by_path("TFMX/Tester/mdat.level1")
    assert catalog.size_with_companions(level1) == (
        size["TFMX/Tester/mdat.level1"] + size["TFMX/Tester/smpl.level1"])
    # a format's and an artist's size have those files in them too
    tfmx = sum(n for p, n in size.items() if p.startswith("TFMX/"))
    assert catalog.format_sizes()["TFMX"] == tfmx == catalog.artist_sizes("TFMX")["Tester"]
    assert catalog.size() == sum(size.values()) == catalog.about("modland")["bytes"]
    assert [a for a, _ in catalog.artists("TFMX")] == ["Tester"]        # counted as before
    assert {f: n for f, n, _ in catalog.formats()}["TFMX"] == 2


def test_what_is_new(catalog):
    from conftest import DATED
    assert catalog.about("modland")["dated"] == DATED
    # by day, the newest first; files that belong to songs and songs that are gone don't count
    assert catalog.change_days() == [("2026-09-27", 1, 1), ("2026-09-20", 2, 0)]
    assert [(kind, s.path) for kind, s in catalog.changed("2026-09-27")] == [
        ("added", "Protracker/Tester/beep two.mod"), ("updated", "TFMX/Tester/mdat.level1")]


def test_changes_are_kept_when_the_catalog_is_made_again(tmp_path):
    from conftest import RAW_LIST, SIZES, SONGLENGTHS
    from bbmp import index as index_mod
    from bbmp.collection import modland
    db = tmp_path / "index.db"
    old = [("2026-09-20", "added", "Protracker/Tester/beep.mod")]
    index_mod.build({"modland": modland.read_list(RAW_LIST, SONGLENGTHS, SIZES, old, 1)}, db)
    assert index_mod.last_changes(db) == {"modland": "2026-09-20"}
    # the next time only the days after that are asked for, and the older ones stay
    new = [("2026-09-27", "added", "Protracker/Tester/beep two.mod")]
    index_mod.build({"modland": modland.read_list(RAW_LIST, SONGLENGTHS, SIZES, new, 2)}, db)
    ix = index_mod.Index(db)
    assert [d for d, _, _ in ix.change_days()] == ["2026-09-27", "2026-09-20"]
    assert ix.about("modland")["dated"] == 2


def test_modlands_lists_are_read():
    from bbmp.collection import modland
    assert modland.read_sizes(b"259547\tAce Tracker/505/a song.am\nnot a line\n\n12\tb\n") == {
        "Ace Tracker/505/a song.am": 259547, "b": 12}
    text = "  added: BP SoundMon 2/Zzzax/visor.alt.bp\nupdated: BP SoundMon 2/Zzzax/visor.bp\n\n"
    assert modland.read_changelog("2026-09-28", text) == [
        ("2026-09-28", "added", "BP SoundMon 2/Zzzax/visor.alt.bp"),
        ("2026-09-28", "updated", "BP SoundMon 2/Zzzax/visor.bp")]


def test_the_catalog_that_comes_with_the_player(hosted, tmp_path, monkeypatch):
    """A new user has the collections and the sizes without a download."""
    from bbmp import snapshot

    packed = tmp_path / "shipped/catalog.xz"
    counts = snapshot.write(packed, hosted.path)
    assert counts == {"modland": len(SONGS), "hvsc": HVSC_COUNT}

    def no_network(*a, **kw):
        raise AssertionError("the network was asked")
    monkeypatch.setattr(index_mod, "update", no_network)
    db = tmp_path / "new user/index.db"
    assert snapshot.install(db, packed)
    new = index_mod.Index(db)
    assert new.collections() == hosted.collections()          # also when they were downloaded
    assert new.about("modland") == hosted.about("modland")
    assert new.size() == hosted.size() and new.format_sizes("hvsc") == hosted.format_sizes("hvsc")
    assert new.change_days() == hosted.change_days()
    key = "modland:HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid"
    assert new.lengths(key) == hosted.lengths(key) != []
    assert [s.key for s in new.search("beep")] == [s.key for s in hosted.search("beep")]
    assert new.companions("TFMX/Tester/mdat.level1") == hosted.companions("TFMX/Tester/mdat.level1")
    # a catalog that is there is left alone, and a snapshot that isn't one is passed over
    assert not snapshot.install(db, packed)
    packed.write_bytes(b"something else")
    assert not snapshot.install(tmp_path / "other/index.db", packed)
    assert not snapshot.install(tmp_path / "other/index.db", tmp_path / "not there")


def test_search_words(catalog):
    from bbmp.index import search_query
    assert search_query("jest ely") == '("jest"*) AND ("ely"*)'
    assert search_query('artist:"other guy" -duet') == 'artist : ("other"* AND "guy"*) NOT ("duet"*)'
    assert search_query("format:tfmx title:level1") == 'format : ("tfmx"*) AND name : ("level1"*)'
    assert search_query("-only -left -out") == "" and search_query('"') == ""
    assert search_query("odd:word") == '("odd"* AND "word"*)'       # no such field: both words
    titles = lambda q: sorted(s.title for s in catalog.search(q))
    assert titles("format:tfmx") == ["level1", "level2"]
    assert titles("artist:tester -beep format:protracker") == []      # duet is Other Guy's
    assert titles("tester -beep format:protracker") == ["duet"]      # with Tester
    assert titles("by:tester title:beep") == ["beep", "beep two"]
    assert catalog.search_count("format:tfmx") == 2
    assert catalog.search("-beep") == [] and catalog.search_count("-beep") == 0


# as Modland's changelog of 2026-09-04 has them
CHANGELOG = """updated: Fasttracker 2/Wierza/emu.xm
  moved: FredMon/- unknown/game shorty.fred
     to: FredMon/Shorty
     as: ghh-first-demo.fred
deleted: JamCracker/Lydian/bronx megademo ii.jam
  added: JamCracker/Lydian/mega_loader_1992.jam
renamed: MGSDRV/Kyoda/Sailormoon - Moon prisum.MGS
     to: sailormoon - moon prisum power make up!.MGS
  moved: Protracker/Skytech/peace and chaos (c).mod
     to: Protracker/Mr. Smooth
  moved: Protracker/Vim!/lost.mod
  added: ../../outside.mod
"""


def test_moves_and_renames_in_modlands_changelog():
    from bbmp.collection import modland
    assert modland.read_changelog("2026-09-04", CHANGELOG) == [
        ("2026-09-04", "updated", "Fasttracker 2/Wierza/emu.xm"),
        ("2026-09-04", "moved", "FredMon/Shorty/ghh-first-demo.fred"),
        ("2026-09-04", "deleted", "JamCracker/Lydian/bronx megademo ii.jam"),
        ("2026-09-04", "added", "JamCracker/Lydian/mega_loader_1992.jam"),
        ("2026-09-04", "moved", "MGSDRV/Kyoda/sailormoon - moon prisum power make up!.MGS"),
        ("2026-09-04", "moved", "Protracker/Mr. Smooth/peace and chaos (c).mod")]
    # (a move without a "to" says nowhere it went; a path out of the collection is none)


def test_only_what_was_updated_counts_as_updated(tmp_path):
    from conftest import RAW_LIST, SIZES
    from bbmp.collection import modland
    changes = [("2026-09-07", "deleted", "Protracker/Tester/beep.mod"),
               ("2026-09-07", "moved", "Protracker/Tester/beep two.mod"),
               ("2026-09-07", "updated", "TFMX/Tester/mdat.level1"),
               ("2026-09-07", "deleted", "Protracker/Gone/removed.mod"),
               ("2026-09-06", "added", "IFF-SMUS/Tester/Album/song.smus")]
    db = tmp_path / "catalogs"
    index_mod.build({"modland": modland.read_list(RAW_LIST, None, SIZES, changes)}, db)
    ix = index_mod.Index(db)
    assert ix.change_days() == [("2026-09-07", 0, 1), ("2026-09-06", 1, 0)]
    assert ix.change_days(moved=True)[0] == ("2026-09-07", 0, 1, 1)
    assert sorted((kind, s.path) for kind, s in ix.changed("2026-09-07")) == [
        ("moved", "Protracker/Tester/beep two.mod"), ("updated", "TFMX/Tester/mdat.level1")]


def test_a_catalog_from_before_lets_go_of_the_changelogs_move_lines(tmp_path):
    from conftest import RAW_LIST, SIZES
    from bbmp.collection import modland
    # as the catalogs had them: a line each, the path of "moved" being where it was
    raw = [("2026-09-04", "to", "Protracker/Tester"),
           ("2026-09-04", "moved", "Protracker/Tester/beep.mod"),
           ("2026-09-04", "as", "beep two.mod"),
           ("2026-09-04", "renamed", "TFMX/Tester/mdat.level1"),
           ("2026-09-04", "updated", "TFMX/Tester/mdat.level2"),
           ("2026-09-03", "added", "IFF-SMUS/Tester/Album/song.smus")]
    db = tmp_path / "catalogs"
    index_mod.build({"modland": modland.read_list(RAW_LIST, None, SIZES, raw)}, db)
    assert index_mod.Index(db).change_days() == [("2026-09-04", 0, 1), ("2026-09-03", 1, 0)]
    assert index_mod.Index(db)._all("SELECT DISTINCT kind FROM changes ORDER BY kind") == [
        ("added",), ("updated",)]
    new = [("2026-09-05", "moved", "Protracker/Tester/beep.mod")]
    index_mod.build({"modland": modland.read_list(RAW_LIST, None, SIZES, new)}, db)
    assert index_mod.Index(db).change_days(moved=True) == [
        ("2026-09-05", 0, 0, 1), ("2026-09-04", 0, 1, 0), ("2026-09-03", 1, 0, 0)]


def test_a_folder_modland_deleted_and_added_again_is_not_new(tmp_path):
    """HVSC's 85th release: Modland deleted every tune one day and added them again
    the next."""
    from conftest import SIZES
    from bbmp.collection import modland
    before = {"HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid": "a" * 32,
              "HVSC/GAMES/A-F/Bionic.sid": "b" * 32}
    after = {**before, "HVSC/GAMES/A-F/Bionic.sid": "c" * 32,      # changed
             "HVSC/GAMES/A-F/Castle.sid": "d" * 32}                   # new

    def listing(files, changes):
        raw = "\n".join(f"{m} {p}" for p, m in files.items()).encode()
        return modland.read_list(raw, None, SIZES, changes)
    db = tmp_path / "catalogs"
    index_mod.build({"modland": listing(before, [])}, db)
    again = [("2026-09-07", "deleted", p) for p in before] + [
        ("2026-09-08", "added", p) for p in after]
    index_mod.build({"modland": listing(after, again)}, db)
    ix = index_mod.Index(db)
    assert sorted((kind, s.path) for kind, s in ix.changed("2026-09-08")) == [
        ("added", "HVSC/GAMES/A-F/Castle.sid"), ("updated", "HVSC/GAMES/A-F/Bionic.sid")]
    # without a catalog before, what was added again can only be said to be updated
    first = tmp_path / "first"
    index_mod.build({"modland": listing(after, again)}, first)
    assert index_mod.Index(first).change_days() == [("2026-09-08", 1, 2)]


def test_paths_that_leave_the_collection_are_not_taken():
    from bbmp.collection import hvsc, keygenmusic, modland
    bad = ["../../etc/passwd", "/etc/passwd", "Protracker//x.mod", "Protracker/./x.mod",
           "Protracker/../../x.mod", "HVSC/../x.sid", "HVSC//x.sid"]
    raw = "\n".join(f"{i:032x} {p}" for i, p in enumerate(
        ["Protracker/Tester/good.mod", "HVSC/MUSICIANS/H/Hubbard_Rob/Good.sid", *bad])).encode()
    assert [e.path for e in modland.read_list(raw).entries] == [
        "Protracker/Tester/good.mod", "HVSC/MUSICIANS/H/Hubbard_Rob/Good.sid"]
    assert [e.path for e in hvsc.read_list(raw).entries] == ["MUSICIANS/H/Hubbard_Rob/Good.sid"]
    lengths = "".join(f"; /{p}\n{'0' * 32}=1:00\n" for p in ["GAMES/A-F/Good.sid", "../x.sid",
                                                            "GAMES//x.sid"])
    assert [p for p, _, _ in hvsc.read_songlengths(lengths)] == ["GAMES/A-F/Good.sid"]
    assert [p for p, _ in modland._lengths(lengths.encode())] == ["HVSC/GAMES/A-F/Good.sid"]
    lib = ('[{"path": "/kgm/G/good.xm"}, {"path": "/kgm//etc/passwd"},'
           ' {"path": "/kgm/../x.xm"}, {"path": "/kgm/G//x.xm"}, {"path": "/kgm/"}]')
    assert [e.path for e in keygenmusic.read_list(lib.encode()).entries] == ["G/good.xm"]


REAL_SIZES_AND_CHANGES = __import__("bbmp.collection.modland").collection.modland.sizes_and_changes


def test_modlands_lists_are_kept_for_one_update_only(tmp_path, monkeypatch):
    import io
    import zipfile

    from conftest import modland_listing
    from bbmp import collection as collections_
    from bbmp.collection import modland
    asked = []

    class Response(io.BytesIO):
        headers = {"Last-Modified": "Mon, 28 Sep 2026 10:00:00 GMT"}

    def ask(path, timeout=60, head=False):
        asked.append(path)
        if path == modland.SIZES:
            packed = io.BytesIO()
            with zipfile.ZipFile(packed, "w") as z:
                z.writestr("allmods.txt", "12\tProtracker/Tester/beep.mod\n")
            return Response(packed.getvalue())
        if path == modland.CHANGELOGS + "/":
            return Response(b"changelog_2026-09-27.txt changelog_2026-09-28.txt")
        return Response(f"added: Protracker/Tester/{path[-14:-4]}.mod\n".encode())
    monkeypatch.setattr(modland, "_ask", ask)
    monkeypatch.setattr(modland, "sizes_and_changes", REAL_SIZES_AND_CHANGES)
    modland.forget()
    one = modland.sizes_and_changes(since="")
    assert [p for _, _, p in one["changes"]] == ["Protracker/Tester/2026-09-28.mod",
                                                 "Protracker/Tester/2026-09-27.mod"]
    # asked with another day, it has the changes after that day, and nothing is asked again
    n = len(asked)
    two = modland.sizes_and_changes(since="2026-09-27")
    assert [p for _, _, p in two["changes"]] == ["Protracker/Tester/2026-09-28.mod"]
    assert two["sizes"] == one["sizes"] and len(asked) == n
    # an update lets go of them when it is over
    monkeypatch.setattr(collections_.get("modland"), "listing", lambda progress=None: (
        modland.file_list(), modland_listing())[1])
    index_mod.update(db_path=tmp_path / "catalogs")
    assert modland._kept == {}


def test_chips_that_come_with_the_player(tmp_path, monkeypatch):
    """A catalog built from a collection's list takes in the chips the player knows, for
    the files that are still the same ones."""
    import sqlite3
    from bbmp import known_chips
    from bbmp.collection import Entry, Listing
    have = tmp_path / "have"
    index_mod.build({"modland": Listing([
        Entry("HVSC/A/one.sid", "HVSC", "A", "", "one", False, "a" * 32, 10),
        Entry("HVSC/A/two.sid", "HVSC", "A", "", "two", False, "b" * 32, 10)])}, have)
    con = sqlite3.connect(have / "modland.db")
    con.executemany("INSERT INTO chips VALUES ('modland', ?, 'sid8580')",
                    [("HVSC/A/one.sid",), ("HVSC/A/two.sid",)])
    con.commit()
    con.close()
    shipped = tmp_path / "chips.xz"
    assert known_chips.write(shipped, {"modland": have / "modland.db"}) == 2
    monkeypatch.setenv("BBMP_CHIPS", str(shipped))
    new = tmp_path / "new"
    index_mod.build({"modland": Listing([
        Entry("HVSC/A/one.sid", "HVSC", "A", "", "one", False, "a" * 32, 10),
        Entry("HVSC/A/two.sid", "HVSC", "A", "", "two", False, "c" * 32, 11)])}, new)
    con = sqlite3.connect(new / "modland.db")
    assert con.execute("SELECT path, chip FROM chips").fetchall() == [("HVSC/A/one.sid", "sid8580")]
    con.close()


def test_an_update_reads_what_new_songs_play_on(monkeypatch):
    """New songs at an update: a new HVSC tune's SID, from its first bytes."""
    from bbmp import song_chips
    from bbmp.collection import Entry, Listing
    header = bytearray(0x78)
    header[:4], header[4:6], header[0x76:0x78] = b"PSID", (2).to_bytes(2, "big"), (0x20).to_bytes(2, "big")
    asked = []

    def head(path):
        asked.append(path)
        return bytes(header)
    monkeypatch.setattr(song_chips, "head", head)
    listing = Listing([Entry("MUSICIANS/A/new.sid", "HVSC - Musicians", "A", "", "new", False, "", 1),
                       Entry("MUSICIANS/A/old.sid", "HVSC - Musicians", "A", "", "old", False, "", 1)],
                      changes=[("2026-09-30", "added", "MUSICIANS/A/new.sid")])
    got = index_mod._new_chips("hvsc", listing, lambda s: None)
    assert asked == ["HVSC/MUSICIANS/A/new.sid"]
    assert list(got.chips) == [("MUSICIANS/A/new.sid", "sid8580")]
    assert index_mod._new_chips("asma", listing, lambda s: None) is listing


def test_an_extension_two_catalogs_name_keeps_the_first_or_the_one_with_a_replayer():
    """ZX-Art files .bbsong and .mod under "Spectrum ..." formats; Modland comes first and
    names Beepola and Protracker. A local .bbsong or .mod went to ZXTune and failed."""
    from bbmp.index import Index
    rows = [(".bbsong", "Beepola"), (".mod", "Protracker"),            # modland
            (".bbsong", "Spectrum"), (".mod", "Spectrum - General Sound"),  # zxart
            (".xyz", "Spectrum"), (".xyz", "Beepola")]
    index = object.__new__(Index)
    index._extensions = None
    index._all = lambda sql, args=(): rows
    assert index.guess_format("dabada.bbsong") == "Beepola"
    assert index.guess_format("song.mod") == "Protracker"
    assert index.guess_format("a.xyz") == "Beepola"    # a format with a replayer wins
