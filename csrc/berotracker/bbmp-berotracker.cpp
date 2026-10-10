// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-berotracker: play a BeRoTracker song (.brt) as raw PCM on stdout.
//
//   bbmp-berotracker <file> <track> <samplerate> [start-ms]
//   bbmp-berotracker --ref <file>   the whole song at 44100 Hz as 32-bit floats, no header
//                                   (what BeRoTracker 1.00's mixer makes, sample for sample)
//
// BeRoTracker is BeRo's (Benjamin Rosseaux's) Impulse-Tracker-like tracker with a
// floating-point mixer: 8-tap FIR resampling, IT-style and biquad channel filters,
// envelopes with several interpolation modes, new-note actions, and a routing graph of
// built-in effects (delay, stereo delay, reverb, pitch shifter, bass booster, 10-band
// equaliser, compressor, distortion). This is a new player, written for bbmp from a
// description of how the tracker behaves (a clean room: its code was not looked at).
//
// Exactness. The tracker mixes in single, double and x87 extended arithmetic, and its
// output (32-bit floats) depends on all three. On x86 (32 or 64 bit) this player does the
// same: long double is the x87 type, and the x87 FSIN, FCOS, F2XM1, FYL2X, FSCALE and
// FRNDINT instructions are used through inline assembly. That gives the tracker's own
// output bit for bit. On other CPUs the C library stands in: the songs sound the same,
// but are not bit-exact (differences far below hearing).
//
// The engine renders at 44100 Hz, the rate of the tracker's own export; other rates are
// resampled. The song plays once, to the end of its order list (the tracker stops there:
// no tail of echoes).
//
// Same protocol as the other helpers: one JSON line describing the track, then signed
// 16-bit little-endian stereo until the song ends (floats clipped to +-1 and scaled by
// 32767). A damaged file is refused with the reason; nothing in it is trusted.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "resample.h"

#if (defined(__x86_64__) || defined(__i386__)) && !defined(BRT_PORTABLE) && !defined(BRT_DOUBLE)
#define BRT_X87 1
#else
#define BRT_X87 0
#endif

namespace {

#ifdef BRT_DOUBLE
typedef double X;        // a test of a CPU whose long double is a double
#else
typedef long double X;   // the x87 extended type on x86
#endif

const int RATE = 44100;
const int BLOCK = 16384;                    // the tracker's buffer (the plug-in tempo, 5.2)
const size_t MAX_FILE = 128u << 20;         // the largest known song is 2.3 MB
const size_t MAX_SAMPLE_BYTES = 512u << 20; // all sample buffers together
const size_t MAX_CELLS = 8u << 20;          // all pattern cells together
const size_t MAX_UNPACKED = 64u << 20;
const long long MAX_FRAMES = 30LL * 60 * RATE;
const int POOL = 128;                       // background (new-note action) voices
const int MAX_OPS = 256;
const int MAX_EFFECTS = 64;
const int MAX_NODES = 1024;          // the songs have at most 21
const long long MAX_TICKS = 2000000; // the longest song has about 21000
const size_t MAX_UNPACKED_ALL = 256u << 20;
const int MAX_ROWS = 4096;
const uint32_t MAX_C4 = 10000000;
const int64_t F = int64_t(1) << 32;
const int64_t PERIOD_MAX = int64_t(1) << 30;

// ---- arithmetic (description section 1)

float fbits(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
uint32_t ubits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
double dbits(uint64_t u) {
  double d;
  std::memcpy(&d, &u, 8);
  return d;
}

X xbits(uint64_t sig, uint16_t ex) {
#if BRT_X87
  unsigned char b[sizeof(X)] = {};
  std::memcpy(b, &sig, 8);
  std::memcpy(b + 8, &ex, 2);
  X r;
  std::memcpy(&r, b, sizeof(X));
  return r;
#else
  const int e = int(ex & 0x7FFF) - 16383 - 63;
  X r = std::ldexp(X(sig >> 32) * X(4294967296.0) + X(sig & 0xFFFFFFFFu), e);
  return (ex & 0x8000) ? -r : r;
#endif
}

inline float S(X x) { return static_cast<float>(x); }
inline double D(X x) { return static_cast<double>(x); }

// zap: zero, denormal, infinity and NaN become +0
inline float zap(float f) {
  const uint32_t e = (ubits(f) >> 23) & 0xFF;
  return (e == 0 || e == 0xFF) ? 0.0f : f;
}

// round half to even, as an integer; out of range or NaN gives `bad`
inline long long rnd(X x, long long lo, long long hi, long long bad) {
  if (!(x == x)) return bad;
  if (x < X(lo)) return lo;
  if (x > X(hi)) return hi;
  return static_cast<long long>(std::nearbyintl(x));
}

X EPSX;   // the plug-ins add 1e-24 as an extended constant, the voices as a single
X PI, LN2, LOG2E, TWO_PI_X, K_PISH, EPS18X, K1, K2, K3, K4, C_1_3, C_0_1, C_1E6, C_1_7, C_1E21;
float EPS24, EPS18, F3E5;
double HI, LO;

#if BRT_X87
inline X fsin(X a) { X r; __asm__("fsin" : "=t"(r) : "0"(a)); return r; }
inline X fcos(X a) { X r; __asm__("fcos" : "=t"(r) : "0"(a)); return r; }
inline X f2xm1(X a) { X r; __asm__("f2xm1" : "=t"(r) : "0"(a)); return r; }
inline X frndint(X a) { X r; __asm__("frndint" : "=t"(r) : "0"(a)); return r; }
inline X fyl2x(X x, X y) {   // y * log2(x)
  X r;
  __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
  return r;
}
inline X fscale(X a, X k) {  // a * 2^trunc(k)
  X r;
  __asm__("fscale" : "=t"(r) : "0"(a), "u"(k));
  return r;
}
inline X fldpi() { X r; __asm__("fldpi" : "=t"(r)); return r; }
inline X fldln2() { X r; __asm__("fldln2" : "=t"(r)); return r; }
inline X fldl2e() { X r; __asm__("fldl2e" : "=t"(r)); return r; }
#else
inline X fsin(X a) { return sinl(a); }
inline X fcos(X a) { return cosl(a); }
inline X f2xm1(X a) { return exp2l(a) - 1; }
inline X frndint(X a) { return nearbyintl(a); }
inline X fyl2x(X x, X y) { return y * log2l(x); }
inline X fscale(X a, X k) {
  const X t = truncl(k);
  const int e = t > 30000 ? 30000 : t < -30000 ? -30000 : int(t);
  return ldexpl(a, e);
}
inline X fldpi() { return xbits(0xC90FDAA22168C235ULL, 0x4000); }
inline X fldln2() { return xbits(0xB17217F7D1CF79ACULL, 0x3FFE); }
inline X fldl2e() { return xbits(0xB8AA3B295C17F0BCULL, 0x3FFF); }
#endif

void constants() {
  PI = fldpi();
  LN2 = fldln2();
  LOG2E = fldl2e();
  TWO_PI_X = xbits(0xC90FDAA21EB79EE8ULL, 0x4001);
  K_PISH = xbits(0xC90FD7E45803CD14ULL, 0x4000);
  EPS18X = xbits(0x9392EE8E921D5D07ULL, 0x3FC3);
  K1 = xbits(0x8000000000000001ULL, 0x3FFF);
  K2 = xbits(0x8000000000000001ULL, 0x3FFE);
  K3 = xbits(0x8000000000000001ULL, 0x3FFD);
  K4 = xbits(0x8000000000000001ULL, 0x3FFC);
  C_1_3 = 1.3L;
  C_0_1 = 0.1L;
  C_1E6 = 1e-6L;
  C_1_7 = 1.7L;
  C_1E21 = 1e-21L;
  EPS24 = fbits(0x179ABE15);
  EPSX = 1e-24L;
  EPS18 = fbits(0x219392EF);
  F3E5 = fbits(0x37FBA882);
  HI = dbits(0x3FE62E42FEE00000ULL);
  LO = dbits(0x3DEA39EF35793C76ULL);
}

// exp(v) the tracker's way (section 1, from step 3)
X xexpv(X v) {
  if (!(v == v)) return v;
  if (v > 11000) return X(INFINITY);
  if (v < -11000) return 0;
  const X k = frndint(v * LOG2E);
  const X r = (v - k * X(HI)) - k * X(LO);
  const X t = r * LOG2E;
  const X u = f2xm1(t);
  return fscale(u + 1, k);
}

// pow(x, y) the tracker's way (section 1)
X xpow(X x, X y) {
  if (!(x > 0)) return x == 0 && y > 0 ? X(0) : X(NAN);
  const X l = fyl2x(x, LN2);
  return xexpv(y * l);
}

inline float powS(float x, float y) { return S(xpow(X(x), X(y))); }

// ---- tables (description section 14)

const int32_t UP[256] = {
    65536, 65773, 66010, 66249, 66489, 66729, 66971, 67213, 67456, 67700, 67945, 68190, 68437, 68685, 68933, 69182,
    69432, 69684, 69936, 70189, 70442, 70697, 70953, 71209, 71467, 71725, 71985, 72245, 72507, 72769, 73032, 73296,
    73561, 73827, 74094, 74362, 74631, 74901, 75172, 75444, 75717, 75991, 76265, 76541, 76818, 77096, 77375, 77655,
    77935, 78217, 78500, 78784, 79069, 79355, 79642, 79930, 80219, 80509, 80800, 81093, 81386, 81680, 81976, 82272,
    82570, 82868, 83168, 83469, 83771, 84074, 84378, 84683, 84989, 85297, 85605, 85915, 86225, 86537, 86850, 87164,
    87480, 87796, 88113, 88432, 88752, 89073, 89395, 89718, 90043, 90369, 90695, 91023, 91353, 91683, 92015, 92347,
    92681, 93017, 93353, 93691, 94029, 94370, 94711, 95053, 95397, 95742, 96088, 96436, 96785, 97135, 97486, 97839,
    98193, 98548, 98904, 99262, 99621, 99981, 100343, 100706, 101070, 101435, 101802, 102170, 102540, 102911, 103283, 103657,
    104031, 104408, 104785, 105164, 105545, 105926, 106309, 106694, 107080, 107467, 107856, 108246, 108637, 109030, 109425, 109820,
    110217, 110616, 111016, 111418, 111821, 112225, 112631, 113038, 113447, 113857, 114269, 114682, 115097, 115514, 115931, 116351,
    116771, 117194, 117618, 118043, 118470, 118898, 119328, 119760, 120193, 120628, 121064, 121502, 121941, 122382, 122825, 123269,
    123715, 124162, 124611, 125062, 125514, 125968, 126424, 126881, 127340, 127801, 128263, 128727, 129192, 129660, 130129, 130599,
    131072, 131546, 132021, 132499, 132978, 133459, 133942, 134426, 134912, 135400, 135890, 136381, 136875, 137370, 137866, 138365,
    138865, 139368, 139872, 140378, 140885, 141395, 141906, 142419, 142935, 143451, 143970, 144491, 145014, 145538, 146064, 146593,
    147123, 147655, 148189, 148725, 149263, 149803, 150344, 150888, 151434, 151982, 152531, 153083, 153637, 154192, 154750, 155310,
    155871, 156435, 157001, 157569, 158138, 158710, 159284, 159860, 160439, 161019, 161601, 162186, 162772, 163361, 163952, 164545};

const int32_t DOWN[256] = {
    65536, 65299, 65064, 64830, 64596, 64363, 64131, 63900, 63670, 63440, 63212, 62984, 62757, 62531, 62305, 62081,
    61857, 61634, 61412, 61191, 60970, 60751, 60532, 60314, 60096, 59880, 59664, 59449, 59235, 59021, 58809, 58597,
    58385, 58175, 57965, 57757, 57548, 57341, 57134, 56928, 56723, 56519, 56315, 56112, 55910, 55709, 55508, 55308,
    55108, 54910, 54712, 54515, 54318, 54123, 53928, 53733, 53540, 53347, 53154, 52963, 52772, 52582, 52392, 52204,
    52015, 51828, 51641, 51455, 51270, 51085, 50901, 50717, 50535, 50353, 50171, 49990, 49810, 49631, 49452, 49274,
    49096, 48919, 48743, 48567, 48392, 48218, 48044, 47871, 47698, 47526, 47355, 47185, 47014, 46845, 46676, 46508,
    46340, 46173, 46007, 45841, 45676, 45511, 45347, 45184, 45021, 44859, 44697, 44536, 44376, 44216, 44056, 43898,
    43740, 43582, 43425, 43268, 43112, 42957, 42802, 42648, 42494, 42341, 42189, 42037, 41885, 41734, 41584, 41434,
    41285, 41136, 40988, 40840, 40693, 40546, 40400, 40254, 40109, 39965, 39821, 39677, 39534, 39392, 39250, 39108,
    38967, 38827, 38687, 38548, 38409, 38270, 38132, 37995, 37858, 37722, 37586, 37450, 37315, 37181, 37047, 36913,
    36780, 36648, 36516, 36384, 36253, 36122, 35992, 35862, 35733, 35604, 35476, 35348, 35221, 35094, 34968, 34842,
    34716, 34591, 34466, 34342, 34218, 34095, 33972, 33850, 33728, 33606, 33485, 33364, 33244, 33124, 33005, 32886,
    32768, 32649, 32532, 32415, 32298, 32181, 32065, 31950, 31835, 31720, 31606, 31492, 31378, 31265, 31152, 31040,
    30928, 30817, 30706, 30595, 30485, 30375, 30266, 30157, 30048, 29940, 29832, 29724, 29617, 29510, 29404, 29298,
    29192, 29087, 28982, 28878, 28774, 28670, 28567, 28464, 28361, 28259, 28157, 28056, 27955, 27854, 27754, 27654,
    27554, 27455, 27356, 27257, 27159, 27061, 26964, 26866, 26770, 26673, 26577, 26481, 26386, 26291, 26196, 26102};

const int32_t FINEUP[16] = {65536, 65595, 65654, 65714, 65773, 65832, 65892, 65951,
                            66011, 66071, 66130, 66190, 66250, 66309, 66369, 66429};
const int32_t FINEDOWN[16] = {65535, 65477, 65418, 65359, 65300, 65241, 65182, 65123,
                              65065, 65006, 64947, 64888, 64830, 64772, 64713, 64645};

const int8_t SINE[256] = {
    0, 2, 3, 5, 6, 8, 9, 11, 12, 14, 16, 17, 19, 20, 22, 23,
    24, 26, 27, 29, 30, 32, 33, 34, 36, 37, 38, 39, 41, 42, 43, 44,
    45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 56, 57, 58, 59,
    59, 60, 60, 61, 61, 62, 62, 62, 63, 63, 63, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 63, 63, 63, 62, 62, 62, 61, 61, 60, 60,
    59, 59, 58, 57, 56, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46,
    45, 44, 43, 42, 41, 39, 38, 37, 36, 34, 33, 32, 30, 29, 27, 26,
    24, 23, 22, 20, 19, 17, 16, 14, 12, 11, 9, 8, 6, 5, 3, 2,
    0, -2, -3, -5, -6, -8, -9, -11, -12, -14, -16, -17, -19, -20, -22, -23,
    -24, -26, -27, -29, -30, -32, -33, -34, -36, -37, -38, -39, -41, -42, -43, -44,
    -45, -46, -47, -48, -49, -50, -51, -52, -53, -54, -55, -56, -56, -57, -58, -59,
    -59, -60, -60, -61, -61, -62, -62, -62, -63, -63, -63, -64, -64, -64, -64, -64,
    -64, -64, -64, -64, -64, -64, -63, -63, -63, -62, -62, -62, -61, -61, -60, -60,
    -59, -59, -58, -57, -56, -56, -55, -54, -53, -52, -51, -50, -49, -48, -47, -46,
    -45, -44, -43, -42, -41, -39, -38, -37, -36, -34, -33, -32, -30, -29, -27, -26,
    -24, -23, -22, -20, -19, -17, -16, -14, -12, -11, -9, -8, -6, -5, -3, -2};

const int RETRIG1[16] = {0, 0, 0, 0, 0, 0, 10, 8, 0, 0, 0, 0, 0, 0, 24, 32};
const int RETRIG2[16] = {0, -1, -2, -4, -8, -16, 0, 0, 0, 1, 2, 4, 8, 16, 0, 0};
const uint32_t NOTEPER[12] = {1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 907};

uint32_t linearPeriod(int n) {
  if (n < 1 || n > 120) return 0;
  return (NOTEPER[(n - 1) % 12] * 32) >> ((n - 1) / 12);
}

// The 8-tap polyphase resampling filter (14.5), made the tracker's way.
float FIR[4096][8];

double besselI0(double x) {
  double s = 1, d = 1;
  const double q = D(X(x) * X(x));
  int k = 0;
  for (int it = 0; it < 500; ++it) {
    k += 2;
    const double t = q / double(k * k);
    d = D(X(d) * X(t));
    s = D(X(d) + X(s));
    if (!(X(d) >= X(s) * C_1E21)) break;
  }
  return s;
}

void makeFir() {
  const double beta = 9.6377, fc = 0.97;
  const double c = D(X(fc) * PI);
  const double i0b = besselI0(beta);
  for (int p = 0; p < 4096; ++p) {
    for (int j = 0; j < 8; ++j) {
      const int m = (7 - j) * 4096 + p;
      double value;
      if (m == 16384) {
        value = 1.0;
      } else {
        const double x = double(m - 16384) * 0.000244140625;
        const double w = D(sqrtl(1 - X(x) * X(x) * 0.0625L) * X(beta));
        value = D(fsin(X(c) * X(x)) * X(besselI0(w)) / (X(x) * X(i0b) * X(c)));
      }
      FIR[p][j] = S(X(fc) * X(value));
    }
  }
}

// ---- the song (description sections 2, 3)

struct Cell {
  uint8_t note = 0, ins = 0, volcmd = 0, vol = 0, pan = 255, filter = 255;
  uint16_t fx = 0, param = 0;
};

struct Pattern {
  int rows = 0;
  bool present = false;
  std::vector<Cell> cells;   // channel by channel: cell index = channel * rows + row
};

struct Envelope {
  uint8_t flags = 0;
  int n = 0, loopS = 0, loopE = 0, susS = 0, susE = 0, swing = 0;
  int pitchCentre = 0, pitchSep = 0;
  uint8_t interp = 0;
  std::vector<int> t, y, m;
};

struct Instrument {
  bool present = false;
  std::string name;
  int nna = 0, dct = 0, dna = 0, gv = 128, fadeout = 0, pan = 128;
  bool panOn = false;
  uint8_t ifc = 0, ifr = 0, ifcg = 0, flags = 0, ftype = 0;
  uint8_t noteMap[128] = {}, sampleMap[128] = {};
  Envelope env[5];
};

enum { SF_LOOP = 1, SF_SUS = 2, SF_PP = 4, SF_SUSPP = 8, SF_PAN = 16, SF_FLOAT = 32, SF_BACK = 64,
       SF_SUSBACK = 128 };

struct Sample {
  bool present = false, hasData = false;
  std::string name;
  uint8_t flags = 0;
  uint32_t length = 0, ls = 0, le = 0, ss = 0, se = 0, c4 = 8363;
  int pan = 128, vol = 256, gv = 64;
  uint8_t vibWave = 0, vibSpeed = 0, vibDepth = 0, vibRate = 0;
  int channels = 1, bits = 8;
  const uint8_t* raw = nullptr;   // the SADA data in the file
  size_t rawSize = 0;
  int pad = 0;                    // frames of padding before index 0
  std::vector<float> main, loopBuf, susBuf;
  const float* frame0(const std::vector<float>& b) const { return b.data() + size_t(pad) * channels; }
};

enum { OP_CONN = 1, OP_FINAL = 2, OP_MASTER = 3, OP_CHANNEL = 4, OP_AMP = 7, OP_EFFECT = 9 };

uint32_t tag4(const char* s) {
  return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
         uint32_t(uint8_t(s[3])) << 24;
}

struct OpDef {
  int type = 0, mode = 1;
  std::vector<int> inputs;
  int channel = -1;
  uint32_t code = 0;
  int plugNum = 0;
  std::vector<uint8_t> data;
  float gain = 1;
};

struct Plef {
  uint32_t plug = 0, param = 0, effect = 0;
};

struct ChanDef {
  std::string name;
  uint8_t flags = 0;
  int pan = 128, vol = 64;
};

struct Song {
  size_t unpacked = 0;
  int version = 0, flags = 0, speed = 6, bpm = 125, sep = 128, gv = 256, mixVol = 48, channels = 0;
  int rpb = 0;
  bool haveInfo = false;
  std::string title, message;
  std::vector<ChanDef> chan;
  int orderCount = 0;
  uint8_t order[257];
  Pattern pat[240];
  Sample smp[256];
  Instrument ins[256];
  std::vector<OpDef> ops;
  std::vector<Plef> plef;
  size_t cells = 0;
};

struct Reader {
  const uint8_t* d;
  size_t n, pos = 0;
  Reader(const uint8_t* data, size_t size) : d(data), n(size) {}
  uint8_t u8() { return pos < n ? d[pos++] : (pos++, 0); }   // zeros past the end
  uint16_t u16() { const uint16_t a = u8(); return uint16_t(a | uint16_t(u8()) << 8); }
  uint32_t u32() { const uint32_t a = u16(); return a | uint32_t(u16()) << 16; }
  int32_t i32() { return int32_t(u32()); }
  float f32() { return fbits(u32()); }
  std::string str8() {
    const size_t k = u8();
    std::string s;
    for (size_t i = 0; i < k; ++i) {
      if (pos >= n) { pos += k - i; break; }   // past the end: empty
      s += char(d[pos++]);
    }
    return pos > n ? std::string() : s;
  }
  bool past() const { return pos > n; }
  size_t left() const { return pos < n ? n - pos : 0; }
};

// aPLib (2.8.2). Reading past the end of the stream gives zeros.
std::string aplib(const uint8_t* src, size_t n, size_t size, std::vector<uint8_t>& out) {
  out.assign(size, 0);
  if (size == 0) return "";
  size_t sp = 0, dp = 0;
  unsigned tag = 0;
  int bits = 0;
  auto byte = [&]() -> uint8_t { return sp < n ? src[sp++] : (sp++, 0); };
  auto bit = [&]() -> unsigned {
    if (bits == 0) {
      tag = byte();
      bits = 8;
    }
    --bits;
    const unsigned b = (tag >> 7) & 1;
    tag = (tag << 1) & 0xFF;
    return b;
  };
  auto gamma = [&]() -> uint32_t {
    uint32_t v = 1;
    do {
      v = (v << 1) + bit();
      if (v > (1u << 28)) return v;
    } while (bit());
    return v;
  };
  bool bad = false;
  auto copy = [&](uint32_t offs, uint32_t len) {
    if (offs == 0 || offs > dp) {
      bad = true;
      return;
    }
    for (uint32_t i = 0; i < len && dp < size; ++i, ++dp) out[dp] = out[dp - offs];
  };
  out[dp++] = byte();
  bool lwm = false;
  uint32_t r0 = 0;
  while (dp < size && !bad) {
    if (sp > n + 16) break;   // the stream ran out long ago
    if (!bit()) {
      out[dp++] = byte();
      lwm = false;
    } else if (!bit()) {
      uint32_t offs = gamma(), len;
      if (!lwm && offs == 2) {
        offs = r0;
        len = gamma();
      } else {
        offs -= lwm ? 2 : 3;
        if (offs > (1u << 23)) { bad = true; break; }
        offs = offs * 256 + byte();
        len = gamma();
        if (offs >= 32000) ++len;
        if (offs >= 1280) ++len;
        if (offs < 128) len += 2;
        r0 = offs;
      }
      copy(offs, len);
      lwm = true;
    } else if (!bit()) {
      const uint32_t v = byte();
      const uint32_t offs = v >> 1, len = 2 + (v & 1);
      if (offs == 0) break;
      copy(offs, len);
      r0 = offs;
      lwm = true;
    } else {
      uint32_t offs = 0;
      for (int i = 0; i < 4; ++i) offs = (offs << 1) | bit();
      if (offs) {
        if (offs > dp) { bad = true; break; }
        out[dp] = out[dp - offs];
        ++dp;
      } else {
        out[dp++] = 0;
      }
      lwm = false;
    }
  }
  return bad ? "packed pattern data refers to data before its start" : "";
}

// The packed form: plain (u32 size, stream) or with an AP32 header.
std::string unpack(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
  if (n >= 4 && !std::memcmp(p, "AP32", 4)) {
    if (n < 24) return "packed pattern data is cut short";
    uint32_t hs, size;
    std::memcpy(&hs, p + 4, 4);
    std::memcpy(&size, p + 16, 4);
    if (hs < 24 || hs > n) return "packed pattern data has a bad header";
    if (size > MAX_UNPACKED) return "packed pattern data is too big";
    return aplib(p + hs, n - hs, size, out);
  }
  if (n < 4) return "packed pattern data is cut short";
  uint32_t size;
  std::memcpy(&size, p, 4);
  if (size > MAX_UNPACKED) return "packed pattern data is too big";
  if (size / 1024 > n + 64) return "packed pattern data unpacks to far too much";
  return aplib(p + 4, n - 4, size, out);
}

std::string readPattern(Song& song, const uint8_t* p, size_t n) {
  Reader r(p, n);
  const int num = r.u8();
  r.str8();
  const int rows = r.u16();
  const uint32_t size = r.u32();
  if (r.past()) return "a pattern is cut short";
  if (num >= 240) return "a pattern number is above 239";
  if (rows > MAX_ROWS) return "a pattern has more than 4096 rows";
  const size_t avail = r.left() < size ? r.left() : size;
  std::vector<uint8_t> raw;
  if (song.flags & 0x8000) {
    const std::string why = unpack(p + r.pos, avail, raw);
    if (!why.empty()) return why;
  } else {
    raw.assign(p + r.pos, p + r.pos + avail);
  }
  song.unpacked += raw.size();
  if (song.unpacked > MAX_UNPACKED_ALL) return "the patterns unpack to more than 256 MB";
  Pattern& pt = song.pat[num];
  const size_t total = size_t(song.channels) * size_t(rows);
  if (pt.present) song.cells -= pt.cells.size();
  song.cells += total;
  if (song.cells > MAX_CELLS) return "the patterns are too big";
  pt.present = true;
  pt.rows = rows;
  pt.cells.assign(total, Cell());
  Reader s(raw.data(), raw.size());
  Cell run;
  size_t at = 0;
  while (at < total && s.pos < s.n) {
    const uint8_t b = s.u8();
    if (b & 0x40) {
      if (b & 1) run.note = 0;
      if (b & 2) run.ins = 0;
      if (b & 4) run.volcmd = 0, run.vol = 0;
      if (b & 8) run.pan = 255, run.filter = 255;
      if (b & 16) run.fx = 0;
      if (b & 32) run.param = 0;
    } else {
      if (b & 1) run.note = s.u8();
      if (b & 2) run.ins = s.u8();
      if (b & 4) run.volcmd = s.u8(), run.vol = s.u8();
      if (b & 8) run.pan = s.u8(), run.filter = s.u8();
      if (b & 16) run.fx = s.u16();
      if (b & 32) run.param = s.u8();
    }
    const int count = (b & 0x80) ? s.u8() : 1;
    for (int k = 0; k < count && at < total; ++k) pt.cells[at++] = run;
  }
  return "";
}

std::string readPaex(Song& song, const uint8_t* p, size_t n) {
  Reader r(p, n);
  const int num = r.u8();
  const uint32_t size = r.u32();
  if (r.past()) return "a pattern's extra data is cut short";
  if (num >= 240 || !song.pat[num].present) return "";
  const size_t avail = r.left() < size ? r.left() : size;
  std::vector<uint8_t> raw;
  if (song.flags & 0x8000) {
    const std::string why = unpack(p + r.pos, avail, raw);
    if (!why.empty()) return why;
  } else {
    raw.assign(p + r.pos, p + r.pos + avail);
  }
  song.unpacked += raw.size();
  if (song.unpacked > MAX_UNPACKED_ALL) return "the patterns unpack to more than 256 MB";
  Pattern& pt = song.pat[num];
  Reader s(raw.data(), raw.size());
  uint16_t high = 0;
  size_t at = 0;
  while (at < pt.cells.size() && s.pos < s.n) {
    const uint8_t b = s.u8();
    if (b & 0x40) {
      if (b & 1) high = 0;
    } else if (b & 1) {
      high = uint16_t(s.u8() << 8);
    }
    const int count = (b & 0x80) ? s.u8() : 1;
    for (int k = 0; k < count && at < pt.cells.size(); ++k, ++at)
      pt.cells[at].param = uint16_t((pt.cells[at].param & 0xFF) | high);
  }
  return "";
}

std::string readEnvelope(Reader& r, Envelope& e) {
  e.flags = r.u8();
  e.n = r.u16();
  e.loopS = r.u16();
  e.loopE = r.u16();
  e.susS = r.u16();
  e.susE = r.u16();
  e.swing = r.u16() & 0xFF;
  e.pitchCentre = int8_t(r.u8());
  e.pitchSep = int8_t(r.u8());
  r.u8(); r.u8(); r.u8(); r.u8();   // LFO waveform, speed, depth, rate
  r.u8();                           // LFO middle
  e.interp = r.u8();
  if (e.n > MAX_NODES) return "an envelope has more than 1024 points";
  e.t.resize(size_t(e.n));
  e.y.resize(size_t(e.n));
  e.m.assign(size_t(e.n), 0x80);
  for (int i = 0; i < e.n; ++i) e.t[size_t(i)] = r.u16();
  for (int i = 0; i < e.n; ++i) e.y[size_t(i)] = r.u8();
  if (e.flags & 16)
    for (int i = 0; i < e.n; ++i) e.m[size_t(i)] = r.u8();
  if (e.n > 0) {
    if ((e.flags & 2) && (e.loopS >= e.n || e.loopE >= e.n)) return "an envelope's loop is outside its points";
    if ((e.flags & 4) && (e.susS >= e.n || e.susE >= e.n)) return "an envelope's sustain loop is outside its points";
  }
  return "";
}

bool sane(float v, float lim) { return std::isfinite(v) && std::fabs(v) <= lim; }

float cfgF(const std::vector<uint8_t>& d, size_t at) {
  uint32_t u = 0;
  if (at + 4 <= d.size()) std::memcpy(&u, &d[at], 4);
  return fbits(u);
}
int32_t cfgI(const std::vector<uint8_t>& d, size_t at) {
  uint32_t u = 0;
  if (at + 4 <= d.size()) std::memcpy(&u, &d[at], 4);
  return int32_t(u);
}
int cfgU8(const std::vector<uint8_t>& d, size_t at) { return at < d.size() ? d[at] : 0; }
int cfgU16(const std::vector<uint8_t>& d, size_t at) {
  return at + 2 <= d.size() ? d[at] | d[at + 1] << 8 : 0;
}

std::string checkPlugin(const OpDef& o) {
  const auto& d = o.data;
  auto fin = [&](size_t at, float lim) { return sane(cfgF(d, at), lim); };
  switch (o.code) {
    case 0x414C4544: {  // DELA
      if (cfgI(d, 0) < 0 || cfgI(d, 0) > 1000 || cfgI(d, 4) < 1 || cfgI(d, 4) > 256 ||
          cfgI(d, 8) < 1 || cfgI(d, 8) > 256 || cfgI(d, 12) < 0 || cfgI(d, 12) > 4096 ||
          cfgI(d, 16) < 0 || cfgI(d, 16) > 65536 || !fin(20, 4) || !fin(24, 16) || !fin(28, 16))
        return "a delay has impossible settings";
      return "";
    }
    case 0x45445453: {  // STDE
      if (cfgI(d, 0) < 0 || cfgI(d, 0) > 1000 || cfgI(d, 4) < 1 || cfgI(d, 4) > 256 ||
          cfgI(d, 8) < 1 || cfgI(d, 8) > 256 || cfgI(d, 12) < 0 || cfgI(d, 12) > 4096 ||
          cfgI(d, 16) < 0 || cfgI(d, 16) > 65536 || !fin(20, 4) || cfgI(d, 24) < 0 ||
          cfgI(d, 24) > 4096 || cfgI(d, 28) < 0 || cfgI(d, 28) > 65536 || !fin(32, 4) ||
          !fin(36, 16) || !fin(40, 16))
        return "a stereo delay has impossible settings";
      return "";
    }
    case 0x45564552: {  // REVE
      const int pre = cfgI(d, 8), extra = cfgI(d, 12), step = cfgI(d, 16), stages = cfgI(d, 28);
      if (!fin(0, 16) || !fin(4, 16) || !fin(20, 4) || !fin(24, 4) || pre < 0 || extra < 0 ||
          pre + extra > 1900 || step < 0 || step > 4000 || stages < 0)
        return "a reverb has impossible settings";
      return "";
    }
    case 0x48534950: {  // PISH
      if (!(cfgF(d, 0) >= 0 && cfgF(d, 0) <= 1) || !(cfgF(d, 4) >= 0 && cfgF(d, 4) <= 1))
        return "a pitch shifter has impossible settings";
      return "";
    }
    case 0x4F424142: {  // BABO
      for (size_t at = 0; at < 12; at += 4)
        if (!(cfgF(d, at) >= 0 && cfgF(d, at) <= 1)) return "a bass booster has impossible settings";
      return "";
    }
    case 0x51454150: {  // PAEQ
      for (int b = 0; b < 10; ++b)
        if (cfgU16(d, 10 + 2 * size_t(b)) == 0) return "an equaliser band has no frequency";
      if (cfgI(d, 4) <= 0 || cfgI(d, 4) > 16 * 65536) return "an equaliser has an impossible width";
      return "";
    }
    case 0x504D4F43: {  // COMP
      if (!(cfgF(d, 1) >= 0 && cfgF(d, 1) <= 64) || !fin(5, 64) || !(cfgF(d, 9) >= 0 && cfgF(d, 9) <= 1) || !fin(13, 64) ||
          !(cfgF(d, 17) > 1e-5f && cfgF(d, 17) <= 100) || !(cfgF(d, 21) > 1e-5f && cfgF(d, 21) <= 100) ||
          !fin(25, 64) || !fin(29, 64))
        return "a compressor has impossible settings";
      return "";
    }
    case 0x54534944:  // DIST
      if (!fin(0, 64) || !fin(4, 64) || !fin(8, 64)) return "a distortion has impossible settings";
      return "";
    default:
      return "the song uses an effect plug-in this player does not know";
  }
}

std::string readModu(Song& song, const uint8_t* p, size_t n) {
  size_t pos = 0;
  while (pos + 4 <= n) {
    int32_t size;
    std::memcpy(&size, p + pos, 4);
    pos += 4;
    if (size < 0 || size_t(size) > n - pos) return "the routing graph is damaged";
    if (song.ops.size() >= size_t(MAX_OPS)) return "the routing graph has too many parts";
    Reader r(p + pos, size_t(size));
    pos += size_t(size);
    OpDef o;
    o.type = r.u8();
    r.f32(); r.f32(); r.f32(); r.f32();
    o.mode = r.i32();
    const int32_t ni = r.i32(), no = r.i32();
    if (ni < 0 || no < 0 || size_t(ni) > r.left() / 4 || size_t(no) > r.left() / 4)
      return "the routing graph is damaged";
    for (int i = 0; i < ni; ++i) {
      const int32_t k = r.i32();
      if (k >= 0) o.inputs.push_back(k);
    }
    for (int i = 0; i < no; ++i) r.i32();
    switch (o.type) {
      case OP_CONN: case OP_FINAL: case OP_MASTER: break;
      case OP_CHANNEL:
        o.channel = r.i32();
        r.i32();
        break;
      case OP_AMP:
        o.gain = r.f32();
        if (!sane(o.gain, 1024)) return "an amplifier in the routing graph has an impossible gain";
        break;
      case OP_EFFECT: {
        o.code = r.u32();
        o.plugNum = r.i32();
        r.u8();
        r.i32();   // configuration size
        r.u8();    // kind
        const int32_t len = r.i32();
        if (len < 0 || len > 65536) return "an effect plug-in's settings are damaged";
        for (int32_t i = 0; i < len; ++i) o.data.push_back(r.u8());
        const std::string why = checkPlugin(o);
        if (!why.empty()) return why;
        break;
      }
      default:
        return "the routing graph uses external plug-ins (not supported)";
    }
    if (r.past()) return "the routing graph is cut short";
    song.ops.push_back(std::move(o));
  }
  return "";
}

std::string load(const std::vector<uint8_t>& data, Song& song) {
  if (data.size() < 8 || std::memcmp(data.data(), "BRTF", 4)) {
    if (data.size() >= 4 && !std::memcmp(data.data(), "PK\3\4", 4))
      return "a BeRoTracker song in the newer ZIP form (not supported)";
    return "not a BeRoTracker song (no BRTF signature)";
  }
  std::memset(song.order, 255, sizeof song.order);
  size_t pos = 8;
  std::vector<std::pair<const uint8_t*, size_t>> patt, paex, sada;
  while (pos + 8 <= data.size()) {
    const uint32_t tag = uint32_t(data[pos]) | uint32_t(data[pos + 1]) << 8 |
                         uint32_t(data[pos + 2]) << 16 | uint32_t(data[pos + 3]) << 24;
    uint32_t n;
    std::memcpy(&n, &data[pos + 4], 4);
    pos += 8;
    if (n > data.size() - pos) return "a part of the file runs past its end";
    const uint8_t* p = data.data() + pos;
    pos += n;
    Reader r(p, n);
    if (tag == tag4("NAME")) {
      song.title = r.str8();
    } else if (tag == tag4("MESS")) {
      const uint32_t k = r.u32();
      if (k <= r.left()) song.message.assign(reinterpret_cast<const char*>(p + 4), k);
    } else if (tag == tag4("INFO")) {
      song.haveInfo = true;
      song.version = r.u16();
      song.flags = r.u16();
      song.speed = r.u8();
      song.bpm = r.u8();
      song.sep = r.u16() & 0xFF;
      song.gv = r.u16();
      song.mixVol = r.u16();
      r.u16();
      song.channels = r.u16();
      if (song.channels < 1 || song.channels > 256) return "the song has no channels, or more than 256";
      if (song.gv > 256) song.gv = 256;
      if (song.mixVol > 128) song.mixVol = 128;
      song.chan.assign(size_t(song.channels), ChanDef());
    } else if (tag == tag4("BPMI")) {
      song.rpb = r.u8();
    } else if (tag == tag4("CHAN")) {
      const int k = r.u8();
      ChanDef c;
      c.name = r.str8();
      c.flags = r.u8();
      c.pan = r.u16();
      c.vol = r.u16();
      if (c.vol > 64) c.vol = 64;
      if (c.pan > 256) c.pan = 128;
      if (k < int(song.chan.size())) song.chan[size_t(k)] = c;
    } else if (tag == tag4("PORD")) {
      const int k = r.u16();
      if (k > 256) return "the order list is longer than 257";
      song.orderCount = k;
      for (int i = 0; i <= k; ++i) song.order[i] = r.u8();
    } else if (tag == tag4("PATT")) {
      patt.push_back({p, n});
      paex.push_back({nullptr, 0});
    } else if (tag == tag4("PAEX")) {
      if (!paex.empty()) paex.back() = {p, n};
    } else if (tag == tag4("SAIN")) {
      const int k = r.u16();
      if (k < 1 || k > 255) continue;
      Sample& s = song.smp[k];
      s = Sample();
      s.present = true;
      s.name = r.str8();
      r.str8();
      s.flags = r.u8();
      s.length = r.u32();
      s.ls = r.u32();
      s.le = r.u32();
      s.ss = r.u32();
      s.se = r.u32();
      s.c4 = r.u32();
      if (s.c4 == 0) s.c4 = 8363;
      s.pan = r.u16();
      s.vol = r.u16();
      s.gv = r.u16();
      s.vibWave = r.u8();
      s.vibSpeed = r.u8();
      s.vibDepth = r.u8();
      s.vibRate = r.u8();
      s.channels = r.u8();
      s.bits = r.u8();
      r.u8();
      if (s.pan > 256) s.pan = 256;
      if (s.vol > 256) s.vol = 256;
      if (s.gv > 64) s.gv = 64;
      if (s.c4 > MAX_C4) return "a sample has an impossible playback rate";
      if (s.flags & SF_FLOAT) return "a sample holds float data (not supported)";
      if (s.channels != 1 && s.channels != 2) return "a sample has neither one nor two channels";
      if (s.bits != 8 && s.bits != 16) return "a sample has neither 8 nor 16 bits";
      if (s.length > (64u << 20)) return "a sample is too long";
    } else if (tag == tag4("SADA")) {
      sada.push_back({p, n});
    } else if (tag == tag4("INST")) {
      const int k = r.u8();
      if (k < 1) continue;
      Instrument& in = song.ins[k];
      in = Instrument();
      in.present = true;
      in.name = r.str8();
      r.str8();
      in.nna = r.u8();
      in.dct = r.u8();
      in.dna = r.u8();
      in.gv = r.u16();
      in.fadeout = r.u16();
      in.panOn = r.u8() == 1;
      in.pan = r.u16();
      in.ifc = r.u8();
      in.ifr = r.u8();
      r.u8(); r.u8(); r.u16(); r.u8();   // MIDI
      for (int i = 0; i < 120; ++i) {
        in.noteMap[i] = r.u8();
        in.sampleMap[i] = r.u8();
      }
      in.flags = r.u8();
      in.ftype = r.u8();
      r.u32();
      for (auto& e : in.env) {
        const std::string why = readEnvelope(r, e);
        if (!why.empty()) return why;
      }
      r.u8(); r.u8();
      in.ifcg = r.u8();
      if (in.gv > 128) in.gv = 128;
      if (in.pan > 256) in.pan = 256;
      if (in.fadeout > 32768) in.fadeout = 32768;
    } else if (tag == tag4("MODU")) {
      song.ops.clear();
      const std::string why = readModu(song, p, n);
      if (!why.empty()) return why;
    } else if (tag == tag4("PLEF")) {
      const uint32_t k = r.u32();
      if (k > 4096 || size_t(k) * 13 > r.left()) return "the plug-in parameter list is damaged";
      for (uint32_t i = 0; i < k; ++i) {
        Plef e;
        e.plug = r.u32();
        e.param = r.u32();
        e.effect = r.u32();
        r.u8();
        song.plef.push_back(e);
      }
    }
  }
  if (!song.haveInfo) return "the song has no header (INFO)";
  if (song.speed < 1) song.speed = 1;   // speed 0 is not a song's
  for (size_t i = 0; i < patt.size(); ++i) {
    std::string why = readPattern(song, patt[i].first, patt[i].second);
    if (!why.empty()) return why;
    if (paex[i].first) {
      why = readPaex(song, paex[i].first, paex[i].second);
      if (!why.empty()) return why;
    }
  }
  for (const auto& sd : sada) {
    Reader r(sd.first, sd.second);
    const int k = r.u16();
    if (k < 1 || k > 255 || !song.smp[k].present) continue;
    song.smp[k].raw = sd.first + 2;
    song.smp[k].rawSize = sd.second >= 2 ? sd.second - 2 : 0;
  }
  // 3.1 fix-ups: listed patterns that are missing become 64 empty rows
  for (int i = 0; i < song.orderCount; ++i) {
    const int p = song.order[i];
    if (p < 240 && (!song.pat[p].present || song.pat[p].rows == 0)) {
      Pattern& pt = song.pat[p];
      song.cells += size_t(song.channels) * 64 - pt.cells.size();
      if (song.cells > MAX_CELLS) return "the patterns are too big";
      pt.present = true;
      pt.rows = 64;
      pt.cells.assign(size_t(song.channels) * 64, Cell());
    }
  }
  // the routing graph
  if (song.ops.empty()) {
    OpDef master, out;
    master.type = OP_MASTER;
    out.type = OP_FINAL;
    out.inputs.push_back(0);
    song.ops.push_back(master);
    song.ops.push_back(out);
  }
  int finals = 0, effects = 0;
  for (const auto& o : song.ops) {
    finals += o.type == OP_FINAL;
    effects += o.type == OP_EFFECT;
    for (int k : o.inputs)
      if (k >= int(song.ops.size())) return "the routing graph refers to a part it does not have";
  }
  if (finals != 1) return "the routing graph has no single output";
  if (effects > MAX_EFFECTS) return "the routing graph has too many effects";
  return "";
}

// 3.2 sample preparation
struct Walker {
  long long cur, lo, hi;
  int dir;
  bool pp;
  long long next() {
    const long long r = cur;
    if (!pp) {
      cur += dir;
      if (dir > 0 && cur >= hi) cur = lo;
      if (dir < 0 && cur < lo) cur = hi - 1;
    } else {
      cur += dir;
      if (cur < lo) cur = lo + 1, dir = 1;
      if (cur >= hi) cur = hi - 1, dir = -1;
    }
    return r;
  }
};

Walker forwardWalker(long long lo, long long hi, bool pp) {
  return pp ? Walker{hi - 2, lo, hi, -1, true} : Walker{lo, lo, hi, 1, false};
}
Walker backwardWalker(long long lo, long long hi, bool pp) {
  return pp ? Walker{lo + 1, lo, hi, 1, true} : Walker{hi - 1, lo, hi, -1, false};
}

void fillFrames(Sample& s, std::vector<float>& dst, const std::vector<float>& src, long long at0,
                int step, Walker w) {
  const long long lo = -s.pad, hi = (long long)s.length + s.pad;
  const int ch = s.channels;
  for (int k = 0; k <= 512; ++k) {
    long long from = w.next();
    const long long to = at0 + (long long)step * k;
    if (from < lo) from = lo;
    if (from >= hi) from = hi - 1;
    if (to < lo || to >= hi) continue;
    for (int c = 0; c < ch; ++c)
      dst[size_t(to + s.pad) * ch + c] = src[size_t(from + s.pad) * ch + c];
  }
}

std::string prepare(Song& song) {
  size_t total = 0;
  for (int k = 1; k < 256; ++k) {
    Sample& s = song.smp[k];
    if (!s.present) continue;
    if (s.le > s.length) s.le = s.length;
    if (s.se > s.length) s.se = s.length;
    if (s.ls >= s.le) s.ls = s.le = 0, s.flags &= uint8_t(~SF_LOOP);
    if (s.ss >= s.se) s.ss = s.se = 0, s.flags &= uint8_t(~SF_SUS);
    if (!s.raw || s.length == 0) continue;
    const int ch = s.channels;
    s.pad = 2048 / ch;
    const size_t frames = size_t(s.length) + size_t(2 * s.pad);
    const size_t bufs = 1 + ((s.flags & SF_LOOP) ? 1 : 0) + ((s.flags & SF_SUS) ? 1 : 0);
    total += frames * size_t(ch) * 4 * bufs;
    if (total > MAX_SAMPLE_BYTES) return "the samples need more than 512 MB";
    s.hasData = true;
    s.main.assign(frames * size_t(ch), 0.0f);
    float* m = s.main.data() + size_t(s.pad) * ch;
    const size_t bytes = size_t(s.bits / 8);
    for (size_t i = 0; i < size_t(s.length) * size_t(ch); ++i) {
      int v = 0;
      if (bytes == 1) {
        if (i < s.rawSize) v = int8_t(s.raw[i]);
        m[i] = static_cast<float>(double(v) * 0.0078125);
      } else {
        if (2 * i + 1 < s.rawSize) v = int16_t(uint16_t(s.raw[2 * i] | s.raw[2 * i + 1] << 8));
        m[i] = static_cast<float>(double(v) * 3.0517578125e-05);
      }
    }
    for (int f = 1; f <= s.pad; ++f)
      for (int c = 0; c < ch; ++c) {
        m[-(long)f * ch + c] = m[c];
        m[(long)(s.length - 1 + f) * ch + c] = m[(long)(s.length - 1) * ch + c];
      }
    if (s.flags & SF_LOOP) {
      const long long ls = s.ls, le = s.le;
      const bool pp = s.flags & SF_PP;
      s.loopBuf = s.main;
      fillFrames(s, s.loopBuf, s.main, le, 1, forwardWalker(ls, le, pp));
      fillFrames(s, s.loopBuf, s.main, ls - 1, -1, backwardWalker(ls, le, pp));
      std::vector<float> copy = s.main;
      fillFrames(s, s.main, copy, le, 1, forwardWalker(ls, le, pp));
    }
    if (s.flags & SF_SUS) {
      s.susBuf = s.main;
      fillFrames(s, s.susBuf, s.main, s.se, 1, backwardWalker(s.ss, s.se, s.flags & SF_SUSPP));
    }
  }
  return "";
}

// ---- the plug-ins (section 12)

struct Engine;

struct Plugin {
  virtual ~Plugin() {}
  virtual void run(Engine& e, float* const in[2], float* const out[2], int n, int inputs) = 0;
  virtual void setParam(uint32_t, float) {}
};

const int DELAY_LEN = 4 * RATE;

struct Delay : Plugin {
  int tempo, tpr, rpb, rows[2], ticks[2];
  float fbk[2], wet, dry;
  std::vector<float> buf[2];
  int w[2] = {0, 0};
  Delay(const std::vector<uint8_t>& d, bool stereo) {
    tempo = cfgI(d, 0);
    tpr = cfgI(d, 4);
    rpb = cfgI(d, 8);
    if (stereo) {
      rows[0] = cfgI(d, 12), ticks[0] = cfgI(d, 16), fbk[0] = cfgF(d, 20);
      rows[1] = cfgI(d, 24), ticks[1] = cfgI(d, 28), fbk[1] = cfgF(d, 32);
      wet = cfgF(d, 36), dry = cfgF(d, 40);
    } else {
      rows[0] = rows[1] = cfgI(d, 12);
      ticks[0] = ticks[1] = cfgI(d, 16);
      fbk[0] = fbk[1] = cfgF(d, 20);
      wet = cfgF(d, 24), dry = cfgF(d, 28);
    }
    buf[0].assign(DELAY_LEN, 0.0f);
    buf[1].assign(DELAY_LEN, 0.0f);
  }
  void run(Engine& e, float* const in[2], float* const out[2], int n, int) override;
};

struct Reverb : Plugin {
  float wet, dry, fb, damping;
  int preL, extra, step, stages;
  static const int M = 2 * RATE;
  std::vector<float> lineL, lineR;
  std::vector<float> ap;   // 16 stages x M stereo pairs
  int wL = 0, wR = 0, rL = 0, rR = 0, cnt[16][4] = {};
  float lpL = 0, lpR = 0;
  bool ready = false;
  explicit Reverb(const std::vector<uint8_t>& d) {
    wet = cfgF(d, 0), dry = cfgF(d, 4);
    preL = cfgI(d, 8), extra = cfgI(d, 12), step = cfgI(d, 16);
    fb = cfgF(d, 20), damping = cfgF(d, 24);
    stages = cfgI(d, 28);
    if (stages > 16) stages = 16;
    lineL.assign(M, 0.0f);
    lineR.assign(M, 0.0f);
    ap.assign(size_t(16) * M * 2, 0.0f);
  }
  void run(Engine&, float* const in[2], float* const out[2], int n, int) override {
    const float cut = S(0.5L * X(damping) * X(double(RATE) / 44100.0));
    if (!ready) {
      wL = wR = M - 4;
      rL = wL - (RATE * preL) / 1000;
      rR = wR - ((extra + preL) * RATE) / 1000;
      for (int i = 0; i < 16; ++i) {
        const int t = (i + 1) * step + i * i;
        cnt[i][0] = M - 4;
        cnt[i][1] = cnt[i][0] - t;
        cnt[i][2] = M - 4;
        cnt[i][3] = cnt[i][2] - (int(S(X(i) * C_1_3)) + t);
        for (int& c : cnt[i])
          if (c < 0) c = 0;
      }
      if (rL < 0) rL = 0;
      if (rR < 0) rR = 0;
      ready = true;
    }
    const float nfb = -fb;
    for (int i = 0; i < n; ++i) {
      if (wL >= M) wL = 0;
      if (wR >= M) wR = 0;
      if (rL >= M) rL = 0;
      if (rR >= M) rR = 0;
      const float inL = in[0][i], inR = in[1][i];
      lineL[size_t(wL)] = inL;
      lineR[size_t(wR)] = inR;
      float oL = lineL[size_t(rL)], oR = lineR[size_t(rR)];
      ++wL, ++wR, ++rL, ++rR;
      for (int s = 0; s < stages; ++s) {
        int* c = cnt[s];
        for (int q = 0; q < 4; ++q)
          if (c[q] >= M) c[q] = 0;
        float* A = &ap[size_t(s) * M * 2];
        const float xL = S(X(nfb) * X(oL) + X(A[size_t(c[1]) * 2]) + EPSX);
        A[size_t(c[0]) * 2] = S(X(fb) * X(xL) + X(oL) + EPSX);
        oL = xL;
        const float xR = S(X(nfb) * X(oR) + X(A[size_t(c[3]) * 2 + 1]) + EPSX);
        A[size_t(c[2]) * 2 + 1] = S(X(fb) * X(xR) + X(oR) + EPSX);
        oR = xR;
        for (int q = 0; q < 4; ++q) ++c[q];
      }
      lpL = S((X(oL) - X(lpL)) * X(cut) + X(lpL) + EPSX);
      lpR = S((X(oR) - X(lpR)) * X(cut) + X(lpR) + EPSX);
      out[0][i] = S(X(dry) * X(inL) + X(wet) * X(lpL));
      out[1][i] = S(X(dry) * X(inR) + X(wet) * X(lpR));
    }
  }
};

struct PitchShift : Plugin {
  float pitch, window;
  float fade[1024];
  std::vector<float> ring[2];
  uint32_t p1 = 0, p2 = 0x200000, ip = 0;
  explicit PitchShift(const std::vector<uint8_t>& d) {
    pitch = cfgF(d, 0);
    window = cfgF(d, 4);
    for (int c = 0; c < 1024; ++c)
      fade[c] = S(fcos((X(double(c) / 1023.0) - 0.5L) * 2 * K_PISH) * 0.5L + 0.5L);
    ring[0].assign(1024, 0.0f);
    ring[1].assign(1024, 0.0f);
  }
  void setParam(uint32_t k, float v) override {
    if (k == 0) pitch = v;
    else if (k == 1) window = v;
  }
  void run(Engine&, float* const in[2], float* const out[2], int n, int) override {
    const float t = (1.0f - pitch) * 48.0f - 24.0f;
    const long long semis = rnd(-X(t), -1000, 1000, 0);
    const float f = powS(2.0f, static_cast<float>(double(semis) / 12.0));
    const float x = 1.0f - window;
    const int32_t inc =
        int32_t(rnd(((0.5L - X(x)) * (X(f) * 0.25L) + X(f)) * 4096, -(1LL << 30), 1LL << 30, 0) - 4096);
    uint32_t q1 = p1, q2 = p2, qi = ip;
    for (int c = 0; c < 2; ++c) {
      q1 = p1, q2 = p2, qi = ip;
      float* r = ring[c].data();
      for (int i = 0; i < n; ++i) {
        const uint32_t u1 = q1 >> 12, u2 = q2 >> 12;
        r[qi] = S(X(in[c][i]) + EPSX);
        const float a = S(X(fade[u1 & 1023]) * X(r[(qi - u1) & 1023]) + EPSX);
        const float b = S(X(fade[u2 & 1023]) * X(r[(qi - u2) & 1023]) + EPSX);
        out[c][i] = S(X(b) + X(a) + EPSX);
        q1 -= uint32_t(inc);
        q2 -= uint32_t(inc);
        qi = (qi + 1) & 1023;
      }
    }
    p1 = q1, p2 = q2, ip = qi;
  }
};

struct BassBoost : Plugin {
  float freq, bandwidth, gain;
  bool ready = false;
  float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float h[2][4] = {};   // a1, a2, b1, b2 histories
  explicit BassBoost(const std::vector<uint8_t>& d) {
    freq = cfgF(d, 0), bandwidth = cfgF(d, 4), gain = cfgF(d, 8);
  }
  void run(Engine&, float* const in[2], float* const out[2], int n, int) override {
    if (!ready) {
      const float A = powS(10.0f, (gain * 64.0f) / 40.0f);
      const float rateF = float(RATE);
      const float w = S((PI + PI) * X(freq * 22050.0f) / X(rateF));
      const float c = S(fcos(X(w))), sn = S(fsin(X(w)));
      const float bw = S(X(powS(bandwidth, 4.0f) * 4.0f) + C_0_1);
      const float be = S(sqrtl(X(A) + X(A)) / X(bw) - X(A) * X(A));
      const float p = A + 1.0f, m = A - 1.0f;
      float B0 = S((X(p) - X(c) * X(m) + X(sn) * X(be)) * X(A));
      float B1 = S((X(m) - X(c) * X(p)) * (X(A) + X(A)));
      float B2 = S((X(p) - X(c) * X(m) - X(sn) * X(be)) * X(A));
      float A0 = S(X(c) * X(m) + X(p) + X(sn) * X(be));
      float A1 = S(-((X(c) * X(p) + X(m)) * 2));
      float A2 = S(X(c) * X(m) + X(p) - X(sn) * X(be));
      A0 = zap(A0);
      if (A0 != 0) {
        a1 = zap(S(-(X(A1) / X(A0))));
        a2 = zap(S(-(X(A2) / X(A0))));
        b0 = zap(S(X(B0) / X(A0)));
        b1 = zap(S(X(B1) / X(A0)));
        b2 = zap(S(X(B2) / X(A0)));
      }
      ready = true;
    }
    for (int ch = 0; ch < 2; ++ch) {
      float* s = h[ch];
      for (int k = 0; k < 4; ++k) s[k] = zap(s[k]);
      for (int i = 0; i < n; ++i) {
        const float x = in[ch][i];
        const float v = S(X(x) * X(b0) + X(s[2]) * X(b1) + X(s[3]) * X(b2) + X(s[0]) * X(a1) +
                          X(s[1]) * X(a2) + EPSX);
        s[1] = s[0], s[0] = v, s[3] = s[2], s[2] = x;
        out[ch][i] = v;
      }
    }
  }
};

struct Equaliser : Plugin {
  int enabled, mode, pre, width, freq[10], gain[10];
  int remFreq[10], remGain[10], remPre = -1, remMode = -1;
  float gf[10] = {}, be[10] = {}, al[10] = {}, ga[10] = {}, pf = 0;
  int i = 0, j = 2, k = 1;
  float hx[2][10][3] = {}, hy[2][10][3] = {};
  explicit Equaliser(const std::vector<uint8_t>& d) {
    enabled = cfgU8(d, 0);
    mode = cfgU8(d, 1);
    pre = cfgU16(d, 2);
    width = cfgI(d, 4);
    for (int b = 0; b < 10; ++b) {
      freq[b] = cfgU16(d, 10 + 2 * size_t(b));
      gain[b] = cfgU16(d, 30 + 2 * size_t(b));
      remFreq[b] = remGain[b] = -1;
    }
  }
  static float gainFactor(int slider) {
    const double g = (double(slider * 24) / 4800.0 - 12.0) / 20.0;
    return powS(10.0f, static_cast<float>(g));
  }
  void coefficients(int b) {
    const double f = freq[b];
    const double inv = 1.0 / double(RATE);
    auto theta = [&](double x) { return D((PI + PI) * X(x) * X(inv)); };
    const float o = float(width) * (1.0f / 65536.0f);
    const float t = S(X(double(o)) * 0.5L);
    const double q = double(powS(2.0f, t));
    const double f1 = D(X(f) / X(q));
    const double t0 = theta(f), t1 = theta(f1);
    const X c0 = fcos(X(t0)), c1 = fcos(X(t1)), s1 = fsin(X(t1));
    const double B0 = D(c0 * c0 * K4 - c1 * K3 * c0 + K4 - s1 * s1 * 0.25L);
    const double B1 = D(c1 * c1 * K1 + c0 * c0 * K2 - c1 * K1 * c0 - K2 + s1 * s1);
    const double B2 = D(c0 * c0 * K2 - c1 * K1 * c0 + K2 - s1 * s1);
    const double a = B2, bb = B1, c = B0;
    be[b] = al[b] = ga[b] = 0;
    if (a == 0) return;
    const double kk = D(X(c) - X(bb) * X(bb) / X(4 * a));
    const double h = D(-(X(bb) / (X(a) + X(a))));
    const double x2 = D(-(X(kk) / X(a)));
    if (!(x2 >= 0) || !std::isfinite(x2) || !std::isfinite(h)) return;
    const double r = D(sqrtl(X(x2)));
    const double xa = D(X(h) - X(r)), xb = D(X(r) + X(h));
    const double x = xa < xb ? xa : xb;
    be[b] = S(X(x) + X(x));
    al[b] = static_cast<float>(D((0.5L - X(x)) * 0.5L) * 2);
    ga[b] = static_cast<float>(D((X(x) + 0.5L) * fcos(X(t0))) * 2);
  }
  void run(Engine&, float* const in[2], float* const out[2], int n, int) override {
    for (int b = 0; b < 10; ++b) {
      if (freq[b] != remFreq[b]) {
        coefficients(b);
        remFreq[b] = freq[b];
      }
      if (gain[b] != remGain[b] || mode != remMode) {
        gf[b] = gainFactor(gain[b]);
        remGain[b] = gain[b];
      }
    }
    if (pre != remPre || mode != remMode) {
      pf = gainFactor(pre);
      remPre = pre;
    }
    remMode = mode;
    if (!enabled) {
      for (int c = 0; c < 2; ++c) std::memcpy(out[c], in[c], sizeof(float) * size_t(n));
      return;
    }
    for (int s = 0; s < n; ++s) {
      for (int c = 0; c < 2; ++c) {
        const float x = in[c][s];
        const float inv = S(X(pf) * X(x));
        float o = 0;
        for (int b = 0; b < 10; ++b) {
          float* Xh = hx[c][b];
          float* Yh = hy[c][b];
          Xh[i] = inv;
          const X v = (X(Xh[i]) - X(Xh[k])) * X(al[b]) + X(ga[b]) * X(Yh[j]) - X(be[b]) * X(Yh[k]) +
                      X(EPS18) - X(EPS18);
          float l = S(v);
          Yh[i] = l;
          l = S(X(l) * X(gf[b]));
          o = S(X(l) + X(o));
        }
        out[c][s] = S(X(inv) * 0.25L + X(o));
      }
      ++i, ++j, ++k;
      if (i == 3) i = 0;
      else if (j == 3) j = 0;
      else if (k == 3) k = 0;
    }
  }
};

struct Compressor : Plugin {
  float threshold, rc, lookahead, kc, att, rel, ig, og;
  float env[2] = {1, 1}, g[2] = {1, 1}, sum = 0, sum2 = 0;
  std::vector<float> ring;
  int idx = 0;
  explicit Compressor(const std::vector<uint8_t>& d) {
    threshold = cfgF(d, 1), rc = cfgF(d, 5), lookahead = cfgF(d, 9), kc = cfgF(d, 13);
    att = cfgF(d, 17), rel = cfgF(d, 21), ig = cfgF(d, 25), og = cfgF(d, 29);
    ring.assign(RATE, 0.0f);
  }
  static float clip(float x, float lo, float hi) {
    const float a = S(fabsl(X(x) - X(lo)));
    const float b = S(fabsl(X(x) - X(hi)));
    return S((X(hi) + X(lo) + X(a) - X(b)) * 0.5L);
  }
  void run(Engine&, float* const in[2], float* const out[2], int n, int inputs) override {
    for (int c = 0; c < 2; ++c) std::memcpy(out[c], in[c], sizeof(float) * size_t(n));
    if (inputs == 0) return;
    const float rateF = float(RATE);
    const float attack = S(xexpv(X(-1.0f / (rateF * att))));
    const float release = S(xexpv(X(-1.0f / (rateF * rel))));
    int la = int(rateF * lookahead);
    if (la < 1) la = 1;
    if (la > RATE) la = RATE;
    const float invLa = static_cast<float>(1.0 / double(la));
    const float r441 = static_cast<float>(double(RATE) / 44100.0);
    const float minKnee = powS(F3E5, r441);
    const float knee = zap(clip(powS(clip(S(X(kc) * X(kc)), F3E5, 1.0f), r441), minKnee, 1.0f));
    float ratio = rc * 64.0f;
    if (!(ratio >= 1.0f)) ratio = 1.0f;
    ratio = 1.0f / ratio;
    const float gainOut = S(X(og) * 8), gainIn = S(X(ig) * 8);
    if (idx >= la) idx = 0;
    for (int i = 0; i < n; ++i) {
      float& L = out[0][i];
      float& R = out[1][i];
      const X e = (X(R) + X(L)) * 0.5L * X(gainIn);
      const float s2 = S(e * e);
      sum = S(X(sum) - X(ring[size_t(idx)]) + X(s2));
      sum2 = S(X(s2) + X(sum2));
      ring[size_t(idx)] = s2;
      if (++idx >= la) idx = 0, sum = sum2, sum2 = 0;
      float rms = S(X(invLa) * X(sum));
      rms = X(rms) > EPS18X ? S(sqrtl(X(rms))) : 0.0f;
      for (int c = 0; c < 2; ++c) {
        env[c] = zap(env[c]);
        const float r = std::fabs(zap(rms));
        const float coef = r > env[c] ? attack : release;
        env[c] = S(X(r) * X(1.0f - coef) + X(env[c]) * X(coef));
        const float target = threshold >= env[c] ? 1.0f : powS(S(X(threshold) / X(env[c])), 1.0f - ratio);
        g[c] = zap(S((X(target) - X(g[c])) * X(knee) + X(g[c])));
      }
      L = S(X(g[0]) * X(L) * X(gainIn) * X(gainOut) + EPS18X - EPS18X);
      R = S(X(g[1]) * X(R) * X(gainIn) * X(gainOut) + EPS18X - EPS18X);
    }
  }
};

struct Distortion : Plugin {
  float thr, gain, phase;
  explicit Distortion(const std::vector<uint8_t>& d) {
    thr = cfgF(d, 0), gain = cfgF(d, 4), phase = cfgF(d, 8);
  }
  void run(Engine&, float* const in[2], float* const out[2], int n, int) override {
    float g = gain * 8.0f;
    for (int c = 0; c < 2; ++c) {
      for (int i = 0; i < n; ++i) {
        float x = in[c][i];
        if (x > thr) x = thr;
        if (x < -thr) x = -thr;
        out[c][i] = S(X(g) * X(x));
      }
      if (0.5f > phase) g = -g;
    }
  }
};

// ---- the engine (sections 4 to 13)

struct Voice {
  // row fields (pattern channels)
  Cell row;
  int vol = 0;           // 0..256
  float cgv = 64;
  int pan = 128, effPan = 128;
  bool surround = false, muted = false, disabled = false, background = false;
  int owner = -1;
  int pendingIns = 0, noteNew = 0, note = 0;
  int64_t period = 0, extraPeriod = 0, portaTarget = 0;
  int portaSpeed = 0, memSlide = 0, memVol = 0, memOffset = 0, memRetrig = 0, retrigCount = 0;
  const Instrument* ins = nullptr;
  const Sample* sample = nullptr;
  const Sample* mixSample = nullptr;
  int64_t length = 0, loopStart = 0, loopEnd = 0;
  uint8_t sflags = 0;
  bool ppDir = false, loopTrig = false, susTrig = false, paused = false;
  uint32_t c4 = 8363;
  int64_t pos = 0, inc = 0;
  float L = 0, R = 0, NL = 0, NR = 0, RL = 0, RR = 0;
  int rampLen = 0;
  bool ramping = false, fastRamp = false;
  float realVol = 0, instVol = 0;
  int volRand = 0, panRand = 0, pitchRand = 0;
  bool fading = false, keyedOff = false;
  int fadeVol = 65536;
  bool envOn[5] = {};
  int envPos[5] = {};
  int nna = 0;
  int avSweep = 0, avDepth = 0, avPos = 0;
  // filter
  float cutoff = 127, cutNow = 127, res = 0, resNow = 0;
  int ftype = 0;
  bool fActive = false;
  int glideLen = 0, glideCount = 0;
  bool glideOn = false;
  float glideStep = 0;
  float fef = 256, resEnv = 0;
  bool feApplied = false;
  float A0 = 0, A1 = 0, A2 = 0, B0 = 0, B1 = 0, B2 = 0;
  float fh[8] = {};   // y1 y2 y3 y4 (IT) / a1L a2L b1L b2L a1R a2R b1R b2R (biquad)
  float lastL = 0, lastR = 0;
};

struct OpRun {
  const OpDef* def;
  std::vector<float> buf[2], obuf[2];
  float accL = 0, accR = 0;
  bool todo = false;
  int inputs = 0;
  std::unique_ptr<Plugin> plug;
};

struct Engine {
  Song& song;
  bool timing;   // only the clock runs (to measure the song)
  std::vector<Voice> ch;
  std::vector<Voice> pool;
  std::vector<Voice*> mixList;
  std::vector<OpRun> ops;
  std::vector<int> chanOp;   // per pattern channel: has a channel operator
  int finalOp = -1;
  // song state
  int speed, bpm, fbpm = 0, tpr, rpb;
  int tick, absTick = 0, patDelayCnt = 0;
  int curRow = 0, nextRow = 0, curOrder = 0, nextOrder = 0, curPat = 0;
  bool ended = false, tickFirst = false, firstTick = true;
  int gv;
  float gvCur, gvTarget, gvStep = 0;
  int64_t tickLen = 0;
  uint32_t tickFrac = 0;
  uint64_t fracAcc = 0;
  int64_t remain = 0;
  int plugBpm = 0, plugFbpm = 0;
  uint32_t rstate = 123;
  float CR;
  float gain;
  std::vector<float> work[2];
  std::vector<int> tempoChans;   // channels whose row sets speed or tempo

  Engine(Song& s, bool timingOnly) : song(s), timing(timingOnly) {
    speed = s.speed;
    bpm = s.bpm < 1 ? 1 : s.bpm > 255 ? 255 : s.bpm;
    tpr = s.speed;
    rpb = s.rpb;
    tick = speed;
    gv = s.gv;
    gvCur = gvTarget = float(gv) * (1.0f / 256.0f);
    CR = static_cast<float>(1.0 - 0.008 * 44100.0 / double(RATE));
    gain = S(1.0L * X(float(s.mixVol) * 0.0078125f));
    ch.resize(size_t(s.channels));
    for (int c = 0; c < s.channels; ++c) {
      Voice& v = ch[size_t(c)];
      const ChanDef& d = s.chan[size_t(c)];
      v.vol = d.vol;
      v.cgv = float(d.vol);
      v.pan = d.pan;
      v.surround = d.flags & 1;
      v.muted = d.flags & 2;
      v.disabled = d.flags & 4;
    }
    pool.resize(POOL);
    computeTickLen();
    if (timing) return;
    work[0].assign(BLOCK, 0.0f);
    work[1].assign(BLOCK, 0.0f);
    chanOp.assign(size_t(s.channels), 0);
    ops.resize(s.ops.size());
    for (size_t i = 0; i < s.ops.size(); ++i) {
      OpRun& o = ops[i];
      o.def = &s.ops[i];
      o.buf[0].assign(BLOCK, 0.0f);
      o.buf[1].assign(BLOCK, 0.0f);
      if (o.def->type == OP_FINAL) finalOp = int(i);
      if (o.def->type == OP_CHANNEL && o.def->channel >= 0 && o.def->channel < s.channels)
        chanOp[size_t(o.def->channel)] = 1;
      if (o.def->type == OP_EFFECT) {
        o.obuf[0].assign(BLOCK, 0.0f);
        o.obuf[1].assign(BLOCK, 0.0f);
        const auto& d = o.def->data;
        switch (o.def->code) {
          case 0x414C4544: o.plug.reset(new Delay(d, false)); break;
          case 0x45445453: o.plug.reset(new Delay(d, true)); break;
          case 0x45564552: o.plug.reset(new Reverb(d)); break;
          case 0x48534950: o.plug.reset(new PitchShift(d)); break;
          case 0x4F424142: o.plug.reset(new BassBoost(d)); break;
          case 0x51454150: o.plug.reset(new Equaliser(d)); break;
          case 0x504D4F43: o.plug.reset(new Compressor(d)); break;
          case 0x54534944: o.plug.reset(new Distortion(d)); break;
          default: break;
        }
      }
    }
  }

  // 5.1
  void computeTickLen() {
    if (rpb > 0 && speed > 0 && tpr > 0) {
      const uint64_t divisor = uint64_t(speed) * uint64_t(rpb) * uint64_t(bpm) * uint64_t(tpr) / uint64_t(speed);
      uint64_t R;
      if (fbpm == 0) {
        R = divisor ? (uint64_t(RATE) * 60 << 32) / divisor : (uint64_t(1) << 32);
      } else {
        const uint64_t B = uint64_t(bpm) * 65536 + uint64_t(fbpm);
        const uint64_t dv = uint64_t(speed) * uint64_t(rpb) * B * uint64_t(tpr) / uint64_t(speed);
        const unsigned __int128 q = dv ? ((unsigned __int128)(uint64_t(RATE) * 60) << 32) / dv : 0;
        R = uint64_t(q << 16);
      }
      if (R < (uint64_t(1) << 32)) R = uint64_t(1) << 32;
      tickLen = int64_t(R >> 32);
      tickFrac = uint32_t(R & 0xFFFFFFFFu);
    } else {
      tickLen = (RATE * 60) / (bpm * 24);
      if (tickLen < 1) tickLen = 1;
      tickFrac = 0;
    }
  }

  // 6.4
  void clampTempo() {
    int64_t t = int64_t(bpm) * 65536 + fbpm;
    if (t <= 65536) t = 65536;
    if (t >= 255 * 65536) t = 255 * 65536;
    bpm = int(t >> 16);
    fbpm = int(t & 0xFFFF);
  }

  uint32_t rnd16() {
    uint32_t r = rstate * 123u;
    r = ((r & 0xFF) << 8) | ((r >> 8) & 0xFF);
    rstate = r;
    return r;
  }

  const Instrument* instrument(int k) const {
    return k >= 1 && k <= 255 && song.ins[k].present ? &song.ins[k] : nullptr;
  }
  const Sample* sampleNo(int k) const {
    return k >= 1 && k <= 255 && song.smp[k].present ? &song.smp[k] : nullptr;
  }

  // ---- one tick (section 6)
  void runTick() {
    tickFirst = false;
    ++absTick;
    ++tick;
    if (!(speed > tick)) {
      tick = 0;
      if (--patDelayCnt <= 0) {
        absTick = 0;
        patDelayCnt = 0;
        tickFirst = true;
      }
    }
    if (tickFirst) advanceRow();
    if (!ended) {
      patternEffects();
      if (!timing) {
        plugParams();
        for (int c = 0; c < song.channels; ++c) channelStep(ch[size_t(c)]);
        clampTempo();
        // the tick's own length, for the ramps of 8.6 (a tempo change counts at once there;
        // the references need this)
        computeTickLen();
        mixList.clear();
        for (auto& v : ch) update(v);
        for (auto& v : pool) update(v);
      } else {
        clampTempo();
      }
    }
    if (!timing) gvTarget_();
  }

  void gvTarget_() {
    gvTarget = float(gv) * 0.00390625f;
    if (firstTick) {
      gvCur = gvTarget;
      firstTick = false;
    }
    if (gvCur != gvTarget) {
      const double den = double(tickFrac) * dbits(0x3DF0000000000000ULL) + double(tickLen);
      gvStep = S((X(gvTarget) - X(gvCur)) / X(den));
    } else {
      gvStep = 0;
    }
  }

  const Cell& cellAt(int p, int r, int c) const {
    static const Cell empty;
    const Pattern& pt = song.pat[p];
    if (!pt.present || r >= pt.rows) return empty;
    return pt.cells[size_t(c) * size_t(pt.rows) + size_t(r)];
  }

  // 6.2
  void advanceRow() {
    curRow = nextRow;
    if (curOrder != nextOrder) curOrder = nextOrder;
    if (curOrder > 256) {
      ended = true;
      return;
    }
    curPat = song.order[curOrder];
    while (curPat >= 240) {
      if (curPat == 255 || curOrder >= 256) {
        ended = true;
        return;
      }
      ++curOrder;
      curPat = song.order[curOrder];
    }
    nextOrder = curOrder;
    nextRow = curRow + 1;
    if (nextRow >= song.pat[curPat].rows) {
      nextOrder = curOrder + 1;
      nextRow = 0;
    }
    tempoChans.clear();
    for (int k = 0; k < song.channels; ++k) {
      Voice& v = ch[size_t(k)];
      v.row = cellAt(curPat, curRow, k);
      if (v.row.fx == 1 || v.row.fx == 20) tempoChans.push_back(k);
      v.L = v.NL;
      v.R = v.NR;
    }
  }

  // 7.1
  void patternEffects() {
    for (int k : tempoChans) {
      Voice& v = ch[size_t(k)];
      if (v.disabled) continue;
      if (v.row.fx == 1) {
        const int p = v.row.param & 0xFF;
        if (tickFirst && p >= 1) speed = p;
      } else if (v.row.fx == 20) {
        const int P = v.row.param;
        if (P != 0 && tickFirst && P >= 32) bpm = P > 255 ? 255 : P, fbpm = 0;
        clampTempo();
      }
    }
  }

  // 7.6
  void plugParams() {
    if (song.plef.empty()) return;
    for (auto& v : ch) {
      if (v.disabled) continue;
      const int e = v.row.fx;
      if (e < 1 || e > 1295) continue;
      for (const Plef& p : song.plef) {
        if (p.effect != uint32_t(e)) continue;
        for (auto& o : ops) {
          if (o.plug && uint32_t(o.def->plugNum) == p.plug) {
            o.plug->setParam(p.param, static_cast<float>(double(v.row.param & 0xFF) * (1.0 / 255.0)));
            break;
          }
        }
      }
    }
  }

  // 7.2
  void channelStep(Voice& c) {
    if (c.disabled) return;
    c.fastRamp = false;
    bool porta = c.row.fx == 7;
    int tickBegin = 0;
    if (c.row.fx == 19 && (c.row.param & 0xF0) == 0xD0 && (c.row.param & 15) > 0) tickBegin = c.row.param & 15;
    if ((tickBegin == 0 && tickFirst) || (tickBegin > 0 && tick == tickBegin)) rowWork(c, porta);
    effect(c);
  }

  void filterColumn(Voice& c) {
    const int f = c.row.filter;
    if (f >= 128) return;
    const float old = c.cutoff;
    c.cutoff = float(f);
    const float diff = std::fabs(S(X(old) - X(c.cutoff)));
    if (c.vol > 0 || diff < 16 || !c.fActive || c.L == 0 || c.R == 0)
      filterSetup(c, !c.fActive, true, false, false);
  }

  // 7.2.1
  void rowWork(Voice& c, bool porta) {
    const int note = c.row.note;
    int ins = c.row.ins;
    if (ins != 0) c.pendingIns = ins;
    if (note >= 253) ins = 0;
    if (note >= 1 && note <= 128) {
      c.noteNew = note;
      if (!porta) nnaCheck(c, ins, note, false);
    }
    if (ins != 0) {
      const Sample* was = c.sample;
      triggerInstrument(c, ins, porta);
      c.pendingIns = 0;
      if (c.sample != was && note >= 1 && note <= 120) porta = false;
    }
    if (note != 0) startNote(c, note, porta, true);
    if (c.row.volcmd == 1 && c.row.vol <= 64) {
      c.vol = c.row.vol * 4;
      c.fastRamp = true;
    }
    filterColumn(c);
  }

  Voice* freeVoice() {
    for (auto& v : pool)
      if (v.length == 0) return &v;
    return nullptr;
  }

  void copyToBackground(Voice& v, const Voice& c) {
    v = c;
    v.muted = false;
    v.disabled = false;
    v.owner = int(&c - ch.data());
    v.background = true;
  }

  // 7.3.3
  void keyOff(Voice& v) {
    const bool wasOn = !v.keyedOff;
    v.keyedOff = true;
    if (!v.fading) v.fading = v.ins ? !v.envOn[0] : false;
    if (v.length != 0) {
      if ((v.sflags & SF_SUS) && v.sample && wasOn) {
        v.sflags &= uint8_t(~(SF_SUS | SF_SUSPP));
        const Sample& s = *v.sample;
        if (s.flags & SF_LOOP) {
          v.sflags = uint8_t((v.sflags & ~SF_PP) | (s.flags & (SF_LOOP | SF_PP)));
          v.loopStart = s.ls;
          v.loopEnd = s.le;
          v.length = s.le;
        } else {
          v.sflags &= uint8_t(~(SF_LOOP | SF_PP));
          v.loopStart = 0;
          v.loopEnd = s.length;
          v.length = s.length;
        }
        v.loopTrig = v.susTrig = false;
        v.ppDir = false;
      }
      if (v.ins && !v.fading) v.fading = (v.ins->env[0].flags & 2) && v.ins->fadeout != 0;
    }
  }

  void noteCut(Voice& v, bool fast) {
    v.fading = true;
    v.fadeVol = 0;
    v.fastRamp = v.fastRamp || fast;
  }

  // 7.3.4
  void duplicateCheck(Voice& w, const Instrument* CI, const Sample* CS, int n) {
    const Instrument* WI = w.ins;
    if (!WI) return;
    bool dup = false;
    switch (WI->dct) {
      case 1: dup = n != 0 && w.note == n && CI == WI; break;
      case 2: dup = CS && CS == w.sample; break;
      case 3: dup = CI == WI; break;
      default: break;
    }
    if (dup) {
      switch (WI->dna) {
        case 0: keyOff(w); w.vol = 0; break;
        case 1: keyOff(w); break;
        case 2: w.fading = true; break;
        default: break;
      }
      if (w.vol == 0) noteCut(w, true);
    }
  }

  // 7.3
  void nnaCheck(Voice& c, int ins, int n, bool forceCut) {
    if (n < 1 || n > 120) return;
    if (forceCut) {
      if (c.length != 0 && !c.muted && !(c.L == 0 && c.R == 0)) {
        if (Voice* v = freeVoice()) {
          copyToBackground(*v, c);
          noteCut(*v, true);
        }
        c.length = 0;
        c.pos = 0;
        c.L = c.R = 0;
        c.lastL = c.lastR = 0;
        c.paused = false;
      }
      return;
    }
    const Sample* CS = c.sample;
    const Instrument* CI = c.ins;
    if (ins != 0) CI = instrument(ins);
    if (CI) {
      const int sn = CI->sampleMap[n - 1];
      if (sn >= 1 && sn <= 511) CS = sampleNo(sn);
      n = CI->noteMap[n - 1];
    }
    if (!c.ins) return;
    duplicateCheck(c, CI, CS, n);
    const int me = int(&c - ch.data());
    for (auto& v : pool)
      if (v.length != 0 && v.owner == me) duplicateCheck(v, CI, CS, n);
    if (c.muted) return;
    if (c.vol == 0 || c.length == 0) return;
    if (Voice* v = freeVoice()) {
      copyToBackground(*v, c);
      switch (c.nna) {
        case 0: v->fadeVol = 0; v->fading = true; break;
        case 2: keyOff(*v); break;
        case 3: v->fading = true; break;
        default: break;
      }
    }
    c.length = 0;
    c.pos = 0;
    c.lastL = c.lastR = 0;
    c.paused = false;
  }

  void resetEnvelopes(Voice& c, bool all) {
    for (int e = 0; e < 5; ++e)
      if (all || !c.ins || !(c.ins->env[e].flags & 8)) c.envPos[e] = 0;
  }

  void setLoop(Voice& c, const Sample* s) {
    if (c.sflags & SF_SUS) {
      c.loopStart = s->ss;
      c.loopEnd = s->se;
      c.sflags = uint8_t((c.sflags | SF_LOOP) & ~SF_PP);
      if (c.sflags & SF_SUSPP) c.sflags |= SF_PP;
    } else if (c.sflags & SF_LOOP) {
      c.loopStart = s->ls;
      c.loopEnd = s->le;
    }
  }

  // 7.4
  void triggerInstrument(Voice& c, int insNo, bool porta) {
    const Instrument* AI = instrument(insNo);
    const Sample* ES = sampleNo(insNo);
    const int note = c.noteNew;
    if (AI && note >= 1 && note <= 128) {
      if (porta && AI == c.ins && c.sample && c.sample->hasData) {
        ES = c.sample;
      } else {
        const int mn = AI->noteMap[note - 1];
        if (mn == 254 || mn == 255) return;
        c.sflags &= uint8_t(~(SF_SUS | SF_SUSPP | SF_SUSBACK));
        ES = sampleNo(AI->sampleMap[note - 1]);
      }
    }
    bool changed = c.ins != AI;
    if (c.inc == 0) changed = true;
    c.ins = AI;
    if (ES) c.vol = ES->vol;
    c.pendingIns = 0;
    c.instVol = S(X(double(AI ? AI->gv : 128) * 0.0078125) * X(double(ES ? ES->gv : 64) * 0.015625));
    if (AI && AI->panOn) c.pan = AI->pan, c.surround = false;
    if (ES && (ES->flags & SF_PAN)) c.pan = ES->pan, c.surround = false;
    const bool reset = !(porta && c.length != 0 && !(c.fading && c.fadeVol == 0));
    const bool resetAll = changed || c.fading || c.keyedOff;
    if (reset) {
      c.fastRamp = true;
      resetEnvelopes(c, resetAll);
    }
    if (porta && c.sample == ES && c.length != 0) return;
    c.fading = false;
    c.keyedOff = false;
    for (bool& b : c.envOn) b = false;
    c.fef = 256.0f;
    c.resEnv = 0;
    if (ES) {
      c.sflags = uint8_t(ES->flags & (SF_LOOP | SF_PP | SF_SUS | SF_SUSPP | SF_BACK | SF_SUSBACK | SF_PAN));
    } else {
      c.sflags = 0;
    }
    if (c.sample != ES || changed) c.ppDir = c.loopTrig = c.susTrig = false;
    if (AI)
      for (int e = 0; e < 5; ++e) c.envOn[e] = AI->env[e].flags & 1;
    if (AI && !(AI->ftype & 0x80) && AI->ftype != c.ftype) {
      c.ftype = AI->ftype;
      filterSetup(c, true, false, false, false);
    }
    if (AI) {
      if (AI->ifc & 0x80) c.cutoff = float(AI->ifc & 127);
      if (AI->ifr & 0x80) c.res = float(AI->ifr & 127);
      if (AI->ifcg & 0x80) c.glideLen = AI->ifcg & 127;
    }
    c.volRand = c.panRand = c.pitchRand = 0;
    c.sample = c.mixSample = ES;
    c.length = ES && ES->hasData ? ES->length : 0;
    c.loopStart = ES ? ES->ls : 0;
    c.loopEnd = ES ? ES->le : 0;
    c.paused = false;
    c.c4 = ES ? ES->c4 : 8363;
    c.avSweep = c.avDepth = c.avPos = 0;
    c.portaTarget = 0;
    if (ES && (c.sflags & SF_SUS)) {
      c.loopStart = ES->ss;
      c.loopEnd = ES->se;
      c.sflags = uint8_t((c.sflags | SF_LOOP) & ~SF_PP);
      if (c.sflags & SF_SUSPP) c.sflags |= SF_PP;
    }
    if ((c.sflags & SF_LOOP) && c.loopEnd < c.length) c.length = c.loopEnd;
  }

  // 7.5
  void startNote(Voice& c, int n, bool porta, bool resetEnv) {
    const Sample* ES = c.sample;
    const Instrument* AI = c.ins;
    if (AI && n >= 1 && n <= 120) {
      ES = sampleNo(AI->sampleMap[n - 1]);
      n = AI->noteMap[n - 1];
      c.sflags &= uint8_t(~(SF_SUS | SF_SUSPP | SF_SUSBACK));
    }
    if (n > 120) {
      if (n == 254) {
        keyOff(c);
        noteCut(c, true);
        c.vol = 0;
        c.fadeVol = 0;
      } else if (n == 255) {
        keyOff(c);
      }
      return;
    }
    if (!porta) c.c4 = ES ? ES->c4 : 8363;
    c.note = n;
    const int64_t period = linearPeriod(n);
    if (period != 0) {
      if (!(porta && c.period != 0)) c.period = period;
      if (!(porta && c.extraPeriod != 0)) c.extraPeriod = period;
      c.portaTarget = period;
      if (!(porta && c.length != 0)) {
        c.sample = c.mixSample = ES;
        c.length = ES && ES->hasData ? ES->length : 0;
        c.paused = false;
        c.loopStart = 0;
        c.loopEnd = ES ? ES->length : 0;
        c.sflags = ES ? uint8_t(ES->flags & (SF_LOOP | SF_PP | SF_SUS | SF_SUSPP | SF_BACK | SF_SUSBACK | SF_PAN)) : 0;
        c.ppDir = c.loopTrig = c.susTrig = false;
        if (ES && (c.sflags & (SF_SUS | SF_LOOP))) {
          setLoop(c, ES);
          if (c.loopEnd < c.length) c.length = c.loopEnd;
        }
        c.pos = 0;
      }
    }
    if (!(porta && !(c.fading && c.fadeVol == 0))) {
      c.fading = false;
      c.fadeVol = 65536;
    }
    if (!porta) c.keyedOff = false;
    if (porta) return;
    c.fastRamp = true;
    c.retrigCount = 0;
    if (resetEnv) {
      c.volRand = c.panRand = c.pitchRand = 0;
      if (AI) {
        c.nna = AI->nna;
        resetEnvelopes(c, false);
        if (AI->env[0].swing) c.volRand = AI->env[0].swing * (int(rnd16() & 255) - 127) / 100;
        if (AI->env[1].swing) c.panRand = AI->env[1].swing * (int(rnd16() & 255) - 127) / 100;
      }
      c.avSweep = c.avDepth = c.avPos = 0;
    }
    c.L = c.R = 0;
    // "active" is the set-up decision here; the voice's filter keeps its state (the
    // references need this reading)
    if (AI) {
      bool active = false, newCut = false, newRes = false;
      if (AI->ifc & 0x80) c.cutoff = float(AI->ifc & 127), active = true, newCut = true;
      if (AI->ifr & 0x80) {
        c.res = float(AI->ifr & 127);
        if (c.fActive) active = true;
        newRes = true;
      }
      if (AI->ifcg & 0x80) c.glideLen = AI->ifcg & 127;
      if (active) filterSetup(c, true, newCut, newRes, false);
    }
  }

  // 7.7
  void effect(Voice& c) {
    const int p = c.row.param & 0xFF;
    const int P = c.row.param;
    switch (c.row.fx) {
      case 4: {
        int q = p;
        if (q) c.memVol = q;
        else q = c.memVol;
        if (!tickFirst) {
          const int x = q & 15, hi = q >> 4;
          if (x != 0 && hi == 0) c.vol -= 4 * x;
          else if (hi != 0 && x == 0) c.vol += 4 * hi;   // slide up: not in the description's songs
          if (c.vol < 0) c.vol = 0;
          if (c.vol > 256) c.vol = 256;
        }
        break;
      }
      case 5:
      case 6: {
        int q = p;
        if (q) c.memSlide = q;
        else q = c.memSlide;
        if ((!tickFirst || speed == 1) && c.period != 0) {
          const int64_t f = c.row.fx == 6 ? DOWN[q & 255] : UP[q & 255];
          c.period = (c.period * f + 32768) / 65536;
          if (c.period > PERIOD_MAX) c.period = PERIOD_MAX;   // far below hearing long before
          if (c.period <= 0) {
            c.period = 1;
            c.fading = true;
            c.fadeVol = 0;
          }
        }
        break;
      }
      case 7: {
        if (p) c.memSlide = p, c.portaSpeed = 4 * p;
        if (c.period == c.portaTarget) c.portaTarget = 0;
        // the slide itself (not covered by the songs; section 16): linear steps
        if (!tickFirst && c.portaTarget != 0 && c.period != 0) {
          int q = c.portaSpeed / 4;
          if (q > 255) q = 255;
          if (c.period < c.portaTarget) {
            c.period = (c.period * UP[q] + 32768) / 65536;
            if (c.period >= c.portaTarget) c.period = c.portaTarget, c.portaTarget = 0;
          } else {
            c.period = (c.period * DOWN[q] + 32768) / 65536;
            if (c.period <= c.portaTarget) c.period = c.portaTarget, c.portaTarget = 0;
          }
          if (c.period < 1) c.period = 1;
        }
        break;
      }
      case 15:
        if (tickFirst) {
          int q = P;
          if (q) c.memOffset = q;
          else q = c.memOffset;
          const int64_t off = int64_t(q) * 256;
          if (c.row.note >= 1 && c.row.note <= 120) c.pos = off * F;
        }
        break;
      case 17: {
        int q = p;
        if (q) c.memRetrig = q;
        else q = c.memRetrig;
        if (tickFirst && c.row.note != 0) {
          c.retrigCount = q & 15;
        } else {
          bool go = false;
          if (c.retrigCount == 0) {
            go = true;
          } else if (--c.retrigCount == 0) {
            go = true;
          }
          if (go) {
            c.retrigCount = q & 15;
            retrigger(c, q);
          }
        }
        break;
      }
      case 22:
        if (p <= 128) gv = 2 * p;
        break;
      case 41:
        if (tickFirst && (c.pos >> 32) == 0 && c.length != 0 && !(c.sflags & SF_LOOP)) {
          c.pos = (c.length - 1) * F + 0xFFFFFFFFLL;
          c.ppDir = true;
        }
        break;
      default:
        break;
    }
  }

  void retrigger(Voice& c, int q) {
    const int d = q >> 4;
    if (d != 0) {
      if (RETRIG1[d]) c.vol = (c.vol * RETRIG1[d]) >> 4;
      else c.vol += 4 * RETRIG2[d];
      if (c.vol < 0) c.vol = 0;
      if (c.vol > 256) c.vol = 256;
      c.fastRamp = true;
    }
    if (c.noteNew >= 1 && c.noteNew <= 120 && c.length != 0) nnaCheck(c, 0, c.noteNew, true);
    if (c.noteNew != 0) startNote(c, c.noteNew, false, false);
    filterColumn(c);
  }

  // ---- 8.3
  float envValue(const Envelope& e, int pos) const {
    const int n = e.n;
    int k = n - 1;
    for (int i = 0; i < n - 1; ++i)
      if (e.t[size_t(i)] >= pos) {
        k = i;
        break;
      }
    const int x2 = e.t[size_t(k)];
    const float y2 = float(e.y[size_t(k)]);
    int x1, k1;
    float y1;
    if (pos >= x2 || k == 0) {
      x1 = x2, y1 = y2, k1 = k;
    } else {
      x1 = e.t[size_t(k - 1)], y1 = float(e.y[size_t(k - 1)]), k1 = k - 1;
    }
    if (pos > x2) pos = x2;
    int mode = e.m[size_t(k1)];
    if (mode == 0x80) mode = e.interp;
    const float y0 = k1 > 0 ? float(e.y[size_t(k1 - 1)]) : y1;
    const float y3 = k < n - 1 ? float(e.y[size_t(k + 1)]) : y2;
    if (x2 > x1 && pos > x1) {
      const float f = static_cast<float>(double(pos - x1) / double(x2 - x1));
      switch (mode) {
        case 1:
          return static_cast<float>(double(int64_t(pos - x1) * int64_t(int(y2) - int(y1))) / double(x2 - x1) +
                                    double(y1));
        case 2: {
          const float FF = S((1 - fcos(PI * X(f))) * 0.5L);
          return S(X(y1) * X(1.0f - FF) + X(FF) * X(y2));
        }
        case 3: {
          const float FF = S(X(f) * X(f));
          const float a = S(X(y3) - X(y2) - X(y0) + X(y1));
          const float b = S(X(y0) - X(y1) - X(a));
          const float c = S(X(y2) - X(y0));
          return S(X(f) * X(a) * X(FF) + X(FF) * X(b) + X(f) * X(c) + X(y1));
        }
        case 4: {
          const float F1 = S(X(f) * X(f));
          const float F2 = S(X(f) * X(F1));
          const float P1 = S((X(y1) - X(y0)) * 1 * 1 * 0.5L + (X(y2) - X(y1)) * 1 * 1 * 0.5L);
          const float P2 = S((X(y2) - X(y1)) * 0.5L + (X(y3) - X(y2)) * 0.5L);
          const float t3 = 3.0f * F1;
          const float h1 = S(X(F2) + X(F2) - X(t3) + 1);
          const float h2 = S(X(F2) - (X(F1) + X(F1)) + X(f));
          const float h3 = S(X(F2) - X(F1));
          const float h4 = S(X(t3) + X(-2.0f * F2));
          return S(X(y1) * X(h1) + X(P1) * X(h2) + X(P2) * X(h3) + X(y2) * X(h4));
        }
        default:
          break;
      }
    }
    return y1;
  }

  // ---- 8, the per-tick update
  void update(Voice& v) {
    if (v.disabled) return;
    if (v.fading && v.fadeVol == 0 && v.L == 0 && v.R == 0) {
      v.paused = false;
      v.length = 0;
      v.lastL = v.lastR = 0;
    }
    v.inc = 0;
    v.realVol = 0;
    v.rampLen = 0;
    v.fef = 256.0f;
    v.resEnv = 0;
    v.feApplied = false;
    v.effPan = v.pan + (v.panRand * v.pan) / 128;
    int64_t V = v.vol;
    const Instrument* I = v.ins;
    if (v.period != 0 && v.length != 0) {
      // 8.2
      V *= 64;
      if (I && v.envOn[0] && I->env[0].n > 0) {
        long long e = rnd(X(envValue(I->env[0], v.envPos[0]) * 4.0f), 0, 256, 0);
        V = (V * e) >> 8;
      }
      if (I && v.envOn[1] && I->env[1].n > 0) {
        const long long e = rnd(X(envValue(I->env[1], v.envPos[1])), -1000, 1000, 32);
        int64_t p = v.pan;
        if (p >= 128) p += ((e - 32) * (256 - p)) / 32;
        else p += ((e - 32) * p) / 32;
        v.effPan = int(p);
      }
      if (v.fadeVol == 0 && I) {
        V = 0;
      } else if (v.fading && I && I->fadeout != 0) {
        v.fadeVol -= 2 * I->fadeout;
        if (v.fadeVol < 0) v.fadeVol = 0;
        V = (int64_t(v.fadeVol) * V) >> 16;
      }
      if (I && I->env[1].pitchSep != 0 && v.effPan != 0 && v.note != 0)
        v.effPan += ((v.note - 1 - I->env[1].pitchCentre) * I->env[1].pitchSep) / 8;
      if (V == 0) {
        v.realVol = 0;
      } else {
        const float iv = S(X(v.instVol) + X((float(v.volRand) * v.instVol) * 0.0078125f));
        float e = float(V) * 6.103515625e-05f;
        e = S(0.015625L * X(v.cgv) * X(e));
        v.realVol = S(X(iv) * X(e));
      }
      // 8.4
      int64_t P = v.period;
      if (I && v.envOn[2] && I->env[2].n > 0) {
        const long long ep = rnd(X((envValue(I->env[2], v.envPos[2]) - 32.0f) * 8.0f), -256, 256, 0);
        if (I->flags & 1) {
          v.fef = (float(ep) * v.fef) * 0.00390625f;
          v.feApplied = true;
          filterSetup(v, !v.fActive, false, false, false);
        } else if (ep < 0) {
          P = (P * UP[-ep > 255 ? 255 : -ep]) / 65536;
        } else {
          P = (P * DOWN[ep > 255 ? 255 : ep]) / 65536;
        }
      }
      if (v.sample && v.sample->vibDepth != 0) {
        const Sample& s = *v.sample;
        // the sweep: the sample's vibrato rate is added every tick (the references need this)
        int D = s.vibRate + v.avDepth;
        if (int(s.vibDepth) * 256 < D) D = s.vibDepth * 256;
        v.avDepth = D;
        D = (D >> 8) & 255;
        const int w = SINE[v.avPos & 255];
        v.avPos += s.vibSpeed;
        int64_t delta = (w * D) >> 6;
        const int64_t l = delta < 0 ? -delta : delta;
        if (delta < 0) {
          delta = (P * DOWN[(l >> 2) & 255]) / 65536 - P;
          if (l & 3) delta += (P * FINEDOWN[l & 3]) / 65536 - P;
        } else {
          delta = (P * UP[(l >> 2) & 255]) / 65536 - P;
          if (l & 3) delta += (P * FINEUP[l & 3]) / 65536 - P;
        }
        P -= delta;
      }
      P += (v.pitchRand * P) / 128;
      if (P > 61440) {
        v.fadeVol = 0;
        v.fading = true;
        v.realVol = 0;
        P = 61440;
      }
      if (P < 1) P = 1;
      const uint32_t freq = uint32_t((uint64_t(v.c4) * 438272u) / (uint64_t(P) * 256u));
      v.inc = int64_t((uint64_t(freq) << 32) / uint64_t(RATE));
      // 8.5
      if (I && v.envOn[3] && I->env[3].n > 0) {
        const long long fe = rnd(X((envValue(I->env[3], v.envPos[3]) - 32.0f) * 8.0f), -256, 256, 0);
        v.fef = (float(fe) * v.fef) * 0.00390625f;
        filterSetup(v, !v.fActive, false, false, false);
        v.feApplied = true;
      }
      if (I) {
        for (int k = 0; k < 5; ++k) {
          const Envelope& e = I->env[k];
          if (!v.envOn[k] || e.n == 0) continue;
          int& pos = v.envPos[k];
          ++pos;
          int Sx, Ex;
          bool endReached = false;
          if ((e.flags & 4) && !v.keyedOff) {
            Sx = e.t[size_t(e.susS)], Ex = e.t[size_t(e.susE)] + 1;
          } else if (e.flags & 2) {
            Sx = e.t[size_t(e.loopS)], Ex = e.t[size_t(e.loopE)] + 1;
          } else {
            Sx = Ex = e.t[size_t(e.n - 1)];
            endReached = pos > Sx;
          }
          if (pos >= Ex) pos = Sx;
          if (k == 0 && endReached) {
            if (e.y[size_t(e.n - 1)] == 0 && v.background) {
              v.fading = true;
              v.fadeVol = 0;
              v.realVol = 0;
            } else {
              v.fading = true;
            }
          }
        }
      }
    }
    // 8.6
    const bool wanted = !(v.realVol == 0 && v.L == 0 && v.R == 0);
    v.NL = v.NR = 0;
    if (!(v.mixSample && v.length > 0 && v.inc != 0)) {
      v.L = v.R = 0;
      v.length = 0;
      v.paused = false;
      return;
    }
    int p = ((v.effPan - 128) * song.sep) / 128;
    p = 128 - p;
    if (p < 0) p = 0;
    if (p > 256) p = 256;
    const float ep = float(p) * 0.00390625f;
    v.NL = S(X(ep) * X(v.realVol));
    v.NR = S(X(v.realVol) * X(1.0f - ep));
    if (v.NL > 1.0f) v.NL = 1.0f;
    if (v.NR > 1.0f) v.NR = 1.0f;
    v.RL = v.RR = 0;
    if (v.ppDir) v.inc = -v.inc;
    if (wanted && (v.NR != v.R || v.NL != v.L)) {
      int64_t len = 64;
      if (!v.fastRamp && !(v.L == 0 && v.R == 0) && v.NL != 0) len = tickLen;
      if (len < 1) len = 1;
      v.RL = S(X(v.NL) - X(v.L)) / float(len);
      v.RR = S(X(v.NR) - X(v.R)) / float(len);
      v.rampLen = int(len > (1 << 30) ? (1 << 30) : len);
      v.ramping = true;
    } else {
      v.ramping = false;
      v.L = v.NL;
      v.R = v.NR;
    }
    if (v.feApplied && v.glideOn && v.fActive) filterSetup(v, false, false, false, true);
    mixList.push_back(&v);
  }

  // ---- 10.1
  void filterSetup(Voice& v, bool reset, bool newCut, bool newRes, bool tickNew) {
    (void)newRes;
    if (newCut) {
      v.glideOn = v.glideLen != 0;
      if (v.glideOn) {
        v.glideCount = 0;
        if (reset) v.cutNow = 0;
        v.glideStep = 0;
      }
    }
    if (v.glideOn) {
      if (tickNew) {
        if (v.glideCount == 0) v.glideStep = S((X(v.cutoff) - X(v.cutNow)) / X(v.glideLen));
        v.cutNow = S(X(v.glideStep) + X(v.cutNow));
        if (++v.glideCount >= v.glideLen) v.glideOn = false, v.glideCount = 0;
      }
    } else if (v.glideLen == 0) {
      v.cutNow = v.cutoff;
    }
    v.resNow = v.res;
    const float cut = S(X(v.cutNow) * X(1.0));
    float res = S(X(v.resNow) + X(0.0f * 0.5f));
    res = S(0.5L * X(v.resEnv) + X(res));
    const float fe256 = v.fef + 256.0f;
    if (!(v.ftype & 16)) {
      float c = cut;
      if (!(c > 0)) c = 0;
      if (c > 127) c = 127;
      const X e = X(c) * X(fe256) / 12288 + 0.25L;
      const float FC = S(xpow(2, e) * 110);
      const double fHz = double(rnd(X(FC), 0, 1000000000LL, 0));
      const double fc = D(TWO_PI_X / X(double(RATE)) * X(fHz));
      const double dmp = D(2 * xpow(10, (-(X(res) * 0.1875L)) / 20));
      double d = D(X(fc) * X(1.0 - dmp));
      if (!(2 >= d)) d = 2;
      d = D((X(dmp) - X(d)) / X(fc));
      const double e2 = D(1 / (X(fc) * X(fc)));
      double diw = D(X(e2) + X(1.0 + d));
      if (diw == 0 || !std::isfinite(diw)) diw = 1;   // only a damaged song gets here
      const double fg = 1.0 / diw;
      const double fb0 = D((X(e2) + X(e2) + X(d)) / X(diw));
      const double fb1 = D(-X(e2) / X(diw));
      v.A0 = zap((v.ftype & 1) ? static_cast<float>(1.0 - fg) : static_cast<float>(fg));
      v.B0 = zap(static_cast<float>(fb0));
      v.B1 = zap(static_cast<float>(fb1));
    } else {
      res = S(X(res) + C_1E6);
      const float x = S(X(cut) * X(fe256) * (1.0L / 512));
      const float chz = S(xpow(X((x + 5.0f) / 132.0f), C_1_7) * 13000 + 30);
      const double w = D((PI + PI) * X(chz) / X(RATE));
      const double cs = D(fcos(X(w))), sn = D(fsin(X(w)));
      const float q = (cut + 70.0f) / 197.0f;
      const float r2 = S(xpow(X(S(X(res) * X(q)) / 127.0f), 4) * 150 + C_0_1);
      const double al = D(X(sn) / X(r2));
      float B0, B1, B2;
      if (!(v.ftype & 1)) {
        B0 = S(0.5L * X(1.0 - cs));
        B1 = static_cast<float>(1.0 - cs);
      } else {
        B0 = S(0.5L * X(1.0 + cs));
        B1 = S(-X(1.0 + cs));
      }
      B2 = B0;
      const float A0 = static_cast<float>(1.0 + al);
      const float A1 = S(-(X(cs) + X(cs)));
      const float A2 = static_cast<float>(1.0 - al);
      if (A0 != 0 && std::isfinite(A0)) {
        v.A1 = S(-(X(A1) / X(A0)));
        v.A2 = S(-(X(A2) / X(A0)));
        v.B0 = S(X(B0) / X(A0));
        v.B1 = S(X(B1) / X(A0));
        v.B2 = S(X(B2) / X(A0));
      } else {
        v.A1 = v.A2 = v.B0 = v.B1 = v.B2 = 0;
      }
      v.A0 = zap(A0);
      v.A1 = zap(v.A1);
      v.B0 = zap(v.B0);
      v.B1 = zap(v.B1);
      v.B2 = zap(v.B2);
    }
    if (reset)
      for (float& h : v.fh) h = 0;
    v.fActive = true;
  }

  // ---- 9.2
  int untilEvent(Voice& v, int w) {
    const int64_t len = v.length;
    if (w <= 0 || len == 0 || v.inc == 0) return 0;
    const bool loop = v.sflags & SF_LOOP;
    const bool pp = loop && (v.sflags & SF_PP);
    const int64_t LS = loop ? v.loopStart : 0;
    if (v.pos < LS * F && v.inc < 0) {
      if (pp) {
        v.loopTrig = true;
        v.pos = LS * F + (LS * F - v.pos);
        v.inc = -v.inc;
        v.ppDir = false;
      } else if (!loop) {
        v.pos = len * F;
        return 0;
      }
    }
    if (v.pos >= len * F) {
      if (!loop) return 0;
      v.loopTrig = true;
      if (pp) {
        v.pos = (len - 1) * F - (v.pos - len * F);
        if (v.inc > 0) v.inc = -v.inc;
        v.ppDir = true;
      } else {
        v.pos += (LS - len) * F;
      }
      // a loop shorter than one step (no song does this): keep inside it
      if (v.pos >= len * F || v.pos < LS * F) {
        const int64_t span = (len - LS) * F;
        if (span <= 0) return 0;
        int64_t o = (v.pos - LS * F) % span;
        if (o < 0) o += span;
        v.pos = LS * F + o;
      }
    }
    int64_t count = w;
    if (v.inc > 0) {
      int64_t mx = 16384 / (v.inc / F + 1);
      if (mx < 2) mx = 2;
      if (w > mx) w = int(mx);
      if (((v.pos + int64_t(w - 1) * v.inc) >> 32) >= len) count = (len * F - v.pos - 1) / v.inc + 1;
    } else {
      const int64_t a = -v.inc;
      int64_t mx = 16384 / (a / F + 1);
      if (mx < 2) mx = 2;
      if (w > mx) w = int(mx);
      if (((v.pos - int64_t(w - 1) * a) >> 32) < LS) count = (v.pos - 1 - LS * F) / a + 1;
    }
    if (count <= 1) return 1;
    return count < w ? int(count) : w;
  }

  // ---- 9.4, 10.3, 9.6, then add
  void resample(Voice& v, int m) {
    const Sample& s = *v.mixSample;
    const std::vector<float>* b = &s.main;
    if (v.loopTrig && !s.loopBuf.empty()) b = &s.loopBuf;
    else if ((v.sflags & SF_SUS) && !s.susBuf.empty()) b = &s.susBuf;
    const float* base = s.frame0(*b);
    const int chn = s.channels;
    const int64_t lo = -int64_t(s.pad) + 3, hi = int64_t(s.length) + s.pad - 5;
    float* wl = work[0].data();
    float* wr = work[1].data();
    int64_t X0 = v.pos;
    const int64_t inc = v.inc;
    const bool nearest = inc == F || inc == -F;
    for (int i = 0; i < m; ++i) {
      int64_t g = X0 >> 32;
      if (g < lo) g = lo;   // only a damaged song reaches outside the padding
      if (g > hi) g = hi;
      if (nearest) {
        for (int c = 0; c < chn; ++c) {
          const float o = S(X(EPS24) + X(base[g * chn + c]));
          (c ? wr : wl)[i] = o;
        }
      } else {
        const float* h = FIR[(X0 & 0xFFFFFFFF) >> 20];
        for (int c = 0; c < chn; ++c) {
          const float* sp = base + g * chn + c;
          const float o = S(X(h[0]) * X(sp[-3 * chn]) + X(h[1]) * X(sp[-2 * chn]) + X(h[2]) * X(sp[-chn]) +
                            X(sp[0]) * X(h[3]) + X(h[4]) * X(sp[chn]) + X(h[5]) * X(sp[2 * chn]) +
                            X(h[6]) * X(sp[3 * chn]) + X(h[7]) * X(sp[4 * chn]) + X(EPS24));
          (c ? wr : wl)[i] = o;
        }
      }
      if (chn == 1) wr[i] = wl[i];
      X0 += inc;
    }
    v.pos = X0;
  }

  void filter(Voice& v, int m) {
    float* wl = work[0].data();
    float* wr = work[1].data();
    for (float& h : v.fh) h = zap(h);
    float* f = v.fh;
    if (!(v.ftype & 16)) {
      const X A0 = v.A0, B0 = v.B0, B1 = v.B1;
      if (!(v.ftype & 1)) {
        for (int i = 0; i < m; ++i) {
          const float x = wl[i];
          const float o = S(B0 * X(f[0]) + X(x) * A0 + B1 * X(f[1]) + X(EPS24));
          f[1] = f[0], f[0] = o, wl[i] = o;
        }
        for (int i = 0; i < m; ++i) {
          const float x = S(X(EPS24) + X(wr[i]));
          const float o = S(B0 * X(f[2]) + X(x) * A0 + B1 * X(f[3]) + X(EPS24));
          f[3] = f[2], f[2] = o, wr[i] = o;
        }
      } else {
        for (int i = 0; i < m; ++i) {
          const float x = wl[i];
          const float o = S(B0 * X(f[0]) + X(x) * A0 + B1 * X(f[1]) + X(EPS24));
          f[1] = f[0], f[0] = S(X(o) - X(x)), wl[i] = o;
        }
        for (int i = 0; i < m; ++i) {
          const float x = wr[i];
          const float o = S(B0 * X(f[2]) + X(x) * A0 + B1 * X(f[3]) + X(EPS24));
          f[3] = f[2], f[2] = S(X(o) - X(x)), wr[i] = o;
        }
      }
    } else {
      const X A1 = v.A1, A2 = v.A2, B0 = v.B0, B1 = v.B1, B2 = v.B2;
      for (int c = 0; c < 2; ++c) {
        float* w = c ? wr : wl;
        float* hh = f + 4 * c;   // a1 a2 b1 b2
        for (int i = 0; i < m; ++i) {
          const float x = w[i];
          const float o = S(B1 * X(hh[2]) + X(x) * B0 + B2 * X(hh[3]) + A1 * X(hh[0]) + A2 * X(hh[1]) + X(EPS24));
          hh[1] = hh[0], hh[0] = o, hh[3] = hh[2], hh[2] = x;
          w[i] = o;
        }
      }
    }
  }

  void amplify(Voice& v, int m, bool useRamp) {
    float* wl = work[0].data();
    float* wr = work[1].data();
    float VL = zap(v.L), VR = zap(v.R);
    if (!useRamp) {
      for (int i = 0; i < m; ++i) {
        wl[i] = S(X(VL) * X(wl[i]) + X(EPS24));
        wr[i] = S(X(VR) * X(wr[i]) + X(EPS24));
      }
    } else {
      const float rl = zap(v.RL), rr = zap(v.RR);
      for (int i = 0; i < m; ++i) {
        VL = S(X(rl) + X(VL));
        VR = S(X(rr) + X(VR));
        wl[i] = S(X(VL) * X(wl[i]) + X(EPS24));
        wr[i] = S(X(VR) * X(wr[i]) + X(EPS24));
      }
      v.L = VL;
      v.R = VR;
    }
  }

  // 9.5
  void channelDeclick(Voice& v, float* OL, float* OR, int k, OpRun& op) {
    float lL = zap(v.lastL), lR = zap(v.lastR);
    if (lL == 0 && lR == 0) return;
    for (int i = 0; i < k; ++i) {
      lL = S(X(lL) * X(CR) + X(EPS24));
      lR = S(X(lR) * X(CR) + X(EPS24));
      OL[i] = S(X(lL) + X(OL[i]));
      OR[i] = S(X(lR) + X(OR[i]));
    }
    v.lastL = v.lastR = 0;
    op.accL = S(X(lL) + X(op.accL));
    op.accR = S(X(lR) + X(op.accR));
  }

  void bufferDeclick(OpRun& op, int n) {
    op.accL = zap(op.accL);
    op.accR = zap(op.accR);
    if (op.accL == 0 && op.accR == 0) return;
    float* OL = op.buf[0].data();
    float* OR = op.buf[1].data();
    for (int i = 0; i < n; ++i) {
      op.accL = S(X(op.accL) * X(CR) + X(EPS24));
      OL[i] = S(X(op.accL) + X(OL[i]));
      op.accR = S(X(op.accR) * X(CR) + X(EPS24));
      OR[i] = S(X(op.accR) + X(OR[i]));
    }
  }

  // 9.1
  void mixVoice(Voice& v, OpRun& op, int n) {
    if (v.paused || !v.mixSample) return;
    float* OL = op.buf[0].data();
    float* OR = op.buf[1].data();
    int remain = n, at = 0;
    bool doAmp = !(v.rampLen == 0 && v.L == 1.0f && v.R == 1.0f);
    bool useRamp = v.ramping;
    while (remain > 0) {
      int want = remain;
      if (v.rampLen > 0 && v.rampLen < want) want = v.rampLen;
      const int m = untilEvent(v, want);
      if (m <= 0) {
        channelDeclick(v, OL + at, OR + at, remain, op);
        v.lastL = v.lastR = 0;
        v.mixSample = nullptr;
        v.length = 0;
        v.pos = 0;
        v.L = v.R = 0;
        v.rampLen = 0;
        v.ppDir = false;
        return;
      }
      if (v.muted || (v.rampLen == 0 && v.L == 0 && v.R == 0)) {
        v.pos += int64_t(m) * v.inc;
        v.lastL = v.lastR = 0;
      } else {
        resample(v, m);
        if (v.fActive) filter(v, m);
        if (doAmp) amplify(v, m, useRamp);
        const float* wl = work[0].data();
        const float* wr = work[1].data();
        for (int i = 0; i < m; ++i) {
          OL[at + i] = S(X(wl[i]) + X(OL[at + i]) + X(EPS24));
          OR[at + i] = S(X(wr[i]) + X(OR[at + i]) + X(EPS24));
        }
        v.lastL = wl[m - 1];
        v.lastR = wr[m - 1];
      }
      at += m;
      if (v.rampLen != 0) {
        v.rampLen -= m;
        if (v.rampLen <= 0) {
          doAmp = !(v.L == 1.0f && v.R == 1.0f);
          useRamp = false;
          v.ramping = false;
          v.rampLen = 0;
          v.L = v.NL;
          v.R = v.NR;
          v.RL = v.RR = 0;
          if (v.fading && v.fadeVol == 0 && v.L == 0 && v.R == 0) {
            v.length = 0;
            v.mixSample = nullptr;
          }
        }
      }
      remain -= m;
    }
  }

  // ---- 11
  bool belongsToChannelOp(const Voice* v) const {
    if (!v->background) {
      const int k = int(v - ch.data());
      return chanOp[size_t(k)] != 0;
    }
    return v->owner >= 0 && chanOp[size_t(v->owner)] != 0;
  }

  void process(int k, int n) {
    OpRun& o = ops[size_t(k)];
    if (!o.todo) return;
    o.todo = false;
    const OpDef& d = *o.def;
    if (d.type == OP_CONN) return;
    // gather
    bool first = true;
    o.inputs = 0;
    for (int in : d.inputs) {
      process(in, n);
      const OpRun& src = ops[size_t(in)];
      ++o.inputs;
      if (first) {
        if (&src != &o) {
          std::memcpy(o.buf[0].data(), src.buf[0].data(), sizeof(float) * size_t(n));
          std::memcpy(o.buf[1].data(), src.buf[1].data(), sizeof(float) * size_t(n));
        }
        first = false;
      } else {
        for (int c = 0; c < 2; ++c) {
          float* b = o.buf[c].data();
          const float* s = src.buf[c].data();
          for (int i = 0; i < n; ++i) b[i] = S(X(s[i]) + X(b[i]));
        }
      }
    }
    if (first) {
      std::fill_n(o.buf[0].data(), n, 0.0f);
      std::fill_n(o.buf[1].data(), n, 0.0f);
    }
    switch (d.type) {
      case OP_MASTER:
        bufferDeclick(o, n);
        for (size_t i = mixList.size(); i-- > 0;)
          if (!belongsToChannelOp(mixList[i])) mixVoice(*mixList[i], o, n);
        break;
      case OP_CHANNEL: {
        bufferDeclick(o, n);
        if (d.channel < 0 || d.channel >= song.channels) break;
        Voice* self = &ch[size_t(d.channel)];
        for (Voice* v : mixList)
          if (v == self) mixVoice(*v, o, n);
        for (Voice* v : mixList)
          if (v->background && v->owner == d.channel) mixVoice(*v, o, n);
        break;
      }
      case OP_AMP:
        if (d.gain != 1.0f)
          for (int c = 0; c < 2; ++c) {
            float* b = o.buf[c].data();
            for (int i = 0; i < n; ++i) b[i] = S(X(b[i]) * X(d.gain));
          }
        break;
      case OP_EFFECT:
        if (o.plug) {
          float* const in[2] = {o.buf[0].data(), o.buf[1].data()};
          float* const out[2] = {o.obuf[0].data(), o.obuf[1].data()};
          o.plug->run(*this, in, out, n, o.inputs);
          std::memcpy(o.buf[0].data(), o.obuf[0].data(), sizeof(float) * size_t(n));
          std::memcpy(o.buf[1].data(), o.obuf[1].data(), sizeof(float) * size_t(n));
        }
        break;
      default:
        break;
    }
  }

  void chunk(float* out, int n) {
    for (auto& o : ops) o.todo = true;
    process(finalOp, n);
    for (size_t k = 0; k < ops.size(); ++k) process(int(k), n);
    const float* fl = ops[size_t(finalOp)].buf[0].data();
    const float* fr = ops[size_t(finalOp)].buf[1].data();
    for (int i = 0; i < n; ++i) {
      if (gvStep != 0) gvCur = S(X(gvStep) + X(gvCur));
      float l = S(X(gvCur) * X(fl[i]));
      float r = S(X(gvCur) * X(fr[i]));
      if (gain != 1.0f) {
        l = S(X(gain) * X(l));
        r = S(X(gain) * X(r));
      }
      if (out) {
        out[2 * i] = l;
        out[2 * i + 1] = r;
      }
    }
    if (gvCur != 1.0f && gvStep != 0) {
      // compared as unsigned bit patterns: a ramp that overshoots below zero is not caught
      // until it comes back (the references need this)
      const uint32_t c = ubits(gvCur), t = ubits(gvTarget);
      if ((gvStep > 0 && c > t) || (gvStep < 0 && c < t)) {
        gvCur = gvTarget;
        gvStep = 0;
      }
    }
  }

  // 5.2: start ticks until one has a length; false at the end of the song
  bool startTicks() {
    do {
      plugBpm = bpm;
      plugFbpm = fbpm;
      runTick();
      computeTickLen();
      fracAcc += tickFrac;
      remain = tickLen + int64_t(fracAcc >> 32);
      fracAcc &= 0xFFFFFFFFu;
    } while (remain <= 0 && tickFrac >= 65536);
    return !(ended && tick == 0);
  }

  // One output block; returns the frames made (fewer at the end of the song).
  int block(float* out) {
    plugBpm = bpm;
    plugFbpm = fbpm;
    int pos = 0;
    while (pos < BLOCK) {
      if (remain <= 0) {
        if (!startTicks()) return pos;
        if (remain <= 0) continue;
      }
      const int n = int(remain < BLOCK - pos ? remain : BLOCK - pos);
      remain -= n;
      chunk(out ? out + 2 * pos : nullptr, n);
      pos += n;
    }
    return pos;
  }
};

void Delay::run(Engine& e, float* const in[2], float* const out[2], int n, int) {
  for (int c = 0; c < 2; ++c) {
    const int64_t B = tempo ? tempo : e.plugBpm;
    const int64_t fb = tempo ? 0 : e.plugFbpm;
    const int64_t div = (B * 65536 + (fb & 0xFFFF)) * rpb * tpr;
    const float T = div > 0 ? static_cast<float>(double(int64_t(RATE) * 60 * 65536) / double(div)) : 0.0f;
    const float a = float(tpr) * T;
    const float b = float(rows[c]) * a;
    const float cc = float(ticks[c]) * T;
    const int delay = int(rnd(X(cc) + X(b), 0, DELAY_LEN, 0));
    float* bf = buf[c].data();
    int wi = w[c];
    const float f = fbk[c];
    for (int i = 0; i < n; ++i) {
      if (wi >= DELAY_LEN) wi = 0;
      int r = wi - delay;
      if (r < 0) r += DELAY_LEN;
      const float x = in[c][i];
      const float v = S(X(f) * X(bf[r]) + EPSX);
      bf[wi] = S(X(v) + X(x));
      out[c][i] = S(X(dry) * X(x) + X(wet) * X(v));
      ++wi;
    }
    w[c] = wi;
  }
}

// The length in frames (section 5.3), or -1 past 30 minutes.
long long measure(Song& song) {
  Engine e(song, true);
  long long frames = 0, ticks = 0;
  for (;;) {
    if (++ticks > MAX_TICKS) return -2;
    e.plugBpm = e.bpm;
    e.runTick();
    if (e.ended && e.tick == 0) return frames;
    e.computeTickLen();
    e.fracAcc += e.tickFrac;
    frames += e.tickLen + int64_t(e.fracAcc >> 32);
    e.fracAcc &= 0xFFFFFFFFu;
    if (frames > MAX_FRAMES) return -1;
  }
}

// ---- output

struct Player {
  Engine* engine;
  std::vector<float> buf = std::vector<float>(size_t(BLOCK) * 2);
  int have = 0, at = 0;
  bool done = false;
  bool next(float* f) {
    if (at == have) {
      if (done) return false;
      have = engine->block(buf.data());
      at = 0;
      if (have < BLOCK) done = true;
      if (have == 0) return false;
    }
    f[0] = buf[size_t(at) * 2];
    f[1] = buf[size_t(at) * 2 + 1];
    ++at;
    return true;
  }
  void frame(int16_t* out) {
    float f[2] = {0, 0};
    next(f);
    for (int c = 0; c < 2; ++c) {
      float v = f[c];
      if (!(v == v)) v = 0;   // NaN only from a damaged song
      if (v > 1) v = 1;
      if (v < -1) v = -1;
      out[c] = static_cast<int16_t>(std::lrint(double(v) * 32767.0));
    }
  }
};

std::string latin1(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    if (c == 0) break;
    if (c < 0x80) out += char(c);
    else {
      out += char(0xC0 | c >> 6);
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

std::string trim(std::string t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n')) t.pop_back();
  return t;
}

void json(const std::string& t) {
  std::putchar('"');
  for (unsigned char c : t) {
    if (c == '"' || c == '\\') std::printf("\\%c", c);
    else if (c < 0x20 || c == 0x7f) std::printf("\\u%04x", c);
    else std::putchar(c);
  }
  std::putchar('"');
}

int fail(const std::string& why) {
  std::printf("{\"tracks\":0,\"error\":");
  json(why);
  std::printf("}\n");
  std::fflush(stdout);
  return 1;
}

bool readFile(const char* path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  out.resize(MAX_FILE + 1);
  const size_t n = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  out.resize(n);
  return true;
}

void source(void* user, int16_t* frame) { static_cast<Player*>(user)->frame(frame); }

std::string effectNames(const Song& song) {
  std::string out;
  const char* names[][2] = {{"DELA", "delay"}, {"STDE", "stereo delay"}, {"REVE", "reverb"},
                            {"PISH", "pitch shifter"}, {"BABO", "bass booster"}, {"PAEQ", "equaliser"},
                            {"COMP", "compressor"}, {"DIST", "distortion"}};
  for (const auto& nm : names) {
    int k = 0;
    for (const auto& o : song.ops) k += o.type == OP_EFFECT && o.code == tag4(nm[0]);
    if (!k) continue;
    if (!out.empty()) out += ", ";
    out += nm[1];
    if (k > 1) out += " x" + std::to_string(k);
  }
  return out.empty() ? "none" : out;
}

}  // namespace

static int run(int argc, char** argv) {
  bool ref = false;
  int a = 1;
  if (a < argc && !std::strcmp(argv[a], "--fir")) {   // the resampling table, for checking
    constants();
    makeFir();
    std::fwrite(FIR, sizeof FIR, 1, stdout);
    return 0;
  }
  if (a < argc && !std::strcmp(argv[a], "--ref")) ref = true, ++a;
  if (ref ? argc - a < 1 : argc - a < 3) {
    std::fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n"
                         "       %s --ref <file>   (44100 Hz, 32-bit float, no header)\n",
                 argv[0], argv[0]);
    return 2;
  }
  constants();
  std::vector<uint8_t> file;
  if (!readFile(argv[a], file) || file.empty()) return fail("can't read the file");
  if (file.size() > MAX_FILE) return fail("the file is too big for a BeRoTracker song");
  std::unique_ptr<Song> songp(new Song());
  Song& song = *songp;
  std::string why = load(file, song);
  if (why.empty()) why = prepare(song);
  if (!why.empty()) return fail(why);
  if (!ref && std::atoi(argv[a + 1]) != 0) return fail("a BeRoTracker song has one track");
  int rate = ref ? RATE : std::atoi(argv[a + 2]);
  if (rate < 8000 || rate > 192000) rate = RATE;
  long long startMs = !ref && argc - a > 3 ? std::atoll(argv[a + 3]) : 0;
  if (startMs < 0) startMs = 0;

  const long long total = measure(song);
  if (total == -2) return fail("the song has more than two million ticks (impossibly short ones)");
  if (total < 0) return fail("the song does not end within 30 minutes");
  makeFir();

  if (!ref) {
    int patterns = 0, samples = 0, instruments = 0, muted = 0;
    for (const auto& p : song.pat) patterns += p.present;
    for (int k = 1; k < 256; ++k) samples += song.smp[k].hasData, instruments += song.ins[k].present;
    for (const auto& c : song.chan) muted += (c.flags & 6) != 0;
    std::printf("{\"tracks\":1,\"track\":0,\"title\":");
    json(trim(latin1(song.title)));
    std::printf(",\"type\":\"BeRoTracker\",\"duration_ms\":%lld,\"facts\":{", total * 1000 / RATE);
    std::printf("\"Channels\":");
    json(std::to_string(song.channels) + (muted ? ", " + std::to_string(muted) + " muted" : ""));
    int positions = 0;
    while (positions <= song.orderCount && positions < 257 && song.order[positions] != 255) ++positions;
    std::printf(",\"Patterns\":");
    json(std::to_string(patterns) + ", played in " + std::to_string(positions) +
         (positions == 1 ? " order position" : " order positions"));
    std::printf(",\"Samples\":");
    json(std::to_string(samples) + " in " + std::to_string(instruments) + " instruments");
    std::printf(",\"Effects\":");
    json(effectNames(song));
    std::printf(",\"Mixer\":");
    json(BRT_X87 ? "BeRoTracker's, 44100 Hz, 8-tap FIR, x87 arithmetic as the tracker's"
                 : "BeRoTracker's, 44100 Hz, 8-tap FIR (close to the tracker's, not bit-exact on this CPU)");
    std::printf("}");
    const std::string msg = trim(latin1(song.message));
    if (!msg.empty()) {
      std::printf(",\"comment\":");
      json(msg);
    }
    std::printf(",\"names\":[");
    int last = 255;
    while (last >= 1 && trim(latin1(song.ins[last].name)).empty()) --last;
    for (int k = 1; k <= last; ++k) {
      if (k > 1) std::putchar(',');
      json(trim(latin1(song.ins[k].name)));
    }
    std::printf("]}\n");
    std::fflush(stdout);
  }

  std::unique_ptr<Engine> engine(new Engine(song, false));
  Player player{engine.get()};
  if (ref) {
    std::vector<float> buf(size_t(BLOCK) * 2);
    for (;;) {
      const int n = engine->block(buf.data());
      if (n > 0 && std::fwrite(buf.data(), 8, size_t(n), stdout) != size_t(n)) return 0;
      if (n < BLOCK) break;
    }
    std::fflush(stdout);
    return 0;
  }

  long long skip = startMs >= total * 1000 / RATE ? total : startMs * RATE / 1000;
  const long long out = total - skip;
  float tmp[2];
  while (skip > 0 && player.next(tmp)) --skip;

  std::vector<int16_t> buf(4096 * 2);
  if (rate == RATE) {
    long long n_left = out;
    while (n_left > 0) {
      const size_t n = n_left > 4096 ? 4096 : size_t(n_left);
      for (size_t i = 0; i < n; ++i) player.frame(&buf[2 * i]);
      if (std::fwrite(buf.data(), 4, n, stdout) != n) return 0;
      n_left -= (long long)n;
    }
  } else {
    Resampler rs(RATE, rate, source, &player);
    long long n_left = (long long)((unsigned long long)out * (unsigned long long)rate / RATE);
    while (n_left > 0) {
      const size_t n = n_left > 4096 ? 4096 : size_t(n_left);
      for (size_t i = 0; i < n; ++i) rs.frame(&buf[2 * i]);
      if (std::fwrite(buf.data(), 4, n, stdout) != n) return 0;
      n_left -= (long long)n;
    }
  }
  std::fflush(stdout);
  return 0;
}

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::bad_alloc&) {
    return fail("the song needs more memory than there is");
  }
}
