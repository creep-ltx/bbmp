# SPDX-License-Identifier: GPL-3.0-or-later
"""bbmp-darkwave: DarkWave Studio's own engine, on projects made here."""

import array
import json
import os
import struct
import subprocess

import pytest

from bbmp.backends import pipe


def _machine(name, uid, props=b""):
    # DWP_MACHINE of project format 3: Name[1024], UniqueID[1024], X, Y, VisibleControl,
    # PropertyBytes, IsInner; then the properties
    return (name.encode().ljust(1024, b"\0") + uid.encode().ljust(1024, b"\0")
            + struct.pack("<4i?", 0, 0, 0, len(props), True) + props)


def _block(data):
    return struct.pack("<i", len(data)) + data


def _project(note=57, start=16, length=16, uid="DarkPlug::ES BassHead"):
    """Master and a BassHead (its own settings), one pattern of 16 steps with one note,
    the sequence one pattern long: 120 BPM, 16 ticks a beat."""
    out = b" PWD" + bytes([3, 0]) + struct.pack("<f", 120.0)
    out += struct.pack("<i", 2)
    out += _machine("Master", "Master") + _machine("ES BassHead 1", uid)
    # the cables are kept with the machine they go into: BassHead's Line Out -> Master
    out += _block(struct.pack("<i", 1) + struct.pack("<4id", 1, 1, 0, 0, 1.0))
    out += _block(struct.pack("<i", 0))                        # Master's patterns
    out += _block(struct.pack("<i", 0))                        # BassHead's cables
    pattern = struct.pack("<ii", 0, 16) + b"P".ljust(1024, b"\0") + struct.pack("<4i", 1, 0, 1, 0)
    pattern += struct.pack("<3i", start, length, note)
    out += _block(struct.pack("<i", 1) + pattern)
    out += _block(struct.pack("<ii", 16, 1) + struct.pack("<ii", 0, 1) + struct.pack("<ii", 0, 0))
    out += _block(b"test\0")
    return out


def _run(path, passes=None, idle="0"):
    exe = pipe.helper("darkwave")[0]
    env = {**os.environ, "BBMP_DARKWAVE_IDLE": idle}
    if passes:
        env["BBMP_DARKWAVE_PASSES"] = str(passes)
    p = subprocess.run(["prlimit", f"--data={2 << 30}", "--", exe, str(path), "0", "48000"],
                       capture_output=True, env=env, timeout=60)
    head, _, pcm = p.stdout.partition(b"\n")
    a = array.array("h")
    a.frombytes(pcm[: len(pcm) // 2 * 2])
    return p.returncode, json.loads(head), a


def _needs_helper():
    if not pipe.available("darkwave"):
        pytest.skip("bbmp-darkwave not built")


def test_a_note_sounds_at_its_step_and_pitch(tmp_path):
    _needs_helper()
    p = tmp_path / "test.dwp"
    p.write_bytes(_project())
    code, head, a = _run(p, passes=1)
    assert code == 0 and head["tracks"] == 1
    # 16 steps of 4 ticks; a tick is 60 / (120 * 16) s
    assert head["duration_ms"] == 2000
    left = a[0::2]
    first = next(i for i, x in enumerate(left) if abs(x) > 30)
    assert first / 48000 == pytest.approx(0.5, abs=0.01)          # step 4, tick 16
    tone = left[first + 2400:first + 2400 + 9600]                   # 0.05 to 0.25 s into it
    # the fundamental by autocorrelation: A-3 (note 57) is 220 Hz
    best = max(range(150, 300), key=lambda k: sum(tone[i] * tone[i + k] for i in range(0, 4000)))
    assert 48000 / best == pytest.approx(220, rel=0.02)


def test_a_short_sequence_is_looped_to_half_a_minute(tmp_path):
    _needs_helper()
    p = tmp_path / "test.dwp"
    p.write_bytes(_project())
    d = pipe.PipeDecoder("darkwave", p)
    try:
        assert d.info.duration == pytest.approx(30.0, abs=0.01)
        assert "15 times" in d._meta["facts"]["Sequence"]
    finally:
        d.close()


def test_a_machine_it_lacks_is_named(tmp_path):
    _needs_helper()
    p = tmp_path / "test.dwp"
    p.write_bytes(_project(uid="VST::Some Synth"))
    code, head, _ = _run(p)
    assert code != 0 and head["tracks"] == 0
    assert "VST plug-in Some Synth" in head["error"]


def test_damaged_projects_are_refused_without_harm(tmp_path):
    _needs_helper()
    good = _project()
    for n, data in enumerate([good[:k] for k in (3, 10, 20, 2100, 4200, 4300, len(good) - 30)]
                             + [good[:4] + bytes(range(256)) * 20]):
        p = tmp_path / f"bad{n}.dwp"
        p.write_bytes(data)
        code, head, _ = _run(p)
        assert code in (0, 1), (n, code)                            # not a signal
        assert head.get("tracks") == 0 or head.get("tracks") == 1
