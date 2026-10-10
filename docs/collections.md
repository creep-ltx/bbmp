# Collections

A collection is an archive of music the player can browse, search and play from.
There are six: Modland; HVSC, which Modland hosts; Keygen Music; ASMA (Atari 8-bit);
ZX-Art (ZX Spectrum); and SNDH (Atari ST). Each is a module in `src/bbmp/collection/`. The player is built so that another one is one new
module: nothing else in the player names Modland.

## What is shared and what is per collection

| | |
|---|---|
| Catalog | One database per collection, `catalogs/<id>.db`, for the collections the user has chosen (`catalogs.py`). They are opened together as one (`Index` joins their tables into views), so search and Browse cover all of them or one. SQLite opens 11 files at once at the most: more collections than that need another way. |
| Cache | One folder per collection: `~/.cache/bbmp/files/<id>/<path>`. The size limit is for all of it together. |
| Kept music | What the user keeps to play without the servers (`keep.py`): a song, an artist, a format or all of it, in `~/Music/ByteBandit Music Player/<the collection's folder>/<path>` unless the user names another place, with `kept.db` beside it. The folder is the collection's name (`folder` says another). A song that is there is played from there. |
| Favorites, playlists, history, queue | Shared. They hold songs from any collection and files on this computer, mixed. |
| Replayers | Shared. A song's format picks the replayer, whichever collection it is from. |

## How a song is saved

As its key: the collection's id, a colon, and the path the file has there.

```
modland:AHX/Pink/inside.ahx
/home/me/Music/own.mod              a file on this computer
AHX/Pink/inside.ahx                 without a collection: Modland (how songs were saved before)
```

`collection.key()` makes one, `collection.split()` takes one apart, `Song.key` is a
song's, and `Index.resolve(key)` finds the song again.

## Adding one

1. Write `src/bbmp/collection/<name>.py` with a subclass of `Collection`
   (`collection/base.py`). `collection/modland.py` is the example.
2. Register it at the end of `collection/__init__.py`: `register(Name())`. The order
   of registration is the order of preference when two collections have the same file.
3. Run `bbmp update <id>`. The other collections' songs stay as they are.

What the class has to say:

| | |
|---|---|
| `id` | Lower-case letters and digits, e.g. `hvsc`. It is in every saved song and names the cache folder, so it can't change later. |
| `name`, `home` | As shown, and its web page. |
| `listing(progress)` | Download the file list and return a `Listing`: an `Entry` per file, and optionally how long songs play and what changed of late (`added`, `updated`, `deleted`, or `moved` with the path the file has now). Raise `OSError` when the server can't be reached; the catalog then keeps what it had. Leave out a path that `safe_path()` doesn't take (`..`, an empty part, a leading `/`). `catalog_dir` is the folder of the catalog there was, to compare with. |
| `done()` | The update is over: let go of anything kept for it (Modland's lists). |
| `about` | A line for the choice at the first start and in Settings: what it has, how big it is. |
| `hosted_by`, `host_path()`, `guest_path()`, `via_host` | Only for a collection another one has all of (HVSC in Modland): where the host has a file, and back. `via_host` says whether `listing()` is to use the host's list (the host is chosen too) or the collection's own. |
| `parse(path)` | The `Entry` for a path on its own. Used for songs an old playlist names that have moved. |
| `transport()` | How files are downloaded: `FtpTransport(host, root)` or `HttpTransport(base_url)` from `collection/transports.py`, or a `Transport` of your own. |
| `url(path)` | The link to a file, written into exported playlists. |
| `path_in(url)` | The reverse: the path a link points at, or `None` if it isn't a link into this collection. Imported playlists are matched with it. |
| `has_lists`, `published_lists()`, `read_list()` | Only if the collection publishes playlists of its own, as Modland does. |

An `Entry` says where a file is filed:

| | |
|---|---|
| `path` | Where it is in the collection. |
| `format` | The category it is browsed under, which also picks the replayer. Use the names Modland's formats have (`bbmp formats` lists them) where the format is the same, so that the songs end up together and play: `Protracker`, `HVSC`, `Video Game Music - Sega Megadrive`. |
| `artist`, `album`, `name` | `"- unknown"` for no artist, `""` for no album. The title is `name` without its extension. |
| `hidden` | True for files that belong to a song: `companions.is_companion()` knows them. |
| `md5` | The file's checksum, if the collection publishes them. With it, what a server gives is checked, a file that two collections have shows once, and kept music knows what has changed. |
| `size` | In bytes, if the collection says. The player shows sizes and checks the disk's room with them. |

A `Listing` can also say what the collection has changed of late (`changes`: day,
"added" or "updated", path), which "★ New" shows, and when the collection made its
list (`dated`). `latest()` asks the server for that date alone, which is how the
player knows at its start that there is a newer list.

## Several servers

`transport()` can give `Mirrors([...])` (`collection/transports.py`): servers that
have the same files. The fastest is asked first: each server's time to the first byte is kept (for as
long as the program runs), and one not heard from yet keeps its place in the list.
A server gets 3 s to start answering (`FIRST_BYTE`), then 30 s for the body; a
silent one is passed over and asked last from then on, and if none answers in 3 s,
they are asked again with 30 s. A server that doesn't have the file is passed over. `Below("HVSC", ...)` is a server that has one folder of the collection
only, under a path of its own.

If the collection's file list has checksums, the fetcher checks what a server gives
against them, and passes over a server that gives another file. When every server
gives another file than the list says, the file was changed since the list was
downloaded, and it is taken.

The file list itself is asked of the servers in the same order.

A collection whose server can be copied as it is builds its servers with
`mirrored(self.id, root, [...])` instead of `Mirrors([...])`: when the user has a home
mirror (the setting `mirror`, "Home mirror" in Settings, or `BBMP_MIRROR`), it puts
`HttpTransport(<mirror>/<id><root>)` first and marks it `Mirrors.home`. `root` is
where the collection's files start on its own server (`/pub/modules` for Modland,
`/asma/` for ASMA, `/sndh/sndh_lf/` for SNDH, `/kgm/` for Keygen Music), so a mirror is
the server's layout below a folder named by the collection's id. The home mirror is
asked first even by a fetcher that asks another server first (`Mirrors.first`); one
that didn't answer is asked last for the rest of the run, for every collection
(its address is remembered), so away from home it costs one wait of `FIRST_BYTE`.
ZX-Art uses plain `Mirrors`: it isn't mirrored.

## A collection that another one hosts

HVSC is a collection of its own, and Modland has all of it, in its HVSC folder. So
HVSC is a collection that Modland hosts (`collection/hvsc.py`, `hosted_by`):

- It is browsed and searched in its own layout: Musicians by musician, Games and
  Demos by title. Modland's view is as it was, with HVSC as one of its formats.
- Its songs are Modland's. `hvsc:MUSICIANS/H/Hubbard_Rob/Commando.sid` names the song
  that is saved as `modland:HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid`, so a tune is
  the same favorite, the same line in the history and the same file in the cache,
  whichever way it was come by.
- It has no transport and no file list of its own: its list is the part of Modland's
  that is below `HVSC/`.
- Browsing or searching every collection leaves it out, since its host has every
  song of it.

A hosted collection says `hosted_by = "<the host's id>"` and has `host_path(path)`,
which gives the path the host has the file at.

## The same file in two collections

Browsing or searching one collection shows all it has. Browsing or searching all of
them shows a file once when two collections have it, and that is decided by the
checksums (`md5`) the collections publish, never by names: two songs called
"intro.mod" by "Jester" can be two files, or two people. A collection that publishes
no checksums is never merged with another.

The same goes for artists. An artist's page lists the songs filed under that name in
every collection, says which collections, and says that the same name in two
collections can be two people.

## What is not prepared

These are needed by some collections and don't exist yet:

- **Songs inside a collection's archives.** Scene.org has many .zip and .lha files.
  Archives on this computer play (archives.py); a collection whose paths name songs
  inside archives would need the fetcher to get the archive and unpack it there.
- **MP3, Ogg and FLAC.** Scene.org's music is mostly those. There is no replayer for
  them yet.
- **Collections without a file list.** Where there is nothing like Modland's
  `allmods_md5.zip` to download, `listing()` has to walk the server or read its web
  pages, which is slow and needs the owner's consent.
- **Information that isn't files.** Demozoo and Pouet are databases about
  productions and people. They would add to what is shown about a song, and are not
  collections in this sense.

## Tests

`tests/conftest.py` has a second collection, Chipland, that exists only in the tests,
and `tests/test_collections.py` runs the player with both: catalog, search, the cache,
playlists, favorites, the player and the TUI.
