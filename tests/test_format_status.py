# SPDX-License-Identifier: GPL-3.0-or-later
"""The format status's sentence of what the census found, and the short list made from it."""

import datetime
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import format_list  # noqa: E402
import format_status  # noqa: E402


def census(**kw):
    c = {"tried": 40, "plays": 40, "failed": 0, "silent": 0, "noise": 0, "trouble": []}
    c.update(kw)
    return c


def test_files_that_hold_no_song_are_not_held_against_their_format():
    c = census(plays=37, failed=3, trouble=[
        {"song": "a", "state": "failed", "note": "the song is empty"},
        {"song": "b", "state": "failed", "note": "the file on Modland is damaged: sent as text"},
        {"song": "c", "state": "failed", "note": "no replayer could play this file"}])
    assert format_status.trouble(c) == "1 of 40 tried can't be played (no replayer could play this file)."
    assert format_status.trouble(census(plays=39, failed=1, trouble=[
        {"song": "a", "state": "failed", "note": "damaged on Modland: cut short"}])) == ""


def test_what_was_found_another_way_counts_though_the_census_heard_sound():
    c = census(trouble=[{"song": "a", "state": "failed", "by": "hand",
                         "note": "UADE's player misreads it"}])
    assert format_status.trouble(c).startswith("1 of 40 tried can't be played")


def test_the_short_list_puts_each_format_where_it_belongs():
    table = [
        {"format": "Good", "songs": 10, "group": "fine", "plays": "", "text": "", "nosong": ""},
        {"format": "Checked", "songs": 5, "group": "verified", "plays": "", "text": "",
         "nosong": "1 file on Modland is empty"},
        {"format": "Half", "songs": 4, "group": "faults", "plays": "2 of 4",
         "text": "2 of 4 tried are silent. More.", "nosong": ""},
        {"format": "None", "songs": 3, "group": "no", "plays": "0", "text": "", "nosong": ""}]
    notes = {"None": {"note": "Its synthesizer was never published. And more."}}
    out = format_list.write(table, notes, datetime.date(2026, 10, 2))
    assert "- **Plays fully:** 2 formats, 15 songs (1 of them checked" in out
    assert "- **None** (3): Its synthesizer was never published." in out
    assert "- **Half** (4, plays 2 of 4): 2 of 4 tried are silent." in out
    assert "- Checked ✓ (5; 1 file on Modland is empty)" in out
