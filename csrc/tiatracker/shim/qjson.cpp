// SPDX-License-Identifier: GPL-2.0-only
// bbmp-tiatracker: a small JSON reader, in place of Qt's QJsonDocument::fromJson.
// Written for bbmp. Strings come out as UTF-8; a byte that is no UTF-8 becomes U+FFFD.
#include "qtshim.h"

#include <cstdlib>
#include <cstring>

namespace {

const int MAX_DEPTH = 64;

struct Reader {
    const std::string &t;
    size_t i = 0;
    std::string why;

    explicit Reader(const std::string &text) : t(text) {}

    bool fail(const char *w) {
        if (why.empty()) why = w;
        return false;
    }
    void space() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i;
    }
    bool eat(char c) {
        space();
        if (i < t.size() && t[i] == c) { ++i; return true; }
        return false;
    }

    static void put(std::string &out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xc0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += char(0xe0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        } else {
            out += char(0xf0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3f));
            out += char(0x80 | ((cp >> 6) & 0x3f));
            out += char(0x80 | (cp & 0x3f));
        }
    }

    bool hex4(uint32_t &v) {
        if (i + 4 > t.size()) return fail("a broken \\u in a string");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return fail("a broken \\u in a string");
        }
        return true;
    }

    // One UTF-8 character from the file, checked; U+FFFD for what isn't UTF-8
    uint32_t utf8() {
        unsigned char c = (unsigned char)t[i++];
        if (c < 0x80) return c;
        int more = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 && c < 0xe0 ? 1 : -1;
        if (more < 0) return 0xfffd;
        uint32_t cp = c & (0x3f >> more);
        for (int k = 0; k < more; ++k) {
            if (i >= t.size() || ((unsigned char)t[i] & 0xc0) != 0x80) return 0xfffd;
            cp = (cp << 6) | ((unsigned char)t[i++] & 0x3f);
        }
        static const uint32_t least[4] = {0, 0x80, 0x800, 0x10000};
        if (cp < least[more] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0xfffd;
        return cp;
    }

    bool string(std::string &out) {
        if (!eat('"')) return fail("a string was expected");
        for (;;) {
            if (i >= t.size()) return fail("a string has no end");
            char c = t[i];
            if (c == '"') { ++i; return true; }
            if ((unsigned char)c < 0x20) return fail("a control character in a string");
            if (c != '\\') { put(out, utf8()); continue; }
            ++i;
            if (i >= t.size()) return fail("a string has no end");
            char e = t[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    uint32_t lo;
                    if (i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                        i += 2;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xdc00 && lo <= 0xdfff) cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        else { put(out, 0xfffd); cp = lo >= 0xd800 && lo <= 0xdfff ? 0xfffd : lo; }
                    } else {
                        cp = 0xfffd;
                    }
                } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                    cp = 0xfffd;
                }
                put(out, cp);
                break;
            }
            default: return fail("a broken escape in a string");
            }
        }
    }

    bool number(QJsonValue &v) {
        size_t start = i;
        if (i < t.size() && t[i] == '-') ++i;
        if (i >= t.size() || !(t[i] >= '0' && t[i] <= '9')) return fail("a broken number");
        if (t[i] == '0') ++i;
        else while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
        if (i < t.size() && t[i] == '.') {
            ++i;
            if (i >= t.size() || !(t[i] >= '0' && t[i] <= '9')) return fail("a broken number");
            while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
        }
        if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
            ++i;
            if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
            if (i >= t.size() || !(t[i] >= '0' && t[i] <= '9')) return fail("a broken number");
            while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i;
        }
        if (i - start > 400) return fail("a number too long");
        std::string s = t.substr(start, i - start);
        double d = std::strtod(s.c_str(), nullptr);   // huge ones become inf: toInt() gives 0
        v = QJsonValue(d);
        return true;
    }

    bool literal(const char *word) {
        size_t n = std::strlen(word);
        if (t.compare(i, n, word) != 0) return fail("an unknown word");
        i += n;
        return true;
    }

    bool value(QJsonValue &v, int depth) {
        space();
        if (i >= t.size()) return fail("the file ends too soon");
        char c = t[i];
        if (c == '{') {
            QJsonObject o;
            if (!object(o, depth + 1)) return false;
            v = QJsonValue(o);
            return true;
        }
        if (c == '[') {
            QJsonArray a;
            if (!array(a, depth + 1)) return false;
            v = QJsonValue(a);
            return true;
        }
        if (c == '"') {
            std::string s;
            if (!string(s)) return false;
            v = QJsonValue(QString(s));
            return true;
        }
        if (c == 't') { v = QJsonValue(true); return literal("true"); }
        if (c == 'f') { v = QJsonValue(false); return literal("false"); }
        if (c == 'n') { v = QJsonValue(QJsonValue::Null); return literal("null"); }
        return number(v);
    }

    bool array(QJsonArray &a, int depth) {
        if (depth > MAX_DEPTH) return fail("nested too deep");
        if (!eat('[')) return fail("an array was expected");
        if (eat(']')) return true;
        for (;;) {
            QJsonValue v;
            if (!value(v, depth)) return false;
            a.append(v);
            if (eat(',')) continue;
            if (eat(']')) return true;
            return fail("a broken array");
        }
    }

    bool object(QJsonObject &o, int depth) {
        if (depth > MAX_DEPTH) return fail("nested too deep");
        if (!eat('{')) return fail("an object was expected");
        if (eat('}')) return true;
        for (;;) {
            space();
            std::string key;
            if (!string(key)) return false;
            if (!eat(':')) return fail("a broken object");
            QJsonValue v;
            if (!value(v, depth)) return false;
            o.insert(QString(key), v);     // a key given twice: the last one counts
            if (eat(',')) continue;
            if (eat('}')) return true;
            return fail("a broken object");
        }
    }
};

}  // namespace

bool bbmpParseJson(const std::string &text, QJsonObject &out, std::string &why) {
    Reader r(text);
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) r.i = 3;
    r.space();
    if (r.i >= text.size() || text[r.i] != '{') {
        why = "not a TIATracker song (no JSON object)";
        return false;
    }
    if (!r.object(out, 1)) {
        why = "not a TIATracker song (" + r.why + " at byte " + std::to_string(r.i) + ")";
        return false;
    }
    r.space();
    while (r.i < text.size() && text[r.i] == '\0') ++r.i;
    r.space();
    if (r.i != text.size()) {
        why = "not a TIATracker song (something after the end)";
        return false;
    }
    return true;
}
