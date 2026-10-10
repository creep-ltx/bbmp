// SPDX-License-Identifier: GPL-3.0-or-later
// A small windowed-sinc resampler for stereo s16, for engines whose hardware has a
// fixed sample rate (the PlayStation's SPU: 44100 Hz; Synder's: 48000 Hz; T0AST's: 44100 Hz).
// A copy of csrc/console/resample.h, so that this helper builds from its own folder.
#pragma once

#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

class Resampler {
 public:
  // One input frame at the rate <from>, asked for when the resampler needs it.
  typedef void (*Source)(void* user, int16_t* frame);

  Resampler(int from, int to, Source source, void* user) : source_(source), user_(user) {
    const int g = std::gcd(from, to);
    step_ = from / g;    // input position advances step_/phases_ per output frame
    phases_ = to / g;
    const double cutoff = (to < from ? static_cast<double>(to) / from : 1.0) * 0.96;
    taps_ = to < from ? static_cast<int>(kTaps * static_cast<double>(from) / to + 0.5) : kTaps;
    taps_ = (taps_ + 1) & ~1;
    table_.resize(static_cast<size_t>(phases_) * taps_);
    for (int p = 0; p < phases_; ++p) {
      double sum = 0;
      std::vector<double> row(taps_);
      for (int t = 0; t < taps_; ++t) {
        // the tap's distance from the point wanted, in input samples
        const double x = (t - taps_ / 2 + 1) - static_cast<double>(p) / phases_;
        const double s = x == 0 ? 1.0 : std::sin(M_PI * x * cutoff) / (M_PI * x * cutoff);
        const double w = kaiser(x / (taps_ / 2.0));
        row[t] = s * w;
        sum += row[t];
      }
      for (int t = 0; t < taps_; ++t) table_[static_cast<size_t>(p) * taps_ + t] = static_cast<float>(row[t] / sum);
    }
    hist_.assign(static_cast<size_t>(taps_) * 2 * 2, 0);
    // fill the window, so that the first output frame is the first input frame
    for (int i = 0; i < taps_ / 2; ++i) pull();
  }

  void frame(int16_t* out) {
    const float* k = &table_[static_cast<size_t>(phase_) * taps_];
    const int16_t* h = &hist_[static_cast<size_t>(pos_) * 2];
    float l = 0, r = 0;
    for (int t = 0; t < taps_; ++t) {
      l += k[t] * h[t * 2];
      r += k[t] * h[t * 2 + 1];
    }
    out[0] = clip(l);
    out[1] = clip(r);
    phase_ += step_;
    while (phase_ >= phases_) {
      phase_ -= phases_;
      pull();
    }
  }

 private:
  enum { kTaps = 32 };

  static double kaiser(double x) {  // x in -1..1, beta 8
    if (x <= -1 || x >= 1) return 0;
    return bessel(8.0 * std::sqrt(1 - x * x)) / bessel(8.0);
  }
  static double bessel(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 30; ++k) {
      term *= (x / 2) / k;
      sum += term * term;
    }
    return sum;
  }
  static int16_t clip(float v) {
    const long n = std::lrintf(v);
    return static_cast<int16_t>(n > 32767 ? 32767 : n < -32768 ? -32768 : n);
  }
  // The history is kept twice, one copy after the other, so that a window of taps_
  // frames is always in one piece.
  void pull() {
    int16_t f[2];
    source_(user_, f);
    for (int c = 0; c < 2; ++c) {
      hist_[static_cast<size_t>(pos_) * 2 + c] = f[c];
      hist_[static_cast<size_t>(pos_ + taps_) * 2 + c] = f[c];
    }
    pos_ = (pos_ + 1) % taps_;
  }

  Source source_;
  void* user_;
  int step_ = 1, phases_ = 1, taps_ = kTaps, phase_ = 0, pos_ = 0;
  std::vector<float> table_;
  std::vector<int16_t> hist_;
};
