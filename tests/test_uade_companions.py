# SPDX-License-Identifier: GPL-3.0-or-later
"""Amiga songs that need something next to them, or a fix, before UADE plays them:
shared sample sets, replay code, sample files, songs with fill bytes for zeros, IFF-SMUS
songs whose instrument Modland lacks, and songs filed under another format's name."""

from bbmp import companions
from bbmp.backends import uade


def test_shared_sample_sets_go_with_the_songs_that_use_them():
    for family, song in (("Synth Dream", "sdr.monsterbusiness 1"),
                         ("Maximum Effect", "MAX.DinosaurDetective jingle1"),
                         ("Hippel ST COSO", "wings of death 2.soc")):
        assert companions.is_companion(family, "smp.set")
        assert companions.goes_with(song, "SMP.set", family)
    assert not companions.goes_with("comic bakery.sog", "smp.set", "Hippel ST")


def test_a_kris_hatlelid_song_brings_the_replay_code_of_its_game():
    assert companions.is_companion("Kris Hatlelid", "songplay")
    assert companions.goes_with("the cycles.kh", "songplay", "Kris Hatlelid")
    assert not companions.is_companion("Kris Hatlelid", "the cycles.kh")


def test_a_paul_robotham_song_brings_its_samples_or_the_shared_ones():
    family = "Paul Robotham"
    assert companions.is_companion(family, "space 1889.ssd")
    assert companions.is_companion(family, "mdtest.ssd")
    assert not companions.is_companion(family, "space 1889.dat")
    assert companions.goes_with("space 1889.dat", "space 1889.ssd", family)
    assert companions.goes_with("dawnpatrol-sad.dat", "mdtest.ssd", family)
    assert not companions.goes_with("dawnpatrol-sad.dat", "space 1889.ssd", family)


def test_mark_cooksey_samples_are_hidden_and_go_with_their_song():
    family = "Mark Cooksey Old"
    assert companions.is_companion(family, "mcs.aquablast")
    assert not companions.is_companion(family, "mcr.aquablast")
    assert not companions.is_companion(family, "mco.overlander")
    assert companions.goes_with("mcr.aquablast", "mcs.aquablast", family)
    assert not companions.is_companion("Mark Cooksey", "mcs.aquablast")


def test_fill_bytes_where_a_maximum_effect_song_has_zeros_are_made_zero(tmp_path):
    song = tmp_path / "MAX.AlloAllo level1"
    # the header with "-" for its zeros, a table entry so, and the song's own "-" notes
    song.write_bytes(b"---\x0f---\x14\x00\x00\x01\x4c" + b"\x2d\x22\x2d\x2d\x0e\x5c"
                     + b"\x00\x2d\x00\x00")
    (tmp_path / "SMP.AlloAllo level1").write_bytes(b"samples")
    tmp, made = uade._unfilled_copy(song)
    try:
        assert made.read_bytes() == (b"\x00\x00\x00\x0f\x00\x00\x00\x14\x00\x00\x01\x4c"
                                     + b"\x00\x22\x00\x00\x0e\x5c" + b"\x00\x2d\x00\x00")
        assert (made.parent / "SMP.AlloAllo level1").read_bytes() == b"samples"
    finally:
        tmp.cleanup()
    whole = tmp_path / "MAX.AlloAllo title"
    whole.write_bytes(b"\x00\x00\x00\x0f\x00\x00\x00\x14\x00\x2d\x00\x00")
    assert uade._unfilled_copy(whole) is None
    other = tmp_path / "song.mod"
    other.write_bytes(b"---\x0f---\x14")
    assert uade._unfilled_copy(other) is None


def _smus(*instruments: str) -> bytes:
    chunks = b"".join(b"INS1" + (4 + len(n)).to_bytes(4, "big") + bytes(4) + n.encode()
                      + (b"\0" if len(n) % 2 else b"") for n in instruments)
    body = b"SMUS" + chunks
    return b"FORM" + len(body).to_bytes(4, "big") + body


def test_an_smus_song_gets_stand_ins_for_the_instruments_modland_lacks(tmp_path):
    song = tmp_path / "wildman.smus"
    song.write_bytes(_smus("Apart", "Bpart", "bass"))
    assert uade._smus_instruments(song.read_bytes()) == ["Apart", "Bpart", "bass"]
    inst = tmp_path / "instruments"
    inst.mkdir()
    (inst / "Apart.instr").write_bytes(b"SampledSound")
    (inst / "Apart.ss").write_bytes(b"\x04" * 0x3E + b"\x7f" * 8)
    (inst / "bass.ss").write_bytes(b"its own sample")
    tmp, made = uade._smus_folder(song)
    try:
        there = made.parent / "instruments"
        assert (there / "Apart.instr").read_bytes() == b"SampledSound"
        made_of_sample = (there / "bass.instr").read_bytes()
        assert made_of_sample.startswith(b"SampledSound") and made_of_sample[0x44:0x49] == b"bass\0"
        assert (there / "bass.ss").read_bytes() == b"its own sample"
        assert (there / "Bpart.instr").read_bytes()[0x44:0x4A] == b"Bpart\0"
        assert (there / "Bpart.ss").read_bytes() == b"\x04" * 0x3E + bytes(8)      # silence
        assert made.read_bytes() == song.read_bytes()
    finally:
        tmp.cleanup()
    (inst / "Bpart.instr").write_bytes(b"SampledSound")
    (inst / "bass.instr").write_bytes(b"SampledSound")
    assert uade._smus_folder(song) is None          # nothing lacking: as it is


def test_songs_are_tried_under_the_names_their_players_know(tmp_path):
    for name, tries in (("comic bakery.sog", ["hst.comic bakery"]),
                        ("feud fake.dw", ["dw.feud fake", "jcb.feud fake"]),
                        ("song.4v", ["qts.song"])):
        (tmp_path / name).write_bytes(b"song")
        made = list(uade._renamed_copies(tmp_path / name))
        try:
            assert [file.name for _, file in made] == tries
        finally:
            for tmp, _ in made:
                tmp.cleanup()
