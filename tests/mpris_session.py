# SPDX-License-Identifier: GPL-3.0-or-later
"""Run under `dbus-run-session`: a player with MPRIS on a private bus, driven with gdbus.

Prints one line per check; test_mpris.py asserts on them.
"""

import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import conftest  # noqa: E402
from bbmp import index as index_mod  # noqa: E402
from bbmp.audio import NullOutput  # noqa: E402
from bbmp.mpris import NAME, PATH, PLAYER, Mpris  # noqa: E402
from bbmp.player import Player  # noqa: E402
from bbmp.settings import Settings  # noqa: E402


def gdbus(*args: str) -> str:
    out = subprocess.run(["gdbus", *args], capture_output=True, text=True, timeout=10)
    return (out.stdout + out.stderr).strip()


def call(method: str, *args: str) -> str:
    return gdbus("call", "--session", "--dest", NAME, "--object-path", PATH,
                 "--method", f"{PLAYER}.{method}", *args)


def prop(name: str) -> str:
    return gdbus("call", "--session", "--dest", NAME, "--object-path", PATH, "--method",
                 "org.freedesktop.DBus.Properties.Get", PLAYER, name)


def wait_for(cond, timeout=5.0) -> bool:
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.05)
    return False


def main() -> None:
    tmp = Path(tempfile.mkdtemp())
    raw = "\n".join(f"{i:032x} {p}" for i, p in enumerate(conftest.SONGS)).encode()
    index_mod.build({"modland": conftest.modland.read_list(raw)}, tmp / "index.db")
    ix = index_mod.Index(tmp / "index.db")
    mod = conftest.make_mod()
    fetcher = conftest.LocalFetcher(tmp / "cache", {p: mod for p in conftest.SONGS if p.endswith(".mod")})
    settings = Settings()
    player = Player(ix, fetcher, NullOutput(realtime=True), settings=settings)
    mpris = Mpris(player, settings)
    print("started", mpris.start())
    print("owned", wait_for(lambda: "org.mpris.MediaPlayer2.bbmp" in gdbus(
        "call", "--session", "--dest", "org.freedesktop.DBus", "--object-path", "/org/freedesktop/DBus",
        "--method", "org.freedesktop.DBus.ListNames")))

    player.play_list(ix.songs("Protracker", "Tester"))
    print("playing", wait_for(lambda: "Playing" in prop("PlaybackStatus")))
    print("length", wait_for(lambda: "mpris:length" in prop("Metadata")))
    meta = prop("Metadata")
    print("metadata", "'xesam:title': <'beep'>" in meta and "Tester" in meta)

    call("PlayPause")
    print("paused", wait_for(lambda: "Paused" in prop("PlaybackStatus")) and player.status().state == "paused")
    call("Play")
    print("resumed", wait_for(lambda: player.status().state == "playing"))
    call("Next")
    print("next", wait_for(lambda: "beep two" in prop("Metadata")))
    call("Previous")
    print("previous", wait_for(lambda: "'beep'" in prop("Metadata")))
    time.sleep(0.4)
    monitor = subprocess.Popen(["gdbus", "monitor", "--session", "--dest", NAME],
                               stdout=subprocess.PIPE, text=True)
    time.sleep(0.3)
    call("Seek", "2000000")
    print("seek", wait_for(lambda: player.status().elapsed >= 2.0))
    time.sleep(0.8)
    monitor.terminate()
    print("seeked", "Seeked" in monitor.communicate(timeout=5)[0])

    song = tmp / "a song.mod"
    song.write_bytes(mod)
    call("OpenUri", "file://localhost" + str(song).replace(" ", "%20"))
    print("openuri", wait_for(lambda: player.status().song is not None
                              and player.status().song.path == str(song)))

    gdbus("call", "--session", "--dest", NAME, "--object-path", PATH, "--method",
          "org.freedesktop.DBus.Properties.Set", PLAYER, "Volume", "<0.5>")
    print("volume", wait_for(lambda: player.status().volume == 50) and settings.volume == 50)
    gdbus("call", "--session", "--dest", NAME, "--object-path", PATH, "--method",
          "org.freedesktop.DBus.Properties.Set", PLAYER, "LoopStatus", "<'Playlist'>")
    print("loop", wait_for(lambda: settings.repeat == "all") and "Playlist" in prop("LoopStatus"))

    call("Stop")
    print("stopped", wait_for(lambda: "Stopped" in prop("PlaybackStatus")))
    mpris.stop()
    player.shutdown()
    print("done")


if __name__ == "__main__":
    main()
