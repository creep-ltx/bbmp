// SPDX-License-Identifier: GPL-3.0-or-later
//
// pmdc: a driver for PMD songs (.m, the PC-98 "Professional Music Driver"), written for
// this player from a description of how PMD behaves (a clean-room rewrite: no code of
// PMD, PMDWin, pmdmini or fmgen was read to write it). It decides which YM2608 register
// gets which value when; the chip itself is emulated elsewhere (ymfm, pmdc_engine.cpp).
//
// The driver is run by the chip's two timers, emulated here exactly as the player it
// replaces did, so that every register write happens at the same moment: Timer A drives
// fades and the SSG drums, Timer B is the music's tick.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pmdc {

// The C library's rand() of glibc for seed 1 (its additive "TYPE_3" generator). The
// songs that draw random numbers draw them from one sequence shared by the length scan
// and playback, so both drivers of a song get the same object.
class Random {
 public:
  Random();
  uint32_t next();          // 0 ... 2^31 - 1
  long drawn = 0;           // numbers drawn so far
  void (*onDraw)(void*, uint32_t) = nullptr;  // told of each number (the log)
  void* user = nullptr;

 private:
  uint32_t r_[34];
  int i_ = 0;
};

// Where the driver's writes go: the chip (and the log), and the calls to the PCM units
// (PPZ8, PPS, P86), which have no sound here but are logged.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void reg(int reg, int value) = 0;     // value 0-255, reg 0x000-0x1FF
  virtual void unit(const std::string& call) = 0;
};

using I = int64_t;   // the driver computes in wide signed numbers; registers get the low 8 bits

struct Lfo {
  I value = 0, delay = 0, delayR = 0, speed = 0, speedR = 0, step = 0, stepR = 0;
  I time = 0, timeR = 0, wave = 0;
  I dcSpeed = 0, dcSpeedR = 0, dcDepth = 0, dcCount = 0, dcCountR = 0;
};

struct Part {
  bool active = false;
  I pos = 0, loopPos = 0;            // offsets in the data (D); loopPos 0 = none
  I len = 0, qcount = 0, qbase = 0, q2 = 0, qmin = 0, randq = 0;
  I vol = 0, oneshot = 0, detune = 0, transpose = 0, mtranspose = 0;
  I note = 0xFF, prev = 0xFF;        // 0xFF = rest
  I pitch = 0;
  I portaAcc = 0, portaStep = 0, portaRem = 0;
  Lfo lfo[2];
  I lfosw = 0, extend = 0;
  // envelope (SSG, ADPCM, PPZ): seven places shared by the normal and extended kinds
  I ep[7] = {0, 0, 0, 0, 0, 0, 0};
  I edrc = 0, esl = 0, eal = 0, elevel = 0, estate = 0;
  bool eext = false;
  I pan = 0, mixer = 0, voice = 0, slotmask = 0, carrier = 0, vwmask = 0, vlfoA = 0, vlfoB = 0;
  I tl[5] = {0, 0, 0, 0, 0};         // by slot 1-4
  I fbalg = 0;
  I hlfoDelay = 0, hlfoCount = 0, sdelay = 0, sdelayCount = 0, sdelayMask = 0;
  I mask = 0, keyoff = 0, loopflag = 0;
  I patPos = 0;                      // rhythm part: position in the pattern, -1 = none
};

class Driver {
 public:
  Driver(Random& rng, Sink* sink);
  void init();                                     // the instance's set-up (§5.1)
  // Load a song (the whole .m file). Returns an error, or "" when the song is taken.
  std::string load(const uint8_t* file, size_t size);
  void start();
  // One timer event: the handlers whose flags are up, then the 0x27 write.
  void event();
  uint32_t nextEvent() const;                      // µs to the next event, 0 = never
  void advance(uint32_t us);

  int loopCount() const { return static_cast<int>(loopCounter_); }
  I barTicks() const { return bars_ * barLen_ + barCount_; }
  // Memos (§2.3): -2 PPZ file, -1 PPS file, 0 PCM file, 1 title, 2 composer, 3 arranger...
  std::string memo(int n) const;
  long rhythmKeys = 0;   // rhythm key-on writes so far (does the song use the chip's drums?)

 private:
  enum { FM, SSG, ADPCM, RHY, PPZ };
  enum ReadResult { NOTE, CONT, STOP };
  static constexpr int NPARTS = 22;  // A-F, G-I, J, K, 3 FM3 extended, 8 PPZ8
  static constexpr int EXT = 11, PPZP = 14, PART_C = 2, PART_I = 8;

  // data
  int rd(I d) const;
  int rs(I d) const { int v = rd(d); return v >= 0x80 ? v - 0x100 : v; }
  int rw(I d) const { return rd(d) | (rd(d + 1) << 8); }
  int rsw(I d) const { int v = rw(d); return v >= 0x8000 ? v - 0x10000 : v; }
  void wd(I d, int v);
  void record(I n, int rec[26]) const;

  // chip
  void w(int reg, I value);
  void ev(const std::string& s) { if (sink_) sink_->unit(s); }
  void timerA();
  void timerB();

  // building blocks
  void stop();
  void silence();
  void partReset();
  void chipReset();
  void timerSetup();
  void partSetup();

  // per-tick
  void fmTick(Part& p);
  void ssgTick(Part& p);
  void adTick(Part& p);
  void rhyTick(Part& p);
  void loopCheck();
  ReadResult fmRead(Part& p);
  ReadResult ssgRead(Part& p);
  ReadResult pcmRead(Part& p);
  bool endOfPart(Part& p, ReadResult& r);
  void fmIdle(Part& p);
  void pcmIdle(Part& p);
  void command(Part& p, I& pos);
  void commandC0(Part& p, I& pos);
  void tempo(I& pos);
  void unknown(I& pos) { wd(pos, 0x80); }

  // notes
  I transpose(const Part& p, I n) const;
  void gate(Part& p, I& pos);
  void oneShotRule(Part& p);
  void lfoStart(Part& p, bool restOrTied, bool fm);
  void lfoInit(Part& p, int set);
  bool lfoStep(Part& p, int set);
  void waveStep(Lfo& l);
  void depthChange(Lfo& l);
  void portaStep(Part& p);

  // FM
  void fmPitchValue(Part& p, I n);
  void fmPitchOut(Part& p);
  void fm3PitchOut(Part& p);
  void fmVolOut(Part& p);
  void fmKeyOn(Part& p);
  void fmKeyOff(Part& p, bool always = false);
  void fmVoice(Part& p, I n);
  void fmVoiceSet(Part& p, bool resend);
  void fmResend(Part& p);
  void fmSilencePart(Part& p);
  void fmPanOut(Part& p);
  void fmPanCopy(Part& p);
  void fm3Check(Part& p);
  bool fm3ch() const { return ctxP_ == 0 && ctxCh_ == 3; }
  int fmBit(const Part& p) const;

  // SSG
  void ssgPitchValue(Part& p, I n);
  void ssgPitchOut(Part& p);
  void ssgVolOut(Part& p);
  void ssgKeyOn(Part& p);
  void envKeyOff(Part& p);
  void envStart(Part& p);
  bool envStep(Part& p);
  bool envStepOnce(Part& p);
  int drumCheck(Part& p, int b);

  // ADPCM and PPZ8
  void adPitchValue(Part& p, I n);
  void pcmPitchOut(Part& p);
  void pcmVolOut(Part& p);
  void pcmKeyOn(Part& p);
  void pcmKeyOff(Part& p);
  bool releasing(const Part& p) const { return p.eext ? p.estate == 4 : p.estate == 2; }

  // rhythm and SSG effects
  void rhythmCommand(int cmd, I& pos);
  void effectStart(int e);
  void effectRow();
  void effectStep();
  void effectEnd();

  Random& rng_;
  Sink* sink_;
  std::vector<uint8_t> area_;   // the song, in a 64 KB area filled with 0x80
  bool hasVoices_ = true;

  Part part_[NPARTS];
  int kind_ = FM;               // the kind of the part being processed
  int ctxP_ = 0, ctxCh_ = 1, ctxIdx_ = 0;

  // timers (§3.1)
  I ta_ = 0, tbReg_ = 0, ctrl_ = 0, cA_ = 0, cB_ = 0;
  bool flagA_ = false, flagB_ = false;

  bool playing_ = false, stopReq_ = false;
  I opm_ = 0, rtable_ = 0;
  I tie_ = 0, oneShotFlag_ = 0;
  I fm3mode_ = 0x3F, fm3special_ = 0, slotdt_[4] = {0, 0, 0, 0}, fm3fbalg_ = 0;
  bool slotdtOn_ = false;
  I keyon_[2][3] = {{0, 0, 0}, {0, 0, 0}};
  I noiseMem_ = 0, noiseLast_ = 0, reg7_ = 0, hwlfo_ = 0;
  I rhy_[6] = {0, 0, 0, 0, 0, 0}, rhyTotal_ = 0, drumRecord_ = 0;
  I vdFM_ = 0, vdSSG_ = 0, vdAD_ = 0, vdRHY_ = 0, vdPPZ_ = 0;
  I fadeLevel_ = 0, fadeSpeed_ = 0;
  I loopCounter_ = 0, bars_ = 0, barCount_ = 0, barLen_ = 96;
  I taCount_ = 0, lastTA_ = 0;
  I tb_ = 0, tbP_ = 0, t_ = 0, tP_ = 0, lastTB_ = -1;
  I adStart_ = 0, adStop_ = 0, adRep1_ = 0, adRep2_ = 0, adRel_ = 0x8000;
  I effNum_ = -1, effPri_ = 0, effDur_ = 0, effIdx_ = 0, effRow_ = 0;
  I effTone_ = 0, effNoise_ = 0, effToneSw_ = 0, effNoiseSw_ = 0, effNoiseCnt_ = 0;
};

// The length scan (§14) of a song on a fresh driver: the length to where every part has
// looped once, and the loop's length, in ms. Returns false if the song is refused.
struct Scan {
  long lengthMs = 0, loopMs = 0;
  bool usesRhythm = false;
};
bool scan(Random& rng, const uint8_t* file, size_t size, Scan& out);

// Title and composer as the old player gave them (memo 1 and 2, with NEC's 2-byte
// half-width characters converted and terminal escapes removed); still Shift-JIS.
std::string memoText(const std::string& raw);

}  // namespace pmdc
