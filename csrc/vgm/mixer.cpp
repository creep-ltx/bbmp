// SPDX-License-Identifier: GPL-2.0-or-later
// bbmp-vgm: bringing each core's output to the output rate, and mixing.
//
// Positions are counted in 1/2048 of a core's sample (11 fraction bits).
//
// A core faster than the output is averaged: output sample j covers the core's samples
// from position a(j) to a(j+1), each weighed by how much of it lies inside. Output is
// made in stretches (20 ms, cut short before each command); within a stretch starting
// at output sample s, a(j) = A(s) + A(j - s), where A(n) = n * coreRate * 2048 /
// outputRate rounded down. Before a command the core has made its samples up to
// A(end of the stretch), counted whole from the start of the song, rounded up.
//
// A core slower than the output is interpolated: output sample j lies between the
// core's samples i-1 and i, where i is the whole part of A(j) counted from the start of
// the song (or from the last change of the core's rate); a silent sample comes first.
//
// Each voice adds sum * volume / width (8.8 volume, C's division) to the mix; the sum is
// shifted down 8 bits, scaled by the master volume and clipped to 16 bits.
#include "vgmplay.h"

#include <algorithm>
#include <cstring>

namespace vgm {

static constexpr int FRAC = 11;
static constexpr int64_t ONE = int64_t(1) << FRAC;
static constexpr int64_t MASK = ONE - 1;

static void makeNative(Voice& v, int64_t upto)
{
    static thread_local std::vector<DEV_SMPL> tl, tr;
    while (v.made < upto) {
        uint32_t n = (uint32_t)std::min<int64_t>(upto - v.made, 4096);
        tl.assign(n, 0);
        tr.assign(n, 0);
        DEV_SMPL* bufs[2] = {tl.data(), tr.data()};
        v.update(v.chip, n, bufs);
        v.bufL.insert(v.bufL.end(), tl.begin(), tl.end());
        v.bufR.insert(v.bufR.end(), tr.begin(), tr.end());
        v.made += n;
    }
}

// Forget the samples before `idx` (now and then: moving the buffer costs).
static void dropBefore(Voice& v, int64_t idx)
{
    if (idx <= v.first)
        return;
    int64_t n = std::min<int64_t>(idx - v.first, (int64_t)v.bufL.size());
    if (n < 4096 && (size_t)n * 2 < v.bufL.size())
        return;
    v.bufL.erase(v.bufL.begin(), v.bufL.begin() + n);
    v.bufR.erase(v.bufR.begin(), v.bufR.begin() + n);
    v.first += n;
}

static inline int64_t steps(int64_t n, int64_t in, int64_t out) { return ((n * in) << FRAC) / out; }

static void averageDown(Voice& v, int64_t out, int64_t from, int64_t to, int32_t* accL, int32_t* accR)
{
    const int64_t in = v.rate;
    const int64_t P = steps(from, in, out);
    auto pos = [&](int64_t j) { return P + steps(j - from, in, out); };
    makeNative(v, (steps(to, in, out) + MASK) >> FRAC);
    const DEV_SMPL* L = v.bufL.data() - v.first;
    const DEV_SMPL* R = v.bufR.data() - v.first;
    int64_t a = pos(from);
    for (int64_t j = from; j < to; j++) {
        int64_t b = pos(j + 1);
        int64_t i = a >> FRAC, ie = b >> FRAC;
        int64_t sl, sr;
        if (i == ie) {
            sl = (int64_t)L[i] * (b - a);
            sr = (int64_t)R[i] * (b - a);
        } else {
            int64_t w = ONE - (a & MASK);
            sl = (int64_t)L[i] * w;
            sr = (int64_t)R[i] * w;
            int64_t fl = 0, fr = 0;
            for (int64_t k = i + 1; k < ie; k++) {
                fl += L[k];
                fr += R[k];
            }
            sl += fl << FRAC;
            sr += fr << FRAC;
            if (b & MASK) {
                sl += (int64_t)L[ie] * (b & MASK);
                sr += (int64_t)R[ie] * (b & MASK);
            }
        }
        int64_t width = b - a;
        accL[j - from] += (int32_t)(sl * v.volume / width);
        accR[j - from] += (int32_t)(sr * v.volume / width);
        a = b;
    }
    dropBefore(v, (pos(to) >> FRAC) - 1);
}

static void sameRate(Voice& v, int64_t from, int64_t to, int32_t* accL, int32_t* accR)
{
    makeNative(v, to - v.originJ + v.originN);
    const DEV_SMPL* L = v.bufL.data() - v.first;
    const DEV_SMPL* R = v.bufR.data() - v.first;
    for (int64_t j = from; j < to; j++) {
        int64_t i = j - v.originJ + v.originN;
        accL[j - from] += L[i] * v.volume;
        accR[j - from] += R[i] * v.volume;
    }
    dropBefore(v, to - v.originJ + v.originN - 1);
}

static void interpolateUp(Voice& v, int64_t out, int64_t from, int64_t to, int32_t* accL, int32_t* accR)
{
    const int64_t in = v.rate, oj = v.originJ, on = v.originN << FRAC;
    auto pos = [&](int64_t j) { return on + steps(j - oj, in, out); };
    makeNative(v, (pos(to - 1) >> FRAC) + 1);
    const DEV_SMPL* L = v.bufL.data() - v.first;
    const DEV_SMPL* R = v.bufR.data() - v.first;
    for (int64_t j = from; j < to; j++) {
        int64_t p = pos(j), i = p >> FRAC, f = p & MASK;
        bool prev = i - 1 >= v.first;
        int64_t l = (prev ? (int64_t)L[i - 1] : 0) * (ONE - f) + (int64_t)L[i] * f;
        int64_t r = (prev ? (int64_t)R[i - 1] : 0) * (ONE - f) + (int64_t)R[i] * f;
        accL[j - from] += (int32_t)((l * v.volume) >> FRAC);
        accR[j - from] += (int32_t)((r * v.volume) >> FRAC);
    }
    dropBefore(v, (pos(from) >> FRAC) - 2);
}

void Mixer::render(int64_t from, int64_t to, int16_t* out)
{
    size_t n = (size_t)(to - from);
    accL_.assign(n, 0);
    accR_.assign(n, 0);
    for (Voice* v : voices_) {
        if (!v->usable())
            continue;
        if (v->newRate) {               // the core changed its rate: count from here
            v->originJ = from;
            v->originN = v->made;
            v->rate = v->newRate;
            v->newRate = 0;
        }
        if (v->rate > rate_)
            averageDown(*v, rate_, from, to, accL_.data(), accR_.data());
        else if (v->rate == rate_)
            sameRate(*v, from, to, accL_.data(), accR_.data());
        else
            interpolateUp(*v, rate_, from, to, accL_.data(), accR_.data());
    }
    for (size_t i = 0; i < n; i++) {
        int64_t l = ((int64_t)(accL_[i] >> 8) * masterVolume) >> 16;
        int64_t r = ((int64_t)(accR_[i] >> 8) * masterVolume) >> 16;
        out[2 * i] = (int16_t)std::clamp<int64_t>(l, -32768, 32767);
        out[2 * i + 1] = (int16_t)std::clamp<int64_t>(r, -32768, 32767);
    }
}

void Mixer::balance()
{
    // the gain halves each time the sum of the shares passes 0x60 times a power of two
    int64_t sum = 0;
    for (Voice* v : voices_)
        sum += v->weight;
    int k = 0;
    if (sum > 0) {
        while (sum > ((int64_t)0x60 << k))
            k++;
        while (k > -16 && sum <= ((int64_t)0x60 << (k - 1)))
            k--;
    }
    for (Voice* v : voices_) {
        int64_t vol = (int64_t)v->weight * v->factor;
        v->volume = (int32_t)(k >= 0 ? vol >> k : vol << -k);
    }
}

void Mixer::fixedVolumes()
{
    for (Voice* v : voices_)
        v->volume = v->paired ? 0xCD : 0x100;
}

}  // namespace vgm
