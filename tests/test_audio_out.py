# SPDX-License-Identifier: GPL-3.0-or-later
"""libbbmp-out, the sound output on macOS, tested through miniaudio's silent null device:
the whole path, and nothing reaches the speakers."""

import time

import pytest

from bbmp import audio
from bbmp.backends.base import FRAME_BYTES, RATE

pytestmark = pytest.mark.skipif(not audio.out_library().exists(),
                                reason="libbbmp-out is not built (tools/build-out.sh)")


def test_the_silent_device_takes_sound_at_the_speed_it_is_heard():
    out = audio.MiniaudioOutput(latency_ms=200, silent=True)
    try:
        second = b"\0" * (RATE * FRAME_BYTES)
        start = time.monotonic()
        out.write(second)
        took = time.monotonic() - start
        assert 0.6 < took < 1.5                      # a second, less what the buffer holds
        assert 0 < out.latency() < 0.5
        out.flush()
        assert out.latency() < 0.1                   # what waited is dropped
        out.write(second[: RATE * FRAME_BYTES // 10])
        out.drain()
        assert out.latency() < 0.1
    finally:
        out.close()


def test_the_output_is_chosen_by_the_system_or_bbmp_audio(monkeypatch):
    monkeypatch.setenv("BBMP_AUDIO", "silent")
    out = audio.open_output()
    assert isinstance(out, audio.MiniaudioOutput)
    out.close()
