# SPDX-License-Identifier: GPL-3.0-or-later
"""pmdc: PMD songs on the player's own driver (csrc/chip/pmdc_driver.cpp) and ymfm.

A tiny song is built here: one FM part playing the A above middle C (octave 4) for 48
ticks, then ending. It must come out at 440 Hz, keyed on at the first tick and off 48
ticks later, and play for its own length (under a second, which the old player turned
into 3 minutes). Damaged files are refused with a reason, never a crash."""

from __future__ import annotations

import json
import os
import struct
import subprocess

import pytest

from bbmp.backends import pipe

TICK_S = 56 * 1181552 / 4096 / 1e6     # Timer B at the driver's start tempo (TB 200)


def tiny_song(note: int = 0x49, length: int = 48) -> bytes:
    """A PMD song: part A plays `note` for `length` ticks with voice 0; the other ten
    parts are empty. Offsets are in the data after the first byte."""
    part_a = 26
    data = bytearray()
    empty = part_a + 5
    rtable = empty + 1
    voices = rtable + 2
    offsets = [part_a] + [empty] * 10
    data += struct.pack("<11H", *offsets)
    data += struct.pack("<HH", rtable, voices)
    data += bytes([0xFF, 0x00, note, length, 0x80])   # voice 0, the note, end
    data += bytes([0x80])                              # the empty parts
    data += struct.pack("<H", 0)                       # the rhythm table (unused)
    # voice 0, algorithm 7: slot 4 sounds (TL 0), the other three are silent (TL 127)
    data += bytes([0x00,
                   0x01, 0x01, 0x01, 0x01,             # DT/MULTI
                   0x7F, 0x7F, 0x7F, 0x00,             # TL: slot 1, 3, 2, 4
                   0x1F, 0x1F, 0x1F, 0x1F,             # KS/AR
                   0x00, 0x00, 0x00, 0x00,             # AM/DR
                   0x00, 0x00, 0x00, 0x00,             # SR
                   0x0F, 0x0F, 0x0F, 0x0F,             # SL/RR
                   0x07])                              # FB/ALG
    return bytes([0]) + bytes(data)


def chip_or_skip() -> list[str]:
    chip = pipe.helper("pmdc")
    if chip is None:
        pytest.skip("bbmp-chip with pmdc not installed")
    return chip


def run(chip: list[str], path, env=None) -> tuple[dict, bytes]:
    out = subprocess.run([*chip, str(path), "0", "48000", "0"], capture_output=True, timeout=60,
                         env=env).stdout
    nl = out.find(b"\n")
    return json.loads(out[:nl]), out[nl + 1:]


def test_an_fm_note_has_its_pitch_and_length(tmp_path):
    chip = chip_or_skip()
    song = tmp_path / "tiny.m"
    song.write_bytes(tiny_song())
    # the register writes: the A of octave 4 is block 4, F-number 0x410
    log = tmp_path / "tiny.log"
    subprocess.run([*chip, str(song), "0", "48000", "0"], capture_output=True, timeout=60,
                   env={**os.environ, "BBMP_PMDC_LOG": str(log)}, check=True)
    play = log.read_text().split("# render")[1].splitlines()[1:]
    writes = [w.split() for w in play]
    assert ["775", "0a4", "24"] in writes and ["775", "0a0", "10"] in writes
    keys = [(int(t), v) for t, r, v in (w for w in writes if len(w) == 3) if r == "028"]
    on = [t for t, v in keys if v == "f0"]
    off = [t for t, v in keys if v == "00" and on and t > on[0]]
    assert on[0] == 775                        # the first tick
    assert abs((off[0] - on[0]) / 48000 - 48 * TICK_S) < 0.02
    # the sound: 440 Hz, sounding through the note and gone after it
    head, pcm = run(chip, song)
    assert head["type"] == "PMD"
    assert head["duration_ms"] < 6000          # its own length plus the fade, not 3 minutes
    import array
    s = array.array("h", pcm)
    left = s[0::2]
    note = left[4800:4800 + 16384]
    # zero crossings over 16384 samples (0.34 s)
    ups = sum(1 for a, b in zip(note, note[1:]) if a < 0 <= b)
    freq = ups / (len(note) / 48000)
    assert abs(freq - 440) < 4
    loud = lambda seg: (sum(x * x for x in seg) / max(len(seg), 1)) ** 0.5
    assert loud(left[int(0.4 * 48000):int(0.5 * 48000)]) > 1000
    assert loud(left[int(1.0 * 48000):int(1.1 * 48000)]) < 50


@pytest.mark.parametrize("damage", ["short", "header", "third", "big", "opm"])
def test_damaged_files_are_refused(tmp_path, damage):
    chip = chip_or_skip()
    data = bytearray(tiny_song())
    if damage == "short":
        data = data[:2]
    elif damage == "header":
        data[1] = 0x20          # part A must start at 0x18 or 0x1A
    elif damage == "third":
        data[2] = 0xE6
    elif damage == "big":
        data += bytes(70000)
    elif damage == "opm":
        data[0] = 0x10          # the first byte is at most 0x0F
    song = tmp_path / "bad.m"
    song.write_bytes(bytes(data))
    done = subprocess.run([*chip, str(song), "0", "48000", "0"], capture_output=True, timeout=60)
    head = json.loads(done.stdout.split(b"\n")[0])
    assert head["tracks"] == 0 and "PMD" in head["error"]
    assert done.returncode == 1


def test_offsets_past_the_end_play_safely(tmp_path):
    """Every offset may point anywhere: parts, voices and loops past the end of the file
    read the 0x80 the 64 KB area is filled with."""
    chip = chip_or_skip()
    data = bytearray(tiny_song())
    data[1 + 2:1 + 22] = struct.pack("<10H", *([0xFFF0] * 10))       # parts B-K far away
    data[1 + 24:1 + 26] = struct.pack("<H", 0xFFFE)                  # the voice table too
    data[1 + 26 + 2:1 + 26 + 4] = bytes([0xF8, 0x00])                # a loop end instead of the note
    song = tmp_path / "far.m"
    song.write_bytes(bytes(data))
    head, pcm = run(chip, song)
    assert head["tracks"] == 1 and len(pcm) % 4 == 0
