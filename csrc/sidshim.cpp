// SPDX-License-Identifier: GPL-3.0-or-later
// A small C interface to libsidplayfp (which only has a C++ API), for ctypes.
#include <cstdio>
#include <cstring>
#include <sidplayfp/sidplayfp.h>
#include <sidplayfp/SidTune.h>
#include <sidplayfp/SidTuneInfo.h>
#include <sidplayfp/SidConfig.h>
#include <sidplayfp/builders/residfp.h>

struct sid {
    ReSIDfpBuilder builder{"modland-player"};  // declared first so it outlives the engine
    sidplayfp engine;
    SidTune *tune = nullptr;
    short *mixbuf = nullptr;
    int mixlen = 0;     // samples waiting in mixbuf
    int mixpos = 0;
};

// The C64's own ROMs, if the user has them. Tunes written in BASIC, and some that
// call the system, are silent without. Each is given once; a null pointer is none.
static unsigned char rom_kernal[8192], rom_basic[8192], rom_chargen[4096];
static bool has_kernal, has_basic, has_chargen;

extern "C" {

void sid_roms(const unsigned char *kernal, const unsigned char *basic,
              const unsigned char *chargen) {
    has_kernal = kernal != nullptr;
    has_basic = basic != nullptr;
    has_chargen = chargen != nullptr;
    if (kernal) std::memcpy(rom_kernal, kernal, sizeof rom_kernal);
    if (basic) std::memcpy(rom_basic, basic, sizeof rom_basic);
    if (chargen) std::memcpy(rom_chargen, chargen, sizeof rom_chargen);
}

// A PSID/RSID header whose data offset is at or past the end of the file: libsidplayfp
// (3.1.1) indexes its file buffer there without looking (SidTuneBase::acceptSidTune,
// &buf[fileOffset]) and aborts on the assertion of std::vector, or reads past the end.
static bool damaged_header(const char *path) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    unsigned char head[8];
    const bool whole = std::fread(head, 1, sizeof head, f) == sizeof head;
    long size = -1;
    if (whole && std::fseek(f, 0, SEEK_END) == 0) size = std::ftell(f);
    std::fclose(f);
    if (!whole || size < 0) return false;
    if (std::memcmp(head, "PSID", 4) != 0 && std::memcmp(head, "RSID", 4) != 0) return false;
    const long offset = (head[6] << 8) | head[7];
    return offset >= size;
}

sid *sid_open(const char *path, unsigned rate) {
    if (damaged_header(path)) return nullptr;
    sid *s = new sid;
    s->tune = new SidTune(path);
    if (!s->tune->getStatus()) {
        delete s->tune;
        delete s;
        return nullptr;
    }
    if (has_kernal)
        s->engine.setRoms(rom_kernal, has_basic ? rom_basic : nullptr,
                          has_chargen ? rom_chargen : nullptr);
    SidConfig cfg = s->engine.config();
    cfg.frequency = rate;
    cfg.samplingMethod = SidConfig::INTERPOLATE;
    cfg.sidEmulation = &s->builder;
    if (!s->engine.config(cfg)) {
        delete s->tune;
        delete s;
        return nullptr;
    }
    s->mixbuf = new short[1 << 16];
    return s;
}

int sid_select(sid *s, unsigned song) {  // 1-based, 0 = default
    s->tune->selectSong(song);
    if (!s->engine.load(s->tune))
        return 0;
    s->engine.initMixer(true);
    s->mixlen = s->mixpos = 0;
    return 1;
}

unsigned sid_songs(sid *s) { return s->tune->getInfo()->songs(); }
unsigned sid_start_song(sid *s) { return s->tune->getInfo()->startSong(); }

const char *sid_info_string(sid *s, unsigned i) {
    const SidTuneInfo *info = s->tune->getInfo();
    return i < info->numberOfInfoStrings() ? info->infoString(i) : "";
}

const char *sid_format(sid *s) { return s->tune->getInfo()->formatString(); }

// Fill `frames` stereo frames into out. Returns frames written.
int sid_render(sid *s, short *out, int frames) {
    int want = frames * 2, got = 0;
    while (got < want) {
        if (s->mixpos >= s->mixlen) {
            int n = s->engine.play(5000);
            if (n < 0)
                break;
            s->mixlen = s->engine.mix(s->mixbuf, n);
            s->mixpos = 0;
            if (s->mixlen <= 0)
                continue;
        }
        int take = s->mixlen - s->mixpos;
        if (take > want - got)
            take = want - got;
        std::memcpy(out + got, s->mixbuf + s->mixpos, take * sizeof(short));
        s->mixpos += take;
        got += take;
    }
    return got / 2;
}

// Run the emulation forward without keeping the sound. Returns frames skipped.
long sid_skip(sid *s, long frames) {
    short scratch[2 * 4096];
    long done = 0;
    while (done < frames) {
        long want = frames - done > 4096 ? 4096 : frames - done;
        int got = sid_render(s, scratch, (int)want);
        if (got <= 0)
            break;
        done += got;
    }
    return done;
}

void sid_close(sid *s) {
    delete s->tune;
    delete[] s->mixbuf;
    delete s;
}

}
