// SPDX-License-Identifier: GPL-2.0-or-later
// bbmp-vgm: plays a VGM, VGZ or S98 file and writes its sound for the ByteBandit Music
// Player.
//
//   bbmp-vgm <file> <track> <samplerate> [start-ms]
//
// Line one of the output is JSON describing the song; raw 16-bit stereo (little
// endian) follows. The song is played round and round, 20 ms at a time, until 10 seconds
// past its length (30 minutes when it has none).
#include "vgmplay.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::string jsonText(const std::string& text)
{
    // spaces and line breaks at either end are left out
    size_t from = 0, to = text.size();
    while (from < to && (unsigned char)text[from] <= 0x20)
        from++;
    while (to > from && (unsigned char)text[to - 1] <= 0x20)
        to--;
    const std::string s = text.substr(from, to - from);
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:
            if (c < 0x20) {
                out += ' ';             // line breaks and other control characters
            } else if (c < 0x80) {
                out += (char)c;
            } else {
                // keep well-formed UTF-8, replace anything else
                int n = (c >= 0xF0 && c < 0xF5) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC2 && c < 0xE0) ? 1 : -1;
                bool ok = n > 0 && i + n < s.size() + 0;
                for (int k = 1; ok && k <= n; k++)
                    ok = i + k < s.size() && ((unsigned char)s[i + k] & 0xC0) == 0x80;
                if (ok) {
                    out.append(s, i, n + 1);
                    i += n;
                } else
                    out += "\xEF\xBF\xBD";
            }
        }
    }
    return out + "\"";
}

// Cut a text to at most `max` bytes, not inside a UTF-8 character.
static void cutText(std::string& s, size_t max)
{
    if (s.size() <= max)
        return;
    size_t n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    s.resize(n);
}

static void fail(const std::string& why)
{
    std::string line = "{\"tracks\":0,\"error\":" + jsonText(why) + "}\n";
    fwrite(line.data(), 1, line.size(), stdout);
    fflush(stdout);
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <file> <track> <samplerate> [start-ms]\n", argv[0]);
        return 2;
    }
    long rate = strtol(argv[3], nullptr, 10);
    long long startMs = argc > 4 ? strtoll(argv[4], nullptr, 10) : 0;
    if (rate < 8000 || rate > 192000) {
        fail("unsupported sample rate");
        return 1;
    }
    if (startMs < 0)
        startMs = 0;

    std::string error;
    std::vector<uint8_t> data;
    if (!vgm::loadFile(argv[1], data, error, 64u << 20)) {
        fail(error);
        return 1;
    }
    auto song = vgm::openSong(std::move(data), error);
    if (!song || !song->start((uint32_t)rate, error)) {
        fail(error.empty() ? "not a VGM or S98 file" : error);
        return 1;
    }

    // the player reads at most 64 KB of this line: very long tags are cut
    for (std::string* t : {&song->title, &song->author, &song->program, &song->computer, &song->date})
        cutText(*t, 4000);
    cutText(song->comment, 20000);
    std::string head = "{\"tracks\":1,\"track\":0,\"subpath\":\"\",\"title\":" + jsonText(song->title) +
        ",\"author\":" + jsonText(song->author) + ",\"program\":" + jsonText(song->program) +
        ",\"type\":" + jsonText(song->type) + ",\"duration_ms\":" + std::to_string(song->lengthMs) +
        ",\"facts\":{\"Computer\":" + jsonText(song->computer) + ",\"Date\":" + jsonText(song->date) +
        "},\"comment\":" + jsonText(song->comment) + "}\n";
    fwrite(head.data(), 1, head.size(), stdout);

    uint64_t lengthMs = song->lengthMs ? song->lengthMs + 10000 : 30ull * 60 * 1000;
    uint64_t limit = lengthMs * (uint64_t)rate / 1000;
    uint64_t done = (uint64_t)startMs * (uint64_t)rate / 1000;
    if (done)
        song->seek(done);
    const uint32_t frame = (uint32_t)rate / 50;          // 20 ms
    std::vector<int16_t> buf(2 * (size_t)frame);
    while (done < limit) {
        uint32_t n = song->render(buf.data(), frame);
        if (n == 0)
            break;
        if (fwrite(buf.data(), 4, n, stdout) != n)
            break;                      // the player stopped listening
        done += n;
    }
    fflush(stdout);
    return 0;
}
