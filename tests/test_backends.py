# SPDX-License-Identifier: GPL-3.0-or-later
"""Backends against real files. openmpt uses the generated module; the others need
Modland (marked `network`, skipped when offline)."""

import array
import math
import os

import pytest

from bbmp.backends import Unsupported, open_song
from bbmp.backends.base import RATE


def rms(pcm: bytes) -> float:
    a = array.array("h", pcm)[::3]
    return math.sqrt(sum(x * x for x in a) / max(1, len(a)))


def test_openmpt_plays_generated_mod(tmp_path, mod_bytes):
    f = tmp_path / "beep.mod"
    f.write_bytes(mod_bytes)
    d = open_song(f, "Protracker")
    try:
        assert d.backend == "openmpt"
        assert d.info.format.startswith("ProTracker")
        assert 5 < d.info.duration < 10
        pcm = d.read(RATE)
        assert len(pcm) == RATE * 4 and rms(pcm) > 100
        assert len(d.channel_levels()) == 4
        cur, lines = d.pattern_view(9)
        assert "C-6 01" in lines[cur]   # openmpt shows ProTracker C-3 as C-6
        assert d.seek(3.0)
        total = 1 + sum(len(p) for p in iter(lambda: d.read(4800), b"")) / 4 / RATE
        assert 4 < total < 7   # the rest of the song after the seek, then b"" at the end
    finally:
        d.close()


def test_garbage_is_unsupported(tmp_path):
    f = tmp_path / "junk.bin"
    f.write_bytes(os.urandom(3000))
    with pytest.raises(Unsupported):
        open_song(f, "Nobody Knows This Format")


REAL = [
    ("AHX/Pink/inside.ahx", "uade"),
    ("Spectrum/Pro Sound Creator/Bear/bloodyny.psc", "zxtune"),
    ("Nintendo DS Sound Format/Yuzo Koshiro/7th Dragon/111 beautiful garden, miross federation.mini2sf",
     "zxtune"),
    ("YM/Jochen Hippel/amiga demo - end screen.ym", "ym"),
    ("SNDH/Mad_Max/Demos/SID/Demo_Music_8.sndh", "sndh"),
    ("SC68/David Whittaker/knightmare.sc68", "sc68"),
    ("Ad Lib/Loudness Sound System/- unknown/dig it - tune0.lds", "adplug"),
    ("PMD/- unknown/Ash/sakaba.m", "pmdc"),
    ("FM sound driver (FMP)/Arena/Genocide 2/STAGE 6 BOSS.OVI", "fmp"),
    ("MGSDRV/nyui/Ys 2 - Termination.MGS", "kss"),
    ("Euphony/Piyo Piyo/Jimbow a/JIMBOW_A.eup", "eup"),
    ("Organya/Oasis/isan-2.org", "organya"),
    ("Piston Collage/Door/the apple field.ptcop", "pxtone"),
    ("Deflemask/DevEd/keep_the_balance_title.dmf", "furnace"),
    ("Capcom Q-Sound Format/- unknown/Warriors Of Fate/wof73.miniqsf", "qsf"),
    ("Super Nintendo Sound Format/Motoi Sakuraba/Tales Of Phantasia/music-0025.minisnsf", "snsf"),
    ("Quartet ST/- unknown/Acid1.4v", "uade"),
    ("TFMX/Chris Huelsbeck/mdat.turrican intro", "uade"),
    ("HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid", "sidplayfp"),
    ("Nintendo SPC/Greg Turner/Big Sky Trooper/09 - space battle.spc", "gme"),
    ("Video Game Music/Sega Megadrive/Shaka/Curse/11 stage 5 boss.vgz", "gme"),
    ("Coconizer/- unknown/I Am Who.coco", "xmp"),
    ("Archimedes Tracker/- unknown/Altair.musx", "xmp"),
    ("Ixalance/Crystal Score/world of noise.ixs", "ixalance"),
    ("Klystrack/Bod/apoptosis.kt", "klystrack"),
    ("Musicline Editor/- unknown/rush.ml", "musicline"),
    ("FAC SoundTracker/- unknown/Toccate/Toccate.MUS", "fac"),
    ("SoundSmith/- unknown/Breathless", "soundsmith"),
    ("SGC/Yoko Wada/Aladdin/Aladdin.sgc", "sgc"),
    ("WonderSwan/- unknown/tetris.wsr", "wsr"),
    ("SPU/- unknown/Jinguji Saburu - Tomoshibi Ga Kienumani/09-rosy waltz.spu", "spu"),
    ("GoatTracker 2/Cadaver/covert ops in 2d (funktempo).sng", "goattracker"),
    ("Zoundmonitor/AJ/hittheroad.sng", "zoundmonitor"),
    ("MaxTrax/David A. Bean/darkseed (12).mxtx", "maxtrax"),
    ("PokeyNoise/- unknown/pn.tetris", "uade"),
    ("SCC-Musixx/- unknown/RUSSIA [50Hz].SNG", "sccmusixx"),
    ("NerdTracker 2/- unknown/mq-bt.ned", "ned"),
    ("Beepola/- unknown/music studio demo.bbsong", "beepola"),
    ("Beepola/Joffa Smifff/cobra - round start.bbsong", "beepola"),
    ("AY STRC/- unknown/mega mix 1.strc", "strc"),
    ("Monotone/Trixter/understa.mon", "monotone"),
    ("Digitrekker/Horst Beham/demorave.dtm", "digitrekker"),
    ("Starkos/Fenyxkell/spectrum castle.sks", "sks"),
    ("Spectrum/Flash Tracker/- unknown/pet 5.fls", "fls"),
]


@pytest.fixture(scope="module")
def modland(tmp_path_factory):
    from bbmp.fetch import Fetcher
    from bbmp.index import Index
    if not Index.exists():
        pytest.skip("no catalog downloaded")
    f = Fetcher(tmp_path_factory.mktemp("modland"))
    f.index = Index()
    try:
        f.get("modland:AHX/Pink/inside.ahx")
    except (OSError, EOFError):
        pytest.skip("Modland not reachable")
    yield f
    f.close()


@pytest.mark.network
@pytest.mark.parametrize("path,backend", REAL)
def test_real_formats(modland, path, backend, monkeypatch):
    from bbmp import paths
    from bbmp.backends import pipe
    from bbmp.backends.base import NeedsDownload
    if backend in pipe.HELPERS and not pipe.available(backend):
        pytest.skip(f"{backend} helper not built")
    if modland.index.by_path(path) is None:
        pytest.skip("not in this catalog")
    local = modland.get(path, modland.index.companions(path))
    fmt, lengths = modland.index.by_path(path).format, [235.5] if backend == "sidplayfp" else []
    try:
        d = open_song(local, fmt, lengths)
    except NeedsDownload:
        d = None
    if d is None or d.backend != backend:
        # its player is downloaded on request (UADE's players; without it another
        # replayer may play the song): the downloads of this computer, if it has them
        if d is not None:
            d.close()
        real = paths.DATA_DIR / "downloads"
        if not real.is_dir():
            pytest.skip("needs a download this computer doesn't have")
        monkeypatch.setenv("BBMP_DOWNLOADS", str(real))
        d = open_song(local, fmt, lengths)
    try:
        assert d.backend == backend
        pcm = b"".join(d.read(4800) for _ in range(30))
        assert len(pcm) == 30 * 4800 * 4 and rms(pcm) > 100
        if d.info.subsongs > 1:
            d.select_subsong(1)
            assert d.info.subsong == 1
            assert d.read(4800)
    finally:
        d.close()


@pytest.mark.network
def test_a_song_brings_the_files_it_needs(modland):
    """The catalog knows which files go with a song of the newer formats."""
    ix = modland.index
    if ix.by_path("SoundSmith/- unknown/Breathless") is None:
        pytest.skip("not in this catalog")
    assert ix.companions("SoundSmith/- unknown/Breathless") == [
        "modland:SoundSmith/- unknown/Breathless.W"]
    assert ix.by_path("SoundSmith/- unknown/Breathless.W") not in ix.songs("SoundSmith", "- unknown")
    lists = ix.companions("SGC/Yoko Wada/Aladdin/Aladdin.sgc")
    assert lists and all(k.endswith(".m3u") for k in lists)
    kits = ix.companions("FAC SoundTracker/- unknown/Toccate/Toccate.MUS")
    assert kits and all(k.lower().endswith((".sm1", ".sm2")) for k in kits)


@pytest.mark.network
def test_pmd_gets_its_drums_from_the_rhythm_rom(modland, tmp_path):
    """With a YM2608 rhythm ROM installed, PMD songs that use the chip's drums sound different."""
    import os
    import subprocess
    from bbmp import paths
    from bbmp.backends import pipe
    chip = pipe.helper("pmdc")
    if chip is None or not paths.RHYTHM_ROM.exists():
        pytest.skip("needs the chip helper and a rhythm ROM")
    # the tests' extras folder is empty: this test is about the ROM this computer has
    from bbmp import extras
    extras.folder().mkdir(parents=True, exist_ok=True)
    (extras.folder() / "ym2608_adpcm_rom.bin").symlink_to(paths.RHYTHM_ROM)
    env = extras.env("pmdc", chip)
    assert env.get("BBMP_YM2608_ROM")
    song = "PMD/- unknown/Brandish 3/br3-1-27.m"
    if modland.index.by_path(song) is None:
        pytest.skip("not in this catalog")
    local = modland.get(song, modland.index.companions(song))

    def render(env):
        out = subprocess.run([*chip, str(local), "0", "48000"], capture_output=True, env=env,
                             timeout=120).stdout
        nl = out.find(b"\n")
        return out[:nl], out[nl + 1:][:48000 * 4 * 30]
    head, plain = render({k: v for k, v in env.items() if k != "BBMP_YM2608_ROM"})
    assert b"ym2608_adpcm_rom.bin" in head      # it says which file is missing
    head, drums = render(env)
    assert b"ym2608_adpcm_rom.bin" not in head
    assert len(plain) == len(drums) and plain != drums


@pytest.mark.network
def test_sunvox_seeks(modland):
    from bbmp.backends import sunvox
    if not sunvox.LIB.exists():
        pytest.skip("SunVox library not installed")
    song = "SunVox/Asdn/out of time.sunvox"
    if modland.index.by_path(song) is None:
        pytest.skip("not in this catalog")
    d = sunvox.SunVox(modland.get(song))
    try:
        d.read(48000 * 3)
        assert d.seek(60.0)
        assert rms(d.read(48000)) > 100
        starts = d._line_starts
        line = sunvox.lib().sv_get_current_line(0)
        assert 59.0 < starts[line] / 48000 < 61.5        # a line that starts just before 60 s
        assert d.seek(0.0) and rms(d.read(48000 * 2)) > 0
        assert d.seek(10000.0)                           # past the end: the song just ends
    finally:
        d.close()


def test_what_a_replayer_of_our_own_refuses_is_not_played_by_another(tmp_path, monkeypatch):
    """ZXTune finds something to play in nearly any file. A song that its format's
    replayer refused, with a reason, must not be played as something else."""
    from bbmp import backends
    tried = []

    def refuses(path, lengths):
        tried.append("psycle")
        raise Unsupported('the song needs "attack (VST)" (37% of its notes)')

    def plays_anything(name):
        def open_(path, lengths):
            tried.append(name)
            raise AssertionError(f"{name} was asked")
        return open_
    monkeypatch.setitem(backends.OPENERS, "psycle", refuses)
    for name in backends.FALLBACK:
        monkeypatch.setitem(backends.OPENERS, name, plays_anything(name))
    f = tmp_path / "song.psy"
    f.write_bytes(b"PSY3SONG")
    with pytest.raises(Unsupported, match="attack"):
        open_song(f, "Psycle")
    assert tried == ["psycle"]


def test_psf_libraries_get_the_names_their_songs_ask_for(tmp_path):
    import struct

    from bbmp import psf

    def psf_file(path, tags):
        data = b"PSF\x01" + struct.pack("<III", 0, 4, 0) + b"\0\0\0\0"
        path.write_bytes(data + b"[TAG]" + "\n".join(tags).encode())
    song = tmp_path / "song-01.minipsf"
    psf_file(song, ["title=x", "_lib=Vab_Kaze.psflib", "_lib2=DRIVER"])
    psf_file(tmp_path / "vab kaze.psflib", ["_lib=abe_drv"])       # Modland's name
    (tmp_path / "abe_drv").write_bytes(b"x")
    (tmp_path / "driver").write_bytes(b"x")
    assert psf.libs_named(song) == ["Vab_Kaze.psflib", "DRIVER"]
    # where names don't tell case apart (a Mac's disk) "driver" is already "DRIVER"
    same_case = (tmp_path / "DRIVER").exists()
    made = psf.fit_libs(song)
    assert sorted(p.name for p in made) == ([] if same_case else ["DRIVER"]) + ["Vab_Kaze.psflib"]
    assert (tmp_path / "Vab_Kaze.psflib").resolve() == (tmp_path / "vab kaze.psflib").resolve()
    assert psf.fit_libs(song) == []                             # once is enough
    assert psf.fit_libs(tmp_path / "not a song.mod") == []
    # Skies of Arcadia's songs ask for "SOA_329.dsflib"; Modland has "soa-329.dsflib"
    dsf = tmp_path / "soa-329-03-00.minidsf"
    psf_file(dsf, ["_lib=SOA_329.dsflib"])
    psf_file(tmp_path / "soa-329.dsflib", [])
    assert [p.name for p in psf.fit_libs(dsf)] == ["SOA_329.dsflib"]


def test_a_psf_driver_without_an_extension_goes_with_the_songs():
    from bbmp import companions
    family = "Playstation Sound Format"
    assert companions.is_companion(family, "abe_drv")
    assert companions.goes_with("f2seq-13.minipsf", "abe_drv", family)
    assert not companions.is_companion("Protracker", "songname")


def test_a_ymst_song_gets_the_replay_routine_it_names(tmp_path):
    from bbmp import companions
    from bbmp.backends import uade

    assert companions.is_companion("YMST", "toki - replay.ymst")
    assert companions.is_companion("YMST", "ym.big_replay")
    assert not companions.is_companion("YMST", "toki - level6-end.ymst")
    assert uade._replay_key("YM.Toki_replay") == uade._replay_key("toki - replay.ymst") == "toki"
    assert uade._replay_key("'ym.coso mmme_replay") == "cosommme"
    root = tmp_path / "YMST"
    (root / "Loriaux").mkdir(parents=True)
    (root / "Hippel").mkdir()
    song = root / "Loriaux" / "toki - level6-end.ymst"
    song.write_bytes(b"YMST" + b"\0" * 40 + b"Mr.Styckx\0YM.Toki_replay\0rest")
    (root / "Loriaux" / "toki - replay.ymst").write_bytes(b"code")
    (root / "Hippel" / "ym.big_replay").write_bytes(b"other code")
    tmp, file = uade._ymst_folder(song)
    try:
        assert file.name == song.name and file.resolve() == song
        assert (file.parent / "YM.Toki_replay").read_bytes() == b"code"
    finally:
        tmp.cleanup()
    song.write_bytes(b"YMST" + b"\0" * 40)                  # names no routine
    assert uade._ymst_folder(song) is None


def test_ymst_songs_bring_every_replay_routine(catalog):
    assert catalog is not None     # the catalog of the tests has no YMST; the rule is in index
    from bbmp import companions
    assert "YMST" in companions.FORMAT_WIDE


def test_a_quartet_st_song_takes_the_folders_one_sample_set(tmp_path):
    from bbmp import companions
    from bbmp.backends import uade
    assert companions.goes_with("Decade Demo 22.4v", "SMP.set", "Quartet ST")
    (tmp_path / "Decade Demo 22.4v").write_bytes(b"song")
    (tmp_path / "Decade Demo 23.4v").write_bytes(b"song")
    (tmp_path / "SMP.set").write_bytes(b"samples")
    tmp, file = uade._renamed_copy(tmp_path / "Decade Demo 22.4v")
    try:
        assert file.name == "qts.Decade Demo 22"
        assert (file.parent / "smp.Decade Demo 22").read_bytes() == b"samples"
    finally:
        tmp.cleanup()
    (tmp_path / "Decade Demo 22.set").write_bytes(b"its own")       # a set of its own wins
    tmp, file = uade._renamed_copy(tmp_path / "Decade Demo 22.4v")
    try:
        assert (file.parent / "smp.Decade Demo 22").read_bytes() == b"its own"
    finally:
        tmp.cleanup()


def test_an_infogrames_song_takes_the_instruments_whose_name_begins_its_own(tmp_path):
    from bbmp import companions
    from bbmp.backends import uade
    names = ["bob4.ins", "bob4d.ins", "rh-bobo.ins", "bruit.ins"]
    assert companions.prefix_bank("bob4e.dum", names, "ins") == "bob4.ins"
    assert companions.prefix_bank("bob4d.dum", names, "ins") == "bob4d.ins"
    assert companions.prefix_bank("rh-bobo.interlude.dum", names, "ins") == "rh-bobo.ins"
    assert companions.prefix_bank("flood.dum", names, "ins") is None
    assert companions.goes_with("bob4e.dum", "bob4.ins", "Infogrames")
    assert not companions.goes_with("bob4e.dum", "bruit.ins", "Infogrames")
    (tmp_path / "bob4e.dum").write_bytes(b"song")
    (tmp_path / "bob4.ins").write_bytes(b"instruments")
    tmp, file = uade._infogrames_folder(tmp_path / "bob4e.dum")
    try:
        assert (file.parent / "bob4e.ins").read_bytes() == b"instruments"
    finally:
        tmp.cleanup()


def test_a_startrekker_am_song_brings_its_synth_file():
    from bbmp import companions
    assert companions.goes_with("dizzy-title.mod", "dizzy-title.mod.nt", "Startrekker AM")
    assert companions.bank_of("Startrekker AM", "dizzy-title.mod.nt", {"dizzy-title.mod"})
    assert not companions.bank_of("Startrekker AM", "kingkeldon.mod.nt", {"other.mod"})
    from bbmp.backends import backend_for
    assert backend_for("Startrekker AM") == "xmp"


def test_amiga_music_in_an_sc68_file_is_taken_out_for_uade(tmp_path, monkeypatch):
    import struct

    from bbmp.backends import sc68_amiga

    def chunk(tag, data=b""):
        return tag + struct.pack("<I", len(data)) + data
    song = tmp_path / "ragnov.sc68"
    song.write_bytes(b"SC68 Music-file / (c) (BeN)jamin Gerard / SasHipA-Dev  "
                     + b"SC68" + struct.pack("<I", 0) + chunk(b"SCFN", b"Ragnov\0\0")
                     + chunk(b"SCMU") + chunk(b"SCRE", b"sidmon1\0") + chunk(b"SCDA", b"module")
                     + chunk(b"SCEF"))
    replays = tmp_path / "Replay"
    replays.mkdir()
    monkeypatch.setattr(sc68_amiga, "SC68_REPLAYS", replays)
    assert sc68_amiga.amiga_song(song) == ("sidmon1", b"module")
    opened = []

    class Fake:
        def close(self):
            opened.append("closed")
    decoder = sc68_amiga.open_amiga(song, lambda f, lengths: opened.append(
        (f.name, f.read_bytes())) or Fake())
    assert opened == [("sid1.ragnov", b"module")]
    decoder.close()
    assert opened[-1] == "closed"
    (replays / "sidmon1.bin").write_bytes(b"")          # sc68 has the routine: sc68 plays it
    assert sc68_amiga.amiga_song(song) is None


def test_an_sc68_replay_name_cannot_name_a_path(tmp_path, monkeypatch):
    """The name of the routine becomes part of a file name: a name with a path in it
    would have the song's data written anywhere."""
    import struct

    from bbmp.backends import sc68_amiga

    def chunk(tag, data=b""):
        return tag + struct.pack("<I", len(data)) + data
    monkeypatch.setattr(sc68_amiga, "SC68_REPLAYS", tmp_path / "Replay")
    song = tmp_path / "evil.sc68"
    for name in (b"../../../tmp/x", b"/etc/x", b"sid\0mon", b""):
        song.write_bytes(b"SC68 Music-file / (c) (BeN)jamin Gerard / SasHipA-Dev  "
                         + b"SC68" + struct.pack("<I", 0) + chunk(b"SCMU")
                         + chunk(b"SCRE", name + b"\0") + chunk(b"SCDA", b"data") + chunk(b"SCEF"))
        assert sc68_amiga.amiga_song(song) is None


def test_hes_starts_at_the_track_it_names():
    from bbmp.backends.gme import _first_track
    head = b"HESM\x00\x2a\x00\xe1" + bytes(8)
    assert _first_track(head, 256) == 42
    assert _first_track(head, 10) == 0          # a track the file doesn't have
    assert _first_track(b"NESM\x1a\x01\x05", 5) == 0


def test_ezpk_gym_unpacks():
    """EZPK: bzip2 with its header and magics disguised, holding the song as five streams."""
    import bz2
    import struct
    from bbmp.backends.gme import _BLOCK, _END, _disguised, _unpack_gym
    song = bytes([0, 1, 0x28, 0xF0, 2, 0xB4, 0xC0, 3, 0x9F, 0, 0])
    streams = [bytes([0, 1, 2, 3, 0, 0]), bytes([0x28, 0xB4]), b"\xf0", b"\xc0", b"\x9f"]
    counts = [len(x) for x in streams]
    raw = struct.pack("<9I", *counts, 0, 0, 0, 36 + sum(counts)) + b"".join(streams)
    packed = bz2.compress(raw, 2)
    bits = format(int.from_bytes(packed[4:], "big"), f"0{(len(packed) - 4) * 8}b")
    bits = (bits.replace(format(_BLOCK, "048b"), _disguised(_BLOCK))
                .replace(format(_END, "048b"), _disguised(_END)))
    body = b"   2" + int(bits, 2).to_bytes(len(packed) - 4, "big")
    ezpk = b"EZPK" + struct.pack("<I", 8 + len(body)) + body
    head = b"GYMX" + bytes(416) + struct.pack("<II", 0, len(song))
    assert _unpack_gym(head + ezpk) == head[:424] + bytes(4) + song


def test_an_audio_file_of_ones_own_plays(tmp_path):
    """A WAV at 44.1 kHz plays at the player's rate, as long as it is."""
    import array
    import math
    import wave
    from bbmp.backends.sndfile import AudioFile
    f = tmp_path / "tone.wav"
    with wave.open(str(f), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(array.array("h", (int(8000 * math.sin(2 * math.pi * 441 * i / 44100))
                                        for i in range(44100 * 2))).tobytes())
    d = AudioFile(f)
    assert d.info.duration == 2.0 and d.info.format == "WAV"
    pcm = b"".join(iter(lambda: d.read(4800), b""))
    assert abs(len(pcm) // 4 - 96000) < 100
    left = array.array("h", pcm)[::2][24000:72000]
    crossings = sum(1 for i in range(1, len(left)) if (left[i] >= 0) != (left[i - 1] >= 0))
    assert abs(crossings / 2 - 441) < 3
    assert d.seek(1.5) and abs(len(b"".join(iter(lambda: d.read(4800), b""))) // 4 - 24000) < 100
    d.close()

