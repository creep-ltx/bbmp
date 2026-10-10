# SPDX-License-Identifier: GPL-3.0-or-later
"""A package built without some engine (BBMP_PUBLIC=1, left-out.txt) says so for its
formats, rather than failing some other way."""

import pytest

from bbmp import formats_info
from bbmp.backends import Unsupported, is_supported, open_song, pipe


def test_a_left_out_engine_says_it_is_not_in_this_package(tmp_path, monkeypatch):
    prefix = tmp_path / "bbmp-amiga"
    (prefix / "bin").mkdir(parents=True)
    (prefix / "bin" / "bbmp-amiga").write_text("#!/bin/sh\nexit 1\n")
    (prefix / "left-out.txt").write_text(
        "jesperolsen\tJesper Olsen's songs are not in this package: from his games\n")
    monkeypatch.setitem(pipe.HELPERS, "jesperolsen", ("bbmp-amiga", prefix, ["jesperolsen"]))
    monkeypatch.setitem(pipe.HELPERS, "maxtrax", ("bbmp-amiga", prefix, ["maxtrax"]))
    song = tmp_path / "song.jo"
    song.write_bytes(b"\0" * 64)

    assert pipe.left_out("jesperolsen") == "Jesper Olsen's songs are not in this package: from his games"
    assert not pipe.available("jesperolsen")
    with pytest.raises(Unsupported, match="not in this package"):
        open_song(song, "Jesper Olsen")
    # the other engines of the same helper are there
    assert pipe.left_out("maxtrax") is None
    # and the lists say so before it is played: not playable, and why
    assert not is_supported("Jesper Olsen")
    info = formats_info.about("Jesper Olsen")
    assert info.group == "no" and "not in this package" in info.text


def test_a_helper_without_a_list_leaves_nothing_out(tmp_path, monkeypatch):
    monkeypatch.setitem(pipe.HELPERS, "pmd", ("bbmp-chip", tmp_path, ["pmd"]))
    assert pipe.left_out("pmd") is None
    assert pipe.left_out("openmpt") is None          # not a helper's
