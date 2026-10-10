// SPDX-License-Identifier: GPL-2.0-or-later
// bbmp-vgm: reading VGM and S98 files and playing their commands.
//
// Written from the published documents: the VGM specification 1.71/1.72 and the GD3
// 1.00 specification (vgmrips.net), the S98 v2 draft (Mamiya) and the S98 v3
// specification (2006-05-15). Everything the file says is checked against its size
// before it is used: a damaged or cut file plays as far as it makes sense, then ends.
#include "vgmplay.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iconv.h>
#include <zlib.h>

namespace vgm {

Song::~Song() = default;

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// ---------------------------------------------------------------------------------
// loading

bool loadFile(const char* path, std::vector<uint8_t>& out, std::string& error, size_t limit)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        error = "can't open the file";
        return false;
    }
    std::vector<uint8_t> raw;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        raw.insert(raw.end(), buf, buf + n);
        if (raw.size() > limit) {
            fclose(f);
            error = "the file is too large";
            return false;
        }
    }
    fclose(f);
    if (raw.size() >= 2 && raw[0] == 0x1F && raw[1] == 0x8B) {
        // gzip (.vgz): unpacked as far as it goes, never beyond the limit
        z_stream z;
        memset(&z, 0, sizeof z);
        if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) {
            error = "can't unpack the file";
            return false;
        }
        z.next_in = raw.data();
        z.avail_in = (uInt)raw.size();
        out.clear();
        int r = Z_OK;
        while (r == Z_OK) {
            z.next_out = buf;
            z.avail_out = sizeof buf;
            r = inflate(&z, Z_NO_FLUSH);
            size_t got = sizeof buf - z.avail_out;
            if (out.size() + got > limit) {
                inflateEnd(&z);
                error = "the file is too large";
                return false;
            }
            out.insert(out.end(), buf, buf + got);
            if (r == Z_BUF_ERROR && z.avail_in == 0)
                break;      // cut short: keep what came out
        }
        inflateEnd(&z);
        if (out.empty()) {
            error = "can't unpack the file";
            return false;
        }
        return true;
    }
    out.swap(raw);
    return true;
}

// ---------------------------------------------------------------------------------
// text

static std::string convert(const char* from, const char* in, size_t len)
{
    iconv_t cd = iconv_open("UTF-8", from);
    if (cd == (iconv_t)-1)
        return std::string(in, len);
    std::string out;
    std::vector<char> src(in, in + len);
    char* ip = src.data();
    size_t il = len;
    char buf[1024];
    while (il > 0) {
        char* op = buf;
        size_t ol = sizeof buf;
        size_t r = iconv(cd, &ip, &il, &op, &ol);
        out.append(buf, (size_t)(op - buf));
        if (r == (size_t)-1) {
            if (errno == E2BIG)
                continue;
            ip++;           // skip what can't be read
            il--;
        }
    }
    iconv_close(cd);
    return out;
}

static std::string utf16le(const uint8_t* p, size_t chars)
{
    std::string out;
    for (size_t i = 0; i < chars; i++) {
        uint32_t c = le16(p + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < chars) {
            uint32_t d = le16(p + 2 * i + 2);
            if (d >= 0xDC00 && d < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i++;
            }
        }
        if (c < 0x80)
            out += (char)c;
        else if (c < 0x800) {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        } else {
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------
// what VGM and S98 share: chips, mixing, the clock of the output

class Player : public Song {
public:
    std::vector<uint8_t> data_;
protected:
    std::vector<std::unique_ptr<Chip>> chips_;
    std::unique_ptr<Mixer> mixer_;
    uint32_t rate_ = 0;
    int64_t out_ = 0;          // output samples made since the (re)start
    int64_t abs_ = 0;          // output samples made since the start
    bool ended_ = false;
    uint64_t passCommands_ = 0; // commands run since the (re)start
    // A song with very many commands for its length (a damaged file with hardly a wait
    // in it) is not played round and round: it would take for ever.
    bool broken() const { return passCommands_ > 1000 && passCommands_ > 20 * (uint64_t)out_; }
    // the same for each time round a loop
    uint64_t loopCommands_ = 0;
    int64_t loopOut_ = 0;
    bool loopBroken_ = false;
    bool loopTooBusy()
    {
        bool busy = passCommands_ - loopCommands_ > 1000 &&
                    passCommands_ - loopCommands_ > 20 * (uint64_t)(out_ - loopOut_);
        loopCommands_ = passCommands_;
        loopOut_ = out_;
        if (busy)
            loopBroken_ = true;
        return busy;
    }
    // At the end of a song without a loop one more output sample is made and dropped,
    // then the chips start again from their reset state; the mixer's positions go on.
    void endExtra()
    {
        int16_t tmp[2];
        mixer_->render(abs_, abs_ + 1, tmp);
        abs_ += 1;
    }
    int64_t mixPos(int64_t j) const { return abs_ + (j - out_); }

    Chip* chip(int type, int instance)
    {
        for (auto& c : chips_)
            if (c->type == type && c->instance == instance)
                return c.get();
        return nullptr;
    }
    void addChip(const ChipSetup& s)
    {
        auto c = makeChip(s, rate_);
        if (!c)
            return;
        for (Voice* v : c->voices)
            mixer_->add(v);
        chips_.push_back(std::move(c));
    }
    void resetChips()
    {
        for (auto& c : chips_)
            c->reset();
    }
};

// ---------------------------------------------------------------------------------
// VGM

// The clocks a chip may have; outside them the header is taken as damaged (the C140's
// "clock" is its sample rate, some 20 kHz).
static constexpr uint32_t MIN_CLOCK = 100000, MIN_C140_CLOCK = 1000, MAX_CLOCK = 100000000;

static const uint8_t CLOCK_AT[CHIP_TYPES] = {
    0x0C, 0x10, 0x2C, 0x30, 0x38, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60,
    0x64, 0x68, 0x6C, 0x70, 0x74, 0x80, 0x84, 0x88, 0x8C, 0x90, 0x98, 0x9C, 0xA0, 0xA4,
    0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD8, 0xDC, 0xE0, 0xE4};

class VgmSong : public Player {
public:
    bool parse(std::string& error);
    bool start(uint32_t outRate, std::string& error) override;
    uint32_t render(int16_t* out, uint32_t frames) override;
    void seek(uint64_t pos) override;

private:
    // header
    uint32_t version_ = 0, dataStart_ = 0, dataEnd_ = 0, loopAt_ = 0;
    uint32_t total_ = 0, loopSamples_ = 0;
    uint32_t headerSize_ = 0;
    int8_t volumeModifier_ = 0;
    uint32_t hdr(uint32_t off) const { return off + 4 <= headerSize_ ? le32(&data_[off]) : 0; }
    uint8_t hdr8(uint32_t off) const { return off < headerSize_ ? data_[off] : 0; }

    // playing
    uint32_t pos_ = 0;            // file position of the next command
    uint64_t tick_ = 0;           // VGM samples (1/44100 s) since the (re)start
    std::vector<uint8_t> banks_[0x40];                 // data of recorded streams, by type
    std::vector<std::pair<uint32_t, uint32_t>> blocks_[0x40];   // their parts (start, length)
    struct Table { bool set = false; uint8_t bd = 0, bc = 0; std::vector<uint16_t> v; };
    Table tables_[2][4];          // decompression tables by type and sub-type
    uint32_t ym2612Pcm_ = 0;      // read position for commands 0x8n

    struct Stream {
        bool set = false, playing = false;
        int chipType = -1, chipInst = 0;
        uint8_t port = 0, cmd = 0;
        uint8_t bank = 0xFF, stepSize = 1, stepBase = 0;
        uint32_t freq = 0;
        uint32_t start = 0;       // byte offset in the bank
        uint32_t length = 0;      // in commands, 0 = to the end of the data
        bool loop = false, reverse = false;
        uint32_t done = 0;        // commands written since starting
        int64_t startedAt = 0;    // output sample of the start
        uint32_t pos = 0;         // next byte offset
    };
    Stream streams_[256];

    void reset();
    bool runCommands();           // until the next wait; false at the end
    uint64_t tickOrigin_ = 0;     // the tick output sample 0 stands for (after a seek)
    int64_t outAt(uint64_t tick) const
    {
        return (int64_t)(((tick - tickOrigin_) * rate_ + 44099) / 44100);
    }
    void dataBlock(uint8_t type, const uint8_t* p, uint32_t size);
    void ramWrite(const uint8_t* p);
    void streamCommand(uint8_t cmd, const uint8_t* p);
    void streamStep(int64_t j);
    int64_t nextStreamWrite() const;
    void streamWrite(Stream& s);
    uint32_t streamDataSize(const Stream& s) const;
};

bool VgmSong::parse(std::string& error)
{
    if (data_.size() < 0x40 || memcmp(data_.data(), "Vgm ", 4) != 0) {
        error = "not a VGM or S98 file";
        return false;
    }
    uint32_t size = (uint32_t)data_.size();
    version_ = le32(&data_[8]);
    uint32_t eof = le32(&data_[4]);
    dataEnd_ = (eof && eof <= size - 4) ? eof + 4 : size;
    uint32_t dataOfs = version_ >= 0x150 ? le32(&data_[0x34]) : 0;
    dataStart_ = dataOfs ? 0x34 + dataOfs : 0x40;
    if (dataStart_ < 0x40 || dataStart_ >= size) {
        error = "damaged VGM file";
        return false;
    }
    headerSize_ = std::min<uint32_t>(dataStart_, std::min<uint32_t>(0x100, size));
    if (version_ < 0x150)
        headerSize_ = 0x40;
    // the commands end where the GD3 tag begins
    uint32_t gd3At = hdr(0x14) ? 0x14 + hdr(0x14) : 0;
    if (gd3At > dataStart_ && gd3At < dataEnd_)
        dataEnd_ = gd3At;
    total_ = hdr(0x18);
    uint32_t loop = hdr(0x1C);
    loopSamples_ = hdr(0x20);
    if (loop && loopSamples_ && 0x1C + (uint64_t)loop >= dataStart_ && 0x1C + (uint64_t)loop < dataEnd_)
        loopAt_ = 0x1C + loop;
    if (version_ >= 0x150)
        volumeModifier_ = (int8_t)hdr8(0x7C);

    type = "VGM";
    lengthMs = (uint64_t)total_ * 1000 / 44100;

    // GD3
    uint32_t gd3 = hdr(0x14);
    if (gd3 && 0x14 + (uint64_t)gd3 + 12 <= size) {
        const uint8_t* g = &data_[0x14 + gd3];
        if (memcmp(g, "Gd3 ", 4) == 0) {
            uint32_t len = le32(g + 8);
            uint64_t avail = size - (0x14 + (uint64_t)gd3 + 12);
            if (len > avail)
                len = (uint32_t)avail;
            std::vector<std::string> fields;
            const uint8_t* s = g + 12;
            size_t chars = len / 2, start = 0;
            for (size_t i = 0; i < chars && fields.size() < 11; i++) {
                if (le16(s + 2 * i) == 0) {
                    fields.push_back(utf16le(s + 2 * start, i - start));
                    start = i + 1;
                }
            }
            fields.resize(11);
            auto pick = [&](int en, int jp) { return !fields[en].empty() ? fields[en] : fields[jp]; };
            title = pick(0, 1);
            program = pick(2, 3);
            computer = pick(4, 5);
            author = pick(6, 7);
            if (author.empty())
                author = fields[9];     // no author: the person who made the file
            date = fields[8];
            comment = fields[10];
        }
    }
    return true;
}

bool VgmSong::start(uint32_t outRate, std::string& error)
{
    rate_ = outRate;
    mixer_ = std::make_unique<Mixer>(outRate);
    uint32_t sizeLimit = (uint32_t)data_.size();
    (void)sizeLimit;

    // extra header: clocks of second chips, chip volumes
    uint32_t extraClock[CHIP_TYPES] = {0};
    struct Vol { int type; bool paired; bool second; uint16_t value; };
    std::vector<Vol> vols;
    uint32_t extra = version_ >= 0x170 ? hdr(0xBC) : 0;
    if (extra && 0xBC + (uint64_t)extra + 4 <= dataStart_) {
        uint32_t eh = 0xBC + extra;
        uint32_t ehSize = le32(&data_[eh]);
        if (ehSize >= 8 && eh + 8 <= data_.size()) {
            uint32_t co = le32(&data_[eh + 4]);
            if (co && eh + 4 + (uint64_t)co < data_.size()) {
                uint32_t p = eh + 4 + co;
                uint8_t n = data_[p++];
                for (uint8_t i = 0; i < n && p + 5 <= data_.size(); i++, p += 5)
                    if (data_[p] < CHIP_TYPES)
                        extraClock[data_[p]] = le32(&data_[p + 1]);
            }
        }
        if (ehSize >= 12 && eh + 12 <= data_.size()) {
            uint32_t vo = le32(&data_[eh + 8]);
            if (vo && eh + 8 + (uint64_t)vo < data_.size()) {
                uint32_t p = eh + 8 + vo;
                uint8_t n = data_[p++];
                for (uint8_t i = 0; i < n && p + 4 <= data_.size(); i++, p += 4)
                    vols.push_back({data_[p] & 0x7F, (data_[p] & 0x80) != 0, (data_[p + 1] & 1) != 0, le16(&data_[p + 2])});
            }
        }
    }

    for (int t = 0; t < CHIP_TYPES; t++) {
        uint32_t clk = hdr(CLOCK_AT[t]);
        // VGM 1.00 and 1.01 have one FM clock: above 5 MHz it is a YM2612's
        if (version_ < 0x110 && t == YM2612)
            clk = hdr(0x10) > 5000000 ? hdr(0x10) : 0;
        if (version_ < 0x110 && t == YM2151)
            clk = 0;
        if (version_ < 0x110 && t == YM2413 && hdr(0x10) > 5000000)
            clk = 0;
        if (!(clk & 0x3FFFFFFF))
            continue;
        if ((clk & 0x3FFFFFFF) < (t == C140 ? MIN_C140_CLOCK : MIN_CLOCK) || (clk & 0x3FFFFFFF) > MAX_CLOCK)
            continue;       // no chip runs at that clock: a damaged header
        ChipSetup s{};
        s.type = t;
        s.clock = clk & 0x3FFFFFFF;
        s.variant = (clk & 0x80000000) != 0;
        switch (t) {
        case SN76489:
            if (version_ >= 0x110) {
                s.flags[0] = hdr8(0x28); s.flags[1] = hdr8(0x29); s.flags[2] = hdr8(0x2A);
            }
            if (version_ >= 0x151) s.flags[3] = hdr8(0x2B);
            break;
        case SEGAPCM:
            s.flags[0] = hdr8(0x3C); s.flags[1] = hdr8(0x3D); s.flags[2] = hdr8(0x3E); s.flags[3] = hdr8(0x3F);
            break;
        case AY8910: s.flags[0] = hdr8(0x78); s.flags[1] = hdr8(0x79); break;
        case YM2203: s.flags[0] = hdr8(0x7A); break;
        case YM2608: s.flags[0] = hdr8(0x7B); break;
        case OKIM6258: s.flags[0] = hdr8(0x94); break;
        case K054539: s.flags[0] = hdr8(0x95); break;
        case C140: s.flags[0] = hdr8(0x96); break;
        case ES5503: s.flags[0] = hdr8(0xD4); break;
        case ES5506: s.flags[0] = hdr8(0xD5); break;
        case C352: s.flags[0] = hdr8(0xD6); break;
        default: break;
        }
        s.instance = 0;
        s.dual = (clk & 0x40000000) != 0;
        addChip(s);
        if (clk & 0x40000000) {
            ChipSetup s2 = s;
            s2.instance = 1;
            uint32_t ec = extraClock[t] & 0x3FFFFFFF;
            if (ec >= (t == C140 ? MIN_C140_CLOCK : MIN_CLOCK) && ec <= MAX_CLOCK) {
                s2.clock = ec;
                s2.variant = (extraClock[t] & 0x80000000) != 0;
            }
            addChip(s2);
        }
    }
    for (const Vol& v : vols) {
        Chip* c = chip(v.type, v.second ? 1 : 0);
        size_t which = v.paired ? 1 : 0;
        if (!c || which >= c->voices.size())
            continue;
        Voice* vo = c->voices[which];
        if (v.value & 0x8000)   // relative: the chip's share times value/0x100
            vo->weight = (int32_t)(((int64_t)vo->weight * (v.value & 0x7FFF)) >> 8);
        else                    // absolute
            vo->weight = v.value;
    }
    mixer_->balance();
    if (chips_.empty()) {
        error = "no sound chip of this file can be played";
        return false;
    }
    int vm = volumeModifier_;
    if (vm == -63)
        vm = -64;
    if (vm < -64) vm = -64;
    if (vm > 192) vm = 192;
    mixer_->masterVolume = (int32_t)(0x10000 * pow(2.0, vm / 32.0) + 0.5);
    reset();
    return true;
}

void VgmSong::reset()
{
    passCommands_ = 0;
    loopCommands_ = 0;
    loopOut_ = 0;
    pos_ = dataStart_;
    tick_ = 0;
    tickOrigin_ = 0;
    out_ = 0;
    ended_ = false;
    for (auto& b : banks_) b.clear();
    for (auto& b : blocks_) b.clear();
    for (auto& t : tables_) for (auto& x : t) x = Table();
    for (auto& s : streams_) s = Stream();
    ym2612Pcm_ = 0;
    resetChips();
}

static int commandLength(uint8_t c)
{
    if (c >= 0x70 && c <= 0x8F) return 1;
    switch (c) {
    case 0x61: return 3;
    case 0x62: case 0x63: case 0x66: return 1;
    case 0x67: return 7;     // plus the data
    case 0x68: return 12;
    case 0x90: return 5;
    case 0x91: return 5;
    case 0x92: return 6;
    case 0x93: return 11;
    case 0x94: return 2;
    case 0x95: return 5;
    case 0xE0: return 5;
    case 0xE1: return 5;
    }
    if (c == 0x00) return 1;                    // no operation
    if (c >= 0x30 && c <= 0x3F) return 2;
    if (c >= 0x40 && c <= 0x4E) return 3;
    if (c == 0x4F || c == 0x50) return 2;
    if (c >= 0x51 && c <= 0x5F) return 3;
    if (c >= 0xA0 && c <= 0xBF) return 3;
    if (c >= 0xC0 && c <= 0xDF) return 4;
    if (c >= 0xE2) return 5;
    return 0;               // a byte that is no command: the data ends here
}

// Run commands at the current tick. Returns false at the end of the song (with no loop).
bool VgmSong::runCommands()
{
    const uint8_t* d = data_.data();
    int jumps = 0;      // a loop with no wait in it would never end: the second jump ends it
    for (;;) {
        passCommands_++;
        if (pos_ >= dataEnd_) {
            if (loopAt_ && jumps++ == 0 && !loopTooBusy()) { pos_ = loopAt_; continue; }
            return false;
        }
        uint8_t c = d[pos_];
        int len = commandLength(c);
        if (len == 0) {
            if (loopAt_ && jumps++ == 0 && !loopTooBusy()) { pos_ = loopAt_; continue; }
            return false;
        }
        if (pos_ + (uint32_t)len > dataEnd_) {
            pos_ = dataEnd_;
            if (loopAt_ && jumps == 0) continue;
            return false;
        }
        const uint8_t* p = d + pos_ + 1;
        uint32_t wait = 0;
        if (c == 0x67) {
            uint8_t type = p[1];
            uint32_t sz = le32(p + 2);
            uint32_t body = sz & 0x7FFFFFFF;
            if (p[0] != 0x66 || (uint64_t)pos_ + 7 + body > dataEnd_) {
                pos_ = dataEnd_;
                if (loopAt_ && jumps == 0) continue;
                return false;
            }
            dataBlock(type, p + 6, sz);
            pos_ += 7 + body;
            continue;
        }
        pos_ += (uint32_t)len;
        if (c >= 0x70 && c <= 0x7F) wait = (c & 0x0F) + 1u;
        else if (c >= 0x80 && c <= 0x8F) {
            Chip* ch = chip(YM2612, 0);
            const auto& bank = banks_[0];
            if (ch && ym2612Pcm_ < bank.size())
                ch->write(0, 0x2A, bank[ym2612Pcm_]);
            ym2612Pcm_++;
            wait = c & 0x0F;
        } else switch (c) {
        case 0x61: wait = le16(p); break;
        case 0x62: wait = 735; break;
        case 0x63: wait = 882; break;
        case 0x66:
            if (loopAt_ && jumps++ == 0 && !loopTooBusy()) { pos_ = loopAt_; break; }
            pos_ -= 1;
            return false;
        case 0x68: ramWrite(p); break;
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
            streamCommand(c, p);
            break;
        case 0xE0: ym2612Pcm_ = le32(p); break;
        case 0x4F: if (Chip* ch = chip(SN76489, 0)) ch->write(1, 0, p[0]); break;
        case 0x50: if (Chip* ch = chip(SN76489, 0)) ch->write(0, 0, p[0]); break;
        case 0x3F: if (Chip* ch = chip(SN76489, 1)) ch->write(1, 0, p[0]); break;
        case 0x30: if (Chip* ch = chip(SN76489, 1)) ch->write(0, 0, p[0]); break;
        case 0x31: {
            int inst = (p[0] >> 7) & 1;
            Chip* ch = (p[0] & 0x40) ? chip(YM2203, inst) : chip(AY8910, inst);
            if (ch) ch->setStereo(p[0] & 0x3F);
            break;
        }
        case 0x40: if (Chip* ch = chip(MIKEY, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58:
        case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xA8:
        case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
            static const int8_t T[16] = {-1, YM2413, YM2612, YM2612, YM2151, YM2203, YM2608, YM2608,
                                         YM2610, YM2610, YM3812, YM3526, Y8950, YMZ280B, YMF262, YMF262};
            static const uint8_t PORT[16] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1};
            int inst = (c & 0xF0) == 0xA0 ? 1 : 0;
            if (Chip* ch = chip(T[c & 0x0F], inst)) ch->write(PORT[c & 0x0F], p[0], p[1]);
            break;
        }
        case 0xA0: if (Chip* ch = chip(AY8910, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xB0: if (Chip* ch = chip(RF5C68, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xB1: if (Chip* ch = chip(RF5C164, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xB2: if (Chip* ch = chip(PWM, p[0] >> 7)) ch->write(0, (p[0] & 0x70) >> 4, (uint16_t)(((p[0] & 0x0F) << 8) | p[1])); break;
        case 0xB3: if (Chip* ch = chip(GB_DMG, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xB4: if (Chip* ch = chip(NES_APU, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xB5: if (Chip* ch = chip(MULTIPCM, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xB6: if (Chip* ch = chip(UPD7759, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xB7: if (Chip* ch = chip(OKIM6258, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xB8: if (Chip* ch = chip(OKIM6295, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xB9: if (Chip* ch = chip(HUC6280, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xBA: if (Chip* ch = chip(K053260, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xBB: if (Chip* ch = chip(POKEY, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xBC: break;   // WonderSwan: no core
        case 0xBD: if (Chip* ch = chip(SAA1099, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xBE: break;   // ES5506: no core
        case 0xBF: if (Chip* ch = chip(GA20, p[0] >> 7)) ch->write(0, p[0] & 0x7F, p[1]); break;
        case 0xC0: if (Chip* ch = chip(SEGAPCM, p[1] >> 7)) ch->write(0, (uint16_t)(le16(p) & 0x7FFF), p[2]); break;
        case 0xC1: if (Chip* ch = chip(RF5C68, p[1] >> 7)) ch->writeMem(le16(p) & 0x7FFF, p[2]); break;
        case 0xC2: if (Chip* ch = chip(RF5C164, p[1] >> 7)) ch->writeMem(le16(p) & 0x7FFF, p[2]); break;
        case 0xC3: {
            if (Chip* ch = chip(MULTIPCM, p[0] >> 7)) {
                uint16_t ofs = le16(p + 1);
                uint8_t sel = p[0] & 0x7F;
                if (sel & 1) ch->write(0, 0x11, (uint8_t)(ofs >> 3));
                if (sel & 2) ch->write(0, 0x12, (uint8_t)(ofs >> 3));
            }
            break;
        }
        case 0xC4: if (Chip* ch = chip(QSOUND, 0)) ch->write(0, p[2], (uint16_t)((p[0] << 8) | p[1])); break;
        case 0xC5: if (Chip* ch = chip(SCSP, p[0] >> 7)) ch->write(0, (uint16_t)(((p[0] & 0x7F) << 8) | p[1]), p[2]); break;
        case 0xC6: break;   // WonderSwan memory
        case 0xC7: if (Chip* ch = chip(VSU, p[0] >> 7)) ch->write(0, (uint16_t)(((p[0] & 0x7F) << 8) | p[1]), p[2]); break;
        case 0xC8: if (Chip* ch = chip(X1_010, p[0] >> 7)) ch->write(0, (uint16_t)(((p[0] & 0x7F) << 8) | p[1]), p[2]); break;
        case 0xD0: if (Chip* ch = chip(YMF278B, p[0] >> 7)) ch->write(p[0] & 0x7F, p[1], p[2]); break;
        case 0xD1: if (Chip* ch = chip(YMF271, p[0] >> 7)) ch->write(p[0] & 0x7F, p[1], p[2]); break;
        case 0xD2: if (Chip* ch = chip(K051649, p[0] >> 7)) ch->write(p[0] & 0x7F, p[1], p[2]); break;
        case 0xD3: if (Chip* ch = chip(K054539, p[0] >> 7)) ch->write(0, (uint16_t)(((p[0] & 0x7F) << 8) | p[1]), p[2]); break;
        case 0xD4: if (Chip* ch = chip(C140, p[0] >> 7)) ch->write(0, (uint16_t)(((p[0] & 0x7F) << 8) | p[1]), p[2]); break;
        case 0xD5: if (Chip* ch = chip(ES5503, p[0] >> 7)) ch->write(0, p[1], p[2]); break;
        case 0xD6: break;   // ES5506
        case 0xE1: if (Chip* ch = chip(C352, 0)) ch->write(0, (uint16_t)((p[0] << 8) | p[1]), (uint16_t)((p[2] << 8) | p[3])); break;
        default: break;     // reserved commands are skipped by their length
        }
        if (wait) {
            tick_ += wait;
            return true;
        }
    }
}

void VgmSong::dataBlock(uint8_t type, const uint8_t* p, uint32_t sizeField)
{
    uint32_t size = sizeField & 0x7FFFFFFF;
    int inst = (sizeField & 0x80000000) ? 1 : 0;
    if (type < 0x40) {
        auto& bank = banks_[type];
        if (bank.size() + size > (256u << 20))
            return;
        blocks_[type].push_back({(uint32_t)bank.size(), size});
        bank.insert(bank.end(), p, p + size);
        return;
    }
    if (type < 0x7F) {
        // compressed stream data
        if (size < 10)
            return;
        uint8_t ctype = p[0];
        uint32_t outSize = le32(p + 1);
        uint8_t bd = p[5], bc = p[6], st = p[7];
        uint16_t add = le16(p + 8);
        const uint8_t* src = p + 10;
        uint32_t srcLen = size - 10;
        uint8_t t = type - 0x40;
        auto& bank = banks_[t];
        if (outSize > (64u << 20) || bank.size() + outSize > (256u << 20) || bd == 0 || bd > 16 || bc == 0 || bc > 16)
            return;
        uint32_t valBytes = (bd + 7) / 8;
        std::vector<uint8_t> out(outSize, 0);
        const Table* tab = (ctype < 2 && st < 4) ? &tables_[ctype][ctype == 1 ? 0 : st] : nullptr;
        uint64_t bitPos = 0, bits = (uint64_t)srcLen * 8;
        uint32_t state = add;
        for (uint32_t o = 0; o + valBytes <= outSize; o += valBytes) {
            if (bitPos + bc > bits)
                break;
            uint32_t v = 0;
            for (int b = 0; b < bc; b++, bitPos++)
                v = (v << 1) | ((src[bitPos >> 3] >> (7 - (bitPos & 7))) & 1);
            uint32_t r = 0;
            if (ctype == 0) {
                if (st == 0) r = v + add;
                else if (st == 1) r = (v << (bd - bc)) + add;
                else if (st == 2) r = (tab && tab->set && v < tab->v.size()) ? tab->v[v] : 0;
            } else if (ctype == 1) {
                if (tab && tab->set && v < tab->v.size())
                    state += tab->v[v];
                r = state;
            }
            out[o] = (uint8_t)r;
            if (valBytes > 1)
                out[o + 1] = (uint8_t)(r >> 8);
        }
        blocks_[t].push_back({(uint32_t)bank.size(), outSize});
        bank.insert(bank.end(), out.begin(), out.end());
        return;
    }
    if (type == 0x7F) {
        if (size < 6)
            return;
        uint8_t ctype = p[0], st = p[1], bd = p[2], bc = p[3];
        uint16_t count = le16(p + 4);
        if (ctype > 1 || st > 3)
            return;
        Table& tab = tables_[ctype][ctype == 1 ? 0 : st];
        tab.set = true;
        tab.bd = bd;
        tab.bc = bc;
        tab.v.clear();
        uint32_t vb = (bd + 7) / 8;
        if (vb == 0 || vb > 2)
            return;
        for (uint32_t i = 0; i < count && 6 + (i + 1) * vb <= size; i++)
            tab.v.push_back(vb == 1 ? p[6 + i] : le16(p + 6 + 2 * i));
        return;
    }
    if (type < 0xC0) {
        // ROM image
        if (size < 8)
            return;
        uint32_t romSize = le32(p), start = le32(p + 4);
        const uint8_t* d = p + 8;
        uint32_t len = size - 8;
        int ct = -1;
        uint16_t kind = 0;
        switch (type) {
        case 0x80: ct = SEGAPCM; break;
        case 0x81: ct = YM2608; break;
        case 0x82: ct = YM2610; kind = 1; break;
        case 0x83: ct = YM2610; kind = 2; break;
        case 0x84: ct = YMF278B; kind = 0x524F; break;
        case 0x85: ct = YMF271; break;
        case 0x86: ct = YMZ280B; break;
        case 0x87: ct = YMF278B; kind = 0x5241; break;
        case 0x88: ct = Y8950; break;
        case 0x89: ct = MULTIPCM; break;
        case 0x8A: ct = UPD7759; break;
        case 0x8B: ct = OKIM6295; break;
        case 0x8C: ct = K054539; break;
        case 0x8D: ct = C140; break;
        case 0x8E: ct = K053260; break;
        case 0x8F: ct = QSOUND; break;
        case 0x91: ct = X1_010; break;
        case 0x92: ct = C352; break;
        case 0x93: ct = GA20; break;
        default: return;
        }
        Chip* ch = chip(ct, inst);
        if (!ch || romSize > (64u << 20))
            return;
        ch->romSize(kind, romSize);
        if (start < romSize && len) {
            if (len > romSize - start)
                len = romSize - start;
            ch->romData(kind, start, len, d);
        }
        return;
    }
    // RAM writes
    uint32_t start, hdrLen = type < 0xE0 ? 2 : 4;
    if (size < hdrLen)
        return;
    start = type < 0xE0 ? le16(p) : le32(p);
    int ct = -1;
    switch (type) {
    case 0xC0: ct = RF5C68; break;
    case 0xC1: ct = RF5C164; break;
    case 0xC2: ct = NES_APU; break;
    case 0xE0: ct = SCSP; break;
    case 0xE1: ct = ES5503; break;
    default: return;
    }
    if (Chip* ch = chip(ct, inst))
        ch->romData(0, start, size - hdrLen, p + hdrLen);
}

void VgmSong::ramWrite(const uint8_t* p)
{
    // 0x66 cc oo oo oo dd dd dd ss ss ss
    if (p[0] != 0x66)
        return;
    uint8_t type = p[1];
    uint32_t from = p[2] | (p[3] << 8) | (p[4] << 16);
    uint32_t to = p[5] | (p[6] << 8) | (p[7] << 16);
    uint32_t size = p[8] | (p[9] << 8) | (p[10] << 16);
    if (!size)
        size = 0x1000000;
    int inst = (type & 0x80) ? 1 : 0;
    type &= 0x7F;
    if (type >= 0x40)
        return;
    const auto& bank = banks_[type];
    if (from >= bank.size())
        return;
    if (size > bank.size() - from)
        size = (uint32_t)(bank.size() - from);
    int ct = -1;
    switch (type) {
    case 0x01: ct = RF5C68; break;
    case 0x02: ct = RF5C164; break;
    case 0x06: ct = SCSP; break;
    case 0x07: ct = NES_APU; break;
    default: return;
    }
    if (Chip* ch = chip(ct, inst))
        ch->romData(0, to, size, bank.data() + from);
}

// ---------------------------------------------------------------------------------
// DAC stream control (commands 0x90-0x95)

static constexpr uint32_t MAX_STREAM_RATE = 2000000;   // writes per second, at most

uint32_t VgmSong::streamDataSize(const Stream& s) const
{
    switch (s.chipType) {
    case PWM: case QSOUND: case C352: return 2;
    default: return 1;
    }
}

void VgmSong::streamCommand(uint8_t c, const uint8_t* p)
{
    Stream& s = streams_[p[0]];
    switch (c) {
    case 0x90:
        if (p[0] == 0xFF) return;
        s.set = true;
        s.chipType = p[1] & 0x7F;
        s.chipInst = (p[1] >> 7) & 1;
        s.port = p[2];
        s.cmd = p[3];
        break;
    case 0x91:
        if (p[0] == 0xFF) return;
        s.bank = p[1];
        s.stepSize = p[2] ? p[2] : 1;
        s.stepBase = p[3];
        break;
    case 0x92:
        if (p[0] == 0xFF) return;
        s.freq = std::min<uint32_t>(le32(p + 1), MAX_STREAM_RATE);
        if (s.playing) {       // a new rate counts from here
            s.startedAt = out_;
            s.done = 0;
        }
        break;
    case 0x93: {
        if (p[0] == 0xFF || !s.set || s.bank >= 0x40) return;
        uint32_t ofs = le32(p + 1);
        uint8_t mode = p[5];
        uint32_t len = le32(p + 6);
        uint32_t ds = streamDataSize(s);
        if (ofs != 0xFFFFFFFF)
            s.start = ofs;
        else if (s.playing)
            s.start = s.pos;
        s.reverse = (mode & 0x10) != 0;
        s.loop = (mode & 0x80) != 0;
        uint32_t bankSize = (uint32_t)banks_[s.bank].size();
        switch (mode & 0x0F) {
        case 0: if (!s.playing) return; s.length = s.length; break;
        case 1: s.length = len; break;
        case 2: s.length = s.freq ? (uint32_t)((uint64_t)len * s.freq / 1000) : 0; break;
        case 3: s.length = (s.start < bankSize) ? (bankSize - s.start) / (ds * s.stepSize) : 0; break;
        default: s.length = len; break;
        }
        s.pos = s.start + s.stepBase * ds;
        s.playing = true;
        s.done = 0;
        s.startedAt = out_;
        break;
    }
    case 0x94:
        if (p[0] == 0xFF) {
            for (auto& x : streams_) x.playing = false;
        } else
            s.playing = false;
        break;
    case 0x95: {
        if (p[0] == 0xFF || !s.set || s.bank >= 0x40) return;
        uint16_t block = le16(p + 1);
        uint8_t flags = p[3];
        const auto& bl = blocks_[s.bank];
        if (block >= bl.size()) return;
        uint32_t ds = streamDataSize(s);
        s.start = bl[block].first;
        s.length = bl[block].second / (ds * s.stepSize);
        s.loop = (flags & 0x01) != 0;
        s.reverse = (flags & 0x10) != 0;
        s.pos = s.start + s.stepBase * ds;
        s.playing = true;
        s.done = 0;
        s.startedAt = out_;
        break;
    }
    }
}

void VgmSong::streamWrite(Stream& s)
{
    const auto& bank = banks_[s.bank];
    uint32_t ds = streamDataSize(s);
    if ((uint64_t)s.pos + ds > bank.size())
        return;
    uint16_t v = bank[s.pos];
    if (ds == 2)
        v |= (uint16_t)(bank[s.pos + 1] << 8);
    Chip* ch = chip(s.chipType, s.chipInst);
    if (!ch)
        return;
    switch (s.chipType) {
    case PWM: ch->write(0, s.cmd, v & 0x0FFF); break;
    case RF5C68: case RF5C164: case HUC6280:
        if (s.port != 0xFF) ch->write(0, s.cmd >> 4, s.port);
        ch->write(0, s.cmd & 0x0F, v);
        break;
    case SN76489: ch->write(0, 0, v); break;
    case SEGAPCM: case K054539: case C140: case SCSP: case VSU: case X1_010:
        ch->write(0, (uint16_t)((s.port << 8) | s.cmd), v);
        break;
    case C352: ch->write(0, (uint16_t)((s.port << 8) | s.cmd), v); break;
    case QSOUND: ch->write(0, s.cmd, v); break;
    case YMF278B: case YMF271: case K051649: ch->write(s.port, s.cmd, v); break;
    case AY8910: case GB_DMG: case NES_APU: case MULTIPCM: case UPD7759: case OKIM6258:
    case OKIM6295: case K053260: case POKEY: case SAA1099: case GA20: case MIKEY: case ES5503:
        ch->write(0, s.cmd, v);
        break;
    default: ch->write(s.port, s.cmd, v); break;
    }
}

// The output sample at which stream write number `k` happens.
static int64_t streamWriteAt(int64_t startedAt, uint32_t k, uint32_t freq, uint32_t rate)
{
    return startedAt + (int64_t)(((uint64_t)k * rate + freq - 1) / freq);
}

int64_t VgmSong::nextStreamWrite() const
{
    int64_t best = INT64_MAX;
    for (const Stream& s : streams_)
        if (s.playing && s.freq)
            best = std::min(best, streamWriteAt(s.startedAt, s.done, s.freq, rate_));
    return best;
}

void VgmSong::streamStep(int64_t j)
{
    for (Stream& s : streams_) {
        if (!s.playing || !s.freq)
            continue;
        while (s.playing && streamWriteAt(s.startedAt, s.done, s.freq, rate_) <= j) {
            if (s.length && s.done >= s.length) {
                if (s.loop) {
                    s.pos = s.start + s.stepBase * streamDataSize(s);
                    s.startedAt = streamWriteAt(s.startedAt, s.done, s.freq, rate_);
                    s.done = 0;
                    continue;
                }
                s.playing = false;
                break;
            }
            streamWrite(s);
            uint32_t step = streamDataSize(s) * s.stepSize;
            if (s.reverse)
                s.pos -= step;
            else
                s.pos += step;
            s.done++;
        }
    }
}

uint32_t VgmSong::render(int16_t* out, uint32_t frames)
{
    uint32_t made = 0;
    if (ended_)
        return 0;
    while (made < frames) {
        // the commands due before this sample
        int64_t j = out_;
        bool ended = false;
        while (outAt(tick_) <= j) {
            if (!runCommands()) {
                ended = true;
                break;
            }
        }
        streamStep(j);
        if (ended) {
            ended_ = true;
            break;
        }
        int64_t cmdAt = outAt(tick_);
        int64_t until = std::min(out_ + (frames - made), std::min(cmdAt, nextStreamWrite()));
        if (until <= j)
            until = j + 1;
        // Output runs up to the last sample before a command; when the command falls
        // between two samples, the sample before it is a stretch of its own.
        if (until == cmdAt && until - j > 1 && ((tick_ - tickOrigin_) * rate_) % 44100 != 0)
            until -= 1;
        mixer_->render(mixPos(j), mixPos(until), out + 2 * made);
        made += (uint32_t)(until - j);
        abs_ += until - j;
        out_ = until;
    }
    if (ended_) {
        // the song is over: it starts again from the beginning at the next call, unless
        // no time passed in it (a song of commands and no waits would never get anywhere)
        bool empty = tick_ == tickOrigin_ || broken() || loopBroken_;
        endExtra();
        reset();
        if (made == 0 || empty)
            ended_ = true;      // say it's over
    }
    return made;
}

void VgmSong::seek(uint64_t pos)
{
    // The commands run up to the tick of the position without the chips making sound;
    // output then starts there afresh, its times counted from that tick.
    uint64_t target = pos * 44100 / rate_;
    while (tick_ < target) {
        if (!runCommands()) {
            ended_ = true;      // past the end of a song without a loop: nothing plays
            return;
        }
    }
    tickOrigin_ = target;
    out_ = 0;
    abs_ = 0;
}

// ---------------------------------------------------------------------------------
// S98

class S98Song : public Player {
public:
    bool parse(std::string& error);
    bool start(uint32_t outRate, std::string& error) override;
    uint32_t render(int16_t* out, uint32_t frames) override;
    void seek(uint64_t pos) override;
private:
    struct Device { uint32_t type, clock, pan; };
    std::vector<Device> devices_;
    std::vector<Chip*> devChips_;
    uint32_t num_ = 10, den_ = 1000;
    uint32_t dumpAt_ = 0, loopAt_ = 0;
    uint32_t pos_ = 0;
    uint64_t sync_ = 0;
    void reset();
    bool runCommands();
    uint64_t syncOrigin_ = 0;     // the sync output sample 0 stands for (after a seek)
    int64_t outAt(uint64_t sync) const
    {
        // a sync lasts num/den seconds
        unsigned __int128 t = (unsigned __int128)(sync - syncOrigin_) * num_ * rate_;
        return (int64_t)((t + den_ - 1) / den_);
    }
    void parseTags(uint32_t at, int version);
};

bool S98Song::parse(std::string& error)
{
    if (data_.size() < 0x20 || memcmp(data_.data(), "S98", 3) != 0) {
        error = "not a VGM or S98 file";
        return false;
    }
    int version = data_[3] - '0';
    if (version < 0 || version > 3) {
        error = "unknown S98 version";
        return false;
    }
    uint32_t size = (uint32_t)data_.size();
    num_ = le32(&data_[4]);
    den_ = version >= 2 ? le32(&data_[8]) : 0;
    if (!num_) num_ = 10;
    if (!den_) den_ = 1000;
    if ((uint64_t)num_ * 1000000 < den_) {      // a sync shorter than a microsecond
        error = "damaged S98 file";
        return false;
    }
    uint32_t tagAt = le32(&data_[0x10]);
    dumpAt_ = le32(&data_[0x14]);
    uint32_t loop = le32(&data_[0x18]);
    if (dumpAt_ < 0x20 || dumpAt_ >= size) {
        error = "damaged S98 file";
        return false;
    }
    loopAt_ = (loop >= dumpAt_ && loop < size) ? loop : 0;
    if (version == 3) {
        uint32_t count = le32(&data_[0x1C]);
        for (uint32_t i = 0; i < count && i < 64 && 0x20 + (i + 1) * 16 <= size; i++) {
            const uint8_t* d = &data_[0x20 + i * 16];
            devices_.push_back({le32(d), le32(d + 4), le32(d + 8)});
        }
    } else if (version == 2) {
        for (uint32_t i = 0; i < 64 && 0x20 + (i + 1) * 16 <= size; i++) {
            const uint8_t* d = &data_[0x20 + i * 16];
            if (le32(d) == 0)
                break;
            devices_.push_back({le32(d), le32(d + 4), 0});
        }
    }
    if (devices_.empty())
        devices_.push_back({4, 7987200, 0});
    type = "S98";
    parseTags(tagAt, version);

    // the length: syncs up to the end
    uint64_t syncs = 0;
    uint32_t p = dumpAt_;
    while (p < size) {
        uint8_t c = data_[p];
        if (c == 0xFF) { syncs++; p++; }
        else if (c == 0xFE) {
            uint64_t n = 0;
            int s = 0;
            p++;
            while (p < size && s < 35) {
                uint8_t b = data_[p++];
                n |= (uint64_t)(b & 0x7F) << s;
                s += 7;
                if (!(b & 0x80))
                    break;
            }
            syncs += n + 2;
        } else if (c == 0xFD)
            break;
        else
            p += 3;
    }
    lengthMs = syncs * num_ * 1000 / den_;
    return true;
}

void S98Song::parseTags(uint32_t at, int version)
{
    if (!at || at >= data_.size())
        return;
    const char* s = (const char*)&data_[at];
    size_t max = data_.size() - at;
    size_t len = strnlen(s, max);
    if (version == 3 && len >= 5 && memcmp(s, "[S98]", 5) == 0) {
        s += 5;
        len -= 5;
        bool utf8 = len >= 3 && memcmp(s, "\xEF\xBB\xBF", 3) == 0;
        if (utf8) { s += 3; len -= 3; }
        std::string text = utf8 ? std::string(s, len) : convert("CP932", s, len);
        size_t i = 0;
        while (i < text.size()) {
            size_t e = text.find('\n', i);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(i, e - i);
            i = e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            for (auto& ch : key) ch = (char)tolower((unsigned char)ch);
            if (key == "title") title = val;
            else if (key == "artist") author = val;
            else if (key == "game") program = val;
            else if (key == "year") date = val;
            else if (key == "comment") comment = val;
            else if (key == "system") computer = val;
        }
        return;
    }
    title = convert("CP932", s, len);
}

bool S98Song::start(uint32_t outRate, std::string& error)
{
    rate_ = outRate;
    mixer_ = std::make_unique<Mixer>(outRate);
    int count[CHIP_TYPES] = {0};
    for (const Device& d : devices_) {
        ChipSetup s{};
        s.clock = d.clock;
        if (d.clock < MIN_CLOCK || d.clock > MAX_CLOCK) {
            devChips_.push_back(nullptr);
            continue;
        }
        switch (d.type) {
        case 1: s.type = AY8910; s.flags[0] = 0x10; s.flags[1] = 0x01; s.clock = d.clock / 2; break;
        case 2: s.type = YM2203; break;
        case 3: s.type = YM2612; break;
        case 4: s.type = YM2608; break;
        case 5: s.type = YM2151; break;
        case 6: s.type = YM2413; break;
        case 7: s.type = YM3526; break;
        case 8: s.type = YM3812; break;
        case 9: s.type = YMF262; break;
        case 15: s.type = AY8910; s.flags[0] = 0x00; s.flags[1] = 0x01; break;
        case 16: s.type = SN76489; s.flags[0] = 0x03; s.flags[2] = 15; break;
        default: devChips_.push_back(nullptr); continue;
        }
        s.instance = count[s.type]++;
        size_t before = chips_.size();
        addChip(s);
        devChips_.push_back(chips_.size() > before ? chips_.back().get() : nullptr);
    }
    if (chips_.empty()) {
        error = "no sound chip of this file can be played";
        return false;
    }
    mixer_->fixedVolumes();
    reset();
    return true;
}

void S98Song::reset()
{
    passCommands_ = 0;
    loopCommands_ = 0;
    loopOut_ = 0;
    pos_ = dumpAt_;
    sync_ = 0;
    syncOrigin_ = 0;
    out_ = 0;
    ended_ = false;
    resetChips();
    // an OPNA starts with its six FM channels on
    for (Chip* c : devChips_)
        if (c && c->type == YM2608)
            c->write(0, 0x29, 0x80);
}

bool S98Song::runCommands()
{
    const uint32_t size = (uint32_t)data_.size();
    int jumps = 0;      // a loop with no sync in it would never end
    for (;;) {
        passCommands_++;
        if (pos_ >= size) {
            if (loopAt_ && jumps++ == 0 && !loopTooBusy()) { pos_ = loopAt_; continue; }
            return false;
        }
        uint8_t c = data_[pos_];
        if (c == 0xFF) {
            pos_++;
            sync_++;
            return true;
        }
        if (c == 0xFE) {
            uint64_t n = 0;
            int s = 0;
            pos_++;
            while (pos_ < size && s < 35) {
                uint8_t b = data_[pos_++];
                n |= (uint64_t)(b & 0x7F) << s;
                s += 7;
                if (!(b & 0x80))
                    break;
            }
            sync_ += n + 2;
            return true;
        }
        if (c == 0xFD) {
            if (loopAt_ && jumps++ == 0 && !loopTooBusy()) { pos_ = loopAt_; continue; }
            return false;
        }
        if (pos_ + 3 > size) {
            pos_ = size;
            continue;
        }
        uint32_t dev = c >> 1;
        uint8_t port = c & 1;
        if (dev < devChips_.size() && devChips_[dev]) {
            Chip* ch = devChips_[dev];
            uint8_t a = data_[pos_ + 1], d = data_[pos_ + 2];
            if (ch->type == SN76489)
                ch->write(port, 0, d);
            else
                ch->write(port, a, d);
        }
        pos_ += 3;
    }
}

uint32_t S98Song::render(int16_t* out, uint32_t frames)
{
    uint32_t made = 0;
    if (ended_)
        return 0;
    while (made < frames) {
        int64_t j = out_;
        bool ended = false;
        while (outAt(sync_) <= j) {
            if (!runCommands()) {
                ended = true;
                break;
            }
        }
        if (ended) {
            ended_ = true;
            break;
        }
        int64_t until = std::min<int64_t>(out_ + (frames - made), outAt(sync_));
        if (until <= j)
            until = j + 1;
        mixer_->render(mixPos(j), mixPos(until), out + 2 * made);
        made += (uint32_t)(until - j);
        abs_ += until - j;
        out_ = until;
    }
    if (ended_) {
        bool empty = sync_ == syncOrigin_ || broken() || loopBroken_;
        endExtra();
        reset();
        if (made == 0 || empty)
            ended_ = true;
    }
    return made;
}

void S98Song::seek(uint64_t pos)
{
    // as for VGM: the commands run up to the sync of the position, output starts there
    uint64_t target = (uint64_t)(((unsigned __int128)pos * den_) / ((unsigned __int128)num_ * rate_));
    while (sync_ < target) {
        if (!runCommands()) {
            ended_ = true;
            return;
        }
    }
    syncOrigin_ = target;
    out_ = 0;
    abs_ = 0;
}

// ---------------------------------------------------------------------------------

std::unique_ptr<Song> openSong(std::vector<uint8_t> data, std::string& error)
{
    if (data.size() >= 4 && memcmp(data.data(), "Vgm ", 4) == 0) {
        auto s = std::make_unique<VgmSong>();
        s->data_ = std::move(data);
        if (!s->parse(error))
            return nullptr;
        return s;
    }
    if (data.size() >= 3 && memcmp(data.data(), "S98", 3) == 0) {
        auto s = std::make_unique<S98Song>();
        s->data_ = std::move(data);
        if (!s->parse(error))
            return nullptr;
        return s;
    }
    error = "not a VGM or S98 file";
    return nullptr;
}

}  // namespace vgm
