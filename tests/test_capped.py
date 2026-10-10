# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads with a ceiling (capped.py, 8 October 2026): a zip bomb or an endless
answer from a bad mirror can't fill the memory or the disk."""

import io
import zipfile

import pytest

from bbmp import capped
from bbmp.collection.modland import file_list as real_file_list   # conftest replaces it


def test_a_download_stops_at_its_ceiling():
    assert capped.read(io.BytesIO(b"x" * 10), 10) == b"x" * 10
    with pytest.raises(capped.TooBig):
        capped.read(io.BytesIO(b"x" * 11), 10)
    with pytest.raises(OSError):                      # TooBig is an OSError: next server
        capped.copy(io.BytesIO(b"x" * (3 << 20)), io.BytesIO(), 2 << 20)


def _bomb(size: int) -> bytes:
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("allmods.txt", b"\0" * size)
    return out.getvalue()


def test_modlands_file_list_is_unpacked_only_so_far(monkeypatch):
    """A zip bomb from a plain http or ftp server can't take all the memory."""
    from bbmp.collection import modland
    modland.forget()
    monkeypatch.setattr(capped, "LIST", 1 << 20)
    bomb = _bomb(8 << 20)
    asked = []

    def urlopen(url, timeout=0):
        asked.append(url)
        return io.BytesIO(bomb)
    monkeypatch.setattr(modland.urllib.request, "urlopen", urlopen)
    with pytest.raises(OSError, match="file list"):
        real_file_list()
    assert len(asked) == len(modland.SERVERS) + len(modland.FTP_SERVERS)   # each refused
    modland.forget()


def test_a_song_download_stops_at_its_ceiling(tmp_path, monkeypatch):
    from test_collections import _server
    from bbmp.collection.transports import HttpTransport
    (tmp_path / "srv").mkdir()
    (tmp_path / "srv/endless.mod").write_bytes(b"x" * 5000)
    monkeypatch.setattr(capped, "SONG", 1000)
    server, url = _server(tmp_path / "srv")
    try:
        t = HttpTransport(url)
        with pytest.raises(capped.TooBig):
            t.download("endless.mod", tmp_path / "got")
        assert (tmp_path / "got").stat().st_size <= 1000
        t.close()
    finally:
        server.shutdown()
        server.server_close()
