# SPDX-License-Identifier: GPL-3.0-or-later
"""MPRIS: the D-Bus interface desktops use for media keys and "now playing" widgets.

With this running, the keyboard's play/pause, next and previous keys, `playerctl`,
and the desktop's media widget all control the player and show the current song.

It runs a GLib main loop in its own thread and talks to the player through the same
thread-safe commands the TUI uses. If there is no session bus, it does nothing.
"""

from __future__ import annotations

import os
import threading
import urllib.parse

from . import collection as collections_
from .backends.pipe import HELPERS
from .player import Player, Status
from .settings import Settings

PATH = "/org/mpris/MediaPlayer2"
ROOT = "org.mpris.MediaPlayer2"
PLAYER = "org.mpris.MediaPlayer2.Player"
NAME = "org.mpris.MediaPlayer2.bbmp"

XML = f"""
<node>
  <interface name="{ROOT}">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="{PLAYER}">
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Play"/>
    <method name="Seek"><arg name="Offset" type="x" direction="in"/></method>
    <method name="SetPosition">
      <arg name="TrackId" type="o" direction="in"/>
      <arg name="Position" type="x" direction="in"/>
    </method>
    <method name="OpenUri"><arg name="Uri" type="s" direction="in"/></method>
    <signal name="Seeked"><arg name="Position" type="x"/></signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="LoopStatus" type="s" access="readwrite"/>
    <property name="Rate" type="d" access="readwrite"/>
    <property name="Shuffle" type="b" access="readwrite"/>
    <property name="Metadata" type="a{{sv}}" access="read"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>
"""

LOOP = {"off": "None", "one": "Track", "all": "Playlist"}
WATCHED = ("PlaybackStatus", "Metadata", "Volume", "LoopStatus", "Shuffle", "CanSeek")


class Mpris:
    def __init__(self, player: Player, settings: Settings):
        self.player = player
        self.settings = settings
        self._loop = None
        self._conn = None
        self._thread: threading.Thread | None = None
        self._last: dict = {}
        self.name: str | None = None
        self._context = None

    # ---- life cycle ----

    def start(self) -> bool:
        if not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
            return False
        try:
            import gi
            gi.require_version("Gio", "2.0")
            from gi.repository import Gio, GLib
        except (ImportError, ValueError):
            return False
        self._gio, self._glib = Gio, GLib
        self._thread = threading.Thread(target=self._run, name="mpris", daemon=True)
        self._thread.start()
        return True

    def stop(self) -> None:
        if self._loop is not None:
            self._loop.quit()
        if self._thread is not None:
            self._thread.join(timeout=2)

    def _run(self) -> None:
        Gio, GLib = self._gio, self._glib
        context = self._context = GLib.MainContext.new()
        context.push_thread_default()
        self._loop = GLib.MainLoop.new(context, False)
        try:
            self._conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        except GLib.Error:
            return
        node = Gio.DBusNodeInfo.new_for_xml(XML)
        for iface in node.interfaces:
            self._conn.register_object(PATH, iface, self._call, self._get, self._set)
        self.name = NAME
        # a second player running at the same time gets its own name, as the spec asks
        reply = self._request_name(NAME)
        if reply != 1:
            self.name = f"{NAME}.instance{os.getpid()}"
            self._request_name(self.name)
        source = GLib.timeout_source_new(250)
        source.set_callback(self._poll)
        source.attach(context)
        self._loop.run()
        context.pop_thread_default()

    def _request_name(self, name: str) -> int:
        GLib = self._glib
        result = self._conn.call_sync(
            "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
            GLib.Variant("(su)", (name, 4)), GLib.VariantType("(u)"), 0, -1, None)   # 4: don't queue
        return result.unpack()[0]

    # ---- properties ----

    def _metadata(self, st: Status) -> dict:
        V = self._glib.Variant
        if st.song is None:
            return {"mpris:trackid": V("o", "/org/mpris/MediaPlayer2/TrackList/NoTrack")}
        song, info = st.song, st.info
        title = song.title
        if info and info.subsongs > 1:
            title += f" ({info.subsong + 1}/{info.subsongs})"
        source = collections_.get(song.collection)
        url = source.url(song.path) if source else "file://" + urllib.parse.quote(song.path)
        meta = {
            "mpris:trackid": V("o", f"/org/mpris/MediaPlayer2/Track/{max(0, st.queue_pos)}"),
            "xesam:title": V("s", title),
            "xesam:artist": V("as", [a for a in (song.artist, song.partner) if a and a != "- unknown"]),
            "xesam:album": V("s", song.album_name or song.format),
            "xesam:genre": V("as", [song.format]),
            "xesam:url": V("s", url),
        }
        if info and info.duration:
            meta["mpris:length"] = V("x", int(info.duration * 1_000_000))
        return meta

    def _values(self, st: Status) -> dict:
        V = self._glib.Variant
        return {
            "PlaybackStatus": V("s", {"playing": "Playing", "loading": "Playing",
                                      "paused": "Paused"}.get(st.state, "Stopped")),
            "LoopStatus": V("s", LOOP[self.settings.repeat]),
            "Rate": V("d", 1.0),
            "Shuffle": V("b", self.settings.shuffle),
            "Metadata": V("a{sv}", self._metadata(st)),
            "Volume": V("d", st.volume / 100),
            "Position": V("x", int(st.elapsed * 1_000_000)),
            "MinimumRate": V("d", 1.0),
            "MaximumRate": V("d", 1.0),
            "CanGoNext": V("b", True),
            "CanGoPrevious": V("b", True),
            "CanPlay": V("b", st.song is not None),
            "CanPause": V("b", st.song is not None),
            "CanSeek": V("b", st.backend in ("openmpt", "gme", "uade", "sidplayfp", "sunvox")
                         or st.backend in HELPERS),      # a helper starts at any position
            "CanControl": V("b", True),
        }

    def _get(self, conn, sender, path, iface, prop):
        V = self._glib.Variant
        if iface == ROOT:
            return {"CanQuit": V("b", False), "CanRaise": V("b", False),
                    "HasTrackList": V("b", False), "Identity": V("s", "ByteBandit Music Player"),
                    "SupportedUriSchemes": V("as", ["file"]),
                    "SupportedMimeTypes": V("as", [])}.get(prop)
        return self._values(self.player.status()).get(prop)

    def _set(self, conn, sender, path, iface, prop, value):
        if prop == "Volume":
            self.player.set_volume(round(max(0.0, min(1.0, value.unpack())) * 100))
        elif prop == "Shuffle":
            self.settings.update(shuffle=bool(value.unpack()))
        elif prop == "LoopStatus":
            mode = {v: k for k, v in LOOP.items()}.get(value.unpack())
            if mode is None:
                return False
            self.settings.update(repeat=mode)
        elif prop != "Rate":
            return False
        return True

    # ---- methods ----

    def _call(self, conn, sender, path, iface, method, params, invocation):
        p, st = self.player, self.player.status()
        if method == "PlayPause":
            p.toggle_pause()
        elif method == "Pause":
            if st.state == "playing":
                p.toggle_pause()
        elif method == "Play":
            if st.state == "paused":
                p.toggle_pause()
            elif st.state == "stopped" and st.queue_len:
                p.jump(max(0, st.queue_pos))
        elif method == "Stop":
            p.stop()
        elif method == "Next":
            p.next()
        elif method == "Previous":
            p.prev()
        elif method == "Seek":
            p.seek(params.unpack()[0] / 1_000_000)
        elif method == "SetPosition":
            p.seek(params.unpack()[1] / 1_000_000 - st.elapsed)
        elif method == "OpenUri":
            uri = params.unpack()[0]
            # file:///home/x/a%20song.mod, or file://localhost/...
            path = self._gio.File.new_for_uri(uri).get_path() if uri.startswith("file:") else None
            song = p.index.local_song(path) if path else None
            if song is not None:
                p.play_list([song])
        if method in ("Seek", "SetPosition"):
            # on this thread's own loop, not the program's default one (which nobody runs)
            source = self._glib.timeout_source_new(300)
            source.set_callback(self._seeked)
            source.attach(self._context)
        invocation.return_value(None)

    def _seeked(self) -> bool:
        pos = int(self.player.status().elapsed * 1_000_000)
        self._conn.emit_signal(None, PATH, PLAYER, "Seeked", self._glib.Variant("(x)", (pos,)))
        return False

    # ---- change notifications ----

    def _poll(self) -> bool:
        st = self.player.status()
        values = self._values(st)
        key = {k: values[k].print_(True) for k in WATCHED}
        changed = {k: values[k] for k in WATCHED if key[k] != self._last.get(k)}
        self._last = key
        if changed and self._conn is not None:
            V = self._glib.Variant
            self._conn.emit_signal(None, PATH, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                   V("(sa{sv}as)", (PLAYER, changed, [])))
        return True

