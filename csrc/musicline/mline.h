// SPDX-License-Identifier: LGPL-2.1-or-later
// A replayer for Musicline Editor modules ("MLEDMODL", files *.ml).
//
// The replay routine is a port of the original Amiga one, written by Conny Cyreus,
// Christian Cyreus, John Carehag and Jimmy Fredriksson and released by them under the
// GNU LGPL (mlineplayer102.asm in UADE, amigasrc/players/other/musicline_editor). It was
// ported with the help of NostalgicPlayer's C# port of the same routine (MIT licence,
// (c) Thomas Neumann), and follows the 68000 code where the two differ. The names of
// the variables are those of the 68000 source, so that the two can be read side by side.
// The licences of both are in csrc/musicline (README.md, COPYING.LGPL, LICENSE.NostalgicPlayer).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mline {

// An address in sample memory: a block and a place in it. Reads outside the block
// give silence instead of whatever came next in the Amiga's memory.
struct Ptr {
    const int8_t *base = nullptr;
    uint32_t size = 0;
    uint32_t ofs = 0;

    bool null() const { return base == nullptr; }
    int8_t operator[](uint32_t i) const {
        uint64_t p = (uint64_t)ofs + i;
        return p < size ? base[p] : 0;
    }
    Ptr plus(uint32_t n) const { Ptr p = *this; p.ofs += n; return p; }
    uint32_t left() const { return ofs < size ? size - ofs : 0; }
};

struct Fx { uint8_t num = 0, par = 0; };
struct PartLine { uint8_t note = 0, inst = 0; Fx fx[5]; };
struct Part { PartLine line[128]; };
struct ArpLine { int8_t note = 0; uint8_t smpl = 0; Fx fx[2]; };
struct Arpg { ArpLine line[128]; };

// what an instrument and a sample both start with
struct SmplHead {
    std::string title;
    uint8_t type = 0;               // 0 sample, 1-5 a wave of 16, 32, 64, 128, 256 bytes
    Ptr pointer;
    uint16_t length = 0;            // in words, like all lengths here
    Ptr repPointer;
    uint16_t repLength = 0;
    int16_t fineTune = 0, semiTone = 0;
};

struct Sample : SmplHead {
    std::vector<int8_t> data;
};

struct CounterInit { uint16_t start = 0, repeat = 0, repEnd = 0, speed = 0, turns = 0, delay = 0; };

struct Inst : SmplHead {
    uint8_t smplNumber = 0;
    uint16_t smplStart = 0, smplEnd = 0, smplRepStart = 0, smplRepLen = 0;
    uint16_t volume = 0;
    uint8_t transpose = 0, slideSpeed = 0;
    uint8_t effects1 = 0, effects2 = 0;
    uint16_t env[12] = {};          // lengths, speeds and volumes of attack, decay, sustain, release
    uint8_t vibDir = 0, vibWaveNum = 0;
    uint16_t vibSpeed = 0, vibDelay = 0, vibAtkSpd = 0, vibAttack = 0, vibDepth = 0;
    uint8_t treDir = 0, treWaveNum = 0;
    uint16_t treSpeed = 0, treDelay = 0, treAtkSpd = 0, treAttack = 0, treDepth = 0;
    uint16_t arpTable = 0;
    uint8_t arpSpeed = 0, arpGroove = 0;
    uint8_t envTraPhaFilBits = 0;
    uint8_t traWaveNums[5] = {};
    CounterInit tra, pha, mix, res, fil;
    uint8_t phaType = 0;
    uint8_t mixResLooBits = 0, mixWaveNum = 0;
    uint8_t mixResFilBoost = 0, resAmp = 0;
    uint8_t filType = 0;
    uint16_t looStart = 0, looRepeat = 0, looRepEnd = 0, looLength = 0, looLpStep = 0;
    uint16_t looWait = 0, looDelay = 0, looTurns = 0;
};

struct Tune {
    std::string title;
    uint16_t tempo = 125;
    uint8_t speed = 6, groove = 0;
    uint16_t volume = 64;
    uint8_t playMode = 0;           // 0: four channels by Paula, 1: eight channels mixed
    uint8_t channels = 0;
    uint16_t seq[8][256];
    int seqLen[8] = {};             // how many entries of seq came from the file
};

struct Module {
    std::string version;
    std::string title, author, date, duration;
    std::vector<std::string> info;
    std::vector<Tune> tunes;
    std::vector<Part> partStore;
    std::vector<Arpg> arpgStore;
    int part[1024];                 // index into partStore, 0 is the empty part
    int arpg[256];
    std::vector<Inst> insts;        // 0 is the empty instrument
    std::vector<Sample> smpls;      // 0 is the empty sample
    int numInst = 0, numSmpl = 0;

    bool load(const uint8_t *d, size_t n, std::string &err);
    const Inst &inst(int i) const { return insts[i >= 0 && i < (int)insts.size() ? i : 0]; }
    const Sample &smpl(int i) const { return smpls[i >= 0 && i < (int)smpls.size() ? i : 0]; }
};

// One of Paula's four channels, or one of the eight of the mixing routine.
struct Voice {
    Ptr cur;  uint32_t curLen = 0;  // what is being played, length in bytes
    Ptr nxt;  uint32_t nxtLen = 0;  // what follows when that has been played: the repeat
    uint64_t pos = 0;               // place in cur, 32.32
    uint16_t period = 0;
    int vol = 0;                    // 0..64
    bool on = false;
    bool loop = true;               // eight channels: whether the sample repeats at all
};

struct Counter {
    int16_t counter = 0, speed = 0;
    uint16_t repeat = 0, repeatend = 0;
    int16_t turns = 0;
    uint16_t delay = 0;
    bool step = false;
    int16_t savecounter = 0;
};

struct Chan {
    uint8_t VoiceOff = 0;
    uint8_t Spd = 0, Grv = 0, SpdPart = 0, GrvPart = 0;
    uint8_t SpdCnt = 0, ArpSpdCnt = 0, PartGrv = 0, ArpgGrv = 0;
    uint8_t WsNumber = 0, WsNumberOld = 0;
    const Inst *InstPtr = nullptr;
    const SmplHead *WsPtr = nullptr;
    uint8_t PartNote = 0, PartInst = 0;
    Fx PartEffects[5];
    uint8_t Arp = 0, ArpPos = 0, ArpTab = 0, ArpWait = 0, ArpgNote = 0;
    uint8_t ArpVolSld = 0, ArpPchSld = 0, ArpPchSldType = 0;
    int16_t ArpNote = 0;
    uint8_t TunePos = 0, PartPos = 0, PartPosWork = 0, TuneJumpCount = 0, PartJmpCnt = 0;
    Ptr WsRepPtrOrg, WsPointer, WsRepPointer;
    uint16_t WsLength = 0, WsRepLength = 0;
    int16_t Volume1 = 0, Volume2 = 0, Volume3 = 0;
    int16_t Note = 0;
    int16_t Period1 = 0, Period2 = 0;
    int16_t Transpose = 0, SemiTone = 0, FineTune = 0;
    uint8_t SmpOfs = 0, SmplOfs = 0, OldInst = 0, Restart = 0;
    uint8_t VolAdd = 0, VolSld = 0, CVolSld = 0, MVolSld = 0;
    int16_t VolSet = 0, CVolume = 0, VolAddNum = 0, CVolAddNum = 0, MVolAddNum = 0;
    int16_t VolSldSpd = 0, CVolSldSpd = 0, MVolSldSpd = 0;
    int16_t VolSldToVol = 0, CVolSldToVol = 0, MVolSldToVol = 0;
    uint8_t VolSldType = 0, CVolSldType = 0, MVolSldType = 0;
    uint8_t VolSldToVolOff = 0, CVolSldToVolOff = 0, MVolSldToVolOff = 0;
    uint8_t Vol = 0, InstPchSld = 0, MixResFilBoost = 0;
    uint8_t TransposeNum = 0;
    uint16_t PartNum = 0;
    uint8_t PchSld = 0, PchSldType = 0;
    int16_t PchSldSpd = 0, PchSldNote = 0, PchSldToNote = 0, PchAdd = 0;
    int16_t ArpVolSldSpd = 0, ArpPchSldSpd = 0, ArpPchSldToNote = 0, ArpPchSldNote = 0;
    uint8_t PTPchSld = 0, PTPchSldType = 0;
    int16_t PTPchSldSpd = 0, PTPchSldSpd2 = 0, PTPchSldNote = 0, PTPchSldToNote = 0, PTPchAdd = 0;
    uint8_t Effects1 = 0, Effects2 = 0, EffectsPar1 = 0, EffectsPar2 = 0;
    uint16_t ADSRVolume = 0;
    uint16_t ADSRData[12] = {};
    uint8_t Play = 0, WaveOrSample = 0;
    uint8_t PhaInit = 0, FilInit = 0, TraInit = 0, TuneWait = 0;
    uint8_t Vib = 0, VibDir = 0, VibWaveNum = 0, PartVibWaveNum = 0;
    uint16_t VibCount = 0, VibCmdSpeed = 0, VibCmdDepth = 0, VibCmdDelay = 0;
    uint16_t VibAtkSpeed = 0, VibAtkLength = 0, VibDepth = 0;
    int16_t VibNote = 0;
    uint8_t PTTrePos = 0, PTTreCmd = 0, PTTreWave = 0, PTVibPos = 0, PTVibCmd = 0, PTVibWave = 0;
    int16_t PTVibNote = 0;
    uint8_t Tre = 0, TreDir = 0, TreWaveNum = 0, PartTreWaveNum = 0;
    uint16_t TreCount = 0, TreCmdSpeed = 0, TreCmdDepth = 0, TreCmdDelay = 0;
    uint16_t TreAtkSpeed = 0, TreAtkLength = 0, TreDepth = 0;
    int8_t FilLastSample = 0, ResLastSample = 0;
    uint8_t FilLastInit = 0, ResLastInit = 0, ResAmp = 0, ResInit = 0, PhaType = 0, FilType = 0;
    Counter TraData, PhaData, MixData, ResData, FilData;
    uint16_t TraSpd = 0, PhaSpd = 0, MixSpd = 0, ResSpd = 0, FilSpd = 0;
    uint8_t MixWaveNum = 0, MixInit = 0;
    uint8_t TraWsPtrs[6] = {};
    uint8_t PlayError = 0, LooInit = 0;
    uint16_t LooRepeat = 0, LooRepEnd = 0, LooLength = 0;
    int32_t LooStep = 0;
    uint16_t LooWait = 0;
    int16_t LooWaitCounter = 0, LooDelay = 0, LooTurns = 0;
    uint16_t LooCounter = 0, LooCounterSave = 0, LooWsCounterMax = 0, LooSpd = 0;
    Ptr LooWsPointer;
    int8_t TraWaveBuffer[256] = {}, PhaWaveBuffer[256] = {}, MixWaveBuffer[256] = {};
    int8_t ResWaveBuffer[256] = {}, FilWaveBuffer[256] = {}, WaveBuffer[256] = {};
    uint8_t MixLoop = 0;

    // not in the original: for finding the end of the song
    bool done = false;              // has played all it has, or has come round
    bool pastData = false;          // is playing the empty rest of its sequence
    std::vector<uint8_t> seen;      // sequencer states this channel has been in
};

class Player {
public:
    explicit Player(const Module &m) : mod(m) {}

    void start(int tune);
    void tick();                    // what the Amiga does on every interrupt
    double tickSeconds() const;     // the time from one interrupt to the next

    int channels() const { return playMode ? 8 : 4; }
    bool eightChannels() const { return playMode != 0; }
    bool stopped() const { return !playing; }        // the song has switched itself off
    bool allDone() const;           // every channel has ended or come round
    bool running() const;           // some channel still reads its sequence
    bool ledFilter() const { return led; }

    Voice voice[8];

    // for comparing with the Amiga: what Paula is told about samples, if anybody asks
    struct Write { int channel; uint16_t words; const int8_t *base; uint32_t ofs; };
    std::vector<Write> *writes = nullptr;

private:
    const Module &mod;
    const Tune *tune = nullptr;
    Chan ch[8];
    uint8_t playMode = 0, TuneSpd = 0, TuneGrv = 0;
    uint16_t TuneTmp = 125;
    int16_t MasterVol = 64 * 16;
    bool playing = false;
    bool led = false;
    uint16_t ciaLatch = 0;
    uint16_t mixLength = 0;

    void setTempo();
    void PlayVoice(int n);
    void CheckInst(Chan &c, int n);
    void PlayPartFx(Chan &c, int n);
    void PartFx(Chan &c, int n, uint8_t num, uint8_t par);
    void PlayArpg(Chan &c);
    void ArpFx(Chan &c, uint8_t num, uint8_t par);
    void ArpWaitStart(Chan &c, uint8_t smpl);
    void PlayInst(Chan &c);
    void FixWaveLength(Chan &c, const Inst &in, uint8_t type, Ptr p);
    void InstPlay(Chan &c, const Inst &in);
    void Sexit(Chan &c);
    void PlayEffects(int n);
    void Per(Chan &c);
    int16_t GetPeriod(const Chan &c, int16_t note) const;
    int paulaVolume(const Chan &c) const;
    void PerVolPlay();
    void DmaPlay();
    void MoveLoop(Chan &c, int n);
    void LoopCounter(Chan &c);
    void SlideNote(Chan &c);
    void SlideArpNote(Chan &c);
    void SlideVol(Chan &c);
    void SlideChannelVol(Chan &c);
    void SlideMasterVol(Chan &c);
    void SlideArpVol(Chan &c);
    void ArpeggioPlay(Chan &c);
    void ADSRPlay(Chan &c);
    void TremoloPlay(Chan &c);
    void VibratoPlay(Chan &c);
    void TransformPlay(Chan &c);
    void PhasePlay(Chan &c);
    void MixPlay(Chan &c);
    void ResonancePlay(Chan &c);
    void FilterPlay(Chan &c);
    bool stepCounter(Chan &c, Counter &k, bool step, bool init, uint16_t spd, int lo, int hi, bool phase,
                     bool oneway, int16_t &d0);
    void markDone(Chan &c) { c.done = true; }
    void setNext(int n, Ptr p, uint32_t words);
};

}  // namespace mline
