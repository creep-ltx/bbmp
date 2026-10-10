# SPDX-License-Identifier: GPL-3.0-or-later
"""The register comparison of tools/verify: what counts as the same, and the PT2 reader."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "verify"))

import aycompare  # noqa: E402
import pt3  # noqa: E402


def frame(**regs) -> bytes:
    row = bytearray(14)
    row[13] = 0xFF
    for k, v in regs.items():
        row[int(k[1:])] = v
    return bytes(row)


def test_only_what_can_be_heard_is_compared():
    # tone A off in the mixer: its period can't be heard
    a = frame(r0=10, r7=0b111001) + frame(r8=15)
    b = frame(r0=99, r7=0b111001) + frame(r8=15)
    assert aycompare.compare(a, b)["differ"] == 0
    # with the tone on it can
    a2 = frame(r0=10, r7=0b111000)
    b2 = frame(r0=99, r7=0b111000)
    assert aycompare.compare(a2, b2)["differ"] == 1
    # bits the chip doesn't have don't count; a write of register 13 does
    assert aycompare.compare(frame(r1=0x13, r7=0x38), frame(r1=0x03, r7=0x38))["differ"] == 0
    assert aycompare.compare(frame(r13=8), frame())["differ"] == 1


def test_a_pt2_channel_that_plays_before_it_chooses_a_sample():
    song = bytearray(256)
    song[99] = 140                  # the patterns' table
    song[131], song[132] = 0, 0xFF  # one position: pattern 0
    song[140:146] = bytes([150, 0, 160, 0, 170, 0])
    song[150:153] = bytes([0xE1, 0x90, 0x00])   # A: sample 1, then a note
    song[160:162] = bytes([0x90, 0x00])         # B: a note, no sample
    song[170:173] = bytes([0x15, 0x90, 0x00])   # C: a volume, then a note
    assert pt3.pt2_unsampled(bytes(song)) == {1, 2}


def test_sid_changes_compare_in_order_and_allow_a_close_swap():
    import sid
    ref = [(0, 24, 15), (100, 1, 2), (110, 2, 3), (20000, 4, 17)]
    same = [(0, 24, 15), (109, 1, 2), (120, 2, 3), (21800, 4, 17)]
    r = sid.compare(same, ref)
    assert r["first"] is None and r["swaps"] == 0
    swapped = [(0, 24, 15), (109, 2, 3), (120, 1, 2), (21800, 4, 17)]
    r = sid.compare(swapped, ref)
    assert r["first"] is None and r["swaps"] == 1
    other = [(0, 24, 15), (109, 1, 9), (120, 2, 3), (21800, 4, 17)]
    assert sid.compare(other, ref)["first"]["index"] == 1
