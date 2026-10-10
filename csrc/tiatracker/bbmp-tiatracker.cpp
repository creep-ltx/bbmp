// SPDX-License-Identifier: GPL-2.0-only
// bbmp-tiatracker: play a TIATracker song (.ttt, Atari 2600 music), as raw PCM on stdout.
//
//   bbmp-tiatracker <file> <track> <samplerate> [start-ms]
//   bbmp-tiatracker --regs <file> [frames]     the TIA's sound registers, a frame a line
//
// TIATracker is Andre "Kylearan" Wichmann's tracker for the Atari 2600's TIA chip; its
// songs are JSON. The song is read and played by the tracker's own code (track/, the
// replay logic of emulation/player.cpp, GPL-2), with Stella's TIA sound emulation
// (emulation/TIASnd.cpp, GPL-2-or-later), and Qt replaced by small stand-ins (shim/).
// Because that code is GPL-2 only, this is a separate program under the GPL-2.
//
// Same protocol as the other helpers: one JSON line describing the track, then signed
// 16-bit little-endian stereo until the song ends. The song is played once from the
// start (the tracker's "play from start", with its start patterns) until it ends or a
// goto would bring it back to a row it has played, at most 30 minutes.
//
// What the replay code trusts, a damaged file can't use: every pattern a channel plays
// must exist and have notes, every note's instrument must exist, the start patterns
// must be in the sequences and the speeds one a song can have; and the lists the code
// reads check their index (shim/qtshim.h), so anything missed ends with an error.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "qtshim.h"
#include "mainwindow.h"
#include "track/track.h"
#include "emulation/player.h"
#include "emulation/TIASnd.h"

QString MainWindow::lastMessage;

namespace {

const size_t MAX_FILE = 2u << 20;   // the largest known song is 460 KB
const int MAX_MINUTES = 30;

// Each row the replay code starts, as positions in the two channels' tracks
std::set<std::pair<int, int>> g_rows;
bool g_watch = false, g_again = false;
std::string g_stopped;   // why the replay code stopped the song early, if it did

}  // namespace

// The replay code's two signals: here, the loop watch and the reason a song stops
void Emulation::Player::newPlayerPos(int pos1, int pos2) {
    if (g_watch && !g_rows.insert({pos1, pos2}).second) g_again = true;
}
void Emulation::Player::invalidNoteFound(int channel, int entry, int note, QString reason) {
    g_stopped = reason.str() + " (channel " + std::to_string(channel + 1) + ", sequence entry "
            + std::to_string(entry + 1) + ", row " + std::to_string(note + 1) + ")";
}

namespace {

void json(const std::string &s) {
    putchar('"');
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c < 0x20 || c == 0x7f) printf("\\u%04x", c);
        else putchar(c);             // UTF-8, checked by the JSON reader
    }
    putchar('"');
}

int fail(const std::string &why) {
    printf("{\"tracks\":0,\"error\":");
    json(why);
    printf("}\n");
    fflush(stdout);
    return 1;
}

bool speedOk(int s) { return s >= 1 && s <= 255; }

// What the replay code would trust. Empty when the song can be played.
std::string check(Track::Track &t) {
    for (int c = 0; c < 2; ++c) {
        const auto &seq = t.channelSequences[c].sequence;
        if (seq.size() == 0) return "channel " + std::to_string(c + 1) + " has no patterns";
        if (t.startPatterns[c] < 0 || t.startPatterns[c] >= seq.size())
            return "channel " + std::to_string(c + 1) + "'s start pattern is not in its sequence";
        for (int e = 0; e < seq.size(); ++e) {
            int p = seq[e].patternIndex;
            if (p < 0 || p >= t.patterns.size())
                return "a sequence plays pattern " + std::to_string(p + 1) + ", which isn't there";
            const auto &notes = t.patterns[p].notes;
            if (notes.size() == 0) return "a pattern that is played has no rows";
            if (notes.size() > 4096) return "a pattern has too many rows";
            for (const auto &n : notes) {
                if (n.type == Track::Note::instrumentType::Instrument
                        && (n.instrumentNumber < 0 || n.instrumentNumber >= Track::Track::numInstruments))
                    return "a note plays instrument " + std::to_string(n.instrumentNumber + 1)
                            + ", which isn't there";
                // a note's pitch is 0-31 (and 32-63 for the two-part "pure combined"
                // waveform); a slide moves it by -7 to 7 in the tracker, allowed to 63
                if (n.type == Track::Note::instrumentType::Instrument && (n.value < 0 || n.value > 63))
                    return "a note has a pitch outside 0-63";
                if (n.type == Track::Note::instrumentType::Slide && (n.value < -63 || n.value > 63))
                    return "a slide is larger than a whole range of pitches";
                if (n.type == Track::Note::instrumentType::Percussion
                        && (n.instrumentNumber < 0 || n.instrumentNumber >= Track::Track::numPercussion))
                    return "a note plays percussion " + std::to_string(n.instrumentNumber + 1)
                            + ", which isn't there";
            }
            // the tempo is the first channel's pattern's when it is set per pattern
            if (c == 0 && !t.globalSpeed && (!speedOk(t.patterns[p].evenSpeed) || !speedOk(t.patterns[p].oddSpeed)))
                return "a pattern's speed is not one a song can have";
        }
    }
    if (t.globalSpeed && (!speedOk(t.evenSpeed) || !speedOk(t.oddSpeed)))
        return "the song's speed is not one a song can have";
    return "";
}

// The tracker's "play from start": each channel from the first row of its start pattern
void start(Emulation::Player &p, Track::Track &t) {
    int r0 = t.channelSequences[0].sequence[t.startPatterns[0]].firstNoteNumber;
    int r1 = t.channelSequences[1].sequence[t.startPatterns[1]].firstNoteNumber;
    p.playTrack(r0, r1);
}

// How many frames the song plays before it ends or comes back to a row it has played
long measure(Track::Track &t, long maxFrames) {
    Emulation::Player p(&t);
    g_rows.clear();
    g_again = false;
    g_watch = true;
    start(p, t);
    long frames = 0;
    while (frames < maxFrames) {
        p.bbmpFrame();
        if (g_again || !p.bbmpPlaying()) break;
        ++frames;
    }
    g_watch = false;
    return frames;
}

std::string tempo(Track::Track &t) {
    if (!t.globalSpeed) return "set per pattern";
    if (t.evenSpeed == t.oddSpeed) return std::to_string(t.evenSpeed) + " frames a row";
    // the replay code takes oddSpeed on the rows it counts from 0 as even
    return std::to_string(t.oddSpeed) + " and " + std::to_string(t.evenSpeed)
            + " frames a row, in turn (funktempo)";
}

}  // namespace

int main(int argc, char **argv) {
    bool regs = argc >= 3 && std::strcmp(argv[1], "--regs") == 0;
    if (!regs && argc < 4) {
        fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n"
                        "       %s --regs <file> [frames]\n", argv[0], argv[0]);
        return 2;
    }
    const char *path = regs ? argv[2] : argv[1];
    int rate = regs ? 48000 : atoi(argv[3]);
    if (rate < 8000 || rate > 192000) rate = 48000;
    long long startMs = !regs && argc > 4 ? atoll(argv[4]) : 0;
    if (startMs < 0) startMs = 0;
    if (!regs && atoi(argv[2]) != 0) return fail("a TIATracker song has one track");

    FILE *in = fopen(path, "rb");
    if (!in) return fail("can't read the file");
    std::string text(MAX_FILE + 1, '\0');
    size_t size = fread(&text[0], 1, MAX_FILE + 1, in);
    fclose(in);
    if (size == 0) return fail("can't read the file");
    if (size > MAX_FILE) return fail("the file is too big for a TIATracker song");
    text.resize(size);

    bool said = false;
    try {
        QJsonObject root;
        std::string why;
        if (!bbmpParseJson(text, root, why)) return fail(why);
        text.clear();
        if (!root.contains("instruments") || !root.contains("patterns") || !root.contains("channels"))
            return fail("not a TIATracker song (no instruments, patterns or channels)");
        Track::Track track;
        if (!track.fromJson(root)) {
            std::string m = MainWindow::lastMessage.str();
            return fail(m.empty() ? "the song is damaged (TIATracker would not load it)"
                                  : "the song is damaged: " + m);
        }
        std::string bad = check(track);
        if (!bad.empty()) return fail("the song is damaged: " + bad);

        bool pal = track.getTvMode() == TiaSound::TvStandard::PAL;
        int fps = pal ? 50 : 60;
        long maxFrames = long(MAX_MINUTES) * 60 * fps;

        if (regs) {
            long want = argc > 3 ? atol(argv[3]) : maxFrames;
            Emulation::Player p(&track);
            start(p, track);
            for (long f = 0; f < want; ++f) {
                p.bbmpFrame();
                const uInt8 *r = p.bbmpSound().registers();
                printf("%ld %d %d %d %d %d %d%s\n", f, r[0], r[2], r[4], r[1], r[3], r[5],
                       p.bbmpPlaying() ? "" : " end");
                if (!p.bbmpPlaying()) break;
            }
            return 0;
        }

        long frames = measure(track, maxFrames);
        if (frames <= 0) {
            return fail(g_stopped.empty() ? "the song has nothing to play"
                                          : "the song stops at once: " + g_stopped);
        }
        bool loops = g_again;

        int melodic = 0, perc = 0;
        for (auto &i : track.instruments) melodic += i.isEmpty() ? 0 : 1;
        for (auto &p : track.percussion) perc += p.isEmpty() ? 0 : 1;

        printf("{\"tracks\":1,\"track\":0,\"title\":");
        json(track.metaName.str());
        printf(",\"author\":");
        json(track.metaAuthor.str());
        printf(",\"type\":\"TIATracker\",\"duration_ms\":%lld,\"facts\":{\"System\":",
               (long long)frames * 1000 / fps);
        json(pal ? "Atari 2600 (TIA), PAL, 50 frames a second"
                 : "Atari 2600 (TIA), NTSC, 60 frames a second");
        printf(",\"Tempo\":");
        json(tempo(track));
        printf(",\"Instruments\":");
        json(std::to_string(melodic) + " melodic, " + std::to_string(perc) + " percussion");
        printf(",\"Ends\":");
        if (frames >= maxFrames) json("cut at 30 minutes");
        else if (loops) json("goes back to the start of its loop");
        else if (!g_stopped.empty()) json("stops at a note TIATracker can't play: " + g_stopped);
        else json("stops at its last row");
        printf("},\"comment\":");
        json(track.metaComment.str());
        printf(",\"names\":[");
        bool first = true;
        // "---" is the tracker's name for an instrument not given one
        for (auto &i : track.instruments) {
            if (i.isEmpty() || i.name == "---") continue;
            if (!first) putchar(',');
            json(i.name.str());
            first = false;
        }
        for (auto &p : track.percussion) {
            if (p.isEmpty() || p.name == "---") continue;
            if (!first) putchar(',');
            json(p.name.str());
            first = false;
        }
        printf("]}\n");
        fflush(stdout);
        said = true;

        // Play: each frame, the replay code sets the registers; the TIA then makes that
        // frame's samples (as SoundSDL2 hands them on at the frame's sample)
        Emulation::TIASound tia(rate);
        tia.channels(2, true);            // the two channels apart, mixed below
        Emulation::Player p(&track);
        start(p, track);
        long skip = long(startMs * fps / 1000);
        std::vector<Int16> buf;
        std::vector<int16_t> out;
        double dc[2] = {0, 0};
        long long sampleAt = 0;
        for (long f = 0; f < frames; ++f) {
            p.bbmpFrame();
            const uInt8 *r = p.bbmpSound().registers();
            for (uInt16 a = Emulation::AUDC0; a <= Emulation::AUDV1; ++a) tia.set(a, r[a - Emulation::AUDC0]);
            long long next = (long long)((double)(f + 1) * rate / fps + 0.5);
            uInt32 n = uInt32(next - sampleAt);
            sampleAt = next;
            buf.assign(size_t(n) * 2, 0);
            tia.process(buf.data(), n);
            out.resize(size_t(n) * 2);
            for (uInt32 k = 0; k < n; ++k) {
                double side[2];
                for (int c = 0; c < 2; ++c) {
                    // the TIA's sound is 0..15 levels above silence: take away the steady part
                    double v = buf[k * 2 + c];
                    dc[c] += (v - dc[c]) * 0.0005;
                    side[c] = (v - dc[c]) * 1.5;
                }
                for (int c = 0; c < 2; ++c) {
                    // each channel mostly on one side, a quarter of it on the other
                    double v = side[c] * 0.75 + side[1 - c] * 0.25;
                    out[k * 2 + c] = int16_t(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            if (f >= skip && fwrite(out.data(), 4, n, stdout) != n) return 0;
        }
        fflush(stdout);
    } catch (const std::exception &e) {
        // a list read outside its end: the song is damaged in a way not checked above
        if (!said) return fail(std::string("the song is damaged (") + e.what() + ")");
        fflush(stdout);
        fprintf(stderr, "bbmp-tiatracker: %s\n", e.what());
        return 1;
    }
    return 0;
}
