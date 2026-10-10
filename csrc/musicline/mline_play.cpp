// SPDX-License-Identifier: LGPL-2.1-or-later
// The Musicline replay routine. See mline.h for where it comes from.
//
// The Amiga code computes in 16 bits and relies on it in places; the casts to int16_t
// and uint16_t below are those places, not decoration.
#include "mline.h"
#include "mline_tables.h"

#include <cstring>

namespace mline {

enum {                      // inst_Effects1
    ADSR = 1, VIBRATO = 2, TREMOLO = 4, ARPEGGIO = 8, LOOP = 16, LOOPSTOP = 32, HOLDSUSTAIN = 64,
    WSLOOP = 128
};
enum { TRANSFORM = 1, PHASE = 2, MIX = 4, RESONANCE = 8, FILTER = 16 };       // inst_Effects2
enum {                      // inst_EnvTraPhaFilBits
    ADSRHOLDSUSTAIN = 1, TRANSFORMINIT = 2, TRANSFORMSTEP = 4, PHASEINIT = 8, PHASESTEP = 16,
    PHASEFILL = 32, FILTERINIT = 64, FILTERSTEP = 128
};
enum {                      // inst_MixResLooBits
    MIXINIT = 1, MIXSTEP = 2, MIXBUFF = 4, MIXCOUNTER = 8, RESONANCEINIT = 16, RESONANCESTEP = 32,
    LOOPINIT = 64, LOOPSTEP = 128
};
enum { PLAY = 1, FIRSTTICK = 2, KEEPFINETUNE = 4 };                            // ch_Play
enum { ARP_ON = 1, ARP_SETSNOTE = 2, ARP_TABLE = 4, ARP_WS = 8, ARP_ONESTEP = 16, ARP_ABSOLUTE = 32 };
enum { RESTART_PARTFX = 1, RESTART_ARP = 2, ARP_SETVOLUME = 4 };              // ch_Restart
enum {
    fx_Portamento = 0x03, fx_VolumeSlideUp = 0x11, fx_VolumeSlideDown = 0x12,
    fx_VolumeSlideToVol = 0x14, fx_ChannelVolSlideUp = 0x21, fx_ChannelVolSlideDown = 0x22,
    fx_ChannelVolSlideToVol = 0x24, fx_MasterVolSlideUp = 0x31, fx_MasterVolSlideDown = 0x32,
    fx_MasterVolSlideToVol = 0x34, fx_PTPortamento = 0xe3, fx_PTVolSlideUp = 0xe6,
    fx_PTVolSlideDown = 0xe7, fx_PTTremolo = 0xe8, fx_PTVibrato = 0xea
};

static const int8_t ZeroSample[2] = {0, 0};
static Ptr zeroSample() { Ptr p; p.base = ZeroSample; p.size = 2; return p; }
static Ptr buffer(const int8_t *b) { Ptr p; p.base = b; p.size = 256; return p; }

static inline int16_t w(int v) { return (int16_t)v; }

void Player::start(int n) {
    tune = &mod.tunes[n];
    playMode = tune->playMode ? 1 : 0;
    for (int i = 0; i < 8; i++) { ch[i] = Chan(); voice[i] = Voice(); }
    MasterVol = 64 * 16;
    led = false;
    TuneTmp = tune->tempo;
    TuneSpd = tune->speed;
    TuneGrv = tune->groove;
    for (int i = 0; i < channels(); i++) {
        Chan &c = ch[i];
        c.CVolume = 64 * 16;
        c.Spd = TuneSpd;
        c.Grv = TuneGrv;
        if (c.Grv) c.PartGrv = 0xff;
        c.SpdCnt = 1;
        c.PchSldToNote = -1;
        c.seen.assign(256 * 32 / 8, 0);
    }
    playing = true;
    // eight channels: the length of a tick is that of the mixing buffer, fixed when playing starts
    uint32_t d1 = (3546895u / 126u) * 125u;
    uint16_t d2 = (uint16_t)(TuneTmp * 50u);
    mixLength = d2 ? (uint16_t)((d1 / d2) & 0xfffe) : 562;
    if (mixLength < 2) mixLength = 2;
    setTempo();
}

void Player::setTempo() {       // four channels: CIA B timer A
    uint32_t q = TuneTmp ? 1773448u / TuneTmp : 1773448u / 125u;
    ciaLatch = (uint16_t)(q > 0xffff ? 1773448u : q);   // DIVU leaves its operand alone on overflow
}

double Player::tickSeconds() const {
    if (playMode) return mixLength * 126.0 / 3546895.0;
    return (ciaLatch + 1) / 709379.0;
}

bool Player::allDone() const {
    if (!playing) return true;
    for (int i = 0; i < channels(); i++) if (!ch[i].done) return false;
    return true;
}

bool Player::running() const {
    if (!playing) return false;
    for (int i = 0; i < channels(); i++) if (!ch[i].VoiceOff && !ch[i].pastData) return true;
    return false;
}

void Player::tick() {
    if (!playing) return;
    for (int i = 0; i < channels(); i++) PlayVoice(i);
    for (int i = 0; i < channels(); i++) PlayEffects(i);
    for (int i = 0; i < channels(); i++) Per(ch[i]);
    PerVolPlay();
    DmaPlay();
    if (!playing) for (Voice &v : voice) v.on = false;      // StopPlay switches the sound off
}

// ---------------------------------------------------------------- the sequence

void Player::PlayVoice(int n) {
    Chan &c = ch[n];
    if (c.VoiceOff || !playing) return;
    if (--c.SpdCnt != 0) return;
    c.PartPosWork = c.PartPos;
    int loopError = 256;
    uint8_t spd = c.Spd;
    c.PartGrv = (uint8_t)~c.PartGrv;
    if (c.PartGrv && c.Grv) spd = c.Grv;
    c.SpdCnt = spd;
    c.SpdPart = 0;
    c.GrvPart = 0;

    for (;;) {
        if (--loopError < 0) return;
        uint8_t pos = c.TunePos;
        if (pos >= tune->seqLen[n]) { c.pastData = true; c.done = true; }
        else c.pastData = false;
        uint16_t d3 = tune->seq[n][pos];
        uint8_t target = (uint8_t)(d3 >> 8);
        int d5 = d3 & 0x1f;
        int cmd = (d3 & 0x20) ? (d3 & 0xc0) >> 6 : 0;
        if (cmd == 1) {                                     // end
            c.VoiceOff = 1;
            c.done = true;
            return;
        }
        if (cmd == 2) {                                     // jump
            if (c.TuneJumpCount) {
                if (--c.TuneJumpCount) c.TunePos = target;
                else c.TunePos++;
            } else if (pos > target) {
                c.TuneJumpCount = (uint8_t)d5;
                if (!d5) c.done = true;                     // for ever
                c.TunePos = target;
            } else {
                c.TunePos++;
            }
            continue;
        }
        if (cmd == 3) {                                     // wait
            if (c.TuneWait) {
                if (--c.TuneWait) return;
                c.TunePos++;
                continue;
            }
            c.TuneWait = target;
            if (!target) { c.TunePos++; continue; }
            c.PTPchSld = 0;
            c.PchSld = 0;
            c.VolSld = 0;
            c.PartNote = 0;
            c.PartInst = 0;
            if (d5) { c.Spd = (uint8_t)d5; c.SpdCnt = (uint8_t)d5; }
            return;
        }

        // a part
        c.PartNum = (uint16_t)(((d3 << 2) & 0x300) | (d3 >> 8));
        c.TransposeNum = (uint8_t)(d5 - 0x10);
        int16_t transpose = w((int8_t)c.TransposeNum * 32);
        if (c.PartPos == 0) {
            // a channel that starts a part in a state it has been in before has come round
            unsigned key = pos * 32u + (c.TuneJumpCount & 31);
            if (c.PartJmpCnt == 0) {
                if (c.seen[key >> 3] & (1 << (key & 7))) c.done = true;
                c.seen[key >> 3] |= (uint8_t)(1 << (key & 7));
            }
        }
        const Part &part = mod.partStore[mod.part[c.PartNum]];
        bool again = false;
        for (int guard = 0; guard < 4096; guard++) {
            uint8_t row = c.PartPos;
            c.PartPos = (c.PartPos + 1) & 0x7f;
            if (c.PartPos == 0) {
                c.TunePos++;
                c.Spd = TuneSpd;
                c.Grv = TuneGrv;
            }
            const PartLine &l = part.line[row & 0x7f];
            c.PartNote = l.note;
            c.PartInst = l.inst;
            for (int i = 0; i < 5; i++) c.PartEffects[i] = l.fx[i];
            uint8_t d1 = c.PartNote;
            if (d1 == 61) {                                 // the end of the part
                if (row == 0) {                             // at its first line: the end of the song
                    playing = false;
                    for (Chan &k : ch) k.WsNumberOld = 0;
                    return;
                }
                c.PartPos = 0;
                c.PartPosWork = 0;
                c.Spd = TuneSpd;
                c.Grv = TuneGrv;
                c.SpdCnt = c.Spd;
                c.TunePos++;
                again = true;
                break;
            }
            if (d1 & 0x80) {                                // a jump in the part
                d1 &= 0x7f;
                if (c.PartJmpCnt) {
                    if (--c.PartJmpCnt) { c.PartPos = d1; c.PartPosWork = d1; }
                } else if (row > d1) {
                    c.PartJmpCnt = c.PartInst;
                    if (!c.PartJmpCnt) c.done = true;
                    c.PartPos = d1;
                    c.PartPosWork = d1;
                }
                continue;
            }
            if (d1) c.Transpose = transpose;
            CheckInst(c, n);
            return;
        }
        if (!again) return;
    }
}

void Player::CheckInst(Chan &c, int n) {
    if (c.PartInst) {
        c.InstPtr = &mod.inst(c.PartInst);
        if (c.PartInst != c.OldInst) {
            c.Arp = 0;
            c.InstPchSld = 0;
            c.OldInst = c.PartInst;
        }
    }
    PlayPartFx(c, n);
    PlayArpg(c);
    PlayInst(c);
}

void Player::PlayPartFx(Chan &c, int n) {
    c.Play |= FIRSTTICK;
    c.Effects1 &= (uint8_t)~HOLDSUSTAIN;
    c.Vib = 0;
    c.VibNote = 0;
    c.PTVibNote = 0;
    c.Tre = 0;
    c.Vol = 0;
    c.VolAdd = 0;
    c.VolSld = 0;
    c.CVolSld = 0;
    c.MVolSld = 0;
    c.PchSld = 0;
    c.PTPchSld = 0;
    c.SmpOfs = 0;
    c.Restart = 0;
    c.Arp &= 0xf5;
    if (c.Arp & ARP_ONESTEP) {
        c.Arp = 0;
        c.ArpVolSld = 0;
        c.ArpPchSld = 0;
        c.ArpPchSldNote = 0;
    }
    if (c.PartNote) {
        c.PchSldToNote = -1;
        c.PTPchSldToNote = -1;
    }
    for (int i = 0; i < 5; i++) PartFx(c, n, c.PartEffects[i].num, c.PartEffects[i].par);
}

void Player::PartFx(Chan &c, int n, uint8_t num, uint8_t par) {
    switch (num) {
    case 0x01:                                              // slide up
        c.PchSld = num;
        c.PchSldType = 0;
        if (par) c.PchSldSpd = par;
        c.PchSldToNote = 59 * 32 + 32;
        break;
    case 0x02:                                              // slide down
        c.PchSld = num;
        c.PchSldType = 0xff;
        if (par) c.PchSldSpd = par;
        c.PchSldToNote = 0;
        break;
    case 0x03: {                                            // portamento
        c.PchSld = num;
        if (par) c.PchSldSpd = par;
        int16_t d1 = w(c.Note + c.PchSldNote);
        if (c.PartNote) {
            int16_t d2 = w(c.PartNote << 5);
            c.PartNote = 0;
            c.PchSldToNote = d2;
            if (d2 != d1) {
                c.PchSldType = w(d2 - d1) < 0 ? 0xff : 0;
                break;
            }
            c.PchSldToNote = -1;
        }
        if (c.PchSldToNote == -1) c.PchSld = 0;
        break;
    }
    case 0x04: c.InstPchSld = 0; break;
    case 0x05: if (par) c.PchAdd = w(c.PchAdd + par); break;
    case 0x06: if (par) c.PchAdd = w(c.PchAdd - par); break;
    case 0x07: if (par) c.VibCmdSpeed = par; break;
    case 0x08:                                              // vibrato up, down
    case 0x09:
        c.Vib = num;
        if (c.PartNote) {
            c.VibDir = num == 0x08 ? 1 : 0;
            c.VibCount = 0;
            c.VibCmdDepth = 0;
            c.VibCmdDelay = 0;
            c.VibAtkSpeed = 0;
            c.VibAtkLength = 0;
        }
        if (par) c.VibDepth = (uint16_t)(par << 8);
        c.VibWaveNum = c.PartVibWaveNum;
        break;
    case 0x0a: if (par <= 3) c.VibWaveNum = c.PartVibWaveNum = par; break;
    case 0x0b: {                                            // fine tune
        int8_t d1 = (int8_t)par;
        if (d1 > 31) d1 = 31;
        if (d1 < -31) d1 = -31;
        c.FineTune = d1;
        c.Play |= KEEPFINETUNE;
        break;
    }

    case 0x10: c.Vol = num; c.VolSet = w(par << 4); break;
    case 0x11:
    case 0x12:
    case 0xe6:                                              // the ProTracker slides are the same,
    case 0xe7:                                              // but not on the first tick of a line
        c.VolSld = num;
        if (par) c.VolSldSpd = par;
        break;
    case 0x13: c.VolSldToVol = w(par << 4); break;
    case 0x14:
        c.VolSld = num;
        if (par) c.VolSldSpd = par;
        if (c.VolSldToVol != c.Volume1) {
            c.VolSldType = w(c.VolSldToVol - c.Volume1) < 0 ? 0xff : 0;
            c.VolSldToVolOff = 0;
        } else {
            c.VolSldToVolOff = 1;
        }
        break;
    case 0x15: c.VolAdd = num; if (par) c.VolAddNum = w(par << 4); break;
    case 0x16: c.VolAdd = num; if (par) c.VolAddNum = w(-(par << 4)); break;
    case 0x17: if (par) c.TreCmdSpeed = par; break;
    case 0x18:
    case 0x19:
        c.Tre = num;
        if (c.PartNote) {
            c.TreDir = num == 0x18 ? 1 : 0;
            c.TreCount = 0;
            c.TreCmdDepth = 0;
            c.TreCmdDelay = 0;
            c.TreAtkSpeed = 0;
            c.TreAtkLength = 0;
        }
        if (par) c.TreDepth = (uint16_t)(par << 8);
        c.TreWaveNum = c.PartTreWaveNum;
        break;
    case 0x1a: if (par <= 3) c.TreWaveNum = c.PartTreWaveNum = par; break;

    case 0x20: c.CVolume = w(par << 4); break;
    case 0x21:
    case 0x22:
        c.CVolSld = num;
        if (par) c.CVolSldSpd = par;
        break;
    case 0x23: c.CVolSldToVol = w(par << 4); break;
    case 0x24:
        c.CVolSld = num;
        if (par) c.CVolSldSpd = par;
        if (c.CVolSldToVol != c.CVolume) {
            c.CVolSldType = w(c.CVolSldToVol - c.CVolume) < 0 ? 0xff : 0;
            c.CVolSldToVolOff = 0;
        } else {
            c.CVolSldToVolOff = 1;
        }
        break;
    case 0x25:
        if (par) c.CVolAddNum = w(par << 4);
        c.CVolume = w(c.CVolume + c.CVolAddNum);
        if ((uint16_t)c.CVolume > 64 * 16) c.CVolume = 64 * 16;
        break;
    case 0x26:
        if (par) c.CVolAddNum = w(par << 4);
        c.CVolume = w(c.CVolume - c.CVolAddNum);
        if (c.CVolume < 0) c.CVolume = 0;
        break;
    case 0x27:
        for (int i = 0; i < channels(); i++) ch[i].CVolume = w(par << 4);
        break;

    case 0x30: MasterVol = w(par << 4); break;
    case 0x31:
    case 0x32:
        c.MVolSld = num;
        if (par) c.MVolSldSpd = par;
        break;
    case 0x33: c.MVolSldToVol = w(par << 4); break;
    case 0x34: {
        c.MVolSld = num;
        if (par) c.MVolSldSpd = par;
        // The Amiga code forgets to fetch the master volume and compares with what is
        // left in the register: four times the number of this effect.
        int16_t d1 = 0x34 * 4;
        if (c.MVolSldToVol != d1) {
            c.MVolSldType = w(c.MVolSldToVol - d1) < 0 ? 0xff : 0;
            c.MVolSldToVolOff = 0;
        } else {
            c.MVolSldToVolOff = 1;
        }
        break;
    }
    case 0x35:
        if (par) c.MVolAddNum = w(par << 4);
        MasterVol = w(MasterVol + c.MVolAddNum);
        if ((uint16_t)MasterVol > 64 * 16) MasterVol = 64 * 16;
        break;
    case 0x36:
        if (par) c.MVolAddNum = w(par << 4);
        MasterVol = w(MasterVol - c.MVolAddNum);
        if (MasterVol < 0) MasterVol = 0;
        break;

    case 0x40:                                              // speed of this channel
        if (par) {
            uint8_t d0 = par > 0x1f ? 0x1f : par;
            c.SpdPart = 1;
            c.Spd = d0;
            if (!c.Grv || !c.PartGrv) c.SpdCnt = d0;
        }
        break;
    case 0x41:                                              // groove of this channel
        if (par) {
            uint8_t d0 = par > 0x1f ? 0x1f : par;
            c.GrvPart = 1;
            c.Grv = d0;
            if (c.PartGrv) c.SpdCnt = d0;
        }
        break;
    case 0x42:                                              // speed of all channels, or the tempo
        if (!par) break;
        if (par >= 0x20) {
            TuneTmp = par;
            setTempo();     // which the eight channel routine never looks at
            break;
        }
        TuneSpd = par;
        for (int i = 0; i < channels(); i++) {
            Chan &k = ch[i];
            if (i > n) { k.Spd = par; continue; }
            if (k.SpdPart) continue;
            k.Spd = par;
            if (!k.Grv || !k.PartGrv) k.SpdCnt = par;
        }
        break;
    case 0x43: {                                            // groove of all channels
        if (!par) break;
        uint8_t d0 = par & 0x1f;
        TuneGrv = d0;
        for (int i = 0; i < channels(); i++) {
            Chan &k = ch[i];
            if (i > n) { k.Grv = d0; continue; }
            if (k.GrvPart) continue;
            k.Grv = d0;
            if (d0 && !k.PartGrv) k.SpdCnt = d0;
        }
        break;
    }
    case 0x44: c.Arp |= ARP_TABLE; c.ArpTab = par; break;
    case 0x45: c.Arp |= ARP_TABLE | ARP_ONESTEP; c.ArpTab = par; break;
    case 0x46:
        c.Effects1 |= HOLDSUSTAIN;
        c.EffectsPar1 &= (uint8_t)~ADSRHOLDSUSTAIN;
        if (par) c.EffectsPar1 |= ADSRHOLDSUSTAIN;
        break;
    case 0x47: led = par != 0; break;
    case 0x48: c.SmpOfs = num; if (par) c.SmplOfs = par; break;
    case 0x49: if (!c.PartInst) c.Restart = RESTART_PARTFX; break;
    case 0x4a:
        if (par) {
            c.WsPtr = &mod.smpl(par);
            c.Arp |= ARP_WS;
        }
        break;
    case 0x4b:
        c.PhaInit = c.ResInit = c.FilInit = c.TraInit = c.MixInit = c.LooInit = 0;
        break;

    case 0xe1:
        c.PTPchSld = num;
        c.PTPchSldType = 0;
        if (par) c.PTPchSldSpd = par;
        c.PTPchSldToNote = 106;
        break;
    case 0xe2:
        c.PTPchSld = num;
        c.PTPchSldType = 0xff;
        if (par) c.PTPchSldSpd = par;
        c.PTPchSldToNote = 3591;
        break;
    case 0xe3: {
        c.PchSld = fx_Portamento;
        c.PTPchSld = num;
        if (par) c.PTPchSldSpd2 = par;
        int16_t d2 = w(GetPeriod(c, c.Note) + c.PTPchSldNote);
        if (c.PartNote) {
            int16_t d0 = GetPeriod(c, w(c.PartNote << 5));
            c.PartNote = 0;
            c.PTPchSldToNote = d0;
            if (d2 != d0) {
                c.PTPchSldType = w(d2 - d0) < 0 ? 0xff : 0;
                break;
            }
            c.PTPchSldToNote = -1;
        }
        if (c.PTPchSldToNote == -1) {
            c.PTPchSld = 0;
            c.PchSld = 0;
        }
        break;
    }
    case 0xe4: if (par) c.PTPchAdd = w(c.PTPchAdd - (par & 15)); break;
    case 0xe5: if (par) c.PTPchAdd = w(c.PTPchAdd + (par & 15)); break;
    case 0xe8:
        c.Tre = num;
        if (par) {
            if (par & 0x0f) c.PTTreCmd = (uint8_t)((c.PTTreCmd & 0xf0) | (par & 0x0f));
            if (par & 0xf0) c.PTTreCmd = (uint8_t)((c.PTTreCmd & 0x0f) | (par & 0xf0));
        }
        break;
    case 0xe9: c.PTTreWave = par & 0x0f; break;
    case 0xea:
        c.Vib = num;
        if (par) {
            if (par & 0x0f) c.PTVibCmd = (uint8_t)((c.PTVibCmd & 0xf0) | (par & 0x0f));
            if (par & 0xf0) c.PTVibCmd = (uint8_t)((c.PTVibCmd & 0x0f) | (par & 0xf0));
        }
        break;
    case 0xeb: c.PTVibWave = par & 0x0f; break;
    }
}

// ---------------------------------------------------------------- notes

void Player::ArpFx(Chan &c, uint8_t num, uint8_t par) {
    switch (num) {
    case 1:
        c.ArpPchSld = num;
        c.ArpPchSldType = 0;
        if (par) { c.ArpPchSldSpd = par; c.ArpPchSldToNote = 59 * 32 + 32; }
        break;
    case 2:
        c.ArpPchSld = num;
        c.ArpPchSldType = 0xff;
        if (par) { c.ArpPchSldSpd = par; c.ArpPchSldToNote = 0; }
        break;
    case 3:
        c.Restart |= ARP_SETVOLUME;
        c.Volume1 = c.Volume2 = c.Volume3 = w(par << 4);
        break;
    case 4:
    case 5:
        c.ArpVolSld = num;
        if (par) c.ArpVolSldSpd = par;
        break;
    case 6: {
        c.Restart |= RESTART_ARP;
        // the bits of resonance and mix are looked for in the wrong byte; it is what the Amiga does
        uint8_t d3 = c.EffectsPar1;
        if (d3 & PHASEINIT) c.PhaInit = 0;
        if (d3 & RESONANCEINIT) c.ResInit = 0;
        if (d3 & FILTERINIT) c.FilInit = 0;
        if (d3 & TRANSFORMINIT) c.TraInit = 0;
        if (d3 & MIXINIT) c.MixInit = 0;
        if (c.EffectsPar2 & LOOPINIT) c.LooInit = 0;
        break;
    }
    }
}

void Player::ArpWaitStart(Chan &c, uint8_t smpl) {
    if (!smpl) return;
    c.WsPtr = &mod.smpl(smpl);
    c.Arp |= ARP_WS;
}

void Player::PlayArpg(Chan &c) {
    if (!c.PartNote || !c.InstPtr) return;
    const Inst &in = *c.InstPtr;
    c.ArpWait = 0;
    if (!(c.Arp & ARP_TABLE) && !(c.Arp & ARP_ON)) {
        if (!(in.effects1 & ARPEGGIO)) return;
        c.Arp |= ARP_ON;
    }
    if (!c.Restart && !c.PartInst) return;
    c.Arp |= ARP_SETSNOTE;
    unsigned table = (c.Arp & ARP_TABLE) ? c.ArpTab : in.arpTable;
    const Arpg &arp = mod.arpgStore[mod.arpg[table & 255]];
    c.ArpPos = 0;
    c.ArpWait = 0;
    c.ArpVolSld = 0;
    c.ArpPchSld = 0;
    c.ArpPchSldNote = 0;
    c.ArpSpdCnt = in.arpSpeed;
    for (int guard = 0; guard < 256; guard++) {
        const ArpLine &l = arp.line[c.ArpPos & 0x7f];
        c.ArpPos = (c.ArpPos + 1) & 0x7f;
        c.ArpgNote = c.PartNote;
        int8_t d0 = l.note;
        if (d0 == 0) { c.ArpWait |= 1; return; }
        if (d0 == 61) { c.Effects1 &= (uint8_t)~ARPEGGIO; return; }
        if (d0 == 62) continue;
        c.WsNumber = l.smpl ? l.smpl : in.smplNumber;
        for (int i = 0; i < 2; i++) if (l.fx[i].num <= 5) ArpFx(c, l.fx[i].num, l.fx[i].par);
        if (d0 < 0) d0 = (int8_t)(d0 + 61 + c.ArpgNote);
        else c.Arp |= ARP_ABSOLUTE;
        c.Note = c.ArpNote = w(d0 * 32);
        ArpWaitStart(c, l.smpl);
        return;
    }
}

void Player::FixWaveLength(Chan &c, const Inst &in, uint8_t type, Ptr p) {
    static const uint16_t offset[6] = {0, 256 + 128 + 64 + 32, 256 + 128 + 64, 256 + 128, 256, 0};
    int t = type > 5 ? 5 : type;
    if (t < 1) t = 1;
    p = p.plus(offset[t]);
    uint16_t len = (uint16_t)(in.length >> (5 - t));
    c.WsPointer = c.WsRepPointer = c.WsRepPtrOrg = p;
    c.WsLength = c.WsRepLength = len;
}

void Player::PlayInst(Chan &c) {
    if ((c.ArpWait & 1) || !c.InstPtr) return;
    const Inst &in = *c.InstPtr;
    const SmplHead *a1 = &in;
    uint8_t type;

    if (c.Restart & 3) goto inst;
    if (!c.PartInst) goto playnote;
    if (c.PchSld == fx_Portamento || !c.PartNote) goto getvol;
    if (c.Arp & ARP_WS) {
        if (!c.WsPtr) return;
        a1 = c.WsPtr;
        type = a1->type ? in.type : 0;
        goto ws;
    }
inst:
    c.WsNumber = in.smplNumber;
    type = in.type;
ws: {
    if (a1->pointer.null()) return;
    Ptr d0 = a1->pointer;
    if (!in.transpose) c.Transpose = 0;
    uint8_t d3 = in.envTraPhaFilBits;
    if (c.Effects1 & HOLDSUSTAIN) {
        d3 &= 0xfe;
        c.EffectsPar1 &= 1;
        d3 |= c.EffectsPar1;
    }
    c.EffectsPar1 = d3;
    c.EffectsPar2 = in.mixResLooBits;
    c.Effects1 = in.effects1;
    c.Effects2 = in.effects2;
    c.WaveOrSample = type;
    if (type) {
        FixWaveLength(c, in, type, d0);
    } else {
        uint16_t d1 = a1->length;
        if (c.SmpOfs) {
            uint16_t ofs = (uint16_t)(c.SmplOfs << 7);
            if (ofs < d1) {
                d1 = (uint16_t)(d1 - ofs);
                d0 = d0.plus(ofs * 2u);
            } else {
                d1 = 1;
            }
        }
        c.WsPointer = d0;
        c.WsLength = d1;
        bool repeat = (c.Arp & ARP_WS) ? a1->repLength != 0 : (in.effects1 & WSLOOP) != 0;
        if (repeat && !a1->repPointer.null()) {
            c.WsRepPointer = c.WsRepPtrOrg = a1->repPointer;
            c.WsRepLength = a1->repLength;
        } else {
            c.WsRepPointer = c.WsRepPtrOrg = zeroSample();
            c.WsRepLength = 1;
        }
    }
    c.Restart &= 7;
    if (c.Restart) goto playnote;
}
getvol:
    c.Volume1 = w(in.volume << 4);
playnote: {
    int16_t d1 = c.Volume1;
    if (c.Vol) d1 = c.VolSet;
    if (c.VolAdd) {
        d1 = w(d1 + c.VolAddNum);
        if (d1 < 0) d1 = 0;
        if ((uint16_t)d1 > 64 * 16) d1 = 64 * 16;
    }
    c.Volume1 = c.Volume2 = c.Volume3 = d1;
    if (c.PchSld == fx_Portamento) return;

    if (in.slideSpeed && c.PartNote) {
        c.PchSldSpd = (int16_t)((c.PchSldSpd & 0xff00) | in.slideSpeed);
        int16_t from = w(c.Note + c.PchSldNote);
        int16_t to = w(c.PartNote << 5);
        c.PchSldToNote = to;
        c.PchSldType = w(to - from) < 0 ? 0xff : 0;
        if (c.InstPchSld) { InstPlay(c, in); return; }
        c.InstPchSld = fx_Portamento;
    }
    bool setsNote = (c.Arp & ARP_SETSNOTE) != 0;
    c.Arp &= (uint8_t)~ARP_SETSNOTE;
    if (!setsNote) {
        if (!c.PartNote) return;
        c.Note = w(c.PartNote << 5);
        c.ArpVolSld = 0;
        c.ArpPchSld = 0;
        c.ArpPchSldNote = 0;
    }
    c.SemiTone = w(a1->semiTone << 5);
    if (!(c.Play & KEEPFINETUNE)) c.FineTune = a1->fineTune;
    c.PTPchSldNote = 0;
    c.PchSldNote = 0;
    c.PTVibNote = 0;
    c.PTTrePos = 0;
    c.PTVibPos = 0;
    c.VibNote = 0;
    c.PTPchAdd = 0;
    c.PchAdd = 0;
    if (c.PartInst || (c.Restart & 3)) InstPlay(c, in);
}
}

static void initCounter(Counter &k, const CounterInit &i) {
    k.counter = (int16_t)i.start;
    k.speed = (int16_t)i.speed;
    k.repeat = i.repeat;
    k.repeatend = i.repEnd;
    if ((int16_t)i.start > (int16_t)i.repeat) k.speed = w(-k.speed);
}

// what transform, phase, mix, resonance and filter all do when a note starts
static void startCounter(Chan &c, Counter &k, const CounterInit &i, bool step, bool init, uint16_t &spd,
                         uint8_t &initOf, uint8_t *lastInit) {
    k.step = step;
    bool fresh;
    if (step) {
        spd = i.turns;
        k.turns = 0;
        fresh = false;
    } else {
        k.turns = (int16_t)i.turns;
        fresh = !init;
        if (fresh) initOf = 0;
    }
    if (!fresh) {
        uint8_t was = initOf;
        initOf = c.PartInst;
        fresh = was != c.PartInst;
    }
    if (fresh) {
        if (lastInit) *lastInit = 1;
        initCounter(k, i);
    }
    k.delay = i.delay;
}

void Player::InstPlay(Chan &c, const Inst &in) {
    if (c.PchSld != fx_Portamento && !(c.ArpWait & 1)) {
        c.Play |= PLAY;
        if (!c.Vib && (c.Effects1 & VIBRATO)) {
            c.VibCount = 0;
            c.VibCmdDepth = 0;
            c.VibCmdSpeed = in.vibSpeed;
            c.VibCmdDelay = in.vibDelay;
            c.VibAtkSpeed = in.vibAtkSpd;
            c.VibAtkLength = in.vibAttack;
            c.VibDepth = in.vibDepth;
            c.VibWaveNum = in.vibWaveNum;
            c.VibDir = in.vibDir;
        }
        if (!c.Tre && (c.Effects1 & TREMOLO)) {
            c.TreCount = 0;
            c.TreCmdDepth = 0;
            c.TreCmdSpeed = in.treSpeed;
            c.TreCmdDelay = in.treDelay;
            c.TreAtkSpeed = in.treAtkSpd;
            c.TreAtkLength = in.treAttack;
            c.TreDepth = in.treDepth;
            c.TreWaveNum = in.treWaveNum;
            c.TreDir = in.treDir;
        }
        if (c.Effects1 & ADSR) {
            c.ADSRVolume = 0;
            memcpy(c.ADSRData, in.env, sizeof c.ADSRData);
        }
        if (c.Effects2 & PHASE) {
            c.PhaType = in.phaType;
            startCounter(c, c.PhaData, in.pha, (c.EffectsPar1 & PHASESTEP) != 0,
                         (c.EffectsPar1 & PHASEINIT) != 0, c.PhaSpd, c.PhaInit, nullptr);
        }
        c.MixResFilBoost = in.mixResFilBoost;
        if (c.Effects2 & RESONANCE) {
            c.ResAmp = in.resAmp;
            startCounter(c, c.ResData, in.res, (c.EffectsPar2 & RESONANCESTEP) != 0,
                         (c.EffectsPar2 & RESONANCEINIT) != 0, c.ResSpd, c.ResInit, &c.ResLastInit);
        }
        if (c.Effects2 & FILTER) {
            c.FilType = in.filType;
            startCounter(c, c.FilData, in.fil, (c.EffectsPar1 & FILTERSTEP) != 0,
                         (c.EffectsPar1 & FILTERINIT) != 0, c.FilSpd, c.FilInit, &c.FilLastInit);
        }
        if (c.Effects2 & MIX) {
            c.MixWaveNum = in.mixWaveNum;
            startCounter(c, c.MixData, in.mix, (c.EffectsPar2 & MIXSTEP) != 0,
                         (c.EffectsPar2 & MIXINIT) != 0, c.MixSpd, c.MixInit, nullptr);
        }
        if (c.Effects2 & TRANSFORM) {
            c.TraWsPtrs[0] = in.smplNumber;
            memcpy(c.TraWsPtrs + 1, in.traWaveNums, 5);
            startCounter(c, c.TraData, in.tra, (c.EffectsPar1 & TRANSFORMSTEP) != 0,
                         (c.EffectsPar1 & TRANSFORMINIT) != 0, c.TraSpd, c.TraInit, nullptr);
        }
        if (c.Effects1 & LOOP) {
            if (in.type || !in.looLength) {
                c.Effects1 &= (uint8_t)~LOOP;
            } else {
                bool fresh;
                if (c.EffectsPar2 & LOOPSTEP) {
                    c.LooSpd = in.looTurns;
                    c.LooTurns = 0;
                    fresh = false;
                } else {
                    c.LooTurns = (int16_t)in.looTurns;
                    fresh = !(c.EffectsPar2 & LOOPINIT);
                    if (fresh) c.LooInit = 0;
                }
                if (!fresh) {
                    uint8_t was = c.LooInit;
                    c.LooInit = c.PartInst;
                    fresh = was != c.PartInst;
                    if (!fresh) c.Play &= (uint8_t)~PLAY;
                }
                if (fresh) {
                    const Sample &s = mod.smpl(in.smplNumber);
                    c.LooWsPointer = s.pointer;
                    c.LooCounter = c.LooCounterSave = in.looStart;
                    c.LooWsCounterMax = (uint16_t)(s.length - in.looLength);
                    c.LooLength = in.looLength;
                    c.LooRepEnd = in.looRepEnd;
                    c.LooWait = in.looWait;
                    c.LooStep = in.looLpStep;
                    c.LooRepeat = in.looRepeat;
                    if ((int16_t)in.looStart > (int16_t)in.looRepeat) c.LooStep = -c.LooStep;
                }
                c.LooDelay = (int16_t)in.looDelay;
                c.LooWaitCounter = 0;
                Ptr p = c.LooWsPointer.null() ? zeroSample() : c.LooWsPointer.plus(c.LooCounterSave * 2u);
                c.WsPointer = c.WsRepPointer = c.WsRepPtrOrg = p;
                c.WsLength = c.WsRepLength = c.LooLength;
            }
        }
    }
    Sexit(c);
}

// the effects that make a new wave of the wave only work on waves of these lengths
void Player::Sexit(Chan &c) {
    uint16_t l = c.WsRepLength;
    if (l != 8 && l != 16 && l != 32 && l != 64 && l != 128) c.Effects2 = 0;
}

// ---------------------------------------------------------------- period and volume

void Player::Per(Chan &c) {
    int16_t d0 = w(c.Note + c.VibNote + c.PchSldNote + c.ArpPchSldNote + c.SemiTone + c.FineTune +
                   c.PchAdd);
    if (!(c.Arp & ARP_ABSOLUTE) && c.Transpose) d0 = w(d0 + c.Transpose);
    if (d0 < -32) d0 = -32;
    if (d0 > 5 * 12 * 32) d0 = 5 * 12 * 32;
    d0 = (int16_t)tab::PalPitchTable[d0 + 32];
    d0 = w(d0 + c.PTPchSldNote + c.PTVibNote + c.PTPchAdd);
    if (d0 < 106) d0 = 106;
    if (d0 > 3591) d0 = 3591;
    c.Period1 = c.Period2 = d0;
}

int16_t Player::GetPeriod(const Chan &c, int16_t note) const {
    int16_t d0 = w(note + c.VibNote + c.PchSldNote + c.ArpPchSldNote + c.SemiTone + c.FineTune +
                   c.PchAdd);
    if (c.Transpose) d0 = w(d0 + c.Transpose);
    if (d0 < -32) d0 = -32;
    if (d0 > 5 * 12 * 32) d0 = 5 * 12 * 32;
    return (int16_t)tab::PalPitchTable[d0 + 32];
}

int Player::paulaVolume(const Chan &c) const {
    uint32_t d1 = (uint16_t)c.Volume3;
    d1 = (uint16_t)((d1 * (uint16_t)c.CVolume) >> 10);
    d1 = (uint16_t)((d1 * (uint16_t)MasterVol) >> 14);
    if (playMode) return d1 > 64 ? 64 : (int)d1;    // beyond the last of its volume tables
    d1 &= 127;                                      // Paula has seven bits for it
    return d1 > 64 ? 64 : (int)d1;
}

void Player::setNext(int n, Ptr p, uint32_t words) {
    Voice &v = voice[n];
    if (writes) writes->push_back({n, (uint16_t)words, p.base, p.ofs});
    uint32_t bytes = words ? words * 2 : 0x20000;   // Paula takes a length of 0 for 65536 words
    if (p.null()) { p = zeroSample(); bytes = 2; }
    if (bytes > p.left()) bytes = p.left();
    if (bytes < 2) { p = zeroSample(); bytes = 2; }
    v.nxt = p;
    v.nxtLen = bytes;
}

void Player::PerVolPlay() {
    for (int i = 0; i < channels(); i++) {
        Chan &c = ch[i];
        if (c.Play & PLAY) continue;
        voice[i].period = (uint16_t)c.Period2;
        voice[i].vol = paulaVolume(c);
    }
}

void Player::DmaPlay() {
    for (int i = 0; i < channels(); i++) {
        Chan &c = ch[i];
        Voice &v = voice[i];
        if (playMode) {
            // the mixing routine asks the channel for its repeat whenever a sample has been played
            if (!(c.Play & PLAY)) {
                setNext(i, c.WsRepPointer, c.WsRepLength);
                v.loop = c.MixLoop != 0;
                continue;
            }
            c.Play &= (uint8_t)~PLAY;
            c.MixLoop = c.Effects1 & (LOOP | WSLOOP);
            v.vol = paulaVolume(c);
            v.period = (uint16_t)c.Period2;
            setNext(i, c.WsPointer, c.WsLength);
            v.cur = v.nxt;
            v.curLen = v.nxtLen;
            v.pos = 0;
            v.on = true;
            v.loop = c.MixLoop != 0;
            setNext(i, c.WsRepPointer, c.WsRepLength);
            continue;
        }
        if (!(c.Play & PLAY)) continue;
        c.Play &= (uint8_t)~PLAY;
        bool trigger = !c.WaveOrSample || c.WsNumber != c.WsNumberOld;
        c.WsNumberOld = c.WsNumber;
        v.vol = paulaVolume(c);
        v.period = (uint16_t)c.Period2;
        setNext(i, c.WsPointer, c.WsLength);
        if (trigger) {
            v.cur = v.nxt;
            v.curLen = v.nxtLen;
            v.pos = 0;
            v.on = true;
            setNext(i, c.WsRepPointer, c.WsRepLength);
        }
    }
}

// ---------------------------------------------------------------- what happens on every tick

void Player::PlayEffects(int n) {
    Chan &c = ch[n];
    c.WsRepPointer = c.WsRepPtrOrg;
    if (!(c.Play & PLAY)) {
        SlideVol(c);
        SlideChannelVol(c);
        SlideMasterVol(c);
        SlideArpVol(c);
        SlideNote(c);
        SlideArpNote(c);
        ArpeggioPlay(c);
        VibratoPlay(c);
        TremoloPlay(c);
    }
    ADSRPlay(c);
    MoveLoop(c, n);
    TransformPlay(c);
    PhasePlay(c);
    MixPlay(c);
    ResonancePlay(c);
    FilterPlay(c);

    // The wave the effects have made is in a buffer of the channel; Paula gets a copy.
    const int8_t *b = c.WsRepPointer.base;
    if (b == c.TraWaveBuffer || b == c.PhaWaveBuffer || b == c.MixWaveBuffer || b == c.ResWaveBuffer ||
        b == c.FilWaveBuffer) {
        unsigned len = c.WsRepLength * 2u;
        if (len > 256) len = 256;
        memcpy(c.WaveBuffer, b, len);
        c.WsRepPointer = buffer(c.WaveBuffer);
        if (c.WaveOrSample || (c.Effects1 & LOOP)) c.WsPointer = c.WsRepPointer;
    }

    if (!(c.Play & PLAY) && (c.Effects1 & LOOP)) {
        if (!playMode) setNext(n, c.WsRepPointer, c.LooLength);
        c.WsRepLength = c.LooLength;
    }
    if (!c.ArpWait) c.PartNote = 0;
    c.Play &= PLAY;
}

void Player::LoopCounter(Chan &c) {
    if (c.LooTurns < 0) return;
    int32_t d0 = c.LooCounter;
    bool ok;
    d0 += c.LooStep;
    if (c.LooRepeat < c.LooRepEnd) ok = c.LooStep < 0 ? d0 >= c.LooRepeat : d0 <= c.LooRepEnd;
    else ok = c.LooStep < 0 ? d0 >= c.LooRepEnd : d0 <= c.LooRepeat;
    if (!ok) {
        if (c.LooTurns && --c.LooTurns == 0) c.LooTurns = -1;
        d0 -= c.LooStep;
        c.LooStep = -c.LooStep;
    }
    c.LooCounter = (uint16_t)d0;
}

void Player::MoveLoop(Chan &c, int n) {
    if (!(c.Effects1 & LOOP)) return;
    auto place = [&](uint16_t counter) {
        Ptr p = c.LooWsPointer.null() ? zeroSample() : c.LooWsPointer.plus(counter * 2u);
        c.WsRepPointer = c.WsRepPtrOrg = p;
    };
    bool step = (c.EffectsPar2 & LOOPSTEP) != 0;
    bool count = true, save = true;
    if (!step) {
        if (--c.LooDelay >= 0) { place(c.LooCounterSave); return; }
        c.LooDelay = 0;
        if (c.LooWait) {
            if (--c.LooWaitCounter >= 0) return;
            c.LooWaitCounter = (int16_t)c.LooWait;
        }
    } else if (!(c.EffectsPar2 & LOOPINIT)) {
        if (!c.PartNote) count = false;
    } else {
        if (!c.PartNote) save = false;
    }
    if (count) {
        if (save) c.LooCounterSave = c.LooCounter;
        LoopCounter(c);
    }
    uint16_t d0 = c.LooCounterSave;

    if (step) {
        bool go = true;
        if (--c.LooDelay >= 0) go = false;
        else {
            c.LooDelay = 0;
            if (c.LooWait) {
                if (--c.LooWaitCounter >= 0) go = false;
                else c.LooWaitCounter = (int16_t)c.LooWait;
            }
        }
        if (go) {
            int32_t d3 = (int16_t)(c.LooStep & 0xffff);
            if (d3 < 0) d3 = -d3;
            int32_t d2 = c.LooCounterSave;
            int32_t d1 = (int8_t)(c.LooSpd & 0xff);
            d2 += d1 * d3;
            if (d1 < 0) {
                if (d2 < 0) d2 = 0;
            } else if ((uint32_t)d2 >= c.LooWsCounterMax) {
                d2 = c.LooWsCounterMax;
            }
            c.LooCounterSave = (uint16_t)d2;
        }
    }
    place(d0);
    if (c.LooTurns >= 0 || !(c.Effects1 & LOOPSTOP)) return;
    c.MixLoop = 0;
    c.Effects1 &= (uint8_t)~LOOP;
    if (!playMode) setNext(n, zeroSample(), 1);
}

void Player::SlideNote(Chan &c) {
    if (c.PTPchSld) {
        if ((c.Play & FIRSTTICK) || c.PTPchSldToNote < 0) return;
        int16_t spd = c.PTPchSld == fx_PTPortamento ? c.PTPchSldSpd2 : c.PTPchSldSpd;
        if (c.PTPchSldType) {       // down: the period grows
            c.PTPchSldNote = w(c.PTPchSldNote + spd);
            int16_t d0 = w(GetPeriod(c, c.Note) + c.PTPchSldNote - c.PTPchSldToNote);
            if (d0 < 0) return;
            c.PTPchSldNote = w(c.PTPchSldNote - d0);
        } else {
            c.PTPchSldNote = w(c.PTPchSldNote - spd);
            int16_t d0 = w(GetPeriod(c, c.Note) + c.PTPchSldNote - c.PTPchSldToNote);
            if (d0 > 0) return;
            c.PTPchSldNote = w(c.PTPchSldNote - d0);
        }
        c.PTPchSldToNote = -1;
        return;
    }
    if (!c.PchSld && !c.InstPchSld) return;
    if (c.PchSldToNote < 0) return;
    if (c.PchSldType) {
        c.PchSldNote = w(c.PchSldNote - c.PchSldSpd);
        int16_t d0 = w(c.Note + c.PchSldNote - c.PchSldToNote);
        if (d0 > 0) return;
        c.PchSldNote = w(c.PchSldNote - d0);
    } else {
        c.PchSldNote = w(c.PchSldNote + c.PchSldSpd);
        int16_t d0 = w(c.Note + c.PchSldNote - c.PchSldToNote);
        if (d0 < 0) return;
        c.PchSldNote = w(c.PchSldNote - d0);
    }
    c.PchSldToNote = -1;
}

void Player::SlideArpNote(Chan &c) {
    if (!c.ArpPchSld || c.ArpPchSldToNote < 0) return;
    if (c.ArpPchSldType) {
        c.ArpPchSldNote = w(c.ArpPchSldNote - c.ArpPchSldSpd);
        int16_t d0 = w(c.ArpNote + c.ArpPchSldNote - c.ArpPchSldToNote);
        if (d0 > 0) return;
        c.ArpPchSldNote = w(c.ArpPchSldNote - d0);
    } else {
        c.ArpPchSldNote = w(c.ArpPchSldNote + c.ArpPchSldSpd);
        int16_t d0 = w(c.ArpNote + c.ArpPchSldNote - c.ArpPchSldToNote);
        if (d0 < 0) return;
        c.ArpPchSldNote = w(c.ArpPchSldNote - d0);
    }
    c.ArpPchSldToNote = -1;
}

// one of the three volumes slides: that of the instrument, the channel or the whole tune
static bool slide(int16_t &vol, uint8_t kind, int16_t spd, int16_t to, uint8_t type, uint8_t &off,
                  uint8_t toVol, uint8_t up, uint8_t down) {
    int16_t d2;
    if (kind == toVol) {
        if (off) return false;
        if (type) {
            d2 = w(vol - spd);
            if (d2 < to) { d2 = to; off = 1; }
        } else {
            d2 = w(vol + spd);
            if (d2 > to) { d2 = to; off = 1; }
        }
    } else if (kind == up) {
        d2 = w(vol + spd);
        if (d2 > 64 * 16) d2 = 64 * 16;
    } else if (kind == down) {
        d2 = w(vol - spd);
        if (d2 < 0) d2 = 0;
    } else {
        return false;
    }
    vol = d2;
    return true;
}

void Player::SlideVol(Chan &c) {
    uint8_t kind = c.VolSld;
    if (kind == fx_PTVolSlideUp || kind == fx_PTVolSlideDown) {
        if (c.Play & FIRSTTICK) return;
        kind = kind == fx_PTVolSlideUp ? fx_VolumeSlideUp : fx_VolumeSlideDown;
    }
    if (slide(c.Volume1, kind, c.VolSldSpd, c.VolSldToVol, c.VolSldType, c.VolSldToVolOff,
              fx_VolumeSlideToVol, fx_VolumeSlideUp, fx_VolumeSlideDown))
        c.Volume2 = c.Volume3 = c.Volume1;
}

void Player::SlideChannelVol(Chan &c) {
    slide(c.CVolume, c.CVolSld, c.CVolSldSpd, c.CVolSldToVol, c.CVolSldType, c.CVolSldToVolOff,
          fx_ChannelVolSlideToVol, fx_ChannelVolSlideUp, fx_ChannelVolSlideDown);
}

void Player::SlideMasterVol(Chan &c) {
    slide(MasterVol, c.MVolSld, c.MVolSldSpd, c.MVolSldToVol, c.MVolSldType, c.MVolSldToVolOff,
          fx_MasterVolSlideToVol, fx_MasterVolSlideUp, fx_MasterVolSlideDown);
}

void Player::SlideArpVol(Chan &c) {
    int16_t d2;
    if (c.ArpVolSld == 4) {
        d2 = w(c.Volume1 + c.ArpVolSldSpd);
        if (d2 > 64 * 16) d2 = 64 * 16;
    } else if (c.ArpVolSld == 5) {
        d2 = w(c.Volume1 - c.ArpVolSldSpd);
        if (d2 < 0) d2 = 0;
    } else {
        return;
    }
    c.Volume1 = c.Volume2 = c.Volume3 = d2;
}

void Player::ArpeggioPlay(Chan &c) {
    if (!(c.Arp & ARP_TABLE) && !(c.Arp & ARP_ON)) return;
    if (--c.ArpSpdCnt != 0) return;
    if (!c.InstPtr) return;
    const Inst &in = *c.InstPtr;
    uint8_t spd = in.arpSpeed;
    c.ArpgGrv = (uint8_t)~c.ArpgGrv;
    if (c.ArpgGrv && in.arpGroove) spd = in.arpGroove;
    c.ArpSpdCnt = spd;
    unsigned table = (c.Arp & ARP_TABLE) ? c.ArpTab : in.arpTable;
    const Arpg &arp = mod.arpgStore[mod.arpg[table & 255]];

    for (int guard = 0; guard < 256; guard++) {
        uint8_t pos = c.ArpPos & 0x7f;
        c.ArpPos = (c.ArpPos + 1) & 0x7f;
        const ArpLine &l = arp.line[pos];
        if (c.ArpWait && !l.note) return;
        int8_t d0 = l.note;
        if (d0 == 61) {
            c.Arp = 0;
            c.Note = c.ArpNote;
            return;
        }
        if (d0 == 62) {
            if (l.smpl != pos) c.ArpPos = l.smpl;
            continue;
        }
        uint8_t d1 = l.smpl;
        c.WsNumber = d1 ? d1 : in.smplNumber;
        c.Restart = 0;
        c.ArpPchSld = 0;
        c.ArpVolSld = 0;
        for (int i = 0; i < 2; i++) if (l.fx[i].num <= 6) ArpFx(c, l.fx[i].num, l.fx[i].par);
        c.Arp &= (uint8_t)~ARP_ABSOLUTE;
        if (!d0) return;
        if (d0 < 0) d0 = (int8_t)(d0 + 61 + c.ArpgNote);
        else c.Arp |= ARP_ABSOLUTE;
        c.ArpNote = c.Note = w(d0 * 32);
        c.ArpPchSldNote = 0;
        if (c.ArpWait & 1) {
            c.ArpWait &= 0xfe;
            ArpWaitStart(c, d1);
            PlayInst(c);
            return;
        }
        if (c.Restart & RESTART_ARP) {
            c.Arp |= ARP_SETSNOTE;
            if (d1) {
                c.WsPtr = &mod.smpl(d1);
                c.Arp |= ARP_WS;
            }
            PlayInst(c);
            return;
        }
        if (!d1) {
            c.SemiTone = w(in.semiTone << 5);
            if (!(c.Play & KEEPFINETUNE)) c.FineTune = in.fineTune;
            return;
        }
        c.Play |= PLAY;
        const Sample &s = mod.smpl(d1);
        c.SemiTone = w(s.semiTone << 5);
        if (!(c.Play & KEEPFINETUNE)) c.FineTune = s.fineTune;
        c.WaveOrSample = s.type;
        if (s.type) {
            uint8_t type = in.type ? in.type : 3;
            c.WaveOrSample = type;
            FixWaveLength(c, in, type, s.pointer.null() ? zeroSample() : s.pointer);
        } else {
            c.WsPointer = s.pointer.null() ? zeroSample() : s.pointer;
            c.WsLength = s.pointer.null() ? 1 : s.length;
            if (s.repLength && !s.repPointer.null()) {
                c.WsRepPointer = c.WsRepPtrOrg = s.repPointer;
                c.WsRepLength = s.repLength;
            } else {
                c.WsRepPointer = c.WsRepPtrOrg = zeroSample();
                c.WsRepLength = 1;
            }
        }
        uint16_t len = c.WsRepLength;
        if (len == 8 || len == 16 || len == 32 || len == 64 || len == 128) c.Effects2 = in.effects2;
        else c.Effects2 = 0;
        return;
    }
}

void Player::ADSRPlay(Chan &c) {
    if (!(c.Effects1 & ADSR)) return;
    uint16_t *a = c.ADSRData;
    uint32_t d1;
    int stage = -1;
    if (a[0]) stage = 0;
    else if (a[1]) stage = 1;
    else if (a[2]) stage = 2;
    else if (c.EffectsPar1 & ADSRHOLDSUSTAIN) stage = -2;
    else if (a[3]) stage = 3;

    if (stage == -2) {
        d1 = a[10];                                 // held at the volume of the sustain
    } else if (stage == -1) {
        d1 = a[11];                                 // all over: the volume of the release
    } else {
        c.ADSRVolume = (uint16_t)(c.ADSRVolume + a[4 + stage]);
        d1 = c.ADSRVolume >> 8;
        if (--a[stage] == 0) d1 = a[8 + stage];
    }
    c.Volume3 = (int16_t)((d1 * (uint16_t)c.Volume2) >> 6);
}

static const int8_t *lfoWave(uint8_t num) {
    switch (num & 3) {
    case 1: return tab::DownRamp;
    case 2: return tab::Sawtooth;
    case 3: return tab::Square;
    }
    return tab::Sine;
}

// ProTracker's vibrato and tremolo
static int16_t ptWave(uint8_t pos, uint8_t wave, uint8_t cmd, int shift) {
    uint8_t d0 = (pos >> 2) & 0x1f;
    uint8_t d2;
    switch (wave & 3) {
    case 0: d2 = tab::PTVibratoTable[d0]; break;
    case 1:
        d0 = (uint8_t)(d0 << 3);
        d2 = (pos & 0x80) ? (uint8_t)(255 - d0) : d0;
        break;
    default: d2 = 255;
    }
    int16_t v = (int16_t)(((cmd & 15) * d2) >> shift);
    return (pos & 0x80) ? w(-v) : v;
}

void Player::TremoloPlay(Chan &c) {
    int16_t d4 = c.Volume1;
    if (c.Tre == fx_PTTremolo) {
        if (c.Play & FIRSTTICK) return;
        int16_t d2 = ptWave(c.PTTrePos, c.PTTreWave, c.PTTreCmd, 2);
        if (d4) {
            d4 = w(d4 + d2);
            if (d4 < 0) d4 = 0;
            if (d4 > 64 * 16) d4 = 64 * 16;
        }
        c.Volume2 = c.Volume3 = d4;
        c.PTTrePos = (uint8_t)(c.PTTrePos + ((c.PTTreCmd >> 2) & 0x3c));
        return;
    }
    if (!c.Tre) {
        if (!(c.Effects1 & TREMOLO)) return;
        if (c.TreCmdDelay) { c.TreCmdDelay--; return; }
    }
    if (!c.TreAtkLength) {
        c.TreCmdDepth = c.TreDepth;
    } else {
        c.TreCmdDepth = (uint16_t)(c.TreCmdDepth + c.TreAtkSpeed);
        if (--c.TreAtkLength == 0) c.TreCmdDepth = c.TreDepth;
    }
    int d3 = lfoWave(c.TreWaveNum)[(c.TreCount >> 2) & 127];
    if (!c.TreDir) d3 = -d3;
    d3 = (d3 * (int16_t)(c.TreCmdDepth >> 8)) >> 1;
    int16_t d = w(d3);
    if (d3 < 0) d = w(d + 16);
    if (d4) {
        d4 = w(d4 + d);
        if (d4 < 0) d4 = 0;
        if (d4 > 64 * 16) d4 = 64 * 16;
    }
    c.Volume2 = c.Volume3 = d4;
    c.TreCount = (uint16_t)((c.TreCount + c.TreCmdSpeed) & 0x1ff);
}

void Player::VibratoPlay(Chan &c) {
    if (c.Vib == fx_PTVibrato) {
        if (c.Play & FIRSTTICK) return;
        c.PTVibNote = ptWave(c.PTVibPos, c.PTVibWave, c.PTVibCmd, 7);
        c.PTVibPos = (uint8_t)(c.PTVibPos + ((c.PTVibCmd >> 2) & 0x3c));
        return;
    }
    if (!c.Vib) {
        if (!(c.Effects1 & VIBRATO)) return;
        if (c.VibCmdDelay) { c.VibCmdDelay--; return; }
    }
    if (!c.VibAtkLength) {
        c.VibCmdDepth = c.VibDepth;
    } else {
        c.VibCmdDepth = (uint16_t)(c.VibCmdDepth + c.VibAtkSpeed);
        if (--c.VibAtkLength == 0) c.VibCmdDepth = c.VibDepth;
    }
    int d3 = lfoWave(c.VibWaveNum)[(c.VibCount >> 2) & 127];
    if (!c.VibDir) d3 = -d3;
    d3 = (d3 * (int16_t)(c.VibCmdDepth >> 8)) >> 4;
    int16_t d = w(d3);
    if (d3 < 0) d = w(d + 1);
    c.VibNote = d;
    c.VibCount = (uint16_t)((c.VibCount + c.VibCmdSpeed) & 0x1ff);
}

// ---------------------------------------------------------------- the effects on the wave

static void count(Counter &k) {
    int16_t d0 = k.counter;
    if (!k.step && k.delay) { k.delay--; return; }
    if (k.turns < 0) return;
    d0 = w(d0 + k.speed);
    bool ok;
    if (k.repeat < k.repeatend) ok = k.speed < 0 ? d0 >= (int16_t)k.repeat : d0 <= (int16_t)k.repeatend;
    else ok = k.speed < 0 ? d0 >= (int16_t)k.repeatend : d0 <= (int16_t)k.repeat;
    if (!ok) {
        if (k.turns && --k.turns == 0) k.turns = -1;
        d0 = w(d0 - k.speed);
        k.speed = w(-k.speed);
    }
    k.counter = d0;
}

static void countOneWay(Counter &k) {
    if (!k.step && k.delay) { k.delay--; return; }
    k.counter = (int16_t)((k.counter + k.speed) & 0x1ff);
}

// The start of all five: move the counter, and give the value the wave is made with.
// An effect that goes "in steps" only moves when a note is played.
bool Player::stepCounter(Chan &c, Counter &k, bool step, bool init, uint16_t spd, int lo, int hi,
                         bool phase, bool oneway, int16_t &d0) {
    bool doCount = true, save = true;
    if (step) {
        if (!init) { if (!c.PartNote) doCount = false; }
        else if (!c.PartNote) save = false;
    }
    if (doCount) {
        if (save) k.savecounter = k.counter;
        if (oneway) countOneWay(k); else count(k);
    }
    d0 = k.savecounter;
    if (step) {
        if (k.delay) {
            k.delay--;
        } else {
            int16_t d1 = (int8_t)(spd & 0xff);
            if (phase) {
                k.savecounter = w(k.savecounter - d1);
                if (d1 < 0) { if (k.savecounter > hi) k.savecounter = (int16_t)hi; }
                else if (k.savecounter < lo) k.savecounter = (int16_t)lo;
            } else {
                k.savecounter = w(k.savecounter + d1);
                if (d1 < 0) { if (k.savecounter < lo) k.savecounter = (int16_t)lo; }
                else if (k.savecounter > hi) k.savecounter = (int16_t)hi;
            }
        }
    }
    return true;
}

// the length of the wave in bytes, never more than the buffers of the channel hold
static unsigned waveBytes(const Chan &c) {
    unsigned n = (uint16_t)(c.WsRepLength * 2u);
    return n > 256 ? 256 : n;
}

void Player::TransformPlay(Chan &c) {
    if (!(c.Effects2 & TRANSFORM)) return;
    unsigned d6 = waveBytes(c);
    unsigned d4 = d6 == 256 ? 0 : d6 == 128 ? 256 : d6 == 64 ? 256 + 128 : d6 == 32 ? 256 + 128 + 64
                                                                                   : 256 + 128 + 64 + 32;
    int16_t d0;
    stepCounter(c, c.TraData, (c.EffectsPar1 & TRANSFORMSTEP) != 0, (c.EffectsPar1 & TRANSFORMINIT) != 0,
                c.TraSpd, 0, 510, false, false, d0);
    d0 = (int16_t)((uint16_t)d0 >> 1);
    int d3 = 0;                                     // between which two of the six waves
    for (int i = 0; i < 5; i++) {
        int16_t d2 = w(d0 - 256);
        if (d2 <= 0) break;
        d3++;
        d0 = d2;
    }
    uint8_t first = c.TraWsPtrs[d3];
    if (!first) return;
    Ptr a1 = mod.smpl(first).repPointer.plus(d4);
    if (d3 == 0) a1 = c.WsRepPointer;
    uint8_t second = d3 + 1 < 6 ? c.TraWsPtrs[d3 + 1] : 0;
    if (!second) return;
    Ptr a0 = mod.smpl(second).repPointer.plus(d4);
    if (mod.smpl(second).repPointer.null() || a1.null()) return;
    c.WsRepPointer = buffer(c.TraWaveBuffer);
    if ((c.Effects1 & LOOP) || c.WaveOrSample) c.WsPointer = c.WsRepPointer;
    for (unsigned i = 0; i < d6; i++) {
        int s1 = a1[i], s2 = a0[i];
        c.TraWaveBuffer[i] = (int8_t)(w((s2 - s1) * d0) >> 8) + s1;
    }
}

void Player::PhasePlay(Chan &c) {
    if (!(c.Effects2 & PHASE)) return;
    unsigned d6 = waveBytes(c);
    int16_t save;
    stepCounter(c, c.PhaData, (c.EffectsPar1 & PHASESTEP) != 0, (c.EffectsPar1 & PHASEINIT) != 0,
                c.PhaSpd, 2, 512, true, false, save);
    unsigned d0 = (uint16_t)save;
    const uint8_t *table;
    const uint16_t *offsets;
    unsigned entries, tableSize;
    if (d6 > 128) {
        d0 = ((d0 + 1) & 0xffff) >> 1; table = tab::SizerTable256; offsets = tab::SizerOffset256;
        entries = 255; tableSize = sizeof tab::SizerTable256;
    } else if (d6 > 64) {
        d0 = ((d0 + 3) & 0xffff) >> 2; table = tab::SizerTable128; offsets = tab::SizerOffset128;
        entries = 127; tableSize = sizeof tab::SizerTable128;
    } else if (d6 > 32) {
        d0 = ((d0 + 7) & 0xffff) >> 3; table = tab::SizerTable64; offsets = tab::SizerOffset64;
        entries = 63; tableSize = sizeof tab::SizerTable64;
    } else if (d6 > 16) {
        d0 = ((d0 + 15) & 0xffff) >> 4; table = tab::SizerTable32; offsets = tab::SizerOffset32;
        entries = 31; tableSize = sizeof tab::SizerTable32;
    } else {
        d0 = ((d0 + 31) & 0xffff) >> 5; table = tab::SizerTable16; offsets = tab::SizerOffset16;
        entries = 15; tableSize = sizeof tab::SizerTable16;
    }
    Ptr a1 = c.WsRepPointer;
    int8_t *a2 = c.PhaWaveBuffer;
    c.WsRepPointer = buffer(c.PhaWaveBuffer);
    if ((c.Effects1 & LOOP) || c.WaveOrSample) c.WsPointer = c.WsRepPointer;

    if (d0 >= d6 || d0 == 0 || d0 > entries) {      // nothing to squeeze
        for (unsigned i = 0; i < d6; i++) a2[i] = a1[i];
        return;
    }
    unsigned todo = d0, rest = d6 - d0;
    unsigned a3 = offsets[d0 - 1];
    auto sizer = [&](unsigned i) -> unsigned { return i < tableSize ? table[i] : 0; };
    bool fill = (c.EffectsPar1 & PHASEFILL) != 0;
    unsigned o = 0, in = 0, idx = 0;

    // The first part of the wave is the whole wave squeezed into it, mixed with the wave as
    // it was in three strengths. The rest is that again ("fill") or the last value of it.
    auto mixed = [&](int squeezed, int plain) -> int8_t {
        switch (c.PhaType) {
        case 1: return (int8_t)(w(squeezed * 3 + plain) >> 2);      // high
        case 2: return (int8_t)(w(squeezed + plain) >> 1);          // medium
        case 3: return (int8_t)(w(squeezed + plain * 3) >> 2);      // low
        }
        return (int8_t)squeezed;
    };
    if (c.PhaType < 1 || c.PhaType > 3) {
        for (unsigned i = 0; i < todo; i++) { idx = sizer(a3++); a2[o++] = a1[idx]; }
        if (fill) {
            for (unsigned i = 0; i < rest; i++) { a2[o] = a2[i]; o++; }
        } else {
            int8_t last = a1[idx];
            for (unsigned i = 0; i < rest; i++) a2[o++] = last;
        }
        return;
    }
    unsigned start = a3;
    for (unsigned i = 0; i < todo; i++) { idx = sizer(a3++); a2[o++] = mixed(a1[idx], a1[in++]); }
    if (fill) {
        // medium starts anew with every round, high and low go on in the table
        if (c.PhaType != 2) a3 = start;
        while (rest) {
            if (c.PhaType == 2) a3 = start;
            unsigned n = todo < rest ? todo : rest;
            for (unsigned i = 0; i < n; i++) { idx = sizer(a3++); a2[o++] = mixed(a1[idx], a1[in++]); }
            rest -= n;
        }
    } else {
        int last = a1[idx];
        for (unsigned i = 0; i < rest; i++) a2[o++] = mixed(last, a1[in++]);
    }
}

void Player::MixPlay(Chan &c) {
    if (!(c.Effects2 & MIX)) return;
    unsigned d7 = waveBytes(c);
    int16_t save;
    stepCounter(c, c.MixData, (c.EffectsPar2 & MIXSTEP) != 0, (c.EffectsPar2 & MIXINIT) != 0, c.MixSpd,
                0, 510, false, (c.EffectsPar2 & MIXCOUNTER) != 0, save);
    unsigned d0 = (uint16_t)save, d4;
    if (d7 > 128) { d0 >>= 1; d4 = 0; }
    else if (d7 > 64) { d0 >>= 2; d4 = 256; }
    else if (d7 > 32) { d0 >>= 3; d4 = 256 + 128; }
    else if (d7 > 16) { d0 >>= 4; d4 = 256 + 128 + 64; }
    else { d0 >>= 5; d4 = 256 + 128 + 64 + 32; }

    Ptr a0 = c.WsRepPointer;
    Ptr a1 = buffer(c.MixWaveBuffer);               // with itself, as it was made the last time
    if (!(c.EffectsPar2 & MIXBUFF)) {
        a1 = a0;
        if (c.MixWaveNum) {
            a1 = mod.smpl(c.MixWaveNum).repPointer;
            if (a1.null()) a1 = zeroSample();
            else a1 = a1.plus(d4);
        }
    }
    int8_t *a2 = c.MixWaveBuffer;
    c.WsRepPointer = buffer(c.MixWaveBuffer);
    if (c.WaveOrSample) c.WsPointer = c.WsRepPointer;
    if (d0 > d7) d0 = d7;
    int shift = (c.MixResFilBoost & 4) ? 0 : 1;
    unsigned o = 0;
    // the second wave is turned by d0 bytes
    for (unsigned i = 0; i < d7 - d0; i++, o++) a2[o] = (int8_t)(w(a0[o] + a1[d0 + i]) >> shift);
    for (unsigned i = 0; i < d0; i++, o++) a2[o] = (int8_t)(w(a0[o] + a1[i]) >> shift);
}

void Player::ResonancePlay(Chan &c) {
    if (!(c.Effects2 & RESONANCE)) return;
    int16_t save;
    stepCounter(c, c.ResData, (c.EffectsPar2 & RESONANCESTEP) != 0, (c.EffectsPar2 & RESONANCEINIT) != 0,
                c.ResSpd, 0, 510, false, false, save);
    Ptr a0 = c.WsRepPointer;
    int8_t *a1 = c.ResWaveBuffer;
    c.WsRepPointer = buffer(c.ResWaveBuffer);
    if ((c.Effects1 & LOOP) || c.WaveOrSample) c.WsPointer = c.WsRepPointer;
    unsigned n = waveBytes(c);
    if (!n) return;

    int8_t last = c.ResLastSample;
    if (c.ResLastInit) {
        c.ResLastInit = 0;
        last = (int8_t)(a0[n - 1] >> 2);
    }
    int16_t d4 = w(last << 7);
    int16_t d5 = (int16_t)tab::ResonanceList[((uint16_t)save & 0x1fe) >> 1];
    uint16_t d2 = (uint16_t)(0x8000 - tab::ResonanceAmpList[c.ResAmp]);
    d2 = (uint16_t)(((uint32_t)d2 * 0xe666u) >> 16);
    int16_t d3 = 0;
    int shift = (c.MixResFilBoost & 2) ? 6 : 7;
    int16_t d6 = 0;
    for (unsigned i = 0; i < n; i++) {
        d6 = w(w(a0[i] << 5) - d4);
        int32_t l = (int32_t)d6 * 128;
        int32_t q = d5 ? l / d5 : 0;
        // DIVS leaves its operand alone when the result doesn't fit in 16 bits
        d6 = (q < -32768 || q > 32767) ? w(l & 0xffff) : w(q);
        d3 = w(d3 + d6);
        d4 = w(d4 + d3);
        d6 = (int16_t)(d4 >> shift);
        a1[i] = (int8_t)d6;
        d3 = (int16_t)(((int32_t)d3 * (int16_t)d2 * 2) >> 16);
    }
    c.ResLastSample = (int8_t)d6;
}

void Player::FilterPlay(Chan &c) {
    if (!(c.Effects2 & FILTER)) return;
    int16_t save;
    stepCounter(c, c.FilData, (c.EffectsPar1 & FILTERSTEP) != 0, (c.EffectsPar1 & FILTERINIT) != 0,
                c.FilSpd, 0, 510, false, false, save);
    Ptr a0 = c.WsRepPointer;
    int8_t *a1 = c.FilWaveBuffer;
    c.WsRepPointer = buffer(c.FilWaveBuffer);
    if ((c.Effects1 & LOOP) || c.WaveOrSample) c.WsPointer = c.WsRepPointer;
    unsigned n = waveBytes(c);
    if (!n) return;

    bool resonant = c.FilType != 0;
    int8_t last = c.FilLastSample;
    if (c.FilLastInit) {
        c.FilLastInit = 0;
        last = a0[n - 1];
        if (resonant) last = (int8_t)(last >> 1);
    }
    int16_t d4 = w(last << 7);
    unsigned index = ((uint16_t)save & 0x1fe) >> 1;
    uint16_t d1 = resonant ? tab::ResFilterList[index] : tab::FilterList[index];
    uint16_t d2 = (uint16_t)(0x8000 - d1);
    d1 >>= 1;
    if (resonant) d2 = (uint16_t)(((uint32_t)d2 * 0xe666u) >> 16);
    else d2 = (uint16_t)(((int32_t)(int16_t)d2 * -4096) >> 16);
    int16_t d3 = 0;
    int shift = (c.MixResFilBoost & 1) ? 6 : 7;
    int in = resonant ? 6 : 7;
    int16_t d6 = 0;
    for (unsigned i = 0; i < n; i++) {
        d6 = w(w(a0[i] << in) - d4);
        d6 = (int16_t)(((int32_t)d6 * (int16_t)d1 * 4) >> 16);
        d3 = w(d3 + d6);
        d4 = w(d4 + d3);
        d6 = (int16_t)(d4 >> shift);
        a1[i] = (int8_t)d6;
        d3 = (int16_t)(((int32_t)d3 * (int16_t)d2 * 2) >> 16);
    }
    c.FilLastSample = (int8_t)d6;
}

}  // namespace mline
