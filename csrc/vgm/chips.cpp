// SPDX-License-Identifier: GPL-2.0-or-later
// bbmp-vgm: starting the sound chips' cores and passing them the songs' writes.
//
// The cores are libvgm's (its emu/cores folder), the licensed ones only. Most come with
// their own interface (EmuStructs.h: a DEV_DEF with Start, Update and a list of
// read/write functions); the few without one (MAME's OPN and OPL families, DOSBox's
// AdLibEmu, NSFPlay's NES cores) are driven through the functions their headers declare.
// Which core plays which chip, and each chip's share of the mix, were chosen to sound as
// the player bbmp used before (libvgm's) did.
#include "vgmplay.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include "emu/EmuCores.h"
#include "emu/cores/sn764intf.h"
#include "emu/cores/sn76496.h"
#include "emu/cores/ayintf.h"
#include "emu/cores/emu2149.h"
#include "emu/cores/emu2413.h"
#include "emu/cores/ym2151.h"
#include "emu/cores/segapcm.h"
#include "emu/cores/rf5c68.h"
#include "emu/cores/fmopn.h"
#include "emu/cores/fmopl.h"
#define OPLTYPE_IS_OPL2
#include "emu/cores/adlibemu.h"
#undef OPLTYPE_IS_OPL2
// the OPL3 build of the same core (its header can be included only once)
void* adlib_OPL3_init(UINT32 clock, UINT32 samplerate);
void adlib_OPL3_stop(void* chip);
void adlib_OPL3_reset(void* chip);
void adlib_OPL3_writeIO(void* chip, UINT8 addr, UINT8 val);
void adlib_OPL3_getsample(void* chip, UINT32 numsamples, DEV_SMPL** sndptr);
void adlib_OPL3_set_mute_mask(void* chip, UINT32 MuteMask);
void adlib_OPL3_set_volume_lr(void* chip, INT32 volL, INT32 volR);
#include "emu/cores/ymf278b.h"
#include "emu/cores/ymf271.h"
#include "emu/cores/ymz280b.h"
#include "emu/cores/pwm.h"
#include "emu/cores/gb.h"
#include "emu/cores/np_nes_apu.h"
#include "emu/cores/np_nes_dmc.h"
#include "emu/cores/np_nes_fds.h"
#include "emu/cores/multipcm.h"
#include "emu/cores/upd7759.h"
#include "emu/cores/okim6258.h"
#include "emu/cores/okim6295.h"
#include "emu/cores/k051649.h"
#include "emu/cores/k054539.h"
#include "emu/cores/c6280_mame.h"
#include "emu/cores/c140.h"
#include "emu/cores/c219.h"
#include "emu/cores/k053260.h"
#include "emu/cores/pokey.h"
#include "emu/cores/qsound_mame.h"
#include "emu/cores/scsp.h"
#include "emu/cores/vsu.h"
#include "emu/cores/saa1099_mame.h"
#include "emu/cores/es5503.h"
#include "emu/cores/x1_010.h"
#include "emu/cores/c352.h"
#include "emu/cores/iremga20.h"
#include "emu/cores/mikey.h"
}

namespace vgm {

static const char* const NAMES[CHIP_TYPES] = {
    "SN76489", "YM2413", "YM2612", "YM2151", "SegaPCM", "RF5C68", "YM2203", "YM2608",
    "YM2610", "YM3812", "YM3526", "Y8950", "YMF262", "YMF278B", "YMF271", "YMZ280B",
    "RF5C164", "PWM", "AY8910", "GameBoy DMG", "NES APU", "MultiPCM", "uPD7759",
    "OKIM6258", "OKIM6295", "K051649", "K054539", "HuC6280", "C140", "K053260", "Pokey",
    "QSound", "SCSP", "WonderSwan", "VSU", "SAA1099", "ES5503", "ES5506", "X1-010",
    "C352", "GA20", "Mikey"};

const char* chipName(int type)
{
    return type >= 0 && type < CHIP_TYPES ? NAMES[type] : "?";
}

Chip::~Chip() = default;

// Each chip's share of the mix and its loudness at that share (see Mixer::balance), as
// measured on the player bbmp used before. A second chip of a kind halves the share of
// both.
struct Share { int32_t weight, factor; };
static Share chipShare(int type, bool paired)
{
    if (paired)     // the SSG of a YM2203/YM2608/YM2610
        return {0x80, 8};
    switch (type) {
    case SN76489: return {0x80, 8};
    case YM2413: return {0x100, 16};
    case YM2608: case YM2610: return {0x80, 8};
    case YM3812: case YM3526: case Y8950: case YMF262: return {0x200, 4};
    case AY8910: return {0x200, 4};
    case GB_DMG: return {0x300, 2};
    case NES_APU: return {0x80, 4};
    case OKIM6258: return {0x380, 4};
    case SEGAPCM: case UPD7759: case OKIM6295: case ES5503: case C352: case MIKEY: case YMF278B:
        return {0x180, 8};
    case GA20: return {0x240, 8};
    default: return {0x100, 8};
    }
}

static void setShare(Voice& v, int type, bool paired, bool dual)
{
    Share s = chipShare(type, paired);
    v.weight = dual ? s.weight / 2 : s.weight;
    v.factor = s.factor;
    v.paired = paired;
}

static void* findFunc(const DEV_DEF* def, uint8_t funcType, uint8_t rwType, uint16_t user)
{
    for (const DEVDEF_RWFUNC* f = def->rwFuncs; f && f->funcPtr; f++)
        if (f->funcType == funcType && f->rwType == rwType && (user == 0 || f->user == user))
            return f->funcPtr;
    return nullptr;
}

// ---------------------------------------------------------------------------------
// cores with their own DEV_DEF

class DefChip : public Chip {
public:
    enum Mode { ADDR_DATA, DIRECT, A16D8, A16D16, A8D16, SN, SAA, QSOUND, PORTREG, SCC };
    DefChip(const DEV_DEF* def, Mode mode) : def_(def), mode_(mode) {}
    ~DefChip() override
    {
        if (started_ && def_->Stop)
            def_->Stop(info_.dataPtr);
        for (UINT32 i = 0; info_.linkDevs && i < info_.linkDevCount; i++)
            free(info_.linkDevs[i].cfg);
        free(info_.linkDevs);
    }
    bool start(const DEV_GEN_CFG* cfg)
    {
        memset(&info_, 0, sizeof info_);
        if (def_->Start(cfg, &info_) != 0)
            return false;
        started_ = true;
        if (def_->SetMuteMask)
            def_->SetMuteMask(info_.dataPtr, 0);
        w8_ = (DEVFUNC_WRITE_A8D8)findFunc(def_, RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0);
        w16_8_ = (DEVFUNC_WRITE_A16D8)findFunc(def_, RWF_REGISTER | RWF_WRITE, DEVRW_A16D8, 0);
        w16_16_ = (DEVFUNC_WRITE_A16D16)findFunc(def_, RWF_REGISTER | RWF_WRITE, DEVRW_A16D16, 0);
        w8_16_ = (DEVFUNC_WRITE_A8D16)findFunc(def_, RWF_REGISTER | RWF_WRITE, DEVRW_A8D16, 0);
        if (!w8_16_)
            w8_16_ = (DEVFUNC_WRITE_A8D16)findFunc(def_, RWF_REGISTER | RWF_QUICKWRITE, DEVRW_A8D16, 0);
        memW_ = (DEVFUNC_WRITE_A16D8)findFunc(def_, RWF_MEMORY | RWF_WRITE, DEVRW_A16D8, 0);
        voice_.chip = info_.dataPtr;
        voice_.update = def_->Update;
        voice_.rate = info_.sampleRate;
        voices.push_back(&voice_);
        if (def_->SetSRateChgCB)
            def_->SetSRateChgCB(info_.dataPtr, rateChanged, &voice_);
        if (def_->Reset)
            def_->Reset(info_.dataPtr);
        return true;
    }
    void reset() override
    {
        if (def_->Reset)
            def_->Reset(info_.dataPtr);
    }
    void write(uint8_t port, uint16_t reg, uint16_t data) override
    {
        void* c = info_.dataPtr;
        switch (mode_) {
        case ADDR_DATA:
            if (w8_) { w8_(c, (uint8_t)(port * 2), (uint8_t)reg); w8_(c, (uint8_t)(port * 2 + 1), (uint8_t)data); }
            break;
        case PORTREG:
            if (w8_) { w8_(c, (uint8_t)(port << 1), (uint8_t)reg); w8_(c, (uint8_t)((port << 1) | 1), (uint8_t)data); }
            break;
        case SCC:
            // the core trusts the register numbers: only those of its five channels
            if ((port == 0 || port == 4) && reg >= (plus_ ? 0xA0 : 0x100)) break;
            if (port == 1 && reg >= 10) break;
            if (port == 2 && reg >= 5) break;
            if (port > 5) break;
            if (w8_) { w8_(c, (uint8_t)(port << 1), (uint8_t)reg); w8_(c, (uint8_t)((port << 1) | 1), (uint8_t)data); }
            break;
        case DIRECT:
            if (w8_) w8_(c, (uint8_t)reg, (uint8_t)data);
            break;
        case A16D8:
            if (w16_8_) w16_8_(c, reg, (uint8_t)data);
            break;
        case A16D16:
            if (w16_16_) w16_16_(c, reg, data);
            break;
        case A8D16:
            if (w8_16_) w8_16_(c, (uint8_t)reg, data);
            break;
        case SN:
            if (w8_) w8_(c, port ? SN76496_W_GGST : SN76496_W_REG, (uint8_t)data);
            break;
        case SAA:
            if (w8_) { w8_(c, 1, (uint8_t)reg); w8_(c, 0, (uint8_t)data); }
            break;
        case QSOUND:
            if (w8_) { w8_(c, 0, (uint8_t)(data >> 8)); w8_(c, 1, (uint8_t)data); w8_(c, 2, (uint8_t)reg); }
            break;
        }
    }
    void writeMem(uint32_t offset, uint8_t data) override
    {
        if (memW_)
            memW_(info_.dataPtr, (uint16_t)offset, data);
    }
    void romSize(uint16_t kind, uint32_t size) override
    {
        auto f = (DEVFUNC_WRITE_MEMSIZE)findFunc(def_, RWF_MEMORY | RWF_WRITE, DEVRW_MEMSIZE, kind ? kind : 0);
        if (f)
            f(info_.dataPtr, size);
    }
    void romData(uint16_t kind, uint32_t offset, uint32_t length, const uint8_t* data) override
    {
        auto f = (DEVFUNC_WRITE_BLOCK)findFunc(def_, RWF_MEMORY | RWF_WRITE, DEVRW_BLOCK, kind ? kind : 0);
        if (f)
            f(info_.dataPtr, offset, length, data);
    }
    void setStereo(uint8_t mask) override
    {
        auto f = (DEVFUNC_OPTMASK)findFunc(def_, RWF_REGISTER | RWF_WRITE, DEVRW_ALL, 0x5354);
        if (f)
            f(info_.dataPtr, mask);
    }
    static void rateChanged(void* p, UINT32 rate)
    {
        Voice* v = (Voice*)p;
        if (rate && rate != v->rate)
            v->newRate = rate;
    }
    void* data() { return info_.dataPtr; }
    DEV_INFO& info() { return info_; }
    const DEV_DEF* def() const { return def_; }
    Voice voice_;
    bool plus_ = false;     // a K052539 (SCC+)
private:
    const DEV_DEF* def_;
    Mode mode_;
    DEV_INFO info_{};
    bool started_ = false;
    DEVFUNC_WRITE_A8D8 w8_ = nullptr;
    DEVFUNC_WRITE_A16D8 w16_8_ = nullptr;
    DEVFUNC_WRITE_A16D16 w16_16_ = nullptr;
    DEVFUNC_WRITE_A8D16 w8_16_ = nullptr;
    DEVFUNC_WRITE_A16D8 memW_ = nullptr;
};

static const DEV_DEF* firstCore(const DEV_DECL& decl)
{
    return decl.cores[0];
}

// ---------------------------------------------------------------------------------
// the AY8910 (also the SSG of the OPN family)

static DEV_DEF* ayCore()
{
    return &devDef_YM2149_Emu;     // EMU2149, as the earlier player used
}

// ---------------------------------------------------------------------------------
// MAME's OPN family (YM2203, YM2608, YM2610, YM2612) with an SSG for the first three

class OpnChip : public Chip {
public:
    ~OpnChip() override
    {
        if (fm_) {
            switch (type) {
            case YM2203: ym2203_shutdown(fm_); break;
            case YM2608: ym2608_shutdown(fm_); break;
            case YM2610: ym2610_shutdown(fm_); break;
            case YM2612: ym2612_shutdown(fm_); break;
            }
        }
    }
    bool start(const ChipSetup& s)
    {
        type = s.type;
        clock = s.clock;
        uint32_t pres = (type == YM2203) ? 72 : 144;
        uint32_t rate = clock / pres;
        switch (type) {
        case YM2203: fm_ = ym2203_init(this, clock, rate, nullptr, nullptr); break;
        case YM2608: fm_ = ym2608_init(this, clock, rate, nullptr, nullptr); break;
        case YM2610: fm_ = ym2610_init(this, clock, rate, nullptr, nullptr); break;
        case YM2612: fm_ = ym2612_init(this, clock, rate, nullptr, nullptr); break;
        }
        if (!fm_)
            return false;
        fmVoice_.chip = fm_;
        fmVoice_.rate = rate;
        switch (type) {
        case YM2203: fmVoice_.update = ym2203_update_one; break;
        case YM2608: fmVoice_.update = ym2608_update_one; break;
        case YM2610: fmVoice_.update = s.variant ? ym2610b_update_one : ym2610_update_one; break;
        case YM2612: fmVoice_.update = ym2612_update_one; break;
        }
        voices.push_back(&fmVoice_);
        if (type != YM2612) {
            AY8910_CFG cfg;
            memset(&cfg, 0, sizeof cfg);
            // the SSG's clock after a reset: the YM2203's halved, the others' quartered
            cfg._genCfg.clock = type == YM2203 ? clock / 2 : clock / 4;
            cfg._genCfg.smplRate = 48000;
            cfg.chipType = type == YM2203 ? AYTYPE_YM2203 : type == YM2608 ? AYTYPE_YM2608 : AYTYPE_YM2610;
            cfg.chipFlags = s.flags[0];
            ssg_.reset(new DefChip(ayCore(), DefChip::ADDR_DATA));
            if (!ssg_->start(&cfg._genCfg))
                ssg_.reset();
        }
        if (ssg_) {
            static const ssg_callbacks cb = {ssgClock, ssgWrite, ssgRead, ssgReset};
            switch (type) {
            case YM2203: ym2203_link_ssg(fm_, &cb, this); break;
            case YM2608: ym2608_link_ssg(fm_, &cb, this); break;
            case YM2610: ym2610_link_ssg(fm_, &cb, this); break;
            }
            voices.push_back(&ssg_->voice_);
        }
        switch (type) {
        case YM2203: ym2203_set_srchg_cb(fm_, rateChanged, this); break;
        case YM2608: ym2608_set_srchg_cb(fm_, rateChanged, this); break;
        default: break;
        }
        reset();
        return true;
    }
    void reset() override
    {
        switch (type) {
        case YM2203: ym2203_reset_chip(fm_); break;
        case YM2608: ym2608_reset_chip(fm_); break;
        case YM2610: ym2610_reset_chip(fm_); break;
        case YM2612: ym2612_reset_chip(fm_); break;
        }
    }
    void write(uint8_t port, uint16_t reg, uint16_t data) override
    {
        uint8_t a = (uint8_t)(port * 2);
        switch (type) {
        case YM2203: ym2203_write(fm_, 0, (uint8_t)reg); ym2203_write(fm_, 1, (uint8_t)data); break;
        case YM2608: ym2608_write(fm_, a, (uint8_t)reg); ym2608_write(fm_, a + 1, (uint8_t)data); break;
        case YM2610: ym2610_write(fm_, a, (uint8_t)reg); ym2610_write(fm_, a + 1, (uint8_t)data); break;
        case YM2612: ym2612_write(fm_, a, (uint8_t)reg); ym2612_write(fm_, a + 1, (uint8_t)data); break;
        }
    }
    void romSize(uint16_t kind, uint32_t size) override
    {
        if (type == YM2608) ym2608_alloc_pcmromb(fm_, size);
        else if (type == YM2610) {
            if (kind == 1) ym2610_alloc_pcmroma(fm_, size); else ym2610_alloc_pcmromb(fm_, size);
        }
    }
    void romData(uint16_t kind, uint32_t offset, uint32_t length, const uint8_t* data) override
    {
        if (type == YM2608) ym2608_write_pcmromb(fm_, offset, length, data);
        else if (type == YM2610) {
            if (kind == 1) ym2610_write_pcmroma(fm_, offset, length, data); else ym2610_write_pcmromb(fm_, offset, length, data);
        }
    }
    void setStereo(uint8_t mask) override { if (ssg_) ssg_->setStereo(mask); }
private:
    static void ssgClock(void* p, UINT32 clk)
    {
        OpnChip* self = (OpnChip*)p;
        auto f = (DEVFUNC_WRITE_CLOCK)findFunc(self->ssg_->def(), RWF_CLOCK | RWF_WRITE, DEVRW_VALUE, 0);
        if (f)
            f(self->ssg_->data(), clk);
        auto r = (DEVFUNC_READ_SRATE)findFunc(self->ssg_->def(), RWF_SRATE | RWF_READ, DEVRW_VALUE, 0);
        if (r) {
            uint32_t nr = r(self->ssg_->data());
            if (nr && nr != self->ssg_->voice_.rate)
                self->ssg_->voice_.newRate = nr;
        }
    }
    static void ssgWrite(void* p, UINT8 a, UINT8 d)
    {
        OpnChip* self = (OpnChip*)p;
        auto f = (DEVFUNC_WRITE_A8D8)findFunc(self->ssg_->def(), RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0);
        if (f)
            f(self->ssg_->data(), a, d);
    }
    static UINT8 ssgRead(void* p, UINT8 a)
    {
        OpnChip* self = (OpnChip*)p;
        auto f = (DEVFUNC_READ_A8D8)findFunc(self->ssg_->def(), RWF_REGISTER | RWF_READ, DEVRW_A8D8, 0);
        return f ? f(self->ssg_->data(), a) : 0;
    }
    static void ssgReset(void* p)
    {
        OpnChip* self = (OpnChip*)p;
        if (self->ssg_->def()->Reset)
            self->ssg_->def()->Reset(self->ssg_->data());
    }
    static void rateChanged(void* p, UINT32 rate)
    {
        OpnChip* self = (OpnChip*)p;
        if (rate && rate != self->fmVoice_.rate)
            self->fmVoice_.newRate = rate;
    }
    void* fm_ = nullptr;
    Voice fmVoice_;
    std::unique_ptr<DefChip> ssg_;
};

// ---------------------------------------------------------------------------------
// the OPL family: the YM3812 by AdLibEmu, the YM3526 and Y8950 by MAME's fmopl

class OplChip : public Chip {
public:
    ~OplChip() override
    {
        if (chip_) {
            switch (type) {
            case YM3812: adlib_OPL2_stop(chip_); break;
            case YM3526: ym3526_shutdown(chip_); break;
            case Y8950: y8950_shutdown(chip_); break;
            }
        }
    }
    bool start(const ChipSetup& s)
    {
        type = s.type;
        clock = s.clock;
        uint32_t rate = clock / 72;
        switch (type) {
        case YM3812: chip_ = adlib_OPL2_init(clock, rate); voice_.update = adlib_OPL2_getsample; break;
        case YM3526: chip_ = ym3526_init(clock, rate); voice_.update = ym3526_update_one; break;
        case Y8950: chip_ = y8950_init(clock, rate); voice_.update = y8950_update_one; break;
        }
        if (!chip_)
            return false;
        voice_.chip = chip_;
        voice_.rate = rate;
        voices.push_back(&voice_);
        reset();
        return true;
    }
    void reset() override
    {
        switch (type) {
        case YM3812: adlib_OPL2_reset(chip_); break;
        case YM3526: ym3526_reset_chip(chip_); break;
        case Y8950: y8950_reset_chip(chip_); break;
        }
    }
    void write(uint8_t, uint16_t reg, uint16_t data) override
    {
        switch (type) {
        case YM3812: adlib_OPL2_writeIO(chip_, 0, (uint8_t)reg); adlib_OPL2_writeIO(chip_, 1, (uint8_t)data); break;
        case YM3526: ym3526_write(chip_, 0, (uint8_t)reg); ym3526_write(chip_, 1, (uint8_t)data); break;
        case Y8950: y8950_write(chip_, 0, (uint8_t)reg); y8950_write(chip_, 1, (uint8_t)data); break;
        }
    }
    void romSize(uint16_t, uint32_t size) override { if (type == Y8950) y8950_alloc_pcmrom(chip_, size); }
    void romData(uint16_t, uint32_t offset, uint32_t length, const uint8_t* data) override
    {
        if (type == Y8950) y8950_write_pcmrom(chip_, offset, length, data);
    }
private:
    void* chip_ = nullptr;
    Voice voice_;
};

// ---------------------------------------------------------------------------------
// the OPL3 (YMF262) by AdLibEmu, also the FM part of the YMF278B: as a DEV_DEF of our
// own, so that the YMF278B core can be linked to it

static UINT8 opl3Start(const DEV_GEN_CFG* cfg, DEV_INFO* info);
static void opl3Stop(void* c) { adlib_OPL3_stop(c); }
static void opl3Reset(void* c) { adlib_OPL3_reset(c); }
static void opl3Update(void* c, UINT32 n, DEV_SMPL** o) { adlib_OPL3_getsample(c, n, o); }
static void opl3Write(void* c, UINT8 a, UINT8 d) { adlib_OPL3_writeIO(c, a, d); }
static void opl3VolLR(void* c, INT32 l, INT32 r) { adlib_OPL3_set_volume_lr(c, l, r); }
static void opl3Mute(void* c, UINT32 m) { adlib_OPL3_set_mute_mask(c, m); }
static DEVDEF_RWFUNC opl3Funcs[] = {
    {RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, (void*)opl3Write},
    {RWF_VOLUME_LR | RWF_WRITE, DEVRW_VALUE, 0, (void*)opl3VolLR},
    {0, 0, 0, nullptr}};
static DEV_DEF opl3Def = {"YMF262", "AdLibEmu", FCC_ADLE, opl3Start, opl3Stop, opl3Reset, opl3Update,
                          nullptr, opl3Mute, nullptr, nullptr, nullptr, nullptr, opl3Funcs};
static UINT8 opl3Start(const DEV_GEN_CFG* cfg, DEV_INFO* info)
{
    uint32_t rate = cfg->clock / 288;
    void* c = adlib_OPL3_init(cfg->clock, rate);
    if (!c)
        return 0xFF;
    memset(info, 0, sizeof *info);
    info->dataPtr = (DEV_DATA*)c;
    info->sampleRate = rate;
    info->devDef = &opl3Def;
    return 0;
}

// ---------------------------------------------------------------------------------
// the YMF278B (OPL4): its wave part, and an OPL3 linked to it for the FM part

class Opl4Chip : public DefChip {
public:
    Opl4Chip() : DefChip(firstCore(sndDev_YMF278B), PORTREG) {}
    ~Opl4Chip() override
    {
        if (fm_.dataPtr)
            opl3Def.Stop(fm_.dataPtr);
    }
    bool startAll(const DEV_GEN_CFG* cfg)
    {
        if (!start(cfg))
            return false;
        DEV_INFO& in = info();
        if (in.linkDevCount < 1 || !in.linkDevs || !in.linkDevs[0].cfg || !def()->LinkDevice)
            return true;
        if (opl3Def.Start(in.linkDevs[0].cfg, &fm_) != 0)
            return true;
        def()->LinkDevice(in.dataPtr, in.linkDevs[0].linkID, &fm_);
        fmVoice_.chip = fm_.dataPtr;
        fmVoice_.update = opl3Def.Update;
        fmVoice_.rate = fm_.sampleRate;
        voices.push_back(&fmVoice_);
        DefChip::reset();
        return true;
    }
    void reset() override
    {
        DefChip::reset();
    }
private:
    DEV_INFO fm_{};
    Voice fmVoice_;
};

// ---------------------------------------------------------------------------------
// the NES APU: NSFPlay's (its squares, and the triangle, noise and DPCM part that also
// drives the frame sequence), with NSFPlay's FDS when the header asks for it

class NesChip : public Chip {
public:
    ~NesChip() override
    {
        if (apu_) NES_APU_np_Destroy(apu_);
        if (dmc_) NES_DMC_np_Destroy(dmc_);
        if (fds_) NES_FDS_Destroy(fds_);
    }
    bool start(const ChipSetup& s)
    {
        type = s.type;
        clock = s.clock;
        rate_ = clock / 4;
        apu_ = NES_APU_np_Create(clock, rate_);
        dmc_ = NES_DMC_np_Create(clock, rate_);
        if (!apu_ || !dmc_)
            return false;
        ram_.assign(0x10000, 0);
        NES_DMC_np_SetAPU(dmc_, apu_);
        NES_DMC_np_SetMemory(dmc_, ram_.data());
        // NSFPlay's usual options: unmute on reset, the nonlinear mixer, phase refresh,
        // $4011 writes, the looped noise, random noise at reset, the muted ultrasonic triangle
        NES_APU_np_SetOption(apu_, 0, 1);
        NES_APU_np_SetOption(apu_, 1, 1);
        NES_APU_np_SetOption(apu_, 2, 1);
        NES_DMC_np_SetOption(dmc_, 0, 1);
        NES_DMC_np_SetOption(dmc_, 1, 1);
        NES_DMC_np_SetOption(dmc_, 2, 1);
        NES_DMC_np_SetOption(dmc_, 3, 1);
        NES_DMC_np_SetOption(dmc_, 5, 1);
        NES_DMC_np_SetOption(dmc_, 6, 1);
        if (s.variant)
            fds_ = NES_FDS_Create(clock, rate_);
        voice_.chip = this;
        voice_.update = render;
        voice_.rate = rate_;
        voices.push_back(&voice_);
        reset();
        return true;
    }
    void reset() override
    {
        NES_APU_np_Reset(apu_);
        NES_DMC_np_Reset(dmc_);
        if (fds_) NES_FDS_Reset(fds_);
    }
    void write(uint8_t, uint16_t reg, uint16_t data) override
    {
        uint8_t r = (uint8_t)reg, d = (uint8_t)data;
        if (r < 0x20) {
            uint16_t adr = (uint16_t)(0x4000 + r);
            NES_APU_np_Write(apu_, adr, d);
            NES_DMC_np_Write(dmc_, adr, d);
        } else if (r < 0x3F) {
            if (fds_) NES_FDS_Write(fds_, (uint16_t)(0x4080 + (r - 0x20)), d);
        } else if (r == 0x3F) {
            if (fds_) NES_FDS_Write(fds_, 0x4023, d);
        } else if (r < 0x80) {
            if (fds_) NES_FDS_Write(fds_, (uint16_t)(0x4040 + (r - 0x40)), d);
        }
    }
    void romData(uint16_t, uint32_t offset, uint32_t length, const uint8_t* data) override
    {
        if (offset >= ram_.size()) return;
        if (length > ram_.size() - offset) length = (uint32_t)(ram_.size() - offset);
        memcpy(&ram_[offset], data, length);
    }
private:
    static void render(void* p, UINT32 n, DEV_SMPL** o)
    {
        NesChip* self = (NesChip*)p;
        INT32 a[2], b[2], f[2];
        for (UINT32 i = 0; i < n; i++) {
            NES_APU_np_Render(self->apu_, a);
            NES_DMC_np_Render(self->dmc_, b);
            o[0][i] = a[0] + b[0];
            o[1][i] = a[1] + b[1];
            if (self->fds_) {
                NES_FDS_Render(self->fds_, f);
                o[0][i] += f[0];
                o[1][i] += f[1];
            }
        }
    }
    void* apu_ = nullptr;
    void* dmc_ = nullptr;
    void* fds_ = nullptr;
    uint32_t rate_ = 0;
    std::vector<uint8_t> ram_;
    Voice voice_;
};

// ---------------------------------------------------------------------------------

static std::unique_ptr<Chip> makeChipRaw(const ChipSetup& s, uint32_t outRate)
{
    DEV_GEN_CFG gen;
    memset(&gen, 0, sizeof gen);
    gen.clock = s.clock;
    gen.smplRate = outRate;
    gen.srMode = DEVRI_SRMODE_NATIVE;
    auto plain = [&](const DEV_DEF* def, DefChip::Mode mode, const DEV_GEN_CFG* cfg) -> std::unique_ptr<Chip> {
        auto c = std::make_unique<DefChip>(def, mode);
        c->type = s.type;
        c->instance = s.instance;
        c->clock = s.clock;
        if (!c->start(cfg ? cfg : &gen))
            return nullptr;
        return c;
    };
    switch (s.type) {
    case SN76489: {
        SN76496_CFG cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg._genCfg = gen;
        uint16_t fb = (uint16_t)(s.flags[0] | (s.flags[1] << 8));
        uint8_t width = s.flags[2], fl = s.flags[3];
        cfg.noiseTaps = fb ? fb : 0x0009;
        cfg.shiftRegWidth = (width && width <= 32) ? width : 16;
        cfg.negate = (fl & 0x02) ? 1 : 0;
        cfg.stereo = (fl & 0x04) ? 0 : 1;
        cfg.clkDiv = (fl & 0x08) ? 1 : 8;
        cfg.ncrPSG = (fl & 0x10) ? 1 : 0;
        cfg.segaPSG = (fl & 0x01) ? 0 : 1;
        return plain(&devDef_SN76496_MAME, DefChip::SN, &cfg._genCfg);
    }
    case YM2413:
        return plain(&devDef_YM2413_Emu, DefChip::ADDR_DATA, nullptr);
    case YM2151:
        return plain(&devDef_YM2151_MAME, DefChip::ADDR_DATA, nullptr);
    case AY8910: {
        AY8910_CFG cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg._genCfg = gen;
        cfg.chipType = s.flags[0];
        cfg.chipFlags = s.flags[1];
        return plain(ayCore(), DefChip::ADDR_DATA, &cfg._genCfg);
    }
    case YM2203: case YM2608: case YM2610: case YM2612: {
        auto c = std::make_unique<OpnChip>();
        c->instance = s.instance;
        if (!c->start(s))
            return nullptr;
        return c;
    }
    case YM3812: case YM3526: case Y8950: {
        auto c = std::make_unique<OplChip>();
        c->instance = s.instance;
        if (!c->start(s))
            return nullptr;
        return c;
    }
    case YMF262:
        return plain(&opl3Def, DefChip::ADDR_DATA, nullptr);
    case YMF278B: {
        auto c = std::make_unique<Opl4Chip>();
        c->type = s.type;
        c->instance = s.instance;
        c->clock = s.clock;
        if (!c->startAll(&gen))
            return nullptr;
        return c;
    }
    case NES_APU: {
        auto c = std::make_unique<NesChip>();
        c->instance = s.instance;
        if (!c->start(s))
            return nullptr;
        return c;
    }
    case SEGAPCM: {
        SEGAPCM_CFG cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg._genCfg = gen;
        uint32_t intf = (uint32_t)s.flags[0] | ((uint32_t)s.flags[1] << 8) | ((uint32_t)s.flags[2] << 16) | ((uint32_t)s.flags[3] << 24);
        cfg.bnkshift = (uint8_t)(intf & 0x0F);
        cfg.bnkmask = (uint8_t)((intf >> 16) & 0xFF);
        return plain(firstCore(sndDev_SegaPCM), DefChip::A16D8, &cfg._genCfg);
    }
    case RF5C68: case RF5C164:
        return plain(&devDef_RF5C68_MAME, DefChip::DIRECT, nullptr);
    case YMF271:
        return plain(firstCore(sndDev_YMF271), DefChip::PORTREG, nullptr);
    case YMZ280B:
        return plain(firstCore(sndDev_YMZ280B), DefChip::ADDR_DATA, nullptr);
    case PWM:
        return plain(firstCore(sndDev_32X_PWM), DefChip::A8D16, nullptr);
    case GB_DMG:
        return plain(firstCore(sndDev_GB_DMG), DefChip::DIRECT, nullptr);
    case MULTIPCM:
        return plain(firstCore(sndDev_YMW258), DefChip::DIRECT, nullptr);
    case UPD7759: {
        DEV_GEN_CFG cfg = gen;
        cfg.flags = s.variant ? 1 : 0;
        return plain(firstCore(sndDev_uPD7759), DefChip::DIRECT, &cfg);
    }
    case OKIM6258: {
        MSM6258_CFG cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg._genCfg = gen;
        cfg.divider = s.flags[0] & 0x03;
        cfg.adpcmBits = (s.flags[0] & 0x04) ? 3 : 4;
        cfg.outputBits = (s.flags[0] & 0x08) ? 12 : 10;
        return plain(firstCore(sndDev_MSM6258), DefChip::DIRECT, &cfg._genCfg);
    }
    case OKIM6295: {
        DEV_GEN_CFG cfg = gen;
        cfg.flags = s.variant ? 1 : 0;
        return plain(firstCore(sndDev_MSM6295), DefChip::DIRECT, &cfg);
    }
    case K051649: {
        DEV_GEN_CFG cfg = gen;
        cfg.flags = s.variant ? 1 : 0;
        auto c = std::make_unique<DefChip>(firstCore(sndDev_K051649), DefChip::SCC);
        c->type = s.type;
        c->instance = s.instance;
        c->clock = s.clock;
        c->plus_ = s.variant;
        if (!c->start(&cfg))
            return nullptr;
        return c;
    }
    case K054539: {
        DEV_GEN_CFG cfg = gen;
        cfg.flags = s.flags[0];
        return plain(firstCore(sndDev_K054539), DefChip::A16D8, &cfg);
    }
    case HUC6280:
        return plain(&devDef_C6280_MAME, DefChip::DIRECT, nullptr);
    case C140: {
        DEV_GEN_CFG cfg = gen;
        if (s.flags[0] == 2)
            return plain(firstCore(sndDev_C219), DefChip::A16D8, &cfg);
        cfg.flags = s.flags[0];
        return plain(firstCore(sndDev_C140), DefChip::A16D8, &cfg);
    }
    case K053260:
        return plain(firstCore(sndDev_K053260), DefChip::DIRECT, nullptr);
    case POKEY:
        return plain(firstCore(sndDev_Pokey), DefChip::DIRECT, nullptr);
    case QSOUND:
        return plain(&devDef_QSound_MAME, DefChip::QSOUND, nullptr);
    case SCSP:
        return plain(firstCore(sndDev_SCSP), DefChip::A16D8, nullptr);
    case VSU:
        return plain(firstCore(sndDev_VBoyVSU), DefChip::A16D8, nullptr);
    case SAA1099:
        return plain(&devDef_SAA1099_MAME, DefChip::SAA, nullptr);
    case ES5503: {
        DEV_GEN_CFG cfg = gen;
        cfg.flags = s.flags[0] ? s.flags[0] : 1;
        return plain(firstCore(sndDev_ES5503), DefChip::DIRECT, &cfg);
    }
    case X1_010:
        return plain(firstCore(sndDev_X1_010), DefChip::A16D8, nullptr);
    case C352: {
        DEV_GEN_CFG cfg = gen;
        uint32_t div = (uint32_t)s.flags[0] * 4;
        if (div)
            cfg.clock = (uint32_t)((uint64_t)s.clock * 288 / div);
        return plain(firstCore(sndDev_C352), DefChip::A16D16, &cfg);
    }
    case GA20:
        return plain(firstCore(sndDev_GA20), DefChip::DIRECT, nullptr);
    case MIKEY:
        return plain(firstCore(sndDev_Mikey), DefChip::DIRECT, nullptr);
    default:
        return nullptr;     // WonderSwan, ES5506: no core with a licence here
    }
}

std::unique_ptr<Chip> makeChip(const ChipSetup& s, uint32_t outRate)
{
    auto c = makeChipRaw(s, outRate);
    if (c)
        for (size_t i = 0; i < c->voices.size(); i++)
            setShare(*c->voices[i], s.type, i > 0, s.dual);
    return c;
}

}  // namespace vgm
