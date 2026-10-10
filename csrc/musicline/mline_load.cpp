// SPDX-License-Identifier: LGPL-2.1-or-later
// Loading a Musicline Editor module. See mline.h for where the code comes from.
#include "mline.h"

#include <cstring>

namespace mline {

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Amiga text is ISO 8859-1
static std::string text(const uint8_t *p, size_t max) {
    std::string s;
    for (size_t i = 0; i < max && p[i]; i++) {
        uint8_t c = p[i];
        if (c < 0x20 || (c >= 0x7f && c < 0xa0)) c = ' ';
        if (c < 0x80) s += (char)c;
        else { s += (char)(0xc0 | (c >> 6)); s += (char)(0x80 | (c & 0x3f)); }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void counter(CounterInit &c, const uint8_t *p) {
    c.start = be16(p); c.repeat = be16(p + 2); c.repEnd = be16(p + 4);
    c.speed = be16(p + 6); c.turns = be16(p + 8); c.delay = be16(p + 10);
}

static void head(SmplHead &h, const uint8_t *p) {
    h.title = text(p, 32);
    h.type = p[33];
    h.length = be16(p + 38);
    h.repLength = be16(p + 44);
    h.fineTune = (int16_t)be16(p + 46);
    h.semiTone = (int16_t)be16(p + 48);
}

static void instrument(Inst &in, const uint8_t *p) {
    head(in, p);
    in.smplNumber = p[32];
    in.smplStart = be16(p + 50); in.smplEnd = be16(p + 52);
    in.smplRepStart = be16(p + 54); in.smplRepLen = be16(p + 56);
    in.volume = be16(p + 58);
    in.transpose = p[60]; in.slideSpeed = p[61];
    in.effects1 = p[62]; in.effects2 = p[63];
    for (int i = 0; i < 12; i++) in.env[i] = be16(p + 64 + 2 * i);
    in.vibDir = p[88]; in.vibWaveNum = p[89];
    in.vibSpeed = be16(p + 90); in.vibDelay = be16(p + 92); in.vibAtkSpd = be16(p + 94);
    in.vibAttack = be16(p + 96); in.vibDepth = be16(p + 98);
    in.treDir = p[100]; in.treWaveNum = p[101];
    in.treSpeed = be16(p + 102); in.treDelay = be16(p + 104); in.treAtkSpd = be16(p + 106);
    in.treAttack = be16(p + 108); in.treDepth = be16(p + 110);
    in.arpTable = be16(p + 112); in.arpSpeed = p[114]; in.arpGroove = p[115];
    in.envTraPhaFilBits = p[116];
    memcpy(in.traWaveNums, p + 117, 5);
    counter(in.tra, p + 122);
    counter(in.pha, p + 134);
    in.phaType = p[147];
    in.mixResLooBits = p[148]; in.mixWaveNum = p[149];
    counter(in.mix, p + 150);
    counter(in.res, p + 162);
    in.mixResFilBoost = p[174]; in.resAmp = p[175];
    counter(in.fil, p + 176);
    in.filType = p[189];
    in.looStart = be16(p + 190); in.looRepeat = be16(p + 192); in.looRepEnd = be16(p + 194);
    in.looLength = be16(p + 196); in.looLpStep = be16(p + 198); in.looWait = be16(p + 200);
    in.looDelay = be16(p + 202); in.looTurns = be16(p + 204);
}

// "Delta" packed samples: a command byte starts a run of 4 bit differences
static void unpack(const uint8_t *p, size_t n, uint8_t command, std::vector<int8_t> &out, size_t max) {
    size_t i = 0, o = 0;
    auto put = [&](uint8_t b) { if (o < max) out[o] = (int8_t)b; o++; };
    auto nibble = [](uint8_t v) { return (uint8_t)((v & 8) ? (v | 0xf0) : v); };
    while (i < n) {
        uint8_t b = p[i++];
        if (b != command) { put(b); continue; }
        if (i + 3 > n) break;
        uint8_t v = p[i];
        unsigned count = (p[i + 1] << 8) | p[i + 2];
        i += 3;
        put(v);
        while (count && i < n) {
            uint8_t d = p[i++];
            v = (uint8_t)(v + nibble(d >> 4));
            put(v);
            if (--count == 0) break;
            v = (uint8_t)(v + nibble(d & 15));
            put(v);
            --count;
        }
    }
}

bool Module::load(const uint8_t *d, size_t n, std::string &err) {
    if (n < 16 || memcmp(d, "MLEDMODL", 8) != 0) { err = "not a Musicline Editor module"; return false; }
    size_t o = 8;
    if (memcmp(d + o, "VERS", 4) != 0) {            // the oldest modules have no length here
        uint64_t skip = (uint64_t)be32(d + o) + 4;
        if (o + skip + 8 > n || memcmp(d + o + skip, "VERS", 4) != 0) {
            err = "no version in the module";
            return false;
        }
        o += skip;
    }
    uint32_t len = be32(d + o + 4);
    o += 8;
    if (o + len > n) { err = "the module is cut short"; return false; }
    if (len >= 3) version = text(d + o + 2, len - 2);
    o += len;

    partStore.assign(1, Part());
    arpgStore.assign(1, Arpg());
    insts.assign(256, Inst());
    smpls.assign(256, Sample());
    for (int &p : part) p = 0;
    for (int &a : arpg) a = 0;
    // to set the pointers of the instruments when their sample arrives
    std::vector<uint32_t> smplPointer(256, 0), smplRepPointer(256, 0);

    while (o + 8 <= n) {
        const uint8_t *id = d + o;
        len = be32(d + o + 4);
        o += 8;
        size_t left = n - o;
        if (memcmp(id, "TUNE", 4) == 0) {
            // the length of this chunk is of no use: old versions of the editor counted
            // pointers into it that are not in the file
            if (left < 40) { err = "a tune is cut short"; return false; }
            Tune t;
            const uint8_t *p = d + o;
            t.title = text(p, 32);
            t.tempo = be16(p + 32);
            t.speed = p[34]; t.groove = p[35];
            t.volume = be16(p + 36);
            t.playMode = p[38]; t.channels = p[39];
            o += 40;
            for (auto &s : t.seq) for (uint16_t &e : s) e = 0x0010;     // part 0, not transposed
            if (t.channels > 8) { err = "a tune of more than eight channels"; return false; }
            if (n - o < (size_t)t.channels * 4) { err = "a tune is cut short"; return false; }
            uint32_t size[8];
            for (int c = 0; c < t.channels; c++) size[c] = be32(d + o + 4 * c);
            o += (size_t)t.channels * 4;
            for (int c = 0; c < t.channels; c++) {
                if (size[c] > n - o) { err = "a tune is cut short"; return false; }
                int entries = (int)(size[c] / 2);
                if (entries > 256) entries = 256;
                for (int e = 0; e < entries; e++) t.seq[c][e] = be16(d + o + 2 * e);
                t.seqLen[c] = entries;
                o += size[c];
            }
            if (t.channels) tunes.push_back(t);     // tunes without channels are only there for their titles
        } else if (memcmp(id, "PART", 4) == 0) {
            if (len > left || len < 2) { err = "a part is cut short"; return false; }
            unsigned num = be16(d + o);
            const uint8_t *p = d + o + 2, *end = d + o + len;
            Part pt;
            int line = 0;
            while (p < end && line < 128) {
                uint8_t flags = *p++;
                if (flags & 0x80) break;
                uint8_t raw[12] = {};
                for (int b = 0; b < 6; b++) {
                    if (flags & (1 << b)) {
                        if (end - p < 2) { p = end; break; }
                        raw[2 * b] = p[0]; raw[2 * b + 1] = p[1];
                        p += 2;
                    }
                }
                PartLine &l = pt.line[line++];
                l.note = raw[0]; l.inst = raw[1];
                for (int f = 0; f < 5; f++) { l.fx[f].num = raw[2 + 2 * f]; l.fx[f].par = raw[3 + 2 * f]; }
            }
            if (num < 1024) {
                partStore.push_back(pt);
                part[num] = (int)partStore.size() - 1;
            }
            o += len;
        } else if (memcmp(id, "ARPG", 4) == 0) {
            if (len > left || len < 2) { err = "an arpeggio is cut short"; return false; }
            unsigned num = be16(d + o);
            Arpg a;
            size_t lines = (len - 2) / 6;
            if (lines > 128) lines = 128;
            for (size_t l = 0; l < lines; l++) {
                const uint8_t *p = d + o + 2 + 6 * l;
                a.line[l].note = (int8_t)p[0]; a.line[l].smpl = p[1];
                a.line[l].fx[0].num = p[2]; a.line[l].fx[0].par = p[3];
                a.line[l].fx[1].num = p[4]; a.line[l].fx[1].par = p[5];
            }
            if (num < 256) {
                arpgStore.push_back(a);
                arpg[num] = (int)arpgStore.size() - 1;
            }
            o += len;
        } else if (memcmp(id, "INST", 4) == 0) {
            if (len > left) { err = "an instrument is cut short"; return false; }
            uint8_t raw[206] = {};
            memcpy(raw, d + o, len < sizeof raw ? len : sizeof raw);
            if (numInst < 255) instrument(insts[++numInst], raw);
            o += len;
        } else if (memcmp(id, "SMPL", 4) == 0) {
            // six bytes, the header of the sample (50 bytes), the sample; len counts the last two
            if (left < 6 || len < 50 || len > left - 6) { err = "a sample is cut short"; return false; }
            uint32_t original = be32(d + o);
            uint8_t command = d[o + 4];
            const uint8_t *h = d + o + 6;
            uint32_t stored = len - 50;
            if (original > (64u << 20)) { err = "a sample of impossible size"; return false; }
            if (numSmpl < 255) {
                Sample &s = smpls[++numSmpl];
                head(s, h);
                smplPointer[numSmpl] = be32(h + 34);
                smplRepPointer[numSmpl] = be32(h + 40);
                bool wave = original == 256;
                s.data.assign((size_t)original + (wave ? 240 : 0), 0);
                if (stored == original) {
                    for (uint32_t i = 0; i < original; i++) s.data[i] = (int8_t)h[50 + i];
                } else {
                    unpack(h + 50, stored, command, s.data, original);
                }
                if (wave) {                         // the wave in five octaves, one after the other
                    for (int i = 0; i < 256; i++) if (s.data[i] == -128) s.data[i] = -127;
                    for (int i = 0; i < 240; i++) s.data[256 + i] = s.data[2 * i];
                }
                Ptr base;
                base.base = s.data.data();
                base.size = (uint32_t)s.data.size();
                for (int i = 1; i <= numInst; i++) {
                    Inst &in = insts[i];
                    if (in.smplNumber != numSmpl) continue;
                    in.pointer = base.plus(in.smplStart * 2u);
                    in.repPointer = base.plus(in.smplRepStart * 2u);
                }
                int32_t rep = (int32_t)(smplRepPointer[numSmpl] - smplPointer[numSmpl]);
                if (rep < 0) rep = 0;
                s.pointer = base;
                s.repPointer = base.plus((uint32_t)rep);
            }
            o += 6 + (size_t)len;
        } else if (memcmp(id, "INFO", 4) == 0) {
            if (len > left) { err = "the information is cut short"; return false; }
            const uint8_t *p = d + o, *end = d + o + len;
            std::vector<std::string> s;
            while (p < end && s.size() < 9) {
                size_t l = strnlen((const char *)p, end - p);
                s.push_back(text(p, l));
                p += l + 1;
            }
            s.resize(9);
            title = s[0]; author = s[1]; date = s[2]; duration = s[3];
            info.assign(s.begin() + 4, s.end());
            while (!info.empty() && info.back().empty()) info.pop_back();
            o += len;
        } else {
            break;      // the Amiga stops reading here, and plays what it has
        }
    }
    if (tunes.empty()) { err = "no tune in the module"; return false; }
    return true;
}

}  // namespace mline
