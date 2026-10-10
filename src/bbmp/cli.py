# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line entry point. With no arguments it starts the TUI."""

from __future__ import annotations

import argparse
import os
import sys
import time

from . import __version__, collection as collections_, index as index_mod, paths
from .backends import is_supported


def _index() -> index_mod.Index:
    """The catalogs of the collections the user has, made first where one is missing."""
    from . import catalogs
    from .settings import Settings

    settings = Settings.load()
    if catalogs.needs_work(settings):
        if catalogs.first_start(settings):
            print(f"No collections chosen yet: {_names(catalogs.DEFAULT)} for now "
                  "(bbmp collections --use changes it).")
        try:
            catalogs.prepare(settings, progress=print)
        except catalogs.Busy as e:
            print(f"! {e}; the catalogs are used as they are.", file=sys.stderr)
    return index_mod.Index()


def _names(ids=None) -> str:
    """"Modland, HVSC and Keygen Music": the collections named, or all the player knows."""
    names = [collections_.name(c) for c in ids] if ids is not None else [
        c.name for c in collections_.known()]
    return f"{', '.join(names[:-1])} and {names[-1]}" if len(names) > 1 else "".join(names)


def _shown(song) -> str:
    """A song as the command line prints it and takes it: a Modland song by its path
    alone, a song from another collection with the collection in front."""
    return song.path if song.collection == collections_.DEFAULT else song.key


def _collection(args) -> str:
    """The collection named with --collection ("" for all of them)."""
    return _collection_id(getattr(args, "collection", None) or "")


def _collection_id(wanted: str) -> str:
    """A collection's id from its id or its name."""
    wanted = wanted.strip().lower()
    if not wanted:
        return ""
    for c in collections_.known():
        if wanted in (c.id, c.name.lower()):
            return c.id
    names = ", ".join(c.id for c in collections_.known())
    raise SystemExit(f"There is no collection called {wanted}. There are: {names}")


def cmd_update(args) -> int:
    from . import catalogs
    from .settings import Settings

    only = [_collection_id(name) for name in args.collections]
    _index()
    settings = Settings.load()
    have = catalogs.chosen(settings)
    for c in only:
        if c not in have:
            print(f"! {collections_.name(c)} is not one of the collections you have, so it is "
                  f"not updated. To have it: bbmp collections --use {' '.join([*have, c])}",
                  file=sys.stderr)
    if not have:
        print("! No collections are chosen, so there is nothing to update. To choose: "
              f"bbmp collections --use {' '.join(catalogs.DEFAULT)}", file=sys.stderr)
    only = [c for c in only if c in have]
    if not have or (args.collections and not only):
        return 1
    try:
        done = catalogs.update(settings, progress=print, only=only)
    except catalogs.Busy as e:
        print(f"! {e}. Try again when it is done.", file=sys.stderr)
        return 1
    except OSError as e:
        print(f"! Nothing was updated: {e}", file=sys.stderr)
        return 1
    for c, why in done.failed.items():
        print(f"! {collections_.name(c)} was not updated ({why}); it keeps the catalog it had.",
              file=sys.stderr)
    return 1 if done.failed else 0


def cmd_collections(args) -> int:
    from .fetch import Fetcher

    from . import units

    from . import catalogs
    from .settings import Settings

    settings = Settings.load()
    if args.use:
        ids = [_collection_id(name) for name in args.use]
        try:
            catalogs.choose(settings, ids, progress=print)
        except catalogs.Busy as e:
            print(f"! {e}. Try again when it is done.", file=sys.stderr)
            return 1
        print(f"You have {catalogs.summary(ids)} now.")
    if args.every:
        settings.update(update_every=args.every)
    ix = _index()
    fetcher = Fetcher()
    print(f"The catalogs update by themselves "
          f"{catalogs.EVERY.get(settings.update_every, catalogs.EVERY['week'])[1]}.")
    for c in collections_.known():
        about = ix.about(c.id)
        if not about:
            print(f"{c.id:12} {c.name:14} not chosen: {c.about}")
            continue
        where = (f"hosted by {collections_.name(c.hosted_by)}" if c.id in ix._hosted
                 else f"its files are {collections_.name(c.hosted_by)}'s" if c.hosted_by
                 else f"{units.size(fetcher.cache_size(c.id)) or 'nothing'} cached")
        print(f"{c.id:12} {c.name:14} {about["songs"]:9,} songs  {units.size(about['bytes']):>9}"
              f"  {where}  {c.home}")
        print(f"{'':25} its list of files is from {units.moment(about['dated'])}")
        if not args.offline and c.id not in ix._hosted:
            try:
                newest = c.latest()
                print(f"{'':27} " + ("and there is none newer" if newest <= about["dated"] + 3600
                                     else f"THERE IS A NEWER ONE, from {units.moment(newest)}: "
                                          f"bbmp update {c.id}"))
            except (OSError, EOFError):
                print(f"{'':27} (its server doesn't answer, so whether there is a newer "
                      "one is not known)")
        if args.servers:
            for n, server in enumerate(
                    c.servers or (collections_.get(c.hosted_by).servers if c.hosted_by else ())):
                print(f"{'':10} {'asked first' if n == 0 else 'or':>14}  {server}")
    return 0


def cmd_new(args) -> int:
    from . import units

    ix = _index()
    collection = _collection(args)
    days = ix.change_days(collection, moved=True)[:args.days]
    for day, added, updated, moved in days:
        print(f"{units.day(day)}: " + ", ".join(
            f"{n:,} {what}" for n, what in ((added, "new"), (updated, "changed"),
                                            (moved, "moved or renamed")) if n))
        for kind, s in ix.changed(day, collection):
            mark = {"added": "+", "updated": "~", "moved": ">"}.get(kind, "?")
            print(f"  {mark} {units.size(s.size):>9}  {_shown(s)}")
    if not days:
        print("The catalogs have no record of changes.", file=sys.stderr)
    return 0


def cmd_downloads(args) -> int:
    from . import downloads

    print(f"Downloads are kept in {downloads.folder()}\n")
    print("These are files that some formats are played with. The player doesn't come with\n"
          "them, because their makers' terms don't allow it, and downloads them from their\n"
          "makers when you say so: bbmp download NAME, or bbmp download all.\n")
    for d in downloads.DOWNLOADS:
        print(f"{'there' if downloads.installed(d) else 'NOT THERE':9}  {d.id}   {d.name}, "
              f"{d.size / 1e6:.1f} MB")
        print(f"           What it is:  {d.what}")
        print(f"           Used by:     {downloads.used_for(d)}")
        print(f"           Without it:  {d.without}")
        print(f"           Whose:       {d.terms}")
        print(f"           From:        {d.url}\n")
    return 0


def cmd_download(args) -> int:
    from . import downloads

    known = {d.id: d for d in downloads.DOWNLOADS}
    names = list(known) if "all" in args.names else args.names
    unknown = [n for n in names if n not in known]
    if unknown:
        print(f"There is no download called {', '.join(unknown)}. There are: {', '.join(known)}",
              file=sys.stderr)
        return 2
    failed = 0
    for name in names:
        d = known[name]
        if args.remove:
            downloads.remove(d)
            print(f"{d.name} is removed.")
        elif downloads.installed(d) and not args.again:
            print(f"{d.name} is there already.")
        else:
            try:
                downloads.install(d, progress=print)
            except (OSError, ValueError) as e:
                print(f"! {d.name}: {e}", file=sys.stderr)
                failed += 1
    return 1 if failed else 0


def cmd_extras(args) -> int:
    from . import extras

    try:
        extras.folder().mkdir(parents=True, exist_ok=True)
    except OSError as e:
        print(f"! can't make {extras.folder()}: {e.strerror or e}", file=sys.stderr)
    print(f"Extra files go into {extras.folder()}\n")
    print("These are files that some formats are played with and that the player can't get\n"
          "by itself, because they are someone else's. If you have one, put it there under\n"
          "the name given here.\n")
    for e in extras.EXTRAS:
        path, note = extras.state(e)
        print(f"{'there' if path else 'NOT THERE':9}  {e.name}   {e.size:,} bytes")
        print(f"           What it is:  {e.what}")
        print(f"           Used by:     {', '.join(e.formats)}")
        print(f"           Without it:  {e.without}")
        print(f"           It is:       {e.source}")
        if path:
            print(f"           Found:       {path}")
        elif note != "not there":
            print(f"           Note:        {note}")
        print()
    return 0


def cmd_search(args) -> int:
    ix = _index()
    songs = ix.search(" ".join(args.words), limit=args.limit, collection=_collection(args))
    for s in songs:
        mark = " " if is_supported(s.format) else "✗"
        print(f"{mark} {_shown(s)}")
    if len(songs) >= args.limit:
        print(f"at least {len(songs):,} results (-n shows more)", file=sys.stderr)
    else:
        print(f"{len(songs):,} results", file=sys.stderr)
    return 0


def cmd_replayers(args) -> int:
    """What plays each format: every replayer, what it is made of, versions, licences."""
    from . import replayer_facts
    from .backends import BACKEND_BY_FORMAT
    formats: dict[str, list[str]] = {}
    for fmt, engine in BACKEND_BY_FORMAT.items():
        formats.setdefault(engine, []).append(fmt)
    wanted = [w.lower() for w in args.replayer]
    for engine in sorted(replayer_facts.facts(), key=lambda e: replayer_facts.line(e).lower()):
        line = replayer_facts.line(engine)
        if wanted and not any(w in f"{engine} {line}".lower() for w in wanted):
            continue
        print(line)
        if args.details or wanted:
            for label, text in replayer_facts.details(engine):
                print(f"    {label}: {text}")
            names = sorted(formats.get(engine, []))
            if names:
                print(f"    plays: {', '.join(names)}")
    return 0


def cmd_formats(args) -> int:
    from . import formats_info

    ix = _index()
    wanted = [w.lower() for w in args.format]
    for fmt, songs, artists in ix.formats(_collection(args)):
        if wanted and not any(w in fmt.lower() for w in wanted):
            continue
        info = formats_info.about(fmt)
        mark = info.mark or " "
        print(f"{mark} {songs:7,} songs {artists:6,} artists  {fmt}")
        if args.notes or wanted:
            print(f"      {info.line}")
    return 0


def _play(songs, radio=None) -> int:
    from .tui import quiet_stderr

    ix = _index()
    with quiet_stderr():
        return _play_loop(ix, songs, radio)


def _play_loop(ix, songs, radio) -> int:
    from .fetch import Fetcher
    from .library import Library
    from .player import Player
    from .settings import Settings

    library = Library()
    plays = {}
    from .trouble import Trouble

    player = Player(ix, Fetcher(), settings=Settings.load(), levels=library,
                    trouble=Trouble(library),
                    on_song=lambda s: plays.update(
                        {s.key: library.start_play(ix.resolve(s.key) or s)}),
                    on_done=lambda s, secs: library.finish_play(plays.pop(s.key), secs)
                    if s.key in plays else None,
                    keep=library.favorite_keys)
    if radio is not None:
        player.radio(*radio)
    else:
        player.play_list(songs)
    last, started = None, False
    try:
        while True:
            time.sleep(0.25)
            st = player.status()
            if st.error and st.error != last:
                print(f"\n! {st.error}")
                last = st.error
            started = started or st.state != "stopped"
            if started and st.state == "stopped" and not st.radio:
                break
            if st.song:
                info = st.info
                dur = f"/{_mmss(info.duration)}" if info and info.duration else ""
                sub = f" [{info.subsong + 1}/{info.subsongs}]" if info and info.subsongs > 1 else ""
                line = (f"{st.state:8} {_mmss(st.elapsed)}{dur}{sub}  {st.song.artist} - "
                        f"{st.song.title}  ({st.song.format}, {st.backend})")
                print("\r" + line[:120].ljust(120), end="", flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        print()
        player.shutdown()
    return 0


def _local_songs(ix, names: list[str]) -> list:
    """Songs for the arguments that are files, folders or archives on this computer."""
    from pathlib import Path

    from . import archives
    songs = []
    for name in names:
        path = Path(name).expanduser()
        if archives.is_archive(path) and path.is_file():
            try:
                songs += ix.archive_songs(path)
            except OSError as e:
                print(f"! {e}", file=sys.stderr)
        elif path.is_dir():
            songs += ix.local_songs(path, recursive=True)
        elif path.is_file() and (song := ix.local_song(path)):
            songs.append(song)
    return songs


def cmd_play(args) -> int:
    ix = _index()
    target = " ".join(args.what)
    songs = _local_songs(ix, args.what)
    if not songs:
        song = ix.resolve(target)
        songs = [song] if song else ix.search(target, limit=200, collection=_collection(args))
    songs = [s for s in songs if is_supported(s.format)]
    if not songs:
        print("Nothing playable found.", file=sys.stderr)
        return 1
    return _play(songs)


def cmd_export(args) -> int:
    from pathlib import Path

    from .export import ExportError, export_song, export_songs
    from .fetch import Fetcher
    from .library import Library
    from .settings import Settings

    ix = _index()
    settings = Settings.load()
    if args.one:
        settings.subsongs = "one"
    if args.no_level:
        settings.level = False
    dest = Path(args.output).expanduser() if args.output else paths.EXPORT_DIR
    kind = "wav" if args.wav else args.format
    if args.playlist or args.artist:
        # a whole list into a folder of its own
        name = " ".join(args.what)
        if args.artist:
            songs = ix.artist_songs(name)
        elif name.lower() == "favorites":
            songs, name = Library().favorites(ix), "Favorites"
        else:
            found = [(pid, n) for pid, n, _ in Library().playlists() if n.lower() == name.lower()]
            if not found:
                print(f"There is no playlist called {name}. `bbmp playlists` lists them.",
                      file=sys.stderr)
                return 1
            (pid, name), = found[:1]
            songs = Library().playlist_songs(pid, ix)
        if not songs:
            print(f"{name} has no songs.", file=sys.stderr)
            return 1

        def said(done, of, song):
            if song is not None:
                print(f"  {done + 1}/{of}  {song.title}", file=sys.stderr)
        folder, files, failed = export_songs(ix, Fetcher(), songs, settings, name, dest, kind,
                                             said)
        for s in files:
            print(s)
        for song, why in failed:
            print(f"! {song.title}: {why}", file=sys.stderr)
        print(f"{len(files):,} files in {folder}", file=sys.stderr)
        return 0 if files else 1
    target = " ".join(args.what)
    songs = _local_songs(ix, args.what)
    if not songs:
        song = ix.resolve(target)
        songs = [song] if song else [s for s in ix.search(target, limit=200,
                                                          collection=_collection(args))
                                     if is_supported(s.format)]
    if not songs:
        print("Nothing playable found.", file=sys.stderr)
        return 1
    if len(songs) > 1 and not args.all:
        print(f"{len(songs)} songs match. Name one more exactly, or add --all to export them all:",
              file=sys.stderr)
        for s in songs[:15]:
            print(f"  {_shown(s)}", file=sys.stderr)
        return 1
    fetcher, failed = Fetcher(), 0
    for song in songs:
        try:
            for f in export_song(ix, fetcher, song, settings, dest, kind,
                                 progress=lambda name: print(f"  rendering {name}", file=sys.stderr)):
                print(f)
        except ExportError as e:
            print(f"! {e}", file=sys.stderr)
            failed += 1
    return 1 if failed == len(songs) else 0


def cmd_import(args) -> int:
    from pathlib import Path

    from . import playlists
    from .library import Library

    ix, failed = _index(), 0
    for name in args.files:
        try:
            _, title, found, missing = playlists.import_file(Path(name), ix, Library(), args.name)
        except OSError as e:
            print(f"! can't read {name}: {e.strerror or e}", file=sys.stderr)
            failed += 1
            continue
        except ValueError as e:
            print(f"! {name}: {e}", file=sys.stderr)
            failed += 1
            continue
        print(f"Imported {title}: {found} songs"
              + (f" ({missing} could not be found)" if missing else ""))
    return 1 if failed else 0


def cmd_playlists(args) -> int:
    from .library import Library

    lists = Library().playlists()
    for _, name, count in lists:
        print(f"{count:6,} songs  {name}")
    if not lists:
        print("No playlists yet.", file=sys.stderr)
    return 0


def _scope(ix, names: list[str]):
    """What `bbmp keep` was given: a collection, and a format and an artist in it."""
    from .keep import Scope

    if len(names) == 1 and "/" in names[0]:         # a song, as `bbmp search` prints it
        song = ix.resolve(names[0])
        if song is None or song.local:
            raise SystemExit(f"There is no song {names[0]} in the catalog.")
        return Scope.of(song)
    collection = _collection_id(names[0])
    fmt = artist = ""
    if len(names) > 1:
        formats = {f.lower(): f for f, _, _ in ix.formats(collection)}
        shown = {f.lower().removeprefix(f"{collections_.name(collection).lower()} - "): f
                 for f in formats.values()}
        fmt = formats.get(names[1].lower()) or shown.get(names[1].lower())
        if fmt is None:
            raise SystemExit(f"{collections_.name(collection)} has no format called "
                             f"{names[1]}. `bbmp formats` lists them.")
    if len(names) > 2:
        artists = {a.lower(): a for a, _ in ix.artists(fmt, collection)}
        artist = artists.get(names[2].lower())
        if artist is None:
            raise SystemExit(f"{fmt} has no artist called {names[2]}.")
    album = ""
    if len(names) > 3:
        albums = {a.lower(): a for (a,) in ix._all(
            "SELECT DISTINCT album FROM files WHERE collection = ? AND format = ? AND artist = ?"
            " AND album != ''", (collection, fmt, artist))}
        album = albums.get(names[3].lower()) or next(
            (a for low, a in albums.items() if low.endswith("/" + names[3].lower())), None)
        if album is None:
            raise SystemExit(f"{artist} has no album or game called {names[3]} in {fmt}.")
    if len(names) > 4:
        raise SystemExit("A collection, a format, an artist and an album: no more than four.")
    return Scope(collection, fmt, artist, album=album)


def cmd_keep(args) -> int:
    """Music kept on this computer."""
    import shutil
    import threading
    from pathlib import Path

    from . import units
    from .keep import Busy, Keeper, Scope

    from .library import Library

    from .settings import Settings

    ix = _index()
    library = Library()
    keeper = Keeper(ix, library=library)
    if args.playlist:
        wanted = " ".join(args.playlist)
        if wanted.lower() == "favorites":
            which = "favorites"
        else:
            found = [pid for pid, name, _ in library.playlists() if name.lower() == wanted.lower()]
            if not found:
                print(f"There is no playlist called {wanted}. `bbmp playlists` lists them.",
                      file=sys.stderr)
                return 1
            which = f"playlist:{found[0]}"
        args.what = []
        playlist_scope = Scope.playlist(which)
    else:
        playlist_scope = None
    if args.folder:
        if paths.env("KEPT"):
            print("! BBMP_KEPT says where kept music is; without it, --folder does.",
                  file=sys.stderr)
            return 1
        folder = Path(args.folder).expanduser().absolute()
        try:
            if Keeper.is_in(folder):
                print(f"{folder} has kept music already, and that is what is kept now.")
            else:
                moved = keeper.move_to(folder)
                print(f"Kept music is in {folder} now"
                      + (f" ({units.size(moved)} moved)." if moved else "."))
        except (OSError, shutil.Error) as e:
            print(f"! The kept music stays where it is: {e}", file=sys.stderr)
            return 1
        Settings.load().update(keep_dir="" if folder == paths.kept_dir() else str(folder))
        keeper.close()
        keeper = Keeper(ix, library=library)
    if args.what or playlist_scope is not None:
        scope = playlist_scope or _scope(ix, args.what)
        if args.stop:
            if scope not in keeper.scopes():
                print(f"{scope.name} is not kept.", file=sys.stderr)
                return 1
            freed = keeper.drop(scope)
            print(f"{scope.name} is no longer kept. {units.size(freed) or 'Nothing'} freed.")
            return 0
        inside = keeper.kept(scope)
        if inside is not None and inside != scope:
            print(f"{scope.name} is kept already, with {inside.name}.")
            return 0
        files, size = keeper.size_of(scope)
        if not files and not scope.list:        # an empty list is kept: it fills later
            print(f"{scope.name} has no files.", file=sys.stderr)
            return 1
        plan = keeper.plan([scope])
        fits, need, free = keeper.room(plan)
        print(f"{scope.name}: {files:,} files, {units.size(size)}. To fetch: "
              f"{units.size(need) or 'nothing'}. Free on the disk: {units.size(free)}.")
        if not fits:
            print("! There is not room for it.", file=sys.stderr)
            return 1
        keeper.add(scope)
    elif args.stop:
        print("Stop keeping what? A collection, and a format and an artist in it.",
              file=sys.stderr)
        return 2
    if args.check:
        lost = keeper.check()
        print(f"{lost:,} files are not there any more." if lost else "Every file is there.")
    if not keeper.scopes():
        print("Nothing is kept on this computer. `bbmp keep Protracker/Jester/elysium.mod` "
              "keeps a song,\n`bbmp keep modland Protracker Jester` an artist's songs, "
              "`bbmp keep modland Protracker` a format,\n`bbmp keep hvsc` a whole "
              "collection.")
        return 0
    plan = keeper.plan()
    if not plan.done and not args.list:
        stop = threading.Event()
        last = [0.0]

        def say(p) -> None:
            if sys.stdout.isatty():
                print(f"\r  {p.line}   ", end="", flush=True)
            elif time.monotonic() - last[0] > 30:
                last[0] = time.monotonic()
                print(f"  {p.line}", flush=True)
        try:
            # asked for: now, whatever the hours in Settings; at its speed
            result = keeper.run(plan, say, stop, speed=Settings.load().keep_speed * 1024)
        except Busy as e:
            print(f"! {e}", file=sys.stderr)
            return 1
        except KeyboardInterrupt:
            stop.set()
            print("\nStopped. `bbmp keep` goes on from here.")
            return 130
        if sys.stdout.isatty():
            print()
        for path, why in result.failed[:20]:
            print(f"! {path}: {why}", file=sys.stderr)
        if len(result.failed) > 20:
            print(f"! and {len(result.failed) - 20:,} more", file=sys.stderr)
        if result.moved or result.removed:
            print(f"{result.moved:,} files moved with the collection, {result.removed:,} "
                  "taken away that nothing kept wants any more.")
        if result.stale:
            print(f"{result.stale:,} kept files the catalog no longer has are left as they are.")
        if result.stopped:
            print(f"! {result.stopped}", file=sys.stderr)
    print(f"Kept in {keeper.folder} ({units.size(keeper.size()) or 'nothing yet'}):")
    states = keeper.states()
    for state in states:
        print(f"  {state.line}")
    return 0 if all(s.complete for s in states) or args.list else 1


def cmd_radio(args) -> int:
    return _play([], radio=(args.format, args.artist, _collection(args)))


def cmd_command(args) -> int:
    from . import command
    if not command.packaged():
        print("The player isn't a Flatpak or an AppImage here: start it as you do now. (In "
              "an AppImage this makes a bbmp command in ~/.local/bin; in a Flatpak it says "
              "the line that makes it.)")
        return 0
    print(command.make())
    return 0


def cmd_wrong(args) -> int:
    from . import wrong
    n = wrong.count()
    if not n:
        print(f"No song is on the list yet. Press ! in the player when a song sounds wrong;\n"
              f"it is noted in {wrong.FILE}")
        return 0
    print(f"{n} song{' that sounds' if n == 1 else 's that sound'} wrong, in {wrong.FILE}\n"
          "(send that file; delete it to begin a new list)\n")
    print(wrong.FILE.read_text(encoding="utf-8").rstrip())
    return 0


def cmd_info(args) -> int:
    from .fetch import Fetcher

    print(f"ByteBandit Music Player {__version__}")
    if index_mod.Index.exists():
        ix = index_mod.Index()
        print(f"Catalog: {ix.song_count():,} songs, {ix.age_days():.1f} days old ({paths.CATALOG_DIR})")
        for _, name, songs, _ in ix.collections():
            print(f"         {songs:,} on {name}")
    else:
        print("Catalog: not downloaded yet")
    f = Fetcher()
    print(f"Cache:   {f.cache_size() / 1e6:.1f} MB in {paths.FILES_DIR}")
    from . import sandbox
    print("Sandbox: " + (f"every replayer runs shut in by {sandbox.protection()}"
                         if sandbox.available() else f"off: {sandbox.why_not()}"))
    if sandbox.mode() == "landlock":
        print(f"         (not bubblewrap: {sandbox._check()[1]})")
    from .backends import BACKEND_BY_FORMAT, pipe, sunvox, uade
    def status(ok: bool, script: str) -> str:
        return "found" if ok else f"missing (run tools/{script})"
    print(f"UADE:    {status(uade.available(), 'build-uade.sh')}  (Amiga)")
    print(f"SunVox:  {status(sunvox.LIB.exists(), 'install-sunvox.sh')}")
    # every helper program, with the formats it plays (from the player's own tables)
    backends: dict[str, list[str]] = {}
    for backend, (exe, _, _) in pipe.HELPERS.items():
        backends.setdefault(exe, []).append(backend)
    formats: dict[str, set[str]] = {}
    for fmt, backend in BACKEND_BY_FORMAT.items():
        formats.setdefault(backend, set()).add(fmt.removeprefix("Spectrum - "))
    script = {"bbmp-rv": "build-retrovert.sh"}
    found = sum(any(pipe.available(b) for b in bs) for bs in backends.values())
    print(f"Helpers: {found} of {len(backends)} found")
    for exe in sorted(backends):
        bs = backends[exe]
        if any(pipe.available(b) for b in bs):
            state = "found"
        elif all(pipe.left_out(b) for b in bs):
            state = "left out of this package"
        else:
            state = f"missing (run tools/{script.get(exe, 'build-' + exe.removeprefix('bbmp-') + '.sh')})"
        names = sorted({f for b in bs for f in formats.get(b, ())}, key=str.lower)
        shown = ", ".join(names[:4]) + (f" and {len(names) - 4} more" if len(names) > 4 else "")
        print(f"         {exe:<18} {state}" + (f"  ({shown})" if shown else ""))
    return 0


def _mmss(t: float | None) -> str:
    t = int(t or 0)
    return f"{t // 60:02d}:{t % 60:02d}"


def main(argv: list[str] | None = None) -> int:
    paths.move_old_folders()
    ap = argparse.ArgumentParser(
        prog="bbmp", description="ByteBandit Music Player: browse, search and stream "
                                 f"{_names()}, and music on this computer.",
        epilog="bbmp FILE or FOLDER opens the player and plays music from this computer.")
    ap.add_argument("--version", action="version", version=__version__)
    sub = ap.add_subparsers(dest="cmd")
    within = {"help": "only this collection (see `bbmp collections`)", "metavar": "NAME"}
    p = sub.add_parser("update", help="update the catalogs of your collections from their "
                                      "servers")
    p.add_argument("collections", nargs="*", metavar="COLLECTION",
                   help="update only these (default: every collection you have)")
    p.set_defaults(func=cmd_update)
    p = sub.add_parser("collections", help="the collections: their size, how old their list "
                                           "of files is, whether there is a newer one")
    p.add_argument("--servers", action="store_true", help="also the servers they are on")
    p.add_argument("--offline", action="store_true", help="don't ask their servers")
    p.add_argument("--use", nargs="+", metavar="NAME",
                   help="have these collections (and no others): bbmp collections --use modland hvsc")
    p.add_argument("--every", choices=["day", "week", "2weeks", "month", "never"],
                   help="how often the catalogs update by themselves")
    p.set_defaults(func=cmd_collections)
    p = sub.add_parser("new", help="what the collections have added or changed of late")
    p.add_argument("-d", "--days", type=int, default=7, metavar="N",
                   help="the last N days on which something changed (default 7)")
    p.add_argument("-c", "--collection", **within)
    p.set_defaults(func=cmd_new)
    sub.add_parser("downloads", help="the files some formats use that the player fetches "
                                     "when you say so").set_defaults(func=cmd_downloads)
    p = sub.add_parser("download", help="fetch one of them, or all")
    p.add_argument("names", nargs="+", metavar="NAME", help="as `bbmp downloads` lists them, or all")
    p.add_argument("--remove", action="store_true", help="remove it instead")
    p.add_argument("--again", action="store_true", help="fetch it though it is there")
    p.set_defaults(func=cmd_download)
    sub.add_parser("extras", help="the extra files some formats use (ROMs, sample sets), "
                                  "and where to put them").set_defaults(func=cmd_extras)
    p = sub.add_parser("keep", help="keep music on this computer, to play it without the "
                                    "servers: a song, an artist, a format or a collection",
                       epilog="Without a name: fetch what is missing of what is kept, "
                              "and show it.")
    p.add_argument("what", nargs="*", metavar="NAME",
                   help="a song's path; or a collection, then a format in it, then an "
                        "artist in that, then an album or game of theirs")
    p.add_argument("--stop", action="store_true", help="stop keeping it, and remove its files")
    p.add_argument("--list", action="store_true", help="only show what is kept")
    p.add_argument("--check", action="store_true", help="look whether every file is there")
    p.add_argument("--playlist", nargs="+", metavar="NAME",
                   help="a playlist (or: favorites), kept as it changes")
    p.add_argument("--folder", metavar="FOLDER",
                   help="have the kept music in this folder from now on, and move what "
                        f"is kept there (it is in {paths.KEPT_DIR} if nothing else is said)")
    p.set_defaults(func=cmd_keep)
    p = sub.add_parser("search", help="search the catalogs")
    p.add_argument("words", nargs="+")
    p.add_argument("-n", "--limit", type=int, default=100, help="at most this many (default 100)")
    p.add_argument("-c", "--collection", **within)
    p.set_defaults(func=cmd_search)
    p = sub.add_parser("formats", help="list formats: ✓ verified against the original, "
                                       "! plays with faults or gaps, ✗ can't be played")
    p.add_argument("format", nargs="*", help="only formats with this in their name, with notes")
    p.add_argument("-n", "--notes", action="store_true", help="what is known about each format")
    p.add_argument("-c", "--collection", **within)
    p.set_defaults(func=cmd_formats)
    p = sub.add_parser("replayers", help="what plays each format: versions and licences")
    p.add_argument("replayer", nargs="*", help="only these, with what they are made of")
    p.add_argument("-d", "--details", action="store_true", help="what each is made of")
    p.set_defaults(func=cmd_replayers)
    p = sub.add_parser("play", help="play files or folders on this computer, a song's path, "
                                    "or search results, in the terminal")
    p.add_argument("what", nargs="+")
    p.add_argument("-c", "--collection", **within)
    p.set_defaults(func=cmd_play)
    p = sub.add_parser("export", help="write songs as FLAC, MP3, Ogg, Opus or WAV files")
    p.add_argument("what", nargs="+", help="a file, a folder, a song's path as `bbmp search` "
                                           "prints it, or search words")
    p.add_argument("-o", "--output", help=f"folder to write to (default {paths.EXPORT_DIR})")
    p.add_argument("-f", "--format", choices=["flac", "mp3", "ogg", "opus", "wav"],
                   help="the kind of file (default: as in Settings, which is FLAC at first)")
    p.add_argument("--wav", action="store_true", help="the same as --format wav")
    p.add_argument("--all", action="store_true", help="export every song that matches")
    p.add_argument("--one", action="store_true", help="only the start song of files with subsongs")
    p.add_argument("--playlist", action="store_true",
                   help="WHAT is a playlist's name (or favorites): all its songs, in a folder")
    p.add_argument("--artist", action="store_true",
                   help="WHAT is an artist: everything by them, in a folder")
    p.add_argument("--no-level", action="store_true",
                   help="as loud as the songs are, not at the player's even loudness")
    p.add_argument("-c", "--collection", **within)
    p.set_defaults(func=cmd_export)
    p = sub.add_parser("import", help="make playlists from .m3u files")
    p.add_argument("files", nargs="+")
    p.add_argument("--name", help="the playlist's name (default: the file's name)")
    p.set_defaults(func=cmd_import)
    sub.add_parser("playlists", help="list your playlists").set_defaults(func=cmd_playlists)
    p = sub.add_parser("radio", help="play random songs, optionally from one format/artist")
    p.add_argument("format", nargs="?")
    p.add_argument("artist", nargs="?")
    p.add_argument("-c", "--collection", **within)
    p.set_defaults(func=cmd_radio)
    sub.add_parser("info", help="catalogs, cache and backend status").set_defaults(func=cmd_info)
    sub.add_parser("wrong", help="the songs you noted as sounding wrong (the ! key), and "
                   "where that list is").set_defaults(func=cmd_wrong)
    sub.add_parser("command", help="as an AppImage: make the bbmp command "
                   "(~/.local/bin/bbmp); as a Flatpak: say the line that makes it"
                   ).set_defaults(func=cmd_command)
    argv = sys.argv[1:] if argv is None else argv
    if argv and all(os.path.exists(os.path.expanduser(a)) for a in argv) \
            and argv[0] not in ("update", "search", "formats", "play", "radio", "info", "export",
                                "import", "playlists", "collections", "extras", "downloads",
                                "download", "new", "keep", "wrong", "replayers",
                                "command"):
        # `bbmp song.mod` or `bbmp ~/Music/AHX`: open the player and play them
        from .tui import run
        return run(play_paths=argv)
    args = ap.parse_args(argv)
    if not args.cmd:
        from .tui import run
        return run()
    return args.func(args)
