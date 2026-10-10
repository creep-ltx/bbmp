# SPDX-License-Identifier: GPL-3.0-or-later
"""The replayers run shut in: the helper programs, and the big libraries in processes
of their own (backends/remote.py).

They are old C code, much of it written for this player in a hurry, and they read
files that come from the internet. So each runs in a sandbox (bubblewrap, the one
Flatpak uses): no network, the whole system read-only, the home folder hidden but for
what a replayer needs to read (the replayers themselves, the song's folder, the
collections' files, the extra files), and a /tmp of its own that is gone when it
ends. It can still use the processor and write sound to the player; nothing else.

Where bubblewrap can't start (inside a Flatpak, which is a sandbox of its own and
gives none of the user namespaces bubblewrap needs), each replayer is shut in by
Landlock instead, the kernel's own sandbox for a process to put itself in: the small
launcher bbmp-shut (csrc/shut, tools/build-shut.sh) sets it up and starts the
replayer. It may read what it may under bubblewrap (the system's programs and
libraries, the same folders of the home folder) and nothing else, write only into a
folder of its own (removed when it ends) and the folders a caller names (to unpack
into), and it has no network (a seccomp filter refuses it any socket; Landlock also
refuses TCP from ABI 4 on), no signals to processes outside it, and no abstract (ABI 6)
or path (ABI 9) Unix sockets outside. Unlike bubblewrap it still sees the names of
files it can't open. An older kernel's Landlock does what it can (`protection()`
says what). BBMP_SANDBOX=landlock uses Landlock where bubblewrap would work too.

Each also gets at most MEMORY_LIMIT of memory, sandbox or not: a damaged song once made
a replayer use all the memory there was. Past the limit, the helper's allocations
fail and it stops; the song doesn't play, and nothing else is harmed.

On macOS each replayer is shut in by the system's own sandbox (Seatbelt, through
/usr/bin/sandbox-exec): it may read the system and what it may read under bubblewrap,
write only into a temporary folder of its own and the folders a caller names, and it
has no network.

With neither (no bubblewrap and a kernel without Landlock, or bbmp-shut not built)
the helpers run as they are: `why_not()` says why, and the player warns once at start
(`warning()`). BBMP_SANDBOX=0 turns it off,
for looking into a replayer.
"""

from __future__ import annotations

import atexit
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import weakref
from concurrent.futures import ThreadPoolExecutor
from functools import lru_cache
from pathlib import Path

from . import paths


# the most memory (its data: heap and private mappings) a replayer may take; the
# largest measured on real songs is under 100 MB (Psycle, 86 MB)
MEMORY_LIMIT = 2 << 30


# Every process the player starts goes through this small shell first: it sets the
# limits and then becomes the program (exec, so the same process). The limits used to be
# set by Popen's preexec_fn, which runs Python in the child between fork and exec; in a
# program with threads (this one has many) that can deadlock. A shell's `ulimit` does it
# with no Python in the child. -d is the data limit in KiB; -c 0: a replayer that crashes
# on a damaged song leaves no core dump (systemd-coredump would keep one, with the song's
# memory in it, for every crash). A limit the system refuses (macOS may refuse one lower
# than what is in use) is left as it is.
_LIMITS = ('ulimit -d %d 2>/dev/null; ulimit -c 0 2>/dev/null; exec "$@"'
           % (MEMORY_LIMIT >> 10))


def limited(cmd: list[str]) -> list[str]:
    """The command, started with the memory limit and no core dumps."""
    return ["/bin/sh", "-c", _LIMITS, "bbmp-limit", *cmd]


def _bwrap() -> str:
    """"" when bubblewrap works here, else why it doesn't."""
    bwrap = shutil.which("bwrap")
    if bwrap is None:
        return "bubblewrap (bwrap) is not installed"
    try:
        done = subprocess.run([bwrap, "--ro-bind", "/", "/", "--dev", "/dev", "--unshare-all",
                               "--die-with-parent", "--", "true"],
                              capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as e:
        return f"bubblewrap can't start: {e}"
    if done.returncode != 0:
        return "bubblewrap can't start here: " + (done.stderr.strip().splitlines() or ["?"])[-1]
    return ""


def shut() -> str | None:
    """The Landlock launcher, bbmp-shut, if it is built."""
    path = paths.helper_prefix("shut") / "bin" / "bbmp-shut"
    return str(path) if path.exists() else shutil.which("bbmp-shut")


@lru_cache(maxsize=1)
def _landlock() -> tuple[int, str]:
    """(Landlock's ABI version, "") when bbmp-shut can shut a program in here, else
    (0, why not)."""
    launcher = shut()
    if launcher is None:
        return 0, "bbmp-shut is not built (tools/build-shut.sh)"
    try:
        abi = int(subprocess.run([launcher, "--abi"], capture_output=True, text=True,
                                 timeout=10).stdout.strip() or 0)
        if abi < 1:
            return 0, "this kernel has no Landlock"
        done = subprocess.run([launcher, "--rx", "/", "--", "true"], capture_output=True,
                              text=True, timeout=10)
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        return 0, f"bbmp-shut can't start: {e}"
    if done.returncode != 0:
        return 0, "Landlock can't be used here: " + (done.stderr.strip().splitlines() or ["?"])[-1]
    return abi, ""


# what Landlock refuses a replayer, from the ABI version on that it can
LANDLOCK_REFUSES = ((1, "reading and writing outside its folders"),
                    (2, "moving files between folders"), (3, "truncating files outside"),
                    (4, "TCP"), (5, "device ioctls"),
                    (6, "signals and abstract Unix sockets outside"),
                    (9, "Unix sockets by path outside"))


@lru_cache(maxsize=1)
def _check() -> tuple[str, str]:
    """(how the replayers are shut in: "bwrap", "landlock" or "", why not bubblewrap)."""
    asked = (paths.env("SANDBOX") or "1").lower()
    if asked == "0":
        return "", "turned off (BBMP_SANDBOX=0)"
    if sys.platform == "darwin":
        why = _seatbelt()
        return ("seatbelt", "") if not why else ("", why)
    why = "BBMP_SANDBOX=landlock" if asked == "landlock" else _bwrap()
    if not why:
        return "bwrap", ""
    abi, no_landlock = _landlock()
    if abi:
        return "landlock", why
    return "", f"{why}; and no Landlock: {no_landlock}"


SANDBOX_EXEC = "/usr/bin/sandbox-exec"


def _seatbelt() -> str:
    """"" when macOS's sandbox works here, else why it doesn't."""
    if not Path(SANDBOX_EXEC).exists():
        return "sandbox-exec is not there"
    try:
        done = subprocess.run([SANDBOX_EXEC, "-p", "(version 1)(allow default)(deny network*)",
                               "/usr/bin/true"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as e:
        return f"sandbox-exec can't start: {e}"
    if done.returncode != 0:
        return "sandbox-exec can't start here: " + (done.stderr.strip().splitlines() or ["?"])[-1]
    return ""


def mode() -> str:
    """How the replayers are shut in: "bwrap" (bubblewrap), "landlock", "seatbelt"
    (macOS), or "" (not)."""
    return _check()[0]


def landlock_abi() -> int:
    return _landlock()[0] if mode() == "landlock" else 0


def protection() -> str:
    """What protection is in force, in a sentence."""
    if mode() == "bwrap":
        return "bubblewrap: no network, nothing to write, the home folder hidden"
    if mode() == "seatbelt":
        return "macOS sandbox: no network, nothing to write, the home folder unreadable"
    if mode() == "landlock":
        abi = landlock_abi()
        refused = ", ".join(what for since, what in LANDLOCK_REFUSES if abi >= since)
        return f"Landlock ABI {abi}: no sockets (seccomp); Landlock refuses {refused}"
    return "none: " + _check()[1]


_spawner: ThreadPoolExecutor | None = None
_spawner_lock = threading.Lock()


# What a replayer is given of the environment: nothing that leads out of the sandbox
# (DISPLAY, WAYLAND_DISPLAY, DBUS_SESSION_BUS_ADDRESS, SSH_AUTH_SOCK, tokens, ...).
_PASSED = ("PATH", "HOME", "USER", "LOGNAME", "LANG", "LANGUAGE", "TZ", "LD_LIBRARY_PATH",
           "PYTHONPATH", "PYTHONHOME", "PYTHONDONTWRITEBYTECODE", "PYTHONIOENCODING")


def environment(env: dict[str, str] | None = None) -> dict[str, str]:
    """The environment for a sandboxed process: what a replayer needs of `env`."""
    env = dict(os.environ if env is None else env)
    return {k: v for k, v in env.items()
            if k in _PASSED or k.startswith(("LC_", "BBMP_", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"))}


def popen(cmd: list[str], **kwargs) -> subprocess.Popen:
    """subprocess.Popen, started from one thread that lives as long as the player.

    bubblewrap's --die-with-parent ends the sandbox when the *thread* that started it
    ends, not the program: a song opened ahead in a short-lived thread was killed the
    moment that thread was done, and played only what had been read ahead.
    A sandboxed process gets only the environment a replayer needs, and every one
    at most MEMORY_LIMIT of memory and no core dump (limited()). A Landlock one's temporary folder is removed
    with it."""
    global _spawner
    shut_in = available() and cmd and cmd[0] in (shutil.which("bwrap"), shut(), SANDBOX_EXEC)
    if shut_in:
        kwargs["env"] = environment(kwargs.get("env"))
    if cmd and not kwargs.get("cwd") and shutil.which(
            cmd[0], path=(kwargs.get("env") or os.environ).get("PATH", os.defpath)) is None:
        # as Popen itself would: the shell in front would only exit 127
        raise FileNotFoundError(2, "No such file or directory", cmd[0])
    with _spawner_lock:
        if _spawner is None:
            _spawner = ThreadPoolExecutor(max_workers=1, thread_name_prefix="sandbox-spawner")
    proc = _spawner.submit(subprocess.Popen, limited(cmd), **kwargs).result()
    if shut_in and "--tmp" in cmd:
        weakref.finalize(proc, shutil.rmtree, cmd[cmd.index("--tmp") + 1], True)
    elif shut_in and cmd[0] == SANDBOX_EXEC:
        tmp = next(a.split("=", 1)[1] for a in cmd if a.startswith("TMPDIR="))
        weakref.finalize(proc, shutil.rmtree, tmp, True)
    return proc


def available() -> bool:
    return mode() != ""


def warning() -> str:
    """"" when the replayers are shut in; else a plain warning, with how to mend it,
    for the player to show once at start."""
    if available():
        return ""
    if (paths.env("SANDBOX") or "").lower() == "0":
        return ("The replayers are not shut in (BBMP_SANDBOX=0): a damaged or harmful song "
                "file could do anything you can.")
    how = ("install bubblewrap (the bwrap program)" if sys.platform != "darwin"
           else "check that /usr/bin/sandbox-exec works")
    return ("Warning: the replayers run without a sandbox, so a harmful song file could "
            f"reach your files and the network. To fix it, {how}"
            + (", or build bbmp-shut (tools/build-shut.sh) for a kernel with Landlock"
               if sys.platform != "darwin" else "")
            + f". Why: {why_not()}")


def why_not() -> str:
    """"" when bubblewrap shuts the replayers in; else why not, and what does instead."""
    if mode() == "seatbelt":
        return ""
    if mode() == "landlock":
        return f"{_check()[1]}; shut in by {protection()}"
    return _check()[1]


def readable() -> list[Path]:
    """What every helper may read in the home folder."""
    opt = paths.OPT
    return [opt, *(p for p in (paths.UADE_PREFIX, paths.ZXTUNE_PREFIX, paths.CHIP_PREFIX,
                                paths.FURNACE_PREFIX, paths.GME_PREFIX, paths.XMP_PREFIX,
                                paths.RV_PREFIX)
                   if not p.is_relative_to(opt)),
            paths.DATA_DIR, paths.FILES_DIR, _kept(), paths.RHYTHM_ROM.parent,
            Path.home() / ".local/lib", *bundled()]


def bundled() -> list[Path]:
    """In an AppImage: the folder it runs from, and the C library's loader its programs
    ask for. Both are under /tmp, which a replayer is otherwise never shown; the
    AppImage's start (packaging/appimage/AppRun) names them in BBMP_BUNDLE."""
    return [Path(p) for p in os.environ.get("BBMP_BUNDLE", "").split(":") if p]


_kept_cache: tuple[float, Path] | None = None


def _kept() -> Path:
    """The folder of saved music, wherever the user has put it. The settings are read
    again only when they have changed."""
    global _kept_cache
    from .settings import Settings
    try:
        changed = paths.SETTINGS.stat().st_mtime
    except OSError:
        changed = 0.0
    if _kept_cache is None or _kept_cache[0] != changed:
        _kept_cache = (changed, paths.kept_dir(Settings.load().keep_dir))
    return _kept_cache[1]


def _open_to_all() -> list[Path]:
    """Folders a sandbox never shows, nor any folder above them: the home folder, and
    the ones where programs leave their sockets (the desktop's, tmux's, ssh-agent's)
    and temporary files."""
    out = []
    for p in (Path.home(), Path(tempfile.gettempdir()), Path("/tmp"), Path("/var/tmp"),
              Path("/run"), Path("/dev/shm")):
        try:
            out.append(p.resolve())
        except OSError:
            pass
    return out


def _shown(song: Path, more: list[Path]) -> list[Path]:
    """What a replayer may read besides the system: the replayers, the collections and
    the other folders of readable(), `more`, and a song's folder and the one above it
    (some formats keep their samples beside the artists' folders, Zoundmonitor/Samples,
    and files on this computer too); never the home folder, /tmp, or a folder above
    them."""
    seen: list[Path] = []
    never = _open_to_all()
    for p in [*readable(), *more, song.parent.parent, song.parent, song]:
        try:
            p = Path(p).resolve()
        except OSError:
            continue
        if p in seen or not p.exists():
            continue
        if p != song.resolve() and any(n.is_relative_to(p) for n in never):
            continue            # the home folder, /tmp, or a folder above them: never
        seen.append(p)
    return seen


# What every replayer may read (and run) of the system under Landlock: the programs
# and libraries, the dynamic linker's cache, the time zone; inside a Flatpak, /app.
# (Landlock's rules hold for what a link leads to: /lib is /usr/lib here.)
_SYSTEM = ("/usr", "/lib", "/lib64", "/bin", "/sbin", "/app", "/etc/ld.so.cache",
           "/etc/ld.so.conf", "/etc/ld.so.conf.d", "/etc/localtime", "/etc/alternatives")
_DEVICES = (("--rw", "/dev/null"), ("--ro", "/dev/zero"), ("--ro", "/dev/urandom"),
            ("--ro", "/dev/random"))

_tmp_root: Path | None = None


def _running(pid: int) -> bool:
    """Is that process still there? (Not /proc: macOS has none.)"""
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _private_tmp() -> Path:
    """A new temporary folder for one replayer, in a folder of this player's that goes
    when the player does. What players that were killed left is removed."""
    global _tmp_root
    if _tmp_root is None or not _tmp_root.is_dir():
        base = Path(tempfile.gettempdir())
        for old in base.glob("bbmp-shut-*"):
            try:
                pid = int(old.name.rsplit("-", 1)[1])
                if old.stat().st_uid == os.getuid() and not _running(pid):
                    shutil.rmtree(old, ignore_errors=True)
            except (ValueError, OSError):
                pass
        if _tmp_root is None:
            _tmp_root = base / f"bbmp-shut-{os.getpid()}"
            atexit.register(shutil.rmtree, _tmp_root, True)
        _tmp_root.mkdir(mode=0o700, exist_ok=True)
    return Path(tempfile.mkdtemp(dir=_tmp_root))


def _wrap_landlock(cmd: list[str], song: Path, more: list[Path],
                   writable: list[Path]) -> list[str]:
    args = [shut(), "--data", str(MEMORY_LIMIT), "--tmp", str(_private_tmp())]
    for p in _SYSTEM:
        args += ["--rx", p]
    for p in _shown(song, more):
        args += ["--rx", str(p)]
    for flag, dev in _DEVICES:
        args += [flag, dev]
    for p in writable:
        args += ["--rw", str(Path(p).resolve())]
    return [*args, "--", *cmd]


# What every replayer may read (and run) of macOS: the programs, libraries and frameworks,
# the dynamic linker's cache, the time zone, Homebrew's libraries
_MAC_SYSTEM = ("/usr", "/bin", "/System", "/Library/Apple", "/private/var/db/dyld",
               "/private/var/db/timezone", "/private/etc", "/opt/homebrew", "/usr/local")


def _quote(p: Path | str) -> str:
    return '"' + str(p).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _seatbelt_profile(song: Path, more: list[Path], writable: list[Path], tmp: Path) -> str:
    """Everything refused but what a replayer needs: to read the system and the folders
    it may read under bubblewrap, to write into its own temporary folder and the ones it
    is given, and no network (refused with the rest)."""
    # and the Python the player runs on: the big libraries play in processes of it
    pythons = {str(Path(p).resolve()) for p in (sys.base_prefix, sys.prefix)}
    readable_ = [*_MAC_SYSTEM, *sorted(pythons), *(str(p) for p in _shown(song, more))]
    written = [str(tmp.resolve()), *(str(Path(p).resolve()) for p in writable)]
    lines = ["(version 1)", "(deny default)",
             '(import "system.sb")',         # Apple's own: what any program needs to start
             "(allow process-exec process-fork)", "(allow signal (target same-sandbox))",
             "(allow sysctl-read)", "(allow file-read-metadata)",
             '(allow file-read* file-write-data (literal "/dev/null") (literal "/dev/zero")'
             ' (literal "/dev/random") (literal "/dev/urandom") (literal "/dev/dtracehelper"))',
             "(allow file-read* process-exec (subpath " + ") (subpath ".join(map(_quote, readable_)) + "))",
             "(allow file-read* file-write* (subpath " + ") (subpath ".join(map(_quote, written)) + "))"]
    return "\n".join(lines)


def _wrap_seatbelt(cmd: list[str], song: Path, more: list[Path],
                   writable: list[Path]) -> list[str]:
    tmp = _private_tmp()
    return [SANDBOX_EXEC, "-p", _seatbelt_profile(song, more, writable, tmp),
            "/usr/bin/env", f"TMPDIR={tmp}", *cmd]


def wrap(cmd: list[str], song: Path, more: list[Path] = (),
         writable: list[Path] = ()) -> list[str]:
    """The command, shut in, to play `song` (or to read a file: an archive).
    `writable`: folders it may write into, where one is needed (to unpack into)."""
    if mode() == "seatbelt":
        return _wrap_seatbelt(cmd, song, list(more), list(writable))
    if mode() == "landlock":
        return _wrap_landlock(cmd, song, list(more), list(writable))
    if not available():
        return cmd
    home = Path.home()
    args = [shutil.which("bwrap"), "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc",
            "--tmpfs", "/tmp", "--tmpfs", "/run"]
    # other people's and removable disks, and this user's home: hidden
    for hidden in ("/home", "/media", "/mnt", "/srv", str(home)):
        if Path(hidden).is_dir() and not Path(hidden).is_symlink():
            args += ["--tmpfs", hidden]
    for p in _shown(song, more):
        args += ["--ro-bind", str(p), str(p)]
    # a song under a link (a kept folder on another disk) is found by its link too
    never = _open_to_all()
    if (song.parent.resolve() != song.parent and song.parent.exists()
            and not any(n.is_relative_to(song.parent.resolve()) for n in never)):
        args += ["--ro-bind", str(song.parent.resolve()), str(song.parent)]
    for p in writable:
        args += ["--bind", str(Path(p).resolve()), str(Path(p).resolve())]
    args += ["--chdir", "/tmp", "--unshare-all", "--die-with-parent", "--new-session", "--"]
    return args + cmd
