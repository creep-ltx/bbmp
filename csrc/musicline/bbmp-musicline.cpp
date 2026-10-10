// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-musicline: plays Musicline Editor modules (*.ml) for Modland Player.
//
//   bbmp-musicline <file> <track> <samplerate> [start-ms]
//
// One line of JSON on stdout, then signed 16 bit little endian stereo until the song
// ends. The song is played once through: to the point where every channel has come
// round to where it has been, or, when the song stops by itself, until it is silent.
//
// Environment, for trying things out:
//   MLINE_FILTER=a500    the low pass of an Amiga 500 (the default is an A1200: none)
//   MLINE_PAN=0..1       how much of the other side is mixed in (0 is the Amiga, 0.4 the default)
//   MLINE_MAX=seconds    where to stop a song of which no end is found (default 900)
//   MLINE_TRACE=file     write what Paula is told on every tick, to compare with the Amiga
#include "mline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

using namespace mline;

static const double PAULA_CLOCK = 3546895.0;        // PAL
static const int OVER = 2;                          // the voices are computed at twice the rate
static const int TAPS = 63;

static std::string json(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

static int fail(const std::string &why) {
    printf("{\"tracks\":0,\"error\":%s}\n", json(why).c_str());
    return 1;
}

// Plays the voices of a Player and turns them into sound, or only keeps the time.
class Machine {
public:
    Machine(const Module &m, int tune, int rate, double pan, bool a500)
        : player(m), rate(rate), pan(pan), a500(a500) {
        player.start(tune);
        eight = player.eightChannels();
        double fc = 24000.0 / (rate * OVER);        // of the rate the voices are computed at
        if (fc > 0.25) fc = 0.25;
        double sum = 0;
        for (int i = 0; i < TAPS; i++) {
            double x = i - (TAPS - 1) / 2.0;
            double s = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
            double r = 2.0 * i / (TAPS - 1) - 1;
            fir[i] = (float)(s * bessel(7.857 * sqrt(1 - r * r)) / bessel(7.857));
            sum += fir[i];
        }
        for (float &f : fir) f = (float)(f / sum);
        memset(hist, 0, sizeof hist);
        // the filter of the power LED, 12 dB an octave from about 3.3 kHz
        double w0 = 2 * M_PI * 3275.0 / rate, q = 0.7071;
        double alpha = sin(w0) / (2 * q), a0 = 1 + alpha;
        lb0 = (1 - cos(w0)) / 2 / a0; lb1 = (1 - cos(w0)) / a0; lb2 = lb0;
        la1 = -2 * cos(w0) / a0; la2 = (1 - alpha) / a0;
        // the fixed filter of the A500, 6 dB an octave from about 4.4 kHz
        rc = 1 - exp(-2 * M_PI * 4420.0 / rate);
    }

    // The song goes on for this many frames; without "out" nothing is heard of it.
    // With "toEnd" it stops where the song ends. The answer is how many frames it went on.
    uint64_t run(int16_t *out, uint64_t frames, bool toEnd = false) {
        uint64_t todo = frames * OVER, done = 0, made = 0;
        while (todo) {
            if (untilTick <= 0) {
                nextTick();
                if (toEnd && endSeen) break;
            }
            uint64_t n = (uint64_t)((untilTick + 65535) / 65536);
            if (n > todo) n = todo;
            if (!out) {
                for (int i = 0; i < nvoices; i++) advance(i, n);
                phase = (int)((phase + n) % OVER);
            } else {
                for (uint64_t k = 0; k < n; k++) {
                    float a = 0, b = 0;
                    for (int i = 0; i < nvoices; i++) {
                        float s = sample(i);
                        if (i == 0 || i == 3 || i == 4 || i == 7) a += s; else b += s;
                    }
                    head = (head + 1) % TAPS;
                    hist[0][head] = hist[0][head + TAPS] = a;
                    hist[1][head] = hist[1][head + TAPS] = b;
                    if (++phase < OVER) continue;
                    phase = 0;
                    const float *ha = hist[0] + head + 1, *hb = hist[1] + head + 1;
                    float l = 0, r = 0;
                    for (int i = 0; i < TAPS; i++) { l += fir[i] * ha[i]; r += fir[i] * hb[i]; }
                    double dl = l, dr = r;
                    if (a500) {
                        lp[0] += rc * (dl - lp[0]); dl = lp[0];
                        lp[1] += rc * (dr - lp[1]); dr = lp[1];
                    }
                    dl = led(0, dl);
                    dr = led(1, dr);
                    double m = (dr - dl) * pan / 2;
                    out[2 * made] = clip((dl + m) * gain);
                    out[2 * made + 1] = clip((dr - m) * gain);
                    made++;
                }
            }
            untilTick -= (int64_t)n * 65536;
            todo -= n;
            done += n;
        }
        return done / OVER;
    }

    bool endSeen = false;           // every channel has ended or come round
    FILE *trace = nullptr;
    long ticks = 0;
    std::vector<Player::Write> writes;
    bool stoppedByItself() const { return player.stopped() || !player.running(); }

    bool audible() const {
        for (int i = 0; i < nvoices; i++) {
            const Voice &v = player.voice[i];
            if (!v.on || !v.vol) continue;
            for (uint32_t k = 0; k < v.curLen; k++) if (v.cur[k]) return true;
            if (v.loop) for (uint32_t k = 0; k < v.nxtLen; k++) if (v.nxt[k]) return true;
        }
        return false;
    }

    Player player;

private:
    int rate;
    double pan;
    bool a500;
    bool eight = false;
    int nvoices = 4;
    int64_t untilTick = 0;                          // in 1/65536 of a computed frame
    uint64_t step[8] = {};
    float fir[TAPS];
    float hist[2][2 * TAPS];
    int head = 0, phase = 0;
    double lp[2] = {0, 0};
    double lb0, lb1, lb2, la1, la2, rc;
    double lx[2][2] = {{0, 0}, {0, 0}}, ly[2][2] = {{0, 0}, {0, 0}};
    bool ledOn = false;
    double gain = 2.0;

    static double bessel(double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 40; k++) { term *= (x / 2) / k; sum += term * term; }
        return sum;
    }

    static int16_t clip(double v) {
        long s = lrint(v);
        return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }

    double led(int c, double x) {
        double y = lb0 * x + lb1 * lx[c][0] + lb2 * lx[c][1] - la1 * ly[c][0] - la2 * ly[c][1];
        lx[c][1] = lx[c][0]; lx[c][0] = x;
        ly[c][1] = ly[c][0]; ly[c][0] = y;
        return ledOn ? y : x;
    }

    void nextTick() {
        if (trace) { player.writes = &writes; writes.clear(); }
        player.tick();
        if (trace) {
            for (const Player::Write &w : writes)
                fprintf(trace, "W %ld %d %u %p %u\n", ticks, w.channel, w.words, (const void *)w.base, w.ofs);
            for (int i = 0; i < player.channels(); i++) {
                const Voice &v = player.voice[i];
                fprintf(trace, "T %ld %d %u %d %d %u %u\n", ticks, i, v.period, v.vol,
                        v.on && v.pos == 0 ? 1 : 0, v.curLen / 2, v.nxtLen / 2);
            }
        }
        ticks++;
        nvoices = player.channels();
        ledOn = player.ledFilter();
        if (player.allDone()) endSeen = true;
        untilTick += (int64_t)llrint(player.tickSeconds() * rate * OVER * 65536.0);
        for (int i = 0; i < nvoices; i++) {
            const Voice &v = player.voice[i];
            step[i] = v.period ? (uint64_t)(PAULA_CLOCK / v.period / (rate * OVER) * 4294967296.0) : 0;
        }
    }

    void wrap(Voice &v) {
        while (v.on && v.pos >= ((uint64_t)v.curLen << 32)) {
            v.pos -= (uint64_t)v.curLen << 32;
            if (!v.loop) { v.on = false; v.pos = 0; break; }
            v.cur = v.nxt;
            v.curLen = v.nxtLen;
            if (v.curLen < 1) { v.on = false; break; }
        }
    }

    void advance(int i, uint64_t frames) {
        Voice &v = player.voice[i];
        if (!v.on) return;
        v.pos += frames * step[i];
        wrap(v);
    }

    // the mean of what Paula puts out during one computed frame
    float sample(int i) {
        Voice &v = player.voice[i];
        if (!v.on) return 0;
        uint32_t at = (uint32_t)(v.pos >> 32);
        uint64_t frac = v.pos & 0xffffffffu, st = step[i];
        float s = v.cur[at];
        if (frac + st > 0x100000000ull && st) {
            float next;
            if (at + 1 < v.curLen) next = v.cur[at + 1];
            else next = v.loop ? v.nxt[0] : 0;
            double part = (double)(0x100000000ull - frac) / (double)st;
            s = (float)(s * part + next * (1 - part));
        }
        v.pos += st;
        wrap(v);
        return s * v.vol * (eight ? 0.5f : 1.0f);
    }
};

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: bbmp-musicline <file> <track> <samplerate> [start-ms]\n");
        return 2;
    }
    const char *path = argv[1];
    int track = atoi(argv[2]);
    int rate = atoi(argv[3]);
    long startMs = argc > 4 ? atol(argv[4]) : 0;
    if (rate < 8000 || rate > 192000) return fail("impossible sample rate");
    if (startMs < 0) startMs = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return fail("can't open the file");
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        data.insert(data.end(), buf, buf + got);
        if (data.size() > (256u << 20)) { fclose(f); return fail("the file is too large"); }
    }
    fclose(f);

    Module mod;
    std::string err;
    if (!mod.load(data.data(), data.size(), err)) return fail(err);
    int tracks = (int)mod.tunes.size();
    if (track < 0 || track >= tracks) return fail("no such track");

    const char *e;
    double pan = (e = getenv("MLINE_PAN")) ? atof(e) : 0.4;
    bool a500 = (e = getenv("MLINE_FILTER")) && !strcasecmp(e, "a500");
    double maxSeconds = (e = getenv("MLINE_MAX")) ? atof(e) : 900;
    if (pan < 0) pan = 0;
    if (pan > 1) pan = 1;
    if (maxSeconds < 1) maxSeconds = 1;

    // Once through without sound, to know how long the song is.
    const uint64_t maxFrames = (uint64_t)(maxSeconds * rate);
    const uint64_t tail = (uint64_t)rate * 6, fade = (uint64_t)rate * 3;
    uint64_t total = 0, fadeFrom = UINT64_MAX;
    bool found = false;
    {
        Machine m(mod, track, rate, pan, a500);
        total = m.run(nullptr, maxFrames, true);
        found = m.endSeen;
        if (!found) {
            fadeFrom = total > fade ? total - fade : 0;
        } else if (m.stoppedByItself()) {
            // the song has stopped: what still sounds may sound, but not for ever
            uint64_t end = total;
            while (m.audible() && total - end < tail) total += m.run(nullptr, rate / 100);
            if (total - end >= tail) fadeFrom = total - fade;
        } else {
            // the song goes round: stop where it would start again, without a click
            uint64_t ms20 = (uint64_t)rate / 50;
            fadeFrom = total > ms20 ? total - ms20 : 0;
        }
    }

    const Tune &tune = mod.tunes[track];
    std::string title = mod.title.empty() ? tune.title : mod.title;
    if (tracks > 1 && !tune.title.empty() && tune.title != title) title += " - " + tune.title;
    std::string type = "Musicline Editor";
    if (!mod.version.empty()) type += " " + mod.version;
    std::string comment;
    if (!mod.date.empty()) comment = mod.date;
    for (const std::string &s : mod.info) {
        if (s.empty()) continue;
        if (!comment.empty()) comment += "\n";
        comment += s;
    }
    std::string names;
    for (int i = 1; i <= mod.numInst; i++) {
        if (i > 1) names += ",";
        names += json(mod.insts[i].title);
    }
    printf("{\"tracks\":%d,\"track\":%d,\"title\":%s,\"author\":%s,\"type\":%s,\"duration_ms\":%llu,"
           "\"comment\":%s,\"names\":[%s],\"channels\":%d,\"end_found\":%s}\n",
           tracks, track, json(title).c_str(), json(mod.author).c_str(), json(type).c_str(),
           (unsigned long long)(total * 1000 / rate), json(comment).c_str(), names.c_str(),
           tune.playMode ? 8 : 4, found ? "true" : "false");
    fflush(stdout);

    Machine m(mod, track, rate, pan, a500);
    if ((e = getenv("MLINE_TRACE"))) m.trace = fopen(e, "w");
    uint64_t at = 0, skip = (uint64_t)startMs * rate / 1000;
    if (skip > total) skip = total;
    std::vector<int16_t> pcm(2 * 4096);
    while (at < total) {
        size_t n = 4096;
        if (total - at < n) n = (size_t)(total - at);
        if (at < skip) {                            // up to the place to start at, unheard
            if (skip - at < n) n = (size_t)(skip - at);
            m.run(nullptr, n);
            at += n;
            continue;
        }
        m.run(pcm.data(), n);
        if (fadeFrom != UINT64_MAX && at + n > fadeFrom) {
            for (size_t i = 0; i < n; i++) {
                uint64_t p = at + i;
                if (p < fadeFrom) continue;
                double g = (double)(total - p) / (double)(total - fadeFrom);
                pcm[2 * i] = (int16_t)lrint(pcm[2 * i] * g);
                pcm[2 * i + 1] = (int16_t)lrint(pcm[2 * i + 1] * g);
            }
        }
        const char *p = (const char *)pcm.data();
        size_t left = n * 4;
        while (left) {
            ssize_t w = write(1, p, left);
            if (w <= 0) return 0;                   // nobody listens any more
            p += w;
            left -= (size_t)w;
        }
        at += n;
    }
    if (m.trace) fclose(m.trace);
    return 0;
}
