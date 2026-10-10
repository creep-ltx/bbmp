#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build UADE (Amiga replayer emulator) and its bencode-tools dependency into
# a user prefix, since the system package needs root. Default: ~/.local/opt/uade
set -euo pipefail
. "$(dirname "$0")/sources.sh"
PREFIX="${1:-$BBMP_OPT/uade}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# pinned: the versions the player was tested with, the ones in modland-player-sources
BENCODE_COMMIT=5fa73d33780cc5a70b8958b955a01d8d12aa7660
ZAKALWE_COMMIT=f40771d472d1a3846969678d893846a48c16b025
UADE_COMMIT=8d77cd773e10d33de44f2b6c52201bb54732b701
fetch() {       # repository, commit, folder
  if kept; then kept_unpack "$3-${2:0:7}.tar.xz" "$3"; return; fi
  git init -q "$3" && git -C "$3" fetch -q --depth 1 "$1" "$2" && git -C "$3" checkout -q FETCH_HEAD
}

fetch https://gitlab.com/heikkiorsila/bencodetools.git "$BENCODE_COMMIT" bencodetools
(cd bencodetools && ./configure --prefix="$PREFIX" --without-python && make -j"$JOBS" && make install)

fetch https://gitlab.com/hors/libzakalwe.git "$ZAKALWE_COMMIT" libzakalwe
(cd libzakalwe && ./configure && make && mkdir -p "$PREFIX/lib" && make install PREFIX="$PREFIX")

fetch https://gitlab.com/uade-music-player/uade.git "$UADE_COMMIT" uade
cd uade
# A file the Amiga side asks for (TFMX's smpl.*, instruments) is looked for one folder
# at a time from /, each folder listed for a name in any case. Under Landlock (the
# sandbox inside a Flatpak) only the song's own folders may be listed, so a folder
# that is there as named is taken without listing the one above it; listing found it
# first anyway (an exact name wins), so what is found is the same.
python3 - src/frontends/common/unixsupport.c <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = "\t\tif (uade_amiga_scandir(real, dirname, fake, sizeof(real))) {\n\t\t\t/* found matching entry */\n\t\t\tif (strlcat(dirname, real, sizeof(dirname)) >= sizeof(dirname)) {\n\t\t\t\tuade_warning(\"Too long dir path (%s + %s)\\n\",\n\t\t\t\t\t     dirname, real);\n\t\t\t\treturn -1;\n\t\t\t}\n\t\t\tif (strlcat(dirname, \"/\""
new = "\t\tif (bbmp_exact_dir(real, dirname, fake, sizeof(real)) ||\n\t\t    uade_amiga_scandir(real, dirname, fake, sizeof(real))) {\n\t\t\t/* found matching entry */\n\t\t\tif (strlcat(dirname, real, sizeof(dirname)) >= sizeof(dirname)) {\n\t\t\t\tuade_warning(\"Too long dir path (%s + %s)\\n\",\n\t\t\t\t\t     dirname, real);\n\t\t\t\treturn -1;\n\t\t\t}\n\t\t\tif (strlcat(dirname, \"/\""
helper = """
/* bbmp: a folder there under the name asked for, found without listing its parent */
static int bbmp_exact_dir(char *real, const char *dirname, const char *fake, int ml)
{
\tchar path[PATH_MAX];
\tstruct stat st;
\tif (snprintf(path, sizeof path, "%s%s", dirname, fake) >= (int) sizeof path)
\t\treturn 0;
\tif (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
\t\treturn 0;
\treturn (int) strlcpy(real, fake, ml) < ml;
}

/* Find file in amiga namespace */"""
if "bbmp_exact_dir" not in s:
    assert s.count(old) == 1 and s.count("\n/* Find file in amiga namespace */") == 1
    s = s.replace(old, new).replace("\n/* Find file in amiga namespace */", helper, 1)
    open(p, "w").write(s)
PY
grep -q bbmp_exact_dir src/frontends/common/unixsupport.c || { echo "the patch did not apply" >&2; exit 1; }
# Only the players whose terms allow passing them on are installed (see the end of this
# script); the player downloads the others on request (bbmp download amigaplayers) into
# a folder of their own. A player that isn't in <base>/players is looked for in the
# folder BBMP_UADE_PLAYERS names; its ENV: files are then found beside it, as UADE looks
# for those in the player's own folder. And a player that can't be loaded is named in
# the song's information (playerfname), so that the caller can say which one is missing.
python3 - src/frontends/common/uadestate.c src/frontends/common/uadeconf.c <<'PY'
import sys
helper = """
/* bbmp: a player not in <basedir>/players is looked for in BBMP_UADE_PLAYERS */
#include <stdlib.h>
#include <unistd.h>
static void bbmp_player_path(char *out, size_t n, const char *basedir, const char *name)
{
\tconst char *extra = getenv("BBMP_UADE_PLAYERS");
\tsnprintf(out, n, "%s/players/%s", basedir, name);
\tif (extra != NULL && extra[0] && strchr(name, '/') == NULL && access(out, R_OK) != 0)
\t\tsnprintf(out, n, "%s/%s", extra, name);
}
"""
edits = {
    sys.argv[1]: [
        ("\n\t\tsnprintf(playername, sizeof playername, \"%s/players/%s\",\n\t\t\t state->config.basedir.name, ep->playername);",
         "\n\t\tbbmp_player_path(playername, sizeof playername,\n\t\t\t state->config.basedir.name, ep->playername);"),
        ("\tif (player == NULL) {\n\t\tuade_warning(\"Error: Could not load player\\n\");",
         "\tif (player == NULL) {\n\t\t/* bbmp: name the player that could not be loaded */\n"
         "\t\tstrlcpy(song->info.playerfname, playername[0] ? playername : state->config.player_file.name,\n"
         "\t\t\tsizeof song->info.playerfname);\n\t\tuade_warning(\"Error: Could not load player\\n\");"),
        ("\nstatic struct eagleplayer *get_eagleplayer(", helper + "\nstatic struct eagleplayer *get_eagleplayer("),
    ],
    sys.argv[2]: [
        ("snprintf(playername, playernamelen, \"%s/players/%s\", uc->basedir.name, attributes->s);",
         "bbmp_player_path(playername, playernamelen, uc->basedir.name, attributes->s);"),
        ("\nstatic void uade_add_ep_option(", helper + "\nstatic void uade_add_ep_option("),
    ],
}
for path, pairs in edits.items():
    s = open(path).read()
    if "bbmp_player_path" in s:
        continue
    for old, new in pairs:
        assert s.count(old) == 1, (path, old)
        s = s.replace(old, new)
    open(path, "w").write(s)
PY
[ "$(grep -c 'bbmp_player_path(' src/frontends/common/uadestate.c src/frontends/common/uadeconf.c | awk -F: '{s += $2} END {print s}')" = 4 ] &&
  grep -q 'name the player that could not be loaded' src/frontends/common/uadestate.c ||
  { echo "the players-folder patch did not apply" >&2; exit 1; }
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" CFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib" \
  ./configure --prefix="$PREFIX" --libzakalwe-prefix="$PREFIX" --bencode-tools-prefix="$PREFIX" --without-write-audio --without-uade123 --without-uadesimple --without-uadefs
make -j"$JOBS"
make install

# ---- Only the Amiga players whose authors' terms allow passing them on stay installed
# (bbmp's survey of all 176, 7 October 2026: written terms that are free, public domain,
# LGPL/BSD, or non-commercial for a player that is run, not linked). The others, and the
# ENV: files only they read, are downloaded by the player from this same UADE commit on
# GitLab when asked (src/bbmp/downloads.py, "amigaplayers"), and found through
# BBMP_UADE_PLAYERS (the patch above). S/ holds UADE's own empty placeholder.
FINE_PLAYERS=(
  ArtOfNoise-4V ArtOfNoise-8V     # FREEWARE sources (Bastian Spiegel), STC decruncher non-commercial
  Fred JochenHippel_UADE ManiacsOfNoise MusicAssembler   # UADE's own re-codings, LGPL
  MED Octa-MED Octa-MED-Ss        # RBF Software: "permitted for any free programm"
  Medley                          # Paul van der Valk: free to use, with his name
  MusiclineEditor                 # LGPL
  MusicMaker-4V MusicMaker-8V     # BSD (Thomas Winischhofer)
  Pokeynoise                      # freeware, source included (Achim Haertel)
  PreTracker                      # LGPL glue + PreTracker 1.5's own replay (Pink/Abyss)
  SIDMon1.0                       # LGPL
  SoundMon2.0 SoundMon2.2         # public domain (Brian Postma)
)
PLAYERS="$PREFIX/share/uade/players"
for f in "$PLAYERS"/*; do
  name="$(basename "$f")"
  [ "$name" = S ] && continue
  printf '%s\n' "${FINE_PLAYERS[@]}" | grep -qxF "$name" || rm -rf "$f"
done
left="$(cd "$PLAYERS" && find . -mindepth 1 -maxdepth 1 | sed 's|^\./||' | LC_ALL=C sort | tr '\n' ' ')"
want="$(printf '%s\n' "${FINE_PLAYERS[@]}" S | LC_ALL=C sort | tr '\n' ' ')"
[ "$left" = "$want" ] && [ "$(find "$PLAYERS/S" -type f | wc -l)" -le 1 ] || {
  echo "UADE's players folder holds more (or less) than the players that may be passed on:" >&2
  echo "  there: $left" >&2; echo "  wanted: $want" >&2; exit 1; }

if [ "$(uname -s)" = Darwin ]; then
  # libzakalwe's own build names it /usr/local/lib/libzakalwe.dylib: name the copy here,
  # in it and in whatever uses it, and sign each again (Apple Silicon runs nothing else)
  for f in "$PREFIX"/lib/*.dylib "$PREFIX"/lib/uade/uadecore; do
    [ -f "$f" ] && [ ! -L "$f" ] || continue
    for dep in $(otool -L "$f" | awk 'NR > 1 && $1 ~ "^/usr/local/lib/" {print $1}'); do
      [ -f "$PREFIX/lib/$(basename "$dep")" ] && install_name_tool -change "$dep" "$PREFIX/lib/$(basename "$dep")" "$f"
    done
    case "$f" in *.dylib) install_name_tool -id "$PREFIX/lib/$(basename "$f")" "$f" ;; esac
    codesign --force --sign - "$f"
  done
fi
echo "UADE installed in $PREFIX"
