# SPDX-License-Identifier: GPL-3.0-or-later
"""The replayer helpers run shut in: no network, nothing to write, little to read.

By bubblewrap, or by Landlock where bubblewrap can't start (a Flatpak). The tests marked
with the `shut_in` fixture run in both ways, each where it works here."""

import array
import hashlib
import shutil
import socket
import subprocess
from pathlib import Path
import sys

import pytest

from bbmp import paths, sandbox

pytestmark = pytest.mark.skipif(not sandbox.available(), reason=sandbox.why_not())


def _use(mode: str, monkeypatch) -> None:
    monkeypatch.setenv("BBMP_SANDBOX", "landlock" if mode == "landlock" else "1")
    sandbox._check.cache_clear()
    if sandbox.mode() != mode:
        why = sandbox.why_not()
        sandbox._check.cache_clear()
        pytest.skip(f"{mode} doesn't work here: {why}")


@pytest.fixture(params=["bwrap", "landlock"])
def shut_in(request, monkeypatch):
    """Each sandbox in turn: its name."""
    _use(request.param, monkeypatch)
    yield request.param
    monkeypatch.undo()
    sandbox._check.cache_clear()


def launcher(mode: str) -> str:
    return "bwrap" if mode == "bwrap" else "bbmp-shut"


def shown(cmd: list[str]) -> list[str]:
    """What a sandboxed command is given to read, outside the system."""
    return [cmd[i + 1] for i, a in enumerate(cmd) if a in ("--ro-bind", "--rx", "--ro")
            and cmd[i + 1] not in sandbox._SYSTEM and not cmd[i + 1].startswith("/dev/")]


PROBE = """
import os, socket, sys, tempfile
music, song, secret, port, home = sys.argv[1:6]
def tried(what, do):
    try:
        do()
        print(what)
    except OSError:
        pass
tried("tcp", lambda: socket.create_connection(("127.0.0.1", int(port)), 2))
try:
    socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(b"x", ("127.0.0.1", int(port)))
except OSError:
    pass
tried("wrote", lambda: open(os.path.join(music, "written"), "w").write("x"))
try:
    open(os.path.join(home, "written"), "w").write("x")     # seen from outside, or not
except OSError:
    pass
tried("song", lambda: open(song, "rb").read())
tried("secret", lambda: open(secret).read())
try:
    if "Documents" in os.listdir(home):
        print("listed-home")
except OSError:
    pass
open(os.path.join(tempfile.gettempdir(), "scratch"), "w").write("fine")   # its own /tmp
print("tmp-" + open(os.path.join(tempfile.gettempdir(), "scratch")).read())
tried("wrote-tmp", lambda: open("/tmp/bbmp-sandbox-probe", "w").write("x"))
"""


def test_a_helper_reads_its_song_and_nothing_else(tmp_path, monkeypatch, shut_in):
    home = tmp_path / "home"
    music = home / ".cache/bbmp/files/modland/AHX/Pink"
    music.mkdir(parents=True)
    song = music / "inside.ahx"
    song.write_bytes(b"THX")
    secret = home / "Documents/private.txt"
    secret.parent.mkdir(parents=True)
    secret.write_text("private")
    monkeypatch.setenv("HOME", str(home))
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    port = listener.getsockname()[1]
    datagrams = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    datagrams.bind(("127.0.0.1", port))
    datagrams.setblocking(False)
    probe = Path("/tmp/bbmp-sandbox-probe")
    probe.unlink(missing_ok=True)
    try:
        cmd = sandbox.wrap([sys.executable, "-c", PROBE, str(music), str(song), str(secret),
                            str(port), str(home)], song)
        assert Path(cmd[0]).name == launcher(shut_in)
        p = sandbox.popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out = p.communicate(timeout=30)[0].decode().split()
        with pytest.raises(BlockingIOError):
            datagrams.recv(10)                  # nothing came by UDP either
    finally:
        listener.close()
        datagrams.close()
    # no network, nothing written outside its own /tmp, the private file unread
    # (bubblewrap's /tmp is its own: what it writes there is gone with it)
    assert out == ["song", "tmp-fine"] + (["wrote-tmp"] if shut_in == "bwrap" else [])
    assert not (music / "written").exists() and not (home / "written").exists()
    assert not probe.exists()


def test_landlock_says_what_it_protects(monkeypatch):
    _use("landlock", monkeypatch)
    try:
        abi = sandbox.landlock_abi()
        assert abi >= 1 and f"Landlock ABI {abi}" in sandbox.protection()
        assert "Landlock" in sandbox.why_not() and "BBMP_SANDBOX=landlock" in sandbox.why_not()
        assert ("TCP" in sandbox.protection()) == (abi >= 4)
    finally:
        sandbox._check.cache_clear()


def test_it_can_be_turned_off(monkeypatch, tmp_path):
    monkeypatch.setenv("BBMP_SANDBOX", "0")
    sandbox._check.cache_clear()
    try:
        assert sandbox.wrap(["x"], tmp_path / "song") == ["x"]
        assert "turned off" in sandbox.why_not()
    finally:
        monkeypatch.delenv("BBMP_SANDBOX")
        sandbox._check.cache_clear()


def test_a_song_in_the_home_folder_does_not_open_the_home_folder(tmp_path, monkeypatch, shut_in):
    home = tmp_path / "home"
    (home / "Documents").mkdir(parents=True)
    song = home / "tune.mod"
    song.write_bytes(b"M.K.")
    monkeypatch.setenv("HOME", str(home))
    binds = shown(sandbox.wrap(["true"], song))
    assert str(song) in binds
    assert str(home) not in binds and str(tmp_path) not in binds and "/" not in binds[1:]


def test_the_big_libraries_play_in_a_sandboxed_process(tmp_path, mod_bytes, shut_in):
    from bbmp.backends import open_song
    from bbmp.backends.remote import Remote
    song = tmp_path / "beep.mod"
    song.write_bytes(mod_bytes)
    d = open_song(song, "Protracker")
    try:
        assert isinstance(d, Remote) and d.backend == "openmpt"
        assert Path(d._proc.args[4]).name == launcher(shut_in)    # after the limits shell
        pcm = d.read(4800)
        assert len(pcm) == 4800 * 4 and any(pcm)
        assert d.channel_levels() is not None and len(d.channel_levels()) == 4
        row, lines = d.pattern_view(5)
        assert lines and d.details().facts
        assert d.seek(1.0)
    finally:
        d.close()
    assert d._proc is None


def test_a_replayer_that_dies_ends_its_song_and_nothing_else(tmp_path, mod_bytes):
    from bbmp.backends import open_song
    song = tmp_path / "beep.mod"
    song.write_bytes(mod_bytes)
    d = open_song(song, "Protracker")
    d._proc.kill()                      # as a crash in the library would
    d._proc.wait()
    assert d.read(4800) == b""          # the song ends; the player goes on
    d.close()


def test_what_a_replayer_says_is_not_trusted_beyond_its_data():
    from bbmp.backends.remote import _info
    info = _info({"title": "x" * 10000, "subsongs": 10 ** 9, "duration": "soon",
                  "extra": {"k": ["not", "text"]}})
    assert len(info.title) == 2000 and info.subsongs == 10000 and info.duration is None
    assert info.extra == {"k": ""}


def test_a_song_opened_in_a_thread_that_ends_still_plays(tmp_path, mod_bytes, shut_in):
    # bubblewrap ends the sandbox with the thread that started it (and bbmp-shut's
    # replayer is killed when that thread ends): the next song is opened in a
    # short-lived thread, and was dead by the time it was to play
    import threading
    from bbmp.backends import open_song
    song = tmp_path / "beep.mod"
    song.write_bytes(mod_bytes)
    opened = []
    t = threading.Thread(target=lambda: opened.append(open_song(song, "Protracker")))
    t.start()
    t.join()
    d = opened[0]
    try:
        import time
        time.sleep(0.5)
        assert d._proc.poll() is None
        assert len(d.read(4800)) == 4800 * 4
    finally:
        d.close()


@pytest.mark.parametrize("garbage", [
    b"\x05\x00\x00\x00nojsn",                              # not JSON
    b"\x02\x00\x00\x00[]",                                  # not a header
    b'\x10\x00\x00\x00{"bytes": "many"}',                   # not a size
    b'\x13\x00\x00\x00{"bytes": 999999999}',                # more than an answer can be
])
def test_garbage_from_a_replayers_process_ends_the_song_and_nothing_else(garbage):
    import subprocess as sp
    from bbmp.backends.remote import Remote
    r = Remote.__new__(Remote)
    r.backend, r._levels = "fake", None
    r._proc = sp.Popen([sys.executable, "-c",
                        f"import sys; sys.stdin.readline(); sys.stdout.buffer.write({garbage!r}); "
                        "sys.stdout.flush(); sys.stdin.read()"],
                       stdin=sp.PIPE, stdout=sp.PIPE)
    try:
        assert r.read(100) == b""                   # an OSError inside, not a crash
        assert r._proc.wait(timeout=5) is not None  # and the process is ended
    finally:
        r.close()


def test_a_helper_that_says_too_much_before_its_first_line_is_refused(tmp_path, monkeypatch):
    import time
    from bbmp.backends import pipe
    from bbmp.backends.base import Unsupported
    script = tmp_path / "noisy"
    script.write_text("#!/bin/sh\nhead -c 50000000 /dev/zero\n")
    script.chmod(0o755)
    monkeypatch.setattr(pipe, "helper", lambda backend: [str(script)])
    t = time.monotonic()
    with pytest.raises(Unsupported):
        pipe.PipeDecoder("noisy", tmp_path / "song")
    assert time.monotonic() - t < 5


def test_a_helpers_first_line_is_checked(tmp_path, monkeypatch):
    from bbmp.backends import pipe
    script = tmp_path / "odd"
    script.write_text('#!/bin/sh\necho \'{"tracks": 2, "title": ["x"], "duration_ms": "long", '
                      '"facts": [1]}\'\nhead -c 4000 /dev/zero\n')
    script.chmod(0o755)
    monkeypatch.setattr(pipe, "helper", lambda backend: [str(script)])
    d = pipe.PipeDecoder("odd", tmp_path / "song")
    try:
        assert d.info.subsongs == 2 and d.info.title == "" and d.info.duration is None
        assert len(d.read(1000)) == 4000 and d.details() is not None
    finally:
        d.close()


def test_a_song_in_tmp_does_not_open_tmp(tmp_path, shut_in):
    # the desktop's sockets are in /tmp (/tmp/.X11-unix), and tmux's, and ssh-agent's
    import tempfile
    folder = Path(tempfile.mkdtemp(prefix="bbmp-test-"))
    try:
        song = folder / "tune.mod"
        song.write_bytes(b"M.K.")
        binds = shown(sandbox.wrap(["true"], song))
        assert str(folder.resolve()) in binds
        assert "/tmp" not in binds and str(Path(tempfile.gettempdir()).resolve()) not in binds
        loose = Path(tempfile.gettempdir()) / "bbmp-test-loose.mod"
        loose.write_bytes(b"M.K.")
        try:
            binds = shown(sandbox.wrap(["true"], loose))
            assert str(loose.resolve()) in binds and "/tmp" not in binds
        finally:
            loose.unlink()
    finally:
        shutil.rmtree(folder)


def test_a_replayer_gets_only_the_environment_it_needs():
    env = sandbox.environment({"PATH": "/usr/bin", "HOME": "/home/x", "DISPLAY": ":0",
                               "WAYLAND_DISPLAY": "wayland-0", "SSH_AUTH_SOCK": "/tmp/ssh",
                               "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
                               "GITHUB_TOKEN": "secret", "BBMP_YM2608_ROM": "/x/rom",
                               "LC_ALL": "C"})
    assert env == {"PATH": "/usr/bin", "HOME": "/home/x", "BBMP_YM2608_ROM": "/x/rom",
                   "LC_ALL": "C"}


def test_a_replayer_finds_the_players_folders_inside_a_flatpak():
    """A Flatpak points XDG_DATA_HOME at ~/.var/app/<id>/data: a sandboxed UADE that
    didn't get it looked for the downloaded Amiga players in ~/.local/share and found none."""
    env = sandbox.environment({"HOME": "/home/x", "XDG_DATA_HOME": "/home/x/.var/app/a/data",
                               "XDG_CACHE_HOME": "/home/x/.var/app/a/cache",
                               "XDG_CONFIG_HOME": "/home/x/.var/app/a/config"})
    assert env["XDG_DATA_HOME"] == "/home/x/.var/app/a/data"
    assert env["XDG_CACHE_HOME"] == "/home/x/.var/app/a/cache"
    assert env["XDG_CONFIG_HOME"] == "/home/x/.var/app/a/config"


@pytest.mark.skipif(sys.platform == "darwin", reason="macOS doesn't hold a process to RLIMIT_DATA")
def test_a_replayer_can_take_only_so_much_memory():
    """A damaged song once made a replayer use all the memory there was: every process
    the player starts is capped (sandbox.MEMORY_LIMIT)."""
    grab = "b = bytearray(%d); print('got it')" % (sandbox.MEMORY_LIMIT + (512 << 20))
    p = sandbox.popen([sys.executable, "-c", grab], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out, err = p.communicate(timeout=30)
    assert p.returncode != 0 and b"MemoryError" in err
    small = sandbox.popen([sys.executable, "-c", "b = bytearray(64 << 20); print('fine')"],
                          stdout=subprocess.PIPE)
    assert small.communicate(timeout=30)[0].strip() == b"fine"


def test_landlock_caps_memory_by_itself(tmp_path, monkeypatch):
    """bbmp-shut sets the cap too, so a caller that starts it without sandbox.popen
    (keep.py's 7z) is capped as well."""
    _use("landlock", monkeypatch)
    try:
        song = tmp_path / "x"
        song.write_bytes(b"x")
        grab = "b = bytearray(%d); print('got it')" % (sandbox.MEMORY_LIMIT + (512 << 20))
        done = subprocess.run(sandbox.wrap([sys.executable, "-c", grab], song),
                              capture_output=True, timeout=30)
        assert done.returncode != 0 and b"MemoryError" in done.stderr
    finally:
        sandbox._check.cache_clear()


def test_landlock_gives_each_replayer_a_temporary_folder_that_goes_with_it(tmp_path, monkeypatch):
    import gc
    _use("landlock", monkeypatch)
    try:
        song = tmp_path / "x"
        song.write_bytes(b"x")
        cmd = sandbox.wrap([sys.executable, "-c", "import os, tempfile; "
                            "open(os.path.join(tempfile.gettempdir(), 'f'), 'w').write('x'); "
                            "print(os.getcwd() == tempfile.gettempdir())"], song)
        folder = Path(cmd[cmd.index("--tmp") + 1])
        p = sandbox.popen(cmd, stdout=subprocess.PIPE)
        assert p.communicate(timeout=30)[0].strip() == b"True"
        assert (folder / "f").exists()
        del p
        gc.collect()
        assert not folder.exists()
    finally:
        sandbox._check.cache_clear()


def test_landlock_lets_a_caller_name_a_folder_to_write_into(tmp_path, monkeypatch):
    _use("landlock", monkeypatch)
    try:
        song = tmp_path / "x"
        song.write_bytes(b"x")
        out = tmp_path / "out"
        out.mkdir()
        write = ("import sys\nfor f in sys.argv[1:]:\n    try:\n        open(f, 'w').write('x')\n"
                 "        print('wrote', f)\n    except OSError:\n        pass\n")
        done = subprocess.run(sandbox.wrap([sys.executable, "-c", write, str(out / "a"),
                                            str(tmp_path / "b")], song, writable=[out]),
                              capture_output=True, text=True, timeout=30)
        assert done.stdout.split() == ["wrote", str(out / "a")]
    finally:
        sandbox._check.cache_clear()


# ---- real songs play the same in both sandboxes ----

def rms(pcm: bytes) -> float:
    a = array.array("h", pcm)
    return (sum(x * x for x in a) / max(1, len(a))) ** 0.5


# (path on Modland, the backend that plays it, whether it renders alike every time).
# libopenmpt dithers its 16-bit output with a random seed, libsidplayfp's and the
# CyberTracker and Onyx replayers start from random state: those are compared by
# backend, length and level.
REAL = [
    ("Archimedes Tracker/D.Salt/Bug music.musx", "xmp", True),
    ("AHX/Pink/inside.ahx", "uade", True),
    ("TFMX/Chris Huelsbeck/mdat.turrican flight", "uade", True),        # smpl.* beside it
    ("YM/Jochen Hippel/scout.ym", "ym", True),
    ("MoonBlaster (edit mode)/- unknown/FST-PRO/MIERDA.MBM", "kss", True),  # its MBK
    ("SCC-Musixx/- unknown/RUSSIA [50Hz].SNG", "sccmusixx", True),
    ("Cybertracker C64/Aki Latvamaki/instruktion.ct", "cybertracker", False),
    ("Onyx Music File/Tonza/tal.omf", "onyx", False),
    ("Psycle/Alk/waveshaper.psy", "psycle", True),
    ("Deflemask/Quirby64/V-Rally.dmf", "furnace", True),
    ("Spectrum/Pro Tracker 3/Macros/natashka.pt3", "zxtune", True),
    ("HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid", "sidplayfp", False),
    ("Protracker/Captain/space_debris.mod", "openmpt", False),
    ("Stonetracker/Bosco/the finalley.spm", "stonetracker", True),
    ("Zoundmonitor/AJ/hittheroad.sng", "zoundmonitor", True),          # ../Samples
]


@pytest.fixture(scope="module")
def songs():
    from bbmp.fetch import Fetcher
    from bbmp.index import Index
    if not Index.exists():
        pytest.skip("no catalog downloaded")
    f = Fetcher()
    f.index = Index()
    yield f
    f.close()


def _song(songs, path: str) -> tuple[Path, str]:
    entry = songs.index.by_path(path)
    if entry is None:
        pytest.skip("not in this catalog")
    local = paths.FILES_DIR / "modland" / path
    if not local.exists():
        try:
            local = songs.get(f"modland:{path}", songs.index.companions(f"modland:{path}"))
        except (OSError, EOFError):
            pytest.skip("Modland not reachable")
    return Path(local), entry.format


def _render(mode: str, song: Path, fmt: str, monkeypatch) -> tuple[str, bytes]:
    from bbmp.backends import open_song
    _use(mode, monkeypatch)
    d = open_song(song, fmt, [60.0] if song.suffix == ".sid" else [])
    try:
        assert Path(d._proc.args[4]).name == launcher(mode)    # after the limits shell
        return d.backend, b"".join(d.read(4800) for _ in range(50))
    finally:
        d.close()
        sandbox._check.cache_clear()


@pytest.mark.network
@pytest.mark.parametrize("path,backend,alike", REAL, ids=[r[1] for r in REAL])
def test_real_songs_play_the_same_in_both_sandboxes(songs, path, backend, alike, monkeypatch):
    from bbmp.backends import pipe
    if backend in pipe.HELPERS and not pipe.available(backend):
        pytest.skip(f"{backend} helper not built")
    monkeypatch.delenv("BBMP_DOWNLOADS")      # the replayers downloaded here (CyberTracker)
    song, fmt = _song(songs, path)
    played = {mode: _render(mode, song, fmt, monkeypatch) for mode in ("bwrap", "landlock")}
    (b1, pcm1), (b2, pcm2) = played["bwrap"], played["landlock"]
    assert b1 == b2 == backend and len(pcm1) == len(pcm2) == 50 * 4800 * 4
    if alike:
        assert hashlib.sha1(pcm1).hexdigest() == hashlib.sha1(pcm2).hexdigest()
    else:
        assert rms(pcm1) > 100 and abs(rms(pcm1) - rms(pcm2)) < 0.02 * rms(pcm1)


@pytest.mark.network
@pytest.mark.skipif(not shutil.which("7z"), reason="needs 7z")
def test_an_archive_unpacks_the_same_in_both_sandboxes(songs, tmp_path, monkeypatch):
    from bbmp import archives, downloads
    # TFMX's player is downloaded on request: this computer's, read in both sandboxes
    real = paths.DATA_DIR / "downloads"
    monkeypatch.setenv("BBMP_DOWNLOADS", str(real))
    if not downloads.installed(downloads.get("amigaplayers")):
        pytest.skip("UADE's Amiga players are not downloaded on this computer")
    song, fmt = _song(songs, "TFMX/Chris Huelsbeck/mdat.turrican flight")
    src = tmp_path / "src/TFMX"
    src.mkdir(parents=True)
    for name in (song.name, "smpl" + song.name[4:]):
        shutil.copy(song.parent / name, src / name)
    packed = tmp_path / "songs.7z"
    subprocess.run(["7z", "a", "-bd", str(packed), "TFMX"], cwd=src.parent, check=True,
                   capture_output=True)
    got = {}
    for mode in ("bwrap", "landlock"):
        _use(mode, monkeypatch)
        try:
            monkeypatch.setattr(paths, "CACHE_DIR", tmp_path / f"cache-{mode[0]}")   # paths as long: UADE hears the length
            folder = archives.unpacked(packed)
            got[mode] = {p.relative_to(folder).as_posix(): p.read_bytes()
                         for p in folder.rglob("*") if p.is_file() and p.name != ".complete"}
            played = folder / "TFMX" / song.name
            got[mode + "-pcm"] = _render(mode, played, fmt, monkeypatch)
        finally:
            sandbox._check.cache_clear()
    assert sorted(got["bwrap"]) == ["TFMX/" + song.name, "TFMX/smpl" + song.name[4:]]
    assert got["bwrap"] == got["landlock"]
    assert got["bwrap-pcm"] == got["landlock-pcm"] and got["bwrap-pcm"][0] == "uade"


def test_an_appimages_folders_are_shown_to_the_replayers(monkeypatch, tmp_path):
    from bbmp import sandbox
    run_from, loader = tmp_path / ".mount_bbmp", tmp_path / ".bbmp-ld-2.44"
    monkeypatch.setenv("BBMP_BUNDLE", f"{run_from}:{loader}")
    assert {run_from, loader} <= set(sandbox.readable())
    monkeypatch.delenv("BBMP_BUNDLE")
    assert sandbox.bundled() == []
