// SPDX-License-Identifier: GPL-2.0-or-later
// bbmp-vgm: a player of VGM and S98 files for the ByteBandit Music Player.
// Written for bbmp from the published format documents (the VGM 1.72 and GD3 1.00
// specifications of vgmrips.net, the S98 v1/v2/v3 specifications); the sound chips are
// emulated by the licensed cores of libvgm's emu/cores folder, driven through their own
// interface (EmuStructs.h).
#ifndef BBMP_VGMPLAY_H
#define BBMP_VGMPLAY_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "stdtype.h"
#include "emu/EmuStructs.h"
#include "emu/SoundDevs.h"
}

namespace vgm {

// The chips a VGM header names, in the order of their clocks in the header (the VGM
// specification numbers chips this way in the extra header and in the DAC stream commands).
enum ChipType {
    SN76489 = 0, YM2413, YM2612, YM2151, SEGAPCM, RF5C68, YM2203, YM2608, YM2610, YM3812,
    YM3526, Y8950, YMF262, YMF278B, YMF271, YMZ280B, RF5C164, PWM, AY8910, GB_DMG,
    NES_APU, MULTIPCM, UPD7759, OKIM6258, OKIM6295, K051649, K054539, HUC6280, C140,
    K053260, POKEY, QSOUND, SCSP, WSWAN, VSU, SAA1099, ES5503, ES5506, X1_010, C352,
    GA20, MIKEY, CHIP_TYPES
};

const char* chipName(int type);

// One sound producer: a core's output at its own rate, brought to the output rate (see
// mixer.cpp).
struct Voice {
    void* chip = nullptr;            // the core's data (DEV_DATA*)
    DEVFUNC_UPDATE update = nullptr;
    uint32_t rate = 0;               // the core's sample rate
    int32_t volume = 0x100;          // gain, 8.8 fixed point (set by the Mixer)
    int32_t weight = 0;              // the chip's share of the mix
    int32_t factor = 8;              // the chip's loudness at that share
    bool paired = false;             // the SSG beside an OPN's FM
    bool usable() const { return chip && update && rate; }
    // what the resampler keeps
    std::vector<DEV_SMPL> bufL, bufR; // native samples from index `first`
    int64_t first = 0;                // native sample number of bufL[0]
    int64_t made = 0;                 // native samples made so far
    uint32_t newRate = 0;             // a rate the core changed to, taken at the next stretch
    int64_t originJ = 0, originN = 0; // where the current rate began (output, native)
};

// A started chip: one core (or two, for the YM2203/2608/2610 and their SSG) with the
// functions the commands need.
class Chip {
public:
    virtual ~Chip();
    int type = -1;
    int instance = 0;
    uint32_t clock = 0;
    std::vector<Voice*> voices;      // the sound it makes
    virtual void write(uint8_t port, uint16_t reg, uint16_t data) = 0;  // a register write
    virtual void writeMem(uint32_t /*offset*/, uint8_t /*data*/) {}      // one byte of RAM
    virtual void romSize(uint16_t /*kind*/, uint32_t /*size*/) {}
    virtual void romData(uint16_t /*kind*/, uint32_t /*offset*/, uint32_t /*length*/, const uint8_t* /*data*/) {}
    virtual void reset() = 0;
    virtual void setStereo(uint8_t /*mask*/) {}
};

struct ChipSetup {
    int type;
    int instance;
    uint32_t clock;        // without the flag bits
    uint8_t flags[4];      // chip-specific header bytes (see makeChip)
    bool variant;          // bit 31 of the clock: YM2610B, YM3438, K052539, ES5506, T6W28 ...
    bool dual;             // the file has two of this chip
};

// Start the core for a chip; nullptr when it can't be played.
std::unique_ptr<Chip> makeChip(const ChipSetup& setup, uint32_t outRate);

// The output: all voices, mixed into 16-bit stereo.
class Mixer {
public:
    explicit Mixer(uint32_t rate) : rate_(rate) {}
    void add(Voice* v) { voices_.push_back(v); }
    // VGM: each voice's volume from the chips' shares, whose sum sets one gain for all
    void balance();
    // S98: every chip at the same volume, the SSG of an OPN at 0.8 of it
    void fixedVolumes();
    // Render output samples [from, to) into `out` (stereo). The numbers count output
    // samples since the start of the song; `from` is where this stretch of output starts.
    void render(int64_t from, int64_t to, int16_t* out);
    int32_t masterVolume = 0x10000;   // 16.16
private:
    uint32_t rate_;
    std::vector<Voice*> voices_;
    std::vector<int32_t> accL_, accR_;
};

// A song being played (VGM or S98).
class Song {
public:
    virtual ~Song();
    // tags and facts for the header line
    std::string type, title, author, program, computer, date, comment;
    uint64_t lengthMs = 0;         // 0: unknown
    bool hasLength = true;         // a VGM without a sample count says no
    virtual bool start(uint32_t outRate, std::string& error) = 0;
    // Render up to `frames` samples (stereo). Returns how many were made, fewer when
    // the song ended (and was set back to its start); 0 when it was at its end already.
    virtual uint32_t render(int16_t* out, uint32_t frames) = 0;
    // Play silently up to sample `pos` of the output.
    virtual void seek(uint64_t pos) = 0;
};

std::unique_ptr<Song> openSong(std::vector<uint8_t> data, std::string& error);

// Read a file, unpacking gzip; at most `limit` bytes. False with `error` set on failure.
bool loadFile(const char* path, std::vector<uint8_t>& out, std::string& error, size_t limit);

}  // namespace vgm

#endif
