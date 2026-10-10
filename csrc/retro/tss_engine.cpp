// SPDX-License-Identifier: BSD-3-Clause
// T'SoundSystem (TSS) by Takashi Toyoshima: music written as MML text (.tss), which
// a compiler turns into sequence data (.tsd) for a driver with simple sound modules
// of its own (square wave, square wave with duty, noise, sine with frequency
// modulation, wave table).
//
// This is a port to C++ of the compiler, the player and the sound device of the
// JavaScript version of T'SoundSystem, https://github.com/toyoshim/tss (js/tss/
// TssCompiler.js, TsdPlayer.js, TssChannel.js, MasterChannel.js; commit f3778995,
// BSD 3-clause licence, copyright (c) 2014 Takashi Toyoshima; the licence, which this
// port keeps, is in csrc/retro/LICENSE.tss). What the JavaScript
// version leaves out and this port adds, after the MML manual
// (https://www.toyoshima-house.net/classic/tss/manual.html):
//   - volume given for left and right (v5,15 and @v80,240), which the JavaScript
//     player reads and ignores, so that such songs stay silent there
//   - relative volume (_ and ~), the OSC module (5)
//   - TSD files of version 0.60, which have no tables
//   - text in Shift-JIS
// Not known here and refused by the compiler: nt, ns, ph, &, the Game Boy mode.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "engine.h"

using namespace retro;

namespace {

// ---------------------------------------------------------------- the commands

enum {
  CMD_LAST_NOTE = 0x7f,
  CMD_NOTE_OFF = 0x80,
  CMD_VOLUME_MONO = 0x81,
  CMD_SUSTAIN_MODE = 0x82,
  CMD_DETUNE = 0x83,
  CMD_PORTAMENT = 0x84,
  CMD_VOLUME_LEFT = 0x85,
  CMD_VOLUME_RIGHT = 0x86,
  CMD_PANPOT = 0x87,
  CMD_RELATIVE_VOLUME_UP = 0x88,
  CMD_RELATIVE_VOLUME_DOWN = 0x89,
  CMD_TEMPO = 0x90,
  CMD_FINENESS = 0x91,
  CMD_KEY_ON_PHASE = 0x92,
  CMD_MULTIPLE = 0x93,
  CMD_PITCH_MODULATION_DELAY = 0xa0,
  CMD_PITCH_MODULATION_DEPTH = 0xa1,
  CMD_PITCH_MODULATION_WIDTH = 0xa2,
  CMD_PITCH_MODULATION_HEIGHT = 0xa3,
  CMD_PITCH_MODULATION_DELTA = 0xa4,
  CMD_AMP_ENVELOPE = 0xb8,
  CMD_NOTE_ENVELOPE = 0xc8,
  CMD_ENDLESS_LOOP_POINT = 0xe0,
  CMD_LOCAL_LOOP_START = 0xe1,
  CMD_LOCAL_LOOP_BREAK = 0xe2,
  CMD_LOCAL_LOOP_END = 0xe3,
  CMD_FREQUENCY_MODE_CHANGE = 0xf0,
  CMD_VOLUME_MODE_CHANGE = 0xf1,
  CMD_FM_IN = 0xf8,
  CMD_FM_OUT = 0xf9,
  CMD_CONTROL_CHANGE = 0xfb,
  CMD_PORT_CHANGE = 0xfc,
  CMD_VOICE_CHANGE = 0xfd,
  CMD_MODULE_CHANGE = 0xfe,
  CMD_END = 0xff,
};

const int kVersionMajor = 0, kVersionMinor = 94;
const int kDefaultTimerCount = 368;

// ---------------------------------------------------------------- the compiler

struct CompileError : std::runtime_error {
  CompileError(int line, const std::string& what)
      : std::runtime_error("line " + std::to_string(line) + ": " + what) {}
};

typedef std::vector<uint8_t> Bytes;

size_t countSpaces(const Bytes& d, size_t offset) {
  size_t n = 0;
  for (size_t i = offset; i < d.size(); i++) {
    if (d[i] != 0 && d[i] != '\t' && d[i] != ' ') break;
    n++;
  }
  return n;
}

int numberAt(const Bytes& d, size_t offset) {
  if (offset >= d.size() || d[offset] < '0' || d[offset] > '9') return -1;
  return d[offset] - '0';
}

char lowerAt(const Bytes& d, size_t offset) {
  const uint8_t c = d[offset];
  return static_cast<char>(c >= 'A' && c <= 'Z' ? c | 0x20 : c);
}

struct Number {
  bool found = false;
  size_t begin = 0, end = 0;
  long value = 0;
};

Number getNumber(const Bytes& d, size_t offset) {
  Number r;
  size_t begin = offset + countSpaces(d, offset);
  r.begin = r.end = begin;
  if (begin >= d.size()) return r;
  long sign = 1;
  if (d[begin] == '-') {
    begin++;
    begin += countSpaces(d, begin);
    sign = -1;
  }
  r.begin = r.end = begin;
  long n = numberAt(d, begin);
  if (n < 0) return r;
  r.found = true;
  size_t i = begin + 1;
  for (; i < d.size(); i++) {
    if (d[i] == 0 || d[i] == '\\') continue;
    const int digit = numberAt(d, i);
    if (digit < 0) break;
    if (n < 100000000) n = n * 10 + digit;
  }
  r.end = i - 1;
  r.value = sign * n;
  return r;
}

// The place of <character> if it is the next thing after blanks, or -1.
long getCharacter(const Bytes& d, size_t offset, char character) {
  const size_t position = offset + countSpaces(d, offset);
  if (position >= d.size() || d[position] != static_cast<uint8_t>(character)) return -1;
  return static_cast<long>(position);
}

bool checkEnd(const Bytes& d, size_t offset) {
  return offset + countSpaces(d, offset) >= d.size();
}

class Compiler {
 public:
  // The TSD data, or a CompileError.
  Bytes compile(const Bytes& source) {
    source_ = source;
    sjis_ = to_utf8(std::string(source.begin(), source.end())) !=
            std::string(source.begin(), source.end());
    parseLines();
    parseDirectives();
    if (!hasTitle_) throw CompileError(0, "no #TITLE");
    if (channelCount_ < 1) throw CompileError(0, "no #CHANNEL");
    if (channelCount_ > 26 * 27) throw CompileError(0, "too many channels");
    parseChannels();
    return generate();
  }
  long notes() const { return notes_; }

 private:
  struct Line {
    int number = 0;
    Bytes data;
    bool empty = true;
    bool named = false;         // has a directive
    bool isChannel = false;
    int channel = 0;
    std::string directive;
    bool continuation = false;
  };

  static bool sjisLead(uint8_t c) { return (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc); }
  static bool sjisTrail(uint8_t c) { return (c >= 0x40 && c <= 0x7e) || (c >= 0x80 && c <= 0xfc); }

  Line preprocess(int number, size_t offset, size_t count) {
    Line line;
    line.number = number;
    line.data.assign(source_.begin() + offset, source_.begin() + offset + count);
    Bytes& d = line.data;
    for (size_t i = 0; i < count; i++) {
      const uint8_t c = d[i];
      // a Japanese character's second byte may look like \ { or }
      const bool wide = sjis_ && sjisLead(c) && i + 1 < count && sjisTrail(d[i + 1]);
      if (commentNest_ > 0) {
        if (wide) {
          d[i++] = 0;
        } else if (c == '\\') {
          d[i++] = 0;
        } else if (c == '{') {
          commentNest_++;
        } else if (c == '}') {
          commentNest_--;
        }
        if (i < count) d[i] = 0;
      } else if (wide) {
        i++;
        line.empty = false;
      } else if (c == '\\') {
        d[i++] = 0;
        line.empty = false;
      } else if (c == '{') {
        commentNest_++;
        d[i] = 0;
      } else if (c == '}') {
        commentNest_--;
        d[i] = 0;
        if (commentNest_ < 0) throw CompileError(number, "'}' appears without '{'");
      } else {
        if (c == '\t') d[i] = ' ';
        if (d[i] != ' ') line.empty = false;    // (the original takes blanks for data)
      }
    }
    if (!line.empty) checkDirective(line);
    return line;
  }

  static void checkDirective(Line& line) {
    Bytes& d = line.data;
    size_t i = 0;
    bool found = false;
    for (; i < d.size() && !found; i++) {
      if (d[i] == 0 || d[i] == ' ') continue;   // (the original allows no blanks before #)
      if (d[i] != '#') return;
      d[i] = 0;
      found = true;
    }
    if (!found) return;
    const size_t start = i;
    for (; i < d.size(); i++)
      if (d[i] == ' ') break;
    std::string name;
    for (size_t k = start; k < i && d[k]; k++) name += static_cast<char>(d[k]);
    for (size_t k = start; k < i; k++) d[k] = 0;
    line.named = true;
    line.directive = name;
    // "#A" or "#zx": a channel
    if (name.size() == 1 || name.size() == 2) {
      int channel = 0;
      bool letters = true;
      for (char ch : name) letters = letters && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'));
      if (letters) {
        size_t at = 0;
        if (name.size() == 2) channel = ((name[at++] | 0x20) - 'a' + 1) * 26;
        channel += (name[at] | 0x20) - 'a';
        line.isChannel = true;
        line.channel = channel;
      }
    }
  }

  void parseLines() {
    bool any = false, currentIsChannel = false;
    int currentChannel = 0;
    std::string currentDirective;
    int number = 1;
    for (size_t offset = 0; offset < source_.size(); number++) {
      size_t count = 0;
      while (offset + count < source_.size() && source_[offset + count] != '\r' &&
             source_[offset + count] != '\n')
        count++;
      Line line = preprocess(number, offset, count);
      if (!line.empty) {
        if (!line.named) {
          if (!any) throw CompileError(number, "invalid line without any directive");
          line.isChannel = currentIsChannel;
          line.channel = currentChannel;
          line.directive = currentDirective;
          line.continuation = true;
        } else {
          any = true;
          currentIsChannel = line.isChannel;
          currentChannel = line.channel;
          currentDirective = line.directive;
        }
        if (line.isChannel) {
          if (channels_.size() <= static_cast<size_t>(line.channel)) channels_.resize(line.channel + 1);
          channels_[line.channel].push_back(line);
        } else {
          if (line.directive == "END") break;
          directives_.push_back(line);
        }
      }
      offset += count;
      if (offset < source_.size() && source_[offset] == '\r') offset++;
      if (offset < source_.size() && source_[offset] == '\n') offset++;
    }
  }

  // ---- tables: "#WAV 0, <(0, 127),16, (-128, 127),16>", over several lines
  struct Work {
    const std::vector<const Line*>* lines;
    size_t line = 0, offset = 0;
    const Bytes& data() const { return (*lines)[line]->data; }
    int number() const { return (*lines)[line]->number; }
  };

  static char findNext(Work& w, const std::string& message) {
    while (checkEnd(w.data(), w.offset) && w.line + 1 < w.lines->size()) {
      w.line++;
      w.offset = 0;
    }
    w.offset += countSpaces(w.data(), w.offset);
    if (w.offset >= w.data().size()) throw CompileError(w.number(), message);
    return static_cast<char>(w.data()[w.offset]);
  }

  static void checkCharacter(Work& w, char ch, const std::string& message) {
    findNext(w, message);
    const long at = getCharacter(w.data(), w.offset, ch);
    if (at < 0) throw CompileError(w.number(), message);
    w.offset = at + 1;
  }

  static long tableNumber(Work& w, const std::string& message, bool ranged) {
    findNext(w, message);
    const Number n = getNumber(w.data(), w.offset);
    if (!n.found) throw CompileError(w.number(), message);
    if (ranged && (n.value < -128 || n.value > 127))
      throw CompileError(w.number(), "number is out of range -128 to 127");
    w.offset = n.end + 1;
    return n.value;
  }

  static std::vector<int> getTable(const std::vector<const Line*>& lines, int& id) {
    Work w;
    w.lines = &lines;
    const std::string in = " in #" + lines[0]->directive;
    const Number n = getNumber(w.data(), 0);
    if (!n.found) throw CompileError(w.number(), "id number not found" + in);
    if (n.value < 0 || n.value > 255) throw CompileError(w.number(), "id is out of range 0 to 255" + in);
    id = static_cast<int>(n.value);
    w.offset = n.end + 1;
    checkCharacter(w, ',', "',' not found after id number" + in);
    checkCharacter(w, '<', "'<' not found" + in);
    std::vector<int> table;
    for (;;) {
      char ch = findNext(w, "incomplete entry" + in);
      if (ch == '(') {
        w.offset++;
        const long x = tableNumber(w, "number not found after '('" + in, true);
        checkCharacter(w, ',', "',' not found after the first number after '('" + in);
        const long y = tableNumber(w, "the second number not found after '('" + in, true);
        checkCharacter(w, ')', "')' not found after the second number" + in);
        checkCharacter(w, ',', "',' not found after '(x,y)' syntax" + in);
        const long count = tableNumber(w, "number not found after '(x,y),' syntax" + in, false);
        if (count > 256) throw CompileError(w.number(), "table is too long" + in);
        for (long i = 0; i < count; i++)
          table.push_back(count < 2 ? 0 : static_cast<int>(x + static_cast<double>(y - x) * i / (count - 1)));
      } else {
        table.push_back(static_cast<int>(tableNumber(w, "number not found" + in, true)));
      }
      ch = findNext(w, "',' or '>' not found" + in);
      w.offset++;
      if (ch == '>') break;
      if (ch != ',') throw CompileError(w.number(), "',' or '>' not found" + in);
    }
    for (; w.line < lines.size(); w.line++, w.offset = 0)
      if (!checkEnd(w.data(), w.offset)) throw CompileError(w.number(), "unknown data after table" + in);
    if (table.size() > 255) throw CompileError(lines[0]->number, "table is too long" + in);
    return table;
  }

  static std::string word(const Line& line) {
    std::string s;
    for (uint8_t c : line.data)
      if (c && c != '\\') s += static_cast<char>(c);
    const size_t a = s.find_first_not_of(' ');
    const size_t b = s.find_last_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
  }

  void parseDirectives() {
    for (size_t i = 0; i < directives_.size(); i++) {
      const Line& line = directives_[i];
      const std::string& name = line.directive;
      const Bytes& d = line.data;
      if (name == "TABLE" || name == "WAV") {
        std::vector<const Line*> lines;
        for (;; i++) {
          lines.push_back(&directives_[i]);
          if (i + 1 == directives_.size() || !directives_[i + 1].continuation) break;
        }
        int id = 0;
        const std::vector<int> table = getTable(lines, id);
        if (name == "WAV") {
          if (table.size() != 32) throw CompileError(line.number, "invalid wave table size");
          waves_[id] = table;
        } else {
          tables_[id] = table;
        }
        continue;
      }
      if (line.continuation) throw CompileError(line.number, "syntax error after #" + name);
      size_t offset = 0;
      if (name == "CHANNEL" || name == "FINENESS") {
        const Number n = getNumber(d, 0);
        if (!n.found || n.value < 0 || n.value > 65535)
          throw CompileError(line.number, "number not found in #" + name);
        (name == "CHANNEL" ? channelCount_ : fineness_) = static_cast<int>(n.value);
        offset = n.end + 1;
      } else if (name == "OCTAVE" || name == "VOLUME") {
        const std::string key = word(line);
        if (key != "NORMAL" && key != "REVERSE")
          throw CompileError(line.number, "invalid argument in #" + name);
        (name == "OCTAVE" ? octaveReverse_ : volumeReverse_) = key == "REVERSE";
        offset = d.size();
      } else if (name == "PRAGMA") {
        const std::string key = word(line);
        if (key == "FAMICOM") {
          famicom_ = true;
          static const int triangle[32] = {
            0, 16, 32, 48, 64, 80, 96, 112, 127, 112, 96, 80, 64, 48, 32, 16,
            0, -16, -32, -48, -64, -80, -96, -112, -128, -112, -96, -80, -64, -48, -32, -16};
          waves_[0].assign(triangle, triangle + 32);
        } else if (key == "GAMEBOY") {
          throw CompileError(line.number, "the Game Boy mode is not supported");
        } else {
          throw CompileError(line.number, "unknown pragma parameter " + key);
        }
        offset = d.size();
      } else if (name == "TITLE") {
        size_t begin = countSpaces(d, 0);
        if (begin >= d.size() || d[begin] != '<') throw CompileError(line.number, "syntax error in #TITLE");
        title_.clear();
        size_t k = begin + 1;
        for (; k < d.size(); k++) {
          const uint8_t c = d[k];
          if (sjis_ && sjisLead(c) && k + 1 < d.size() && sjisTrail(d[k + 1])) {
            title_ += static_cast<char>(c);
            title_ += static_cast<char>(d[++k]);
            continue;
          }
          if (c == 0 || c == '\\') continue;
          if (c == '>') break;
          title_ += static_cast<char>(c);
        }
        if (k >= d.size()) throw CompileError(line.number, "syntax error in #TITLE");
        if (title_.size() > 65535) title_.resize(65535);
        hasTitle_ = true;
        offset = k + 1;
      } else {
        throw CompileError(line.number, "unknown directive: " + name);
      }
      if (!checkEnd(d, offset)) throw CompileError(line.number, "syntax error after #" + name);
    }
  }

  // ---- the MML of the channels
  struct ChannelWork {
    size_t offset = 0, line = 0;
    const std::vector<Line>* lines = nullptr;
    int clock = 192, maxGate = 16;
    bool upperVolumes = false;
    int volumeShift = 0;
    int volumeL = 0, volumeR = 0;
    int octave = 4, gate = 16;
    int defaultDot = 0, defaultLength = 4;
    long loopCount = 0;
    size_t loopLine = 0, loopOffset = 0, loopEndLine = 0, loopEndOffset = 0;
    int localLoopId = 0;
    int lastDown = 1, lastUp = 1;
    long expanded = 0;
    Bytes data;
    const Line& at() const { return (*lines)[line]; }
  };

  static void pushLength(ChannelWork& w, long count, const char* what) {
    if (count < 255) {
      w.data.push_back(static_cast<uint8_t>(count));
    } else if (count < 65535) {
      w.data.push_back(255);
      w.data.push_back(static_cast<uint8_t>(count >> 8));
      w.data.push_back(static_cast<uint8_t>(count & 0xff));
    } else {
      throw CompileError(w.at().number, std::string(what) + " length is too long");
    }
  }

  static uint8_t toUint8(const ChannelWork& w, long n) {
    if (n < -128 || n > 127) throw CompileError(w.at().number, "number out of range -128 to 127");
    return static_cast<uint8_t>(n & 0xff);
  }

  static uint8_t byteOf(const ChannelWork& w, long n, const std::string& command) {
    if (n < 0 || n > 255)
      throw CompileError(w.at().number, "number out of range for '" + command + "'");
    return static_cast<uint8_t>(n);
  }

  void note(ChannelWork& w, char command) {
    if (command != 'r' && (w.octave < 0 || w.octave > 10))
      throw CompileError(w.at().number, "current octave is out of range");
    const Bytes& d = w.at().data;
    w.offset += countSpaces(d, w.offset);
    int fine = 0;
    if (w.offset < d.size()) {
      const uint8_t c = d[w.offset];
      if (c == '-' || c == '+' || c == '#') {
        w.offset++;
        fine = c == '-' ? -1 : 1;
      }
    }
    long total = 0;
    for (;;) {
      const Number n = getNumber(d, w.offset);
      long length;
      int dot = 0;
      if (!n.found) {
        length = w.defaultLength;
        dot = w.defaultDot;
      } else {
        length = n.value;
        w.offset = n.end + 1;
      }
      for (;;) {
        const long at = getCharacter(d, w.offset, '.');
        if (at < 0) break;
        w.offset = at + 1;
        dot++;
      }
      if (length < 1) throw CompileError(w.at().number, "invalid note length");
      long count = w.clock / length;
      total += count;
      while (dot-- > 0) {
        if (count % 2) throw CompileError(w.at().number, "too many '.' against time resolution");
        count /= 2;
        total += count;
      }
      const long tie = getCharacter(d, w.offset, '^');
      if (tie < 0) break;
      w.offset = tie + 1;
    }
    if (getCharacter(d, w.offset, '&') >= 0)
      throw CompileError(w.at().number, "command '&' not implemented");
    long rest = 0;
    if (command == 'r') {
      w.data.push_back(CMD_NOTE_OFF);
    } else {
      static const int tones[7] = {9, 11, 0, 2, 4, 5, 7};     // a b c d e f g
      fine += w.octave * 12 + tones[command - 'a'];
      if (fine < 0) fine = 0;
      if (fine > 127) fine = 127;
      w.data.push_back(static_cast<uint8_t>(fine));
      rest = total;
      total = total * w.gate / w.maxGate;
      rest -= total;
      notes_++;
    }
    pushLength(w, total, "note");
    if (rest > 0) {
      w.data.push_back(CMD_NOTE_OFF);
      pushLength(w, rest, "rest");
    }
  }

  void relativeVolume(ChannelWork& w, bool up) {
    const bool stereo = w.volumeR >= 0;
    const int step = (1 << w.volumeShift) * (up ? 1 : -1);
    w.volumeL += step;
    if (stereo) w.volumeR += step;
    if (w.volumeL < 0 || w.volumeL > 255 || (stereo && (w.volumeR < 0 || w.volumeR > 255)))
      throw CompileError(w.at().number, "relative volume causes volume range error");
    pushVolume(w);
  }

  static void pushVolume(ChannelWork& w) {
    if (w.volumeR >= 0) {
      w.data.push_back(CMD_VOLUME_LEFT);
      w.data.push_back(static_cast<uint8_t>(w.volumeL));
      w.data.push_back(CMD_VOLUME_RIGHT);
      w.data.push_back(static_cast<uint8_t>(w.volumeR));
    } else {
      w.data.push_back(CMD_VOLUME_MONO);
      w.data.push_back(static_cast<uint8_t>(w.volumeL));
    }
  }

  struct Arg {
    bool given = false;
    long value = 0;
  };

  // How many numbers a command takes, and what a missing one is.
  static bool syntax(const std::string& c, std::vector<Arg>& defaults, std::string& sequence) {
    struct Entry {
      const char* name;
      const char* sequence;
      int count;          // -1: not a command by itself
      long def[5];
      bool has[5];
    };
    static const Entry table[] = {
      {"$", "", 0, {}, {}},
      {"%", "", 1, {0}, {true}},
      {"(", "", 0, {}, {}},
      {")", "", 0, {}, {}},
      {"/", ":", 0, {}, {}},
      {"/:", "", 1, {2}, {true}},
      {":", "/", -1, {}, {}},
      {":/", "", 0, {}, {}},
      {"<", "", 0, {}, {}},
      {">", "", 0, {}, {}},
      {"@", "ciopv", 1, {0}, {true}},
      {"@c", "", 2, {0, 0}, {true, true}},
      {"@i", "", 2, {0, 0}, {true, true}},
      {"@o", "", 2, {0, 0}, {true, true}},
      {"@p", "", 2, {0, 0}, {true, true}},
      {"@v", "", 2, {10, 0}, {true, true}},
      {"]", "", 0, {}, {}},
      {"[", "", 1, {2}, {true}},
      {"_", "", 1, {0}, {false}},
      {"|", "", 0, {}, {}},
      {"~", "", 1, {0}, {false}},
      {"k", "", 1, {0}, {true}},
      {"l", "", 1, {4}, {true}},
      {"m", "lp", -1, {}, {}},
      {"ml", "", 1, {1}, {true}},
      {"mp", "", 5, {}, {false, false, false, false, false}},
      {"n", "ast", -1, {}, {}},
      {"na", "", 2, {0, 0}, {true, true}},
      {"ns", "", -2, {}, {}},
      {"nt", "", -2, {}, {}},
      {"o", "", 1, {4}, {true}},
      {"p", "h", 1, {0}, {true}},
      {"ph", "", -2, {}, {}},
      {"q", "", 1, {16}, {true}},
      {"r", "", 0, {}, {}},
      {"s", "", 2, {}, {false, false}},
      {"t", "", 1, {120}, {true}},
      {"v", "", 2, {10, 0}, {true, true}},
      {"x", "", 2, {}, {false, false}},
    };
    for (const Entry& e : table) {
      if (c != e.name) continue;
      sequence = e.sequence;
      defaults.clear();
      if (e.count < 0) return e.count == -1;
      for (int i = 0; i < e.count; i++) {
        Arg a;
        a.given = e.has[i];
        a.value = e.def[i];
        defaults.push_back(a);
      }
      return true;
    }
    return false;
  }

  void command(ChannelWork& w, const std::string& c, char letter, const std::vector<Arg>& args) {
    const int number = w.at().number;
    // A number whose ',' is missing isn't there at all (the loop that reads them
    // stops); the original's JavaScript then reads it as undefined, which the
    // commands take for 0.
    auto arg = [&](size_t i) { return i < args.size() ? args[i].value : 0L; };
    auto value = [&](size_t i, long low, long high) {
      if (arg(i) < low || arg(i) > high)
        throw CompileError(number, "number out of range for '" + c + "'");
      return arg(i);
    };
    if (c == "r") {
      note(w, letter);
    } else if (c == "$") {
      w.data.push_back(CMD_ENDLESS_LOOP_POINT);
    } else if (c == "%") {
      if (famicom_) throw CompileError(number, "'%' is not supported in NES mode");
      w.data.push_back(CMD_MODULE_CHANGE);
      w.data.push_back(byteOf(w, args[0].value, c));
    } else if (c == "(" || c == ")") {
      relativeVolume(w, (c == "(") != volumeReverse_);
    } else if (c == "/") {
      if (w.localLoopId == 0) throw CompileError(number, "'/' found without '/:'");
      w.data.push_back(CMD_LOCAL_LOOP_BREAK);
      w.data.push_back(static_cast<uint8_t>(w.localLoopId - 1));
    } else if (c == "/:") {
      if (w.localLoopId > 15) throw CompileError(number, "local loop is too deep (>16)");
      w.data.push_back(CMD_LOCAL_LOOP_START);
      w.data.push_back(static_cast<uint8_t>(w.localLoopId++));
      w.data.push_back(static_cast<uint8_t>(value(0, 1, 255)));
    } else if (c == ":/") {
      if (w.localLoopId == 0) throw CompileError(number, "':/' found without '/:'");
      w.data.push_back(CMD_LOCAL_LOOP_END);
      w.data.push_back(static_cast<uint8_t>(--w.localLoopId));
    } else if (c == "<" || c == ">") {
      w.octave += ((c == "<") != octaveReverse_) ? 1 : -1;
    } else if (c == "@") {
      if (famicom_) {
        if (w.at().channel == 2) throw CompileError(number, "'@' is not supported in famicom mode channel 3");
        const long v = args[0].value;
        if (v != 2 && v != 4 && v != 6 && v != 7)
          throw CompileError(number, "voice id is invalid in famicom mode");
      }
      w.data.push_back(CMD_VOICE_CHANGE);
      w.data.push_back(byteOf(w, args[0].value, c));
    } else if (c == "@c") {
      throw CompileError(number, "command '@c' (MIDI) not supported");
    } else if (c == "@i" || c == "@o") {
      w.data.push_back(c == "@i" ? CMD_FM_IN : CMD_FM_OUT);
      w.data.push_back(static_cast<uint8_t>((value(0, 0, 15) << 4) | value(1, 0, 15)));
    } else if (c == "@p") {
      w.data.push_back(CMD_PORT_CHANGE);
      w.data.push_back(byteOf(w, arg(0), c));
      w.data.push_back(byteOf(w, arg(1), c));
    } else if (c == "@v" || c == "v") {
      int base = 0;
      w.volumeShift = 0;
      if (c == "v") {
        w.volumeShift = 3;
        if (!w.upperVolumes) w.volumeShift = 4;
        else base = 128;
      }
      const long high = c == "v" ? 15 : 255;
      w.volumeL = base + static_cast<int>(value(0, 0, high) << w.volumeShift);
      w.volumeR = args.size() == 1 ? -1 : base + static_cast<int>(value(1, 0, high) << w.volumeShift);
      pushVolume(w);
    } else if (c == "]") {
      if (w.loopCount == 0) throw CompileError(number, "']' found without '['");
      if (--w.loopCount == 0) return;
      if (++w.expanded > 200000) throw CompileError(number, "too many loops");
      w.loopEndLine = w.line;
      w.loopEndOffset = w.offset;
      w.line = w.loopLine;
      w.offset = w.loopOffset;
    } else if (c == "[") {
      w.loopCount = value(0, 1, 65535);
      w.loopLine = w.line;
      w.loopOffset = w.offset;
    } else if (c == "|") {
      if (w.loopCount > 1) return;
      w.line = w.loopEndLine;
      w.offset = w.loopEndOffset;
    } else if (c == "_" || c == "~") {
      // by steps of the fine volume; a missing number is the one before, first 1
      int& last = c == "_" ? w.lastDown : w.lastUp;
      if (args[0].given) last = static_cast<int>(value(0, 0, 255));
      w.data.push_back(c == "_" ? CMD_RELATIVE_VOLUME_DOWN : CMD_RELATIVE_VOLUME_UP);
      w.data.push_back(static_cast<uint8_t>(last));
    } else if (c == "k") {
      w.data.push_back(CMD_DETUNE);
      w.data.push_back(toUint8(w, args[0].value));
    } else if (c == "l") {
      w.defaultLength = static_cast<int>(value(0, 1, 1024));
      w.defaultDot = 0;
      for (;;) {
        const long at = getCharacter(w.at().data, w.offset, '.');
        if (at < 0) break;
        w.offset = at + 1;
        w.defaultDot++;
      }
    } else if (c == "ml") {
      w.data.push_back(CMD_MULTIPLE);
      w.data.push_back(byteOf(w, args[0].value, c));
    } else if (c == "mp") {
      static const int commands[5] = {CMD_PITCH_MODULATION_DELAY, CMD_PITCH_MODULATION_DEPTH,
                                      CMD_PITCH_MODULATION_WIDTH, CMD_PITCH_MODULATION_HEIGHT,
                                      CMD_PITCH_MODULATION_DELTA};
      for (size_t i = 0; i < args.size(); i++) {
        if (!args[i].given) continue;
        w.data.push_back(static_cast<uint8_t>(commands[i]));
        if (i == 0) {
          const long delay = value(0, 0, 65535);
          w.data.push_back(static_cast<uint8_t>(delay >> 8));
          w.data.push_back(static_cast<uint8_t>(delay & 0xff));
        } else if (i == 3) {
          w.data.push_back(toUint8(w, args[i].value));
        } else {
          w.data.push_back(byteOf(w, args[i].value, c));
        }
      }
    } else if (c == "na") {
      w.data.push_back(CMD_AMP_ENVELOPE);
      w.data.push_back(byteOf(w, arg(0), c));
      w.data.push_back(byteOf(w, arg(1), c));
    } else if (c == "o") {
      w.octave = static_cast<int>(value(0, 0, 10));
    } else if (c == "p") {
      w.data.push_back(CMD_PANPOT);
      w.data.push_back(static_cast<uint8_t>(value(0, 0, 3)));
    } else if (c == "q") {
      w.gate = static_cast<int>(value(0, 0, w.maxGate));
    } else if (c == "s") {
      if (args[0].given) {
        w.data.push_back(CMD_SUSTAIN_MODE);
        w.data.push_back(byteOf(w, args[0].value, c));
      }
      if (args.size() == 2 && args[1].given) {
        w.data.push_back(CMD_PORTAMENT);
        w.data.push_back(toUint8(w, args[1].value));
      }
    } else if (c == "t") {
      const long n = 22050 * 4 * 60 / 192 / value(0, 1, 512);     // 27562 / tempo
      w.data.push_back(CMD_TEMPO);
      w.data.push_back(static_cast<uint8_t>(n >> 8));
      w.data.push_back(static_cast<uint8_t>(n & 0xff));
    } else if (c == "x") {
      if (args[0].given) {
        const long mode = args[0].value;
        if (mode < 0 || (mode & 0x0f) > 1 || mode > 17)
          throw CompileError(number, "invalid volume mode for 'x'");
        w.upperVolumes = (mode & 0x10) != 0;
        w.data.push_back(CMD_VOLUME_MODE_CHANGE);
        w.data.push_back(static_cast<uint8_t>(mode & 0x0f));
      }
      if (args.size() == 2 && args[1].given) {
        w.data.push_back(CMD_FREQUENCY_MODE_CHANGE);
        w.data.push_back(static_cast<uint8_t>(value(1, 0, 3)));
      }
    } else {
      throw CompileError(number, "command '" + c + "' not implemented");
    }
  }

  void parseChannels() {
    static const std::vector<Line> none;
    for (int ch = 0; ch < channelCount_; ch++) {
      ChannelWork w;
      w.lines = static_cast<size_t>(ch) < channels_.size() ? &channels_[ch] : &none;
      if (ch == 0) {
        w.data.push_back(CMD_FINENESS);
        w.data.push_back(static_cast<uint8_t>(fineness_ >> 8));
        w.data.push_back(static_cast<uint8_t>(fineness_ & 0xff));
      }
      if (famicom_) {
        w.data.push_back(CMD_MODULE_CHANGE);
        w.data.push_back(ch != 2 ? 1 : 4);
      }
      for (w.line = 0; w.line < w.lines->size(); w.line++) {
        for (w.offset = 0; w.offset < w.at().data.size();) {
          const Bytes& d = w.at().data;
          w.offset += countSpaces(d, w.offset);
          if (w.offset >= d.size()) break;
          const char letter = lowerAt(d, w.offset);
          std::string c(1, letter);
          if (letter >= 'a' && letter <= 'g') c = "r";
          std::vector<Arg> args;
          std::string sequence;
          if (!syntax(c, args, sequence))
            throw CompileError(w.at().number, "unknown command '" + c + "'");
          w.offset++;
          if (!sequence.empty()) {
            w.offset += countSpaces(d, w.offset);
            if (w.offset < d.size()) {
              const char next = lowerAt(d, w.offset);
              if (sequence.find(next) != std::string::npos) {
                c += next;
                w.offset++;
              }
            }
            std::string unused;
            if (!syntax(c, args, unused) || (c.size() == 1 && (c == ":" || c == "m" || c == "n")))
              throw CompileError(w.at().number, c.size() == 1 ? "unknown command '" + c + "'"
                                                              : "command '" + c + "' not implemented");
          }
          std::vector<Arg> got;
          for (size_t i = 0; i < args.size(); i++) {
            if (i != 0) {
              const long at = getCharacter(d, w.offset, ',');
              if (at < 0) break;
              w.offset = at + 1;
            }
            const Number n = getNumber(d, w.offset);
            Arg a = args[i];
            if (n.found) {
              a.given = true;
              a.value = n.value;
              w.offset = n.end + 1;
            }
            got.push_back(a);
          }
          command(w, c, letter, got);
          // all channels together: loops ([...]) repeat what is in them
          if (size_ + w.data.size() > (1u << 24)) throw CompileError(w.at().number, "the song is too long");
        }
      }
      w.data.push_back(CMD_END);
      size_ += w.data.size();
      channelData_.push_back(w.data);
    }
  }

  static void put16(Bytes& out, size_t n) {
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.push_back(static_cast<uint8_t>(n));
  }
  static void put32(Bytes& out, size_t n) {
    put16(out, n >> 16);
    put16(out, n & 0xffff);
  }

  Bytes generate() const {
    Bytes out;
    static const char magic[] = "T'SoundSystem";
    out.insert(out.end(), magic, magic + 14);
    out.push_back(kVersionMajor);
    out.push_back(kVersionMinor);
    put16(out, title_.size());
    out.insert(out.end(), title_.begin(), title_.end());
    if (title_.size() % 2) out.push_back(0);
    put16(out, channelCount_);
    size_t at = out.size() + 8 * channelCount_ + 4;
    for (const Bytes& data : channelData_) {
      put32(out, at);
      put32(out, data.size());
      at += data.size();
    }
    put32(out, at);
    for (const Bytes& data : channelData_) out.insert(out.end(), data.begin(), data.end());
    for (int pass = 0; pass < 2; pass++) {
      const std::vector<int>* all = pass ? tables_ : waves_;
      size_t count = 0;
      for (int i = 0; i < 256; i++) count += !all[i].empty();
      put16(out, count);
      for (int i = 0; i < 256; i++) {
        if (all[i].empty()) continue;
        out.push_back(static_cast<uint8_t>(i));
        out.push_back(static_cast<uint8_t>(all[i].size()));
        for (int v : all[i]) out.push_back(static_cast<uint8_t>(v & 0xff));
      }
    }
    return out;
  }

  Bytes source_;
  bool sjis_ = false;
  int commentNest_ = 0;
  std::vector<Line> directives_;
  std::vector<std::vector<Line>> channels_;
  std::vector<int> waves_[256], tables_[256];
  std::string title_;
  bool hasTitle_ = false;
  int channelCount_ = 0, fineness_ = kDefaultTimerCount;
  bool famicom_ = false, octaveReverse_ = false, volumeReverse_ = false;
  std::vector<Bytes> channelData_;
  size_t size_ = 0;           // of the channels' data so far
  long notes_ = 0;
};

// ---------------------------------------------------------------- the sound device

enum { TYPE_PSG = 0, TYPE_FC = 1, TYPE_NOISE = 2, TYPE_SIN = 3, TYPE_SCC = 4, TYPE_OSC = 5 };
enum { FM_OUT_OFF = 0, FM_OUT_NEW = 1, FM_OUT_ADD = 2 };

struct Tables {
  int8_t random[4096];
  int8_t sine[256];
  int8_t osc[4][256];
  uint16_t noteHz[128];
  uint16_t noteMsx[128];
  uint16_t msxHz[4096];
  uint16_t fmHz[0x2000];
  uint8_t fmVolume[256];

  Tables() {
    // the original takes what the browser's random numbers are; here they are
    // the same every time
    uint32_t seed = 0x12345678;
    for (int i = 0; i < 4096; i++) {
      seed = seed * 1103515245u + 12345u;
      const double r = (seed >> 1) / 2147483648.0;
      const int v = static_cast<int>(r * 2147483647.0);
      random[i] = static_cast<int8_t>((v >> 8) & 0xff);
    }
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < 256; i++) {
      sine[i] = static_cast<int8_t>(std::sin(pi * i / 128) * 64 + 0.5);
      osc[0][i] = sine[i];
      osc[1][i] = static_cast<int8_t>((i - 128) / 2);                 // saw, rising
      osc[2][i] = static_cast<int8_t>((127 - i) / 2);                 // saw, falling
      osc[3][i] = static_cast<int8_t>(i < 64 ? i : i < 192 ? 128 - i : i - 256);   // triangle
    }
    static const uint16_t msx[96] = {
      0x0D5D, 0x0C9C, 0x0BE7, 0x0B3C, 0x0A9B, 0x0A02, 0x0973, 0x08EB,
      0x086B, 0x07F2, 0x0780, 0x0714, 0x06AF, 0x064E, 0x05F4, 0x059E,
      0x054E, 0x0501, 0x04BA, 0x0476, 0x0436, 0x03F9, 0x03C0, 0x038A,
      0x0357, 0x0327, 0x02FA, 0x02CF, 0x02A7, 0x0281, 0x025D, 0x023B,
      0x021B, 0x01FD, 0x01E0, 0x01C5, 0x01AC, 0x0194, 0x017D, 0x0168,
      0x0153, 0x0140, 0x012E, 0x011D, 0x010D, 0x00FE, 0x00F0, 0x00E3,
      0x00D6, 0x00CA, 0x00BE, 0x00B4, 0x00AA, 0x00A0, 0x0097, 0x008F,
      0x0087, 0x007F, 0x0078, 0x0071, 0x006B, 0x0065, 0x005F, 0x005A,
      0x0055, 0x0050, 0x004C, 0x0047, 0x0043, 0x0040, 0x003C, 0x0039,
      0x0035, 0x0032, 0x0030, 0x002D, 0x002A, 0x0028, 0x0026, 0x0024,
      0x0022, 0x0020, 0x001E, 0x001C, 0x001B, 0x0019, 0x0018, 0x0016,
      0x0015, 0x0014, 0x0013, 0x0012, 0x0011, 0x0010, 0x000F, 0x000E};
    for (int i = 0; i < 128; i++) {
      noteHz[i] = static_cast<uint16_t>(static_cast<int>(440 * std::pow(2.0, (i - 69) / 12.0) + 0.5));
      noteMsx[i] = i >= 12 && i < 108 ? msx[i - 12] : 0;
    }
    msxHz[0] = 0;
    for (int i = 1; i < 4096; i++)
      msxHz[i] = static_cast<uint16_t>(static_cast<int>(1.7897725e+6 / 16 / i / 2 + 0.5));
    for (int i = 0; i < 0x2000; i++) {
      const double power = (((i >> 6) - 69) + (i & 0x3f) / 64.0) / 12;
      fmHz[i] = static_cast<uint16_t>(static_cast<int>(440 * std::pow(2.0, power) + 0.5));
    }
    for (int i = 0; i < 256; i++)
      fmVolume[i] = static_cast<uint8_t>(
          static_cast<int>(255 * std::pow(10.0, -0.75 * (255 - i) / 2 / 20) + 0.5));
  }
};

const Tables& tables() {
  static const Tables all;
  return all;
}

struct Module {
  int type = TYPE_PSG;
  int volumeL = 0, volumeR = 0, volumeC = 0;
  int frequency = 0;
  int fmInRate = 0, fmInPipe = 0, fmOutMode = 0, fmOutPipe = 0;
  int multiple = 1;
  long long count = 0;
  int phase = 0;
  int voice = 0;

  void setType(int t) {
    type = t;
    count = 0;
    phase = 0;
    voice = t == TYPE_FC ? 3 : 0;
  }
};

class Device {
 public:
  int sampleRate = 48000;
  std::vector<Module> module;
  int8_t wave[256][32];
  bool hasWave[256];

  Device() { std::memset(hasWave, 0, sizeof(hasWave)); }

  // <frames> frames of all modules, added up, into <buffer> (left, right, ...).
  void generate(int32_t* buffer, int frames) {
    for (int i = 0; i < 2 * frames; i++) buffer[i] = 0;
    for (int p = 0; p < 4; p++)
      if (fm_[p].size() < static_cast<size_t>(2 * frames)) fm_[p].resize(2 * frames, 0);
    for (Module& m : module) {
      switch (m.type) {
        case TYPE_PSG: square(m, buffer, frames, 2, 1); break;
        case TYPE_FC: square(m, buffer, frames, 8, m.voice); break;
        case TYPE_NOISE: noise(m, buffer, frames); break;
        case TYPE_SIN: table(m, buffer, frames, tables().sine, 256, 1); break;
        case TYPE_SCC:
          if (hasWave[m.voice & 0xff]) table(m, buffer, frames, wave[m.voice & 0xff], 32, 2);
          break;
        case TYPE_OSC: table(m, buffer, frames, tables().osc[m.voice & 3], 256, 1); break;
        default: break;       // Game Boy modules and MIDI: nothing
      }
    }
  }

 private:
  // PSG: a square wave of two steps. FC: one of eight steps, <voice> of them low.
  void square(Module& m, int32_t* buffer, int frames, int steps, int low) {
    int volumeL = m.volumeL << 4, volumeR = m.volumeR << 4;
    const long long plus = static_cast<long long>(m.frequency) * steps * m.multiple;
    long long count = m.count;
    int phase = m.phase;
    if (steps == 2 ? phase == 0 : phase < low) {
      volumeL = -volumeL;
      volumeR = -volumeR;
    }
    for (int i = 0; i < 2 * frames; i += 2) {
      buffer[i] += volumeL;
      buffer[i + 1] += volumeR;
      count += plus;
      if (count > sampleRate) {
        // as many steps as it takes to bring the count down to the rate, counted
        // rather than taken one by one (a high note times a high multiple is
        // thousands of them a sample)
        const long long k = (count - 1) / sampleRate;
        count -= k * sampleRate;
        long long flips;
        if (steps == 2) {
          flips = k;
          phase = static_cast<int>((phase + k) & 1);
        } else {
          flips = k / steps * (low > 0 && low < steps ? 2 : 1);   // a round has 0 and low
          if (k >= steps) phase &= steps - 1;   // (the key-on phase may be above the steps)
          for (long long r = k % steps; r > 0; r--) {
            phase = (phase + 1) & (steps - 1);
            if (phase == 0 || phase == low) flips++;
          }
        }
        if (flips & 1) {
          volumeL = -volumeL;
          volumeR = -volumeR;
        }
      }
    }
    m.count = count;
    m.phase = phase;
  }

  void noise(Module& m, int32_t* buffer, int frames) {
    const int volumeL = m.volumeL >> 2, volumeR = m.volumeR >> 2;
    const long long plus = static_cast<long long>(m.frequency) * m.multiple;
    long long count = m.count;
    int phase = m.phase & 0x0fff;
    for (int i = 0; i < 2 * frames; i += 2) {
      const int rnd = tables().random[phase];
      buffer[i] += rnd * volumeL;
      buffer[i + 1] += rnd * volumeR;
      count += plus;
      if (count > 0) {   // one step for every 880 or part of it, counted at once
        const long long k = (count + 879) / 880;
        phase = static_cast<int>((phase + k) & 0x0fff);
        count -= k * 880;
      }
    }
    m.count = count;
    m.phase = phase;
  }

  // SIN, SCC and OSC: a wave table, which can take its phase from a pipe and give
  // its sound to a pipe instead of the speakers.
  void table(Module& m, int32_t* buffer, int frames, const int8_t* wave, int size, int shift) {
    int32_t* out = m.fmOutMode != FM_OUT_OFF ? fm_[m.fmOutPipe & 3].data() : buffer;
    const int32_t* in = m.fmInRate ? fm_[m.fmInPipe & 3].data() : nullptr;
    const int volumeL = m.volumeL >> shift, volumeR = m.volumeR >> shift;
    const long long plus = static_cast<long long>(m.frequency) * size * m.multiple;
    const int mask = size - 1;
    // (the wave table module shifts by eight times the rate, and as in the original
    // only the lowest five bits of that count)
    const int inRate = (size == 32 ? m.fmInRate << 3 : m.fmInRate) & 31;
    const bool fresh = m.fmOutMode == FM_OUT_NEW;
    long long count = m.count;
    int phase = m.phase & mask;
    for (int i = 0; i < 2 * frames; i += 2) {
      int phaseL = phase, phaseR = phase;
      if (in) {
        phaseL = (phase + (in[i] >> inRate)) & mask;
        phaseR = (phase + (in[i + 1] >> inRate)) & mask;
      }
      const int32_t l = wave[phaseL] * volumeL, r = wave[phaseR] * volumeR;
      if (fresh) {
        out[i] = l;
        out[i + 1] = r;
      } else {
        out[i] += l;
        out[i + 1] += r;
      }
      count += plus;
      if (count > sampleRate) {   // as in square()
        const long long k = (count - 1) / sampleRate;
        count -= k * sampleRate;
        phase = static_cast<int>((phase + k) & mask);
      }
    }
    m.count = count;
    m.phase = phase;
  }

  std::vector<int32_t> fm_[4];
};

// ---------------------------------------------------------------- the player

class Player {
 public:
  std::string title;
  int version = 0;

  // False and a reason when the data isn't a TSD file this player knows.
  bool load(const Bytes& input, int rate, std::string& error) {
    input_ = input;
    rate_ = rate;
    device_.sampleRate = rate;
    if (input_.size() < 22 || std::memcmp(input_.data(), "T'SoundSystem", 14) != 0) {
      error = "not a T'SoundSystem file";
      return false;
    }
    version = input_[14] * 100 + input_[15];
    if (version < 60 || version > kVersionMajor * 100 + kVersionMinor) {
      error = "TSD version " + std::to_string(input_[14]) + "." + std::to_string(input_[15]) +
              " is not supported";
      return false;
    }
    const size_t titleSize = u16(16);
    if (18 + titleSize > input_.size()) return broken(error);
    title.assign(input_.begin() + 18, input_.begin() + 18 + titleSize);
    title = title.c_str();
    size_t offset = 18 + ((titleSize + 1) & ~static_cast<size_t>(1));
    const size_t count = u16(offset);
    offset += 2;
    if (count < 1 || count > 26 * 27 || offset + 8 * count > input_.size()) return broken(error);
    device_.module.assign(count, Module());
    channel_.assign(count, Channel());
    for (size_t i = 0; i < count; i++) {
      Channel& ch = channel_[i];
      ch.id = static_cast<int>(i);
      ch.base = u32(offset);
      ch.size = u32(offset + 4);
      offset += 8;
      if (ch.base < offset || ch.base > input_.size() || ch.size > input_.size() - ch.base)
        return broken(error);
    }
    if (version > 60) {
      // wave tables and envelope tables
      size_t at = u32(offset);
      if (at < offset + 4 || at + 4 > input_.size()) return broken(error);
      const size_t waves = u16(at);
      at += 2;
      for (size_t i = 0; i < waves; i++) {
        if (at + 34 > input_.size() || input_[at + 1] != 32) return broken(error);
        const int id = input_[at];
        std::memcpy(device_.wave[id], &input_[at + 2], 32);
        device_.hasWave[id] = true;
        at += 34;
      }
      if (at + 2 > input_.size()) return broken(error);
      const size_t envelopes = u16(at);
      at += 2;
      for (size_t i = 0; i < envelopes; i++) {
        if (at + 2 > input_.size() || at + 2 + input_[at + 1] > input_.size()) return broken(error);
        table_[input_[at]].assign(input_.begin() + at + 2, input_.begin() + at + 2 + input_[at + 1]);
        at += 2 + input_[at + 1];
      }
    }
    setTimer(0, kDefaultTimerCount);
    setTimer(1, kDefaultTimerCount);
    return true;
  }

  // Up to <frames> frames, as far as the next tick of a timer. At least 2.
  int generate(int32_t* buffer, int frames) {
    int units = frames >> 1;
    for (const Timer& t : timer_)
      if (t.count < units) units = t.count;
    if (units < 1) units = 1;
    device_.generate(buffer, units * 2);
    for (int id = 0; id < 2; id++) {
      timer_[id].count -= units;
      if (timer_[id].count > 0) continue;
      timer_[id].count = timer_[id].timer;
      if (id == 0) automation();
      else sequencer();
    }
    return units * 2;
  }

  // Every channel has come to its end, or has begun its endless loop anew.
  bool played() const {
    for (const Channel& ch : channel_)
      if (!ch.ended && ch.loops < 1) return false;
    return true;
  }
  bool loops() const {
    for (const Channel& ch : channel_)
      if (!ch.ended) return true;
    return false;
  }
  bool silent() const {
    for (const Module& m : device_.module)
      if ((m.volumeL || m.volumeR) && m.fmOutMode == FM_OUT_OFF) return false;
    return true;
  }
  long notes() const { return notes_; }
  const std::string& fault() const { return fault_; }
  size_t channels() const { return channel_.size(); }

 private:
  enum { PAN_L = 1, PAN_R = 2, PAN_C = 3 };
  enum { FREQ_NORMAL = 0, FREQ_MSX = 1, FREQ_FM = 2, FREQ_GB = 3 };
  enum { CH_L = 0, CH_R = 1, CH_C = 2 };

  struct Channel {
    int id = 0;
    size_t base = 0, size = 0, offset = 0;
    size_t loopOffset = 0;
    int loops = 0;
    bool ended = false;
    struct {
      size_t offset = 0, end = 0;
      int count = 0;
    } localLoop[16];
    unsigned wait = 1;
    int sustain = 0, portament = 0, detune = 0;
    bool keyOn = false;
    int volumeType = 0, volumeL = 0, volumeC = 0, volumeR = 0;
    int pan = PAN_C;
    int frequencyType = 0, note = 0, param = 0, hz = 0;
    int phase = 0;
    struct {
      bool enable = false;
      int base = 0, delayCount = 0, widthCount = 0, deltaCount = 0;
      int currentDepth = 0, currentHeight = 0, currentDiff = 0;
      int delay = 0, depth = 0, width = 0, height = 0, delta = 0;
    } pm;
    struct {
      bool enable = false;
      int id = 0, wait = 0, state = 0, count = 0;
      int volumeL = 0, volumeR = 0;
    } ae;
    long waited = 0;          // ticks since the endless loop began
  };

  struct Timer {
    int timer = 1, count = 1;
  };

  static bool broken(std::string& error) {
    error = "the T'SoundSystem file is damaged";
    return false;
  }
  unsigned u16(size_t at) const {
    return at + 2 <= input_.size() ? input_[at] << 8 | input_[at + 1] : 0;
  }
  size_t u32(size_t at) const {
    return at + 4 <= input_.size() ? static_cast<size_t>(u16(at)) << 16 | u16(at + 2) : 0;
  }
  // the next byte of a channel; past the end of the file there are only ends
  int next(Channel& ch) {
    const size_t at = ch.base + ch.offset++;
    return at < input_.size() ? input_[at] : static_cast<int>(CMD_END);
  }
  int nextSigned(Channel& ch) {
    const int v = next(ch);
    return v >= 0x80 ? v - 0x100 : v;
  }
  int next16(Channel& ch) {
    const int high = next(ch);
    return high << 8 | next(ch);
  }
  static int clamp(int v, int low, int high) { return v < low ? low : v > high ? high : v; }

  void setTimer(int id, int count) {
    if (count <= 0) return;
    int biased = static_cast<int>(static_cast<double>(count) * rate_ / 44100);
    if (biased < 1) biased = 1;
    timer_[id].timer = timer_[id].count = biased;
  }

  void setVolume(Channel& ch, int lrc, int volume) {
    int data = clamp(volume, 0, 255);
    if (lrc == CH_L && !(ch.pan & PAN_L)) data = 0;
    else if (lrc == CH_R && !(ch.pan & PAN_R)) data = 0;
    else if (ch.volumeType == 1) data = tables().fmVolume[data];
    Module& m = device_.module[ch.id];
    (lrc == CH_L ? m.volumeL : lrc == CH_R ? m.volumeR : m.volumeC) = data;
  }

  void automation() {
    for (Channel& ch : channel_) {
      if (!ch.keyOn) {
        if (ch.sustain) sustain(ch);
        if (ch.portament) portament(ch);
      }
      if (ch.pm.enable) pitchModulation(ch);
      if (ch.ae.enable) ampEnvelope(ch);
    }
  }

  void sustain(Channel& ch) {
    ch.ae.volumeL = ch.ae.volumeL > ch.sustain ? ch.ae.volumeL - ch.sustain : 0;
    ch.ae.volumeR = ch.ae.volumeR > ch.sustain ? ch.ae.volumeR - ch.sustain : 0;
    // (the original keeps a fault of its first version here: sustain knows no panpot)
    const int pan = ch.pan;
    ch.pan = PAN_C;
    setVolume(ch, CH_L, ch.ae.volumeL);
    setVolume(ch, CH_R, ch.ae.volumeR);
    ch.pan = pan;
  }

  int frequencyOf(const Channel& ch, int param) const {
    switch (ch.frequencyType) {
      case FREQ_MSX: return tables().msxHz[clamp(param, 0, 0x0fff)];
      case FREQ_FM: return tables().fmHz[clamp(param, 0, 0x1fff)];
      default: return param;
    }
  }

  void portament(Channel& ch) {
    int frequency = ch.hz;
    switch (ch.frequencyType) {
      case FREQ_NORMAL:
        ch.param = clamp(ch.param + ch.portament, 1, 0xffff);
        frequency = ch.param;
        break;
      case FREQ_MSX:
        ch.param = clamp(ch.param - ch.portament, 0, 0x0fff);
        frequency = frequencyOf(ch, ch.param);
        break;
      case FREQ_FM:
        ch.param = clamp(ch.param + ch.portament, 0, 0x1fff);
        frequency = frequencyOf(ch, ch.param);
        break;
      default:
        break;
    }
    device_.module[ch.id].frequency = frequency;
  }

  void pitchModulation(Channel& ch) {
    auto& pm = ch.pm;
    if (pm.delayCount < pm.delay) {
      pm.delayCount++;
      return;
    }
    if (pm.delayCount == pm.delay) {
      if (ch.frequencyType == FREQ_NORMAL) pm.base = ch.hz;
      else if (ch.frequencyType != FREQ_GB) pm.base = ch.param;
      pm.currentDepth = pm.depth;
      pm.currentHeight = pm.height;
      pm.currentDiff = 0;
      pm.widthCount = 0;
      pm.deltaCount = 0;
      pm.delayCount++;
      return;
    }
    if (++pm.widthCount != pm.width) return;
    pm.widthCount = 0;
    pm.currentDiff += pm.currentHeight;
    if (pm.currentDiff >= pm.currentDepth || pm.currentDiff <= -pm.currentDepth) {
      pm.currentHeight = -pm.currentHeight;
      pm.deltaCount++;
      if (pm.deltaCount == pm.delta) {
        pm.deltaCount = 0;
        pm.currentDepth++;
      }
    }
    int frequency = ch.hz;
    if (ch.frequencyType == FREQ_NORMAL) frequency = pm.base + pm.currentDiff;
    else if (ch.frequencyType != FREQ_GB) frequency = frequencyOf(ch, pm.base + pm.currentDiff);
    device_.module[ch.id].frequency = frequency;
  }

  void ampEnvelope(Channel& ch) {
    auto& ae = ch.ae;
    if (++ae.count != ae.wait) return;
    ae.count = 0;
    const std::vector<uint8_t>& table = table_[ae.id & 0xff];
    if (table.empty()) return;
    // an envelope changed during a note (na) goes on from the same step, and a
    // shorter one stays at its last step, as a table that has come to its end does
    if (ae.state >= static_cast<int>(table.size())) ae.state = static_cast<int>(table.size()) - 1;
    const int diff = static_cast<int8_t>(table[ae.state]);
    if (++ae.state == static_cast<int>(table.size())) ae.state--;
    setVolume(ch, CH_L, (ch.pan & PAN_L) ? clamp(ae.volumeL + diff, 0, 255) : 0);
    setVolume(ch, CH_R, (ch.pan & PAN_R) ? clamp(ae.volumeR + diff, 0, 255) : 0);
  }

  void noteOn(Channel& ch, int note) {
    int param = 0, hz = 0;
    switch (ch.frequencyType) {
      case FREQ_NORMAL:
        param = ch.detune + tables().noteHz[note];
        hz = param;
        break;
      case FREQ_MSX:
        param = ch.detune + tables().noteMsx[note];
        hz = tables().msxHz[clamp(param, 0, 0x0fff)];
        break;
      case FREQ_FM:
        param = (ch.detune + note) << 6;       // as the original has it
        hz = tables().fmHz[clamp(param, 0, 0x1fff)];
        break;
      default:
        break;
    }
    Module& m = device_.module[ch.id];
    m.frequency = hz;
    ch.note = note;
    ch.param = param;
    ch.hz = hz;
    setVolume(ch, CH_L, ch.volumeL);
    setVolume(ch, CH_R, ch.volumeR);
    setVolume(ch, CH_C, ch.volumeC);
    ch.keyOn = true;
    m.phase = ch.phase;
    ch.ae.volumeL = ch.volumeL;
    ch.ae.volumeR = ch.volumeR;
    ch.ae.state = 0;
    ch.ae.count = 0;
    ch.pm.delayCount = 0;
    notes_++;
  }

  void noteOff(Channel& ch) {
    Module& m = device_.module[ch.id];
    if (ch.sustain == 0) {
      if (!ch.ae.enable) {
        m.volumeL = m.volumeR = 0;
      } else {
        ch.ae.volumeL = m.volumeL;
        ch.ae.volumeR = m.volumeR;
      }
    }
    ch.keyOn = false;
  }

  void end(Channel& ch) {
    noteOff(ch);
    ch.ended = true;
    ch.wait = 0;
  }

  void setVoice(Channel& ch, int voice) {
    Module& m = device_.module[ch.id];
    m.voice = voice;
    if (m.type != TYPE_SIN) return;
    // the old way to say how the sines modulate each other
    const int in = voice >> 4, out = voice & 0x0f;
    m.fmInRate = in ? 9 - 5 : 0;
    m.fmInPipe = in ? ((in % 5) - 1) & 3 : 0;
    m.fmOutMode = out ? 1 : 0;
    m.fmOutPipe = out ? ((out % 5) - 1) & 3 : 0;
  }

  void sequencer() {
    for (Channel& ch : channel_) {
      if (ch.wait == 0) continue;
      ch.waited++;
      if (--ch.wait != 0) continue;
      for (long guard = 0;; guard++) {
        if (guard > 1000000) {
          end(ch);
          break;
        }
        const int cmd = next(ch);
        Module& m = device_.module[ch.id];
        if (cmd <= CMD_NOTE_OFF) {
          if (cmd == CMD_NOTE_OFF) noteOff(ch);
          else noteOn(ch, cmd);
          ch.wait = next(ch);
          if (ch.wait == 0xff) ch.wait = next16(ch);
          if (ch.wait != 0) break;
          continue;
        }
        switch (cmd) {
          case CMD_VOLUME_MONO: {
            const int v = next(ch);
            ch.volumeC = v;
            if (ch.pan & PAN_L) ch.volumeL = v;
            if (ch.pan & PAN_R) ch.volumeR = v;
            break;
          }
          case CMD_SUSTAIN_MODE: ch.sustain = next(ch); break;
          case CMD_DETUNE: ch.detune = nextSigned(ch); break;
          case CMD_PORTAMENT:
            ch.portament = nextSigned(ch);
            ch.pm.enable = false;
            break;
          case CMD_VOLUME_LEFT: ch.volumeL = next(ch); break;
          case CMD_VOLUME_RIGHT: ch.volumeR = next(ch); break;
          case CMD_PANPOT: ch.pan = next(ch) & 3; break;
          case CMD_RELATIVE_VOLUME_UP:
          case CMD_RELATIVE_VOLUME_DOWN: {
            const int step = cmd == CMD_RELATIVE_VOLUME_UP ? next(ch) : -next(ch);
            ch.volumeC = clamp(ch.volumeC + step, 0, 255);
            ch.volumeL = clamp(ch.volumeL + step, 0, 255);
            ch.volumeR = clamp(ch.volumeR + step, 0, 255);
            break;
          }
          case CMD_TEMPO: setTimer(1, next16(ch)); break;
          case CMD_FINENESS: setTimer(0, next16(ch)); break;
          case CMD_KEY_ON_PHASE: ch.phase = next(ch); break;
          case CMD_MULTIPLE: m.multiple = next(ch); break;
          case CMD_PITCH_MODULATION_DELAY:
            ch.pm.delay = next16(ch);
            ch.pm.enable = ch.pm.delay != 0;
            ch.portament = 0;
            break;
          case CMD_PITCH_MODULATION_DEPTH: ch.pm.depth = next(ch); break;
          case CMD_PITCH_MODULATION_WIDTH: ch.pm.width = next(ch); break;
          case CMD_PITCH_MODULATION_HEIGHT: ch.pm.height = nextSigned(ch); break;
          case CMD_PITCH_MODULATION_DELTA: ch.pm.delta = next(ch); break;
          case CMD_AMP_ENVELOPE:
            ch.ae.id = next(ch);
            ch.ae.wait = next(ch);
            ch.ae.enable = ch.ae.wait != 0;
            break;
          case CMD_ENDLESS_LOOP_POINT:
            ch.loopOffset = ch.offset;
            ch.waited = 0;
            break;
          case CMD_LOCAL_LOOP_START: {
            const int id = next(ch) & 15;
            ch.localLoop[id].count = next(ch);
            ch.localLoop[id].offset = ch.offset;
            break;
          }
          case CMD_LOCAL_LOOP_BREAK: {
            const int id = next(ch) & 15;
            if (ch.localLoop[id].count == 1) ch.offset = ch.localLoop[id].end;
            break;
          }
          case CMD_LOCAL_LOOP_END: {
            const int id = next(ch) & 15;
            ch.localLoop[id].end = ch.offset;
            if (--ch.localLoop[id].count != 0) ch.offset = ch.localLoop[id].offset;
            break;
          }
          case CMD_FREQUENCY_MODE_CHANGE: ch.frequencyType = next(ch) & 3; break;
          case CMD_VOLUME_MODE_CHANGE: ch.volumeType = next(ch); break;
          case CMD_FM_IN: {
            const int v = next(ch);
            m.fmInRate = (v >> 4) ? 9 - (v >> 4) : 0;
            m.fmInPipe = v & 3;
            break;
          }
          case CMD_FM_OUT: {
            const int v = next(ch);
            m.fmOutMode = v >> 4;
            m.fmOutPipe = v & 3;
            break;
          }
          case CMD_PORT_CHANGE:
            next(ch);
            next(ch);
            break;
          case CMD_VOICE_CHANGE: setVoice(ch, next(ch)); break;
          case CMD_MODULE_CHANGE: {
            const int v = next(ch);
            m.setType(v & 0x0f);
            ch.frequencyType = (v & 0x80) ? v >> 7 : (v >> 4) & 3;
            break;
          }
          case CMD_END:
            // an endless loop without a note or a rest in it is an end
            if (ch.loopOffset != 0 && ch.waited > 0) {
              ch.offset = ch.loopOffset;
              ch.loops++;
              ch.waited = 0;
            } else {
              end(ch);
            }
            break;
          default:
            fault_ = "unknown command " + std::to_string(cmd);
            end(ch);
            break;
        }
        if (ch.ended) break;
      }
    }
  }

  Bytes input_;
  int rate_ = 48000;
  Device device_;
  std::vector<Channel> channel_;
  std::vector<uint8_t> table_[256];
  Timer timer_[2];
  long notes_ = 0;
  std::string fault_;
};

// ---------------------------------------------------------------- the engine

const int kFadeSeconds = 3;        // after the loop point, into the second round
const int kTailSeconds = 3;        // at most, for the last note to die away
const long long kLongest = 30 * 60;

class TssEngine : public Engine {
 public:
  bool open(const Bytes& file, const std::string&, int, int rate, SongInfo& info,
            std::string& error) override {
    rate_ = rate;
    const bool compiled = file.size() > 14 && std::memcmp(file.data(), "T'SoundSystem", 14) == 0;
    if (compiled) {
      tsd_ = file;
    } else {
      // MML is text with directives; don't even try to compile anything else
      if (!looksLikeMml(file)) {
        error = "not a T'SoundSystem file";
        return false;
      }
      try {
        Compiler compiler;
        tsd_ = compiler.compile(file);
        if (compiler.notes() == 0) {
          error = "no notes in this T'SoundSystem file";
          return false;
        }
      } catch (const CompileError& e) {
        error = std::string("T'SoundSystem MML, ") + e.what();
        return false;
      }
    }
    // Once through without anyone listening: how long the song is, how loud it
    // gets, and whether the player knows all of it.
    Player probe;
    if (!probe.load(tsd_, rate, error)) return false;
    std::vector<int32_t> buffer(2 * 4096);
    long long frames = 0, end = -1;
    int32_t peak = 1;
    bool looped = false;
    long long quiet = 0;
    while (frames < kLongest * rate) {
      const int n = probe.generate(buffer.data(), 4096);
      frames += n;
      bool sound = false;
      for (int i = 0; i < 2 * n; i++) {
        const int32_t v = buffer[i] < 0 ? -buffer[i] : buffer[i];
        if (v > peak) peak = v;
        sound = sound || v != 0;
      }
      if (end < 0 && probe.played()) {
        end = frames;
        looped = probe.loops();
      }
      if (end >= 0) {
        if (looped) {
          if (frames >= end + static_cast<long long>(kFadeSeconds) * rate) break;
        } else {
          // what sounds after the last note: until it is quiet, or not for long
          quiet = sound ? 0 : quiet + n;
          if (quiet > rate / 10 || probe.silent() ||
              frames >= end + static_cast<long long>(kTailSeconds) * rate)
            break;
        }
      }
    }
    if (!probe.fault().empty()) {
      error = "T'SoundSystem data with " + probe.fault();
      return false;
    }
    if (probe.notes() == 0) {
      error = "no notes in this T'SoundSystem file";
      return false;
    }
    if (end < 0) end = frames;
    length_ = frames;
    fadeFrom_ = looped ? end : -1;
    // The original multiplies by 8 and clips whatever is too loud, which is most
    // of what three channels and more play. Here: by 8 at most, and no clipping.
    gain_ = 30000.0 / peak;
    if (gain_ > 8) gain_ = 8;
    if (debug_on()) {
      // to compare with the original: its gain and its clipping, and the data
      if (std::getenv("BBMP_RETRO_TSS_GAIN")) gain_ = std::atof(std::getenv("BBMP_RETRO_TSS_GAIN"));
      std::fprintf(stderr, "tss: %lld frames, peak %d, gain %.3f, tsd:", length_, peak, gain_);
      for (uint8_t b : tsd_) std::fprintf(stderr, " %02x", b);
      std::fprintf(stderr, "\n");
    }

    if (!player_.load(tsd_, rate, error)) return false;
    info.title = to_utf8(player_.title);
    info.type = compiled ? "T'SoundSystem TSD " + std::to_string(player_.version / 100) + "." +
                               std::to_string(player_.version / 10 % 10) + std::to_string(player_.version % 10)
                         : "T'SoundSystem MML";
    info.duration_ms = length_ * 1000 / rate;
    info.comment = std::to_string(player_.channels()) + " channels" +
                   (looped ? ", loops; played once and faded" : "");
    return true;
  }

  size_t render(int16_t* out, size_t frames) override {
    size_t done = 0;
    while (done < frames && position_ < length_) {
      if (have_ == 0) {
        have_ = player_.generate(chunk_, 4096);
        at_ = 0;
      }
      while (have_ > 0 && done < frames && position_ < length_) {
        double gain = gain_;
        if (fadeFrom_ >= 0 && position_ > fadeFrom_)
          gain *= static_cast<double>(length_ - position_) / (length_ - fadeFrom_);
        for (int k = 0; k < 2; k++) {
          double v = chunk_[2 * at_ + k] * gain;
          if (v > 32767) v = 32767;
          if (v < -32767) v = -32767;
          out[2 * done + k] = static_cast<int16_t>(v);
        }
        at_++;
        have_--;
        done++;
        position_++;
      }
    }
    return done;
  }

 private:
  // Text, with a #TITLE or #CHANNEL directive at the start of a line.
  static bool looksLikeMml(const Bytes& file) {
    if (file.size() > (1u << 20)) return false;
    size_t odd = 0;
    for (uint8_t c : file)
      if (c < 0x20 && c != '\t' && c != '\r' && c != '\n' && c != 0x1a) odd++;
    if (odd > 0) return false;
    const std::string text(file.begin(), file.end());
    for (const char* key : {"#CHANNEL", "#TITLE"}) {
      size_t at = text.find(key);
      bool found = false;
      while (at != std::string::npos && !found) {
        size_t k = at;
        while (k > 0 && (text[k - 1] == ' ' || text[k - 1] == '\t')) k--;
        found = k == 0 || text[k - 1] == '\n' || text[k - 1] == '\r' || text[k - 1] == '}' ||
                text[k - 1] == '>';
        at = text.find(key, at + 1);
      }
      if (!found) return false;
    }
    return true;
  }

  Bytes tsd_;
  Player player_;
  int rate_ = 48000;
  long long length_ = 0, fadeFrom_ = -1, position_ = 0;
  double gain_ = 1;
  int32_t chunk_[2 * 4096];
  int have_ = 0, at_ = 0;
};

}  // namespace

REGISTER_ENGINE("tss", TssEngine)
