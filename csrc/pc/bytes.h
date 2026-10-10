// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-pc: reading a file's bytes with bounds checked, and writing a module
// into memory. For the engines that convert a file for libopenmpt.
#ifndef BBMP_PC_BYTES_H
#define BBMP_PC_BYTES_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace pcbytes {

struct Bad : std::runtime_error {
  explicit Bad(const std::string& what) : std::runtime_error(what) {}
};

// A view of the file. Reading past the end throws Bad.
class In {
 public:
  In(const uint8_t* data, size_t size) : d_(data), n_(size) {}
  size_t size() const { return n_; }
  const uint8_t* at(size_t off, size_t len) const {
    if (off > n_ || len > n_ - off) throw Bad("the file is cut short");
    return d_ + off;
  }
  uint8_t u8(size_t off) const { return *at(off, 1); }
  int8_t s8(size_t off) const { return static_cast<int8_t>(u8(off)); }
  uint16_t le16(size_t off) const { const uint8_t* p = at(off, 2); return p[0] | p[1] << 8; }
  uint32_t le32(size_t off) const {
    const uint8_t* p = at(off, 4);
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
  }
  uint16_t be16(size_t off) const { const uint8_t* p = at(off, 2); return p[0] << 8 | p[1]; }
  uint32_t be32(size_t off) const {
    const uint8_t* p = at(off, 4);
    return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
  }
  // a text field of fixed size: up to the first NUL, trailing blanks removed
  std::string text(size_t off, size_t len) const {
    const uint8_t* p = at(off, len);
    size_t n = 0;
    while (n < len && p[n]) ++n;
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) --n;
    return std::string(reinterpret_cast<const char*>(p), n);
  }

 private:
  const uint8_t* d_;
  size_t n_;
};

class Out {
 public:
  std::vector<uint8_t> v;
  size_t size() const { return v.size(); }
  void u8(unsigned x) { v.push_back(static_cast<uint8_t>(x)); }
  void le16(unsigned x) { u8(x); u8(x >> 8); }
  void le32(uint32_t x) { le16(x); le16(x >> 16); }
  void bytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    v.insert(v.end(), b, b + n);
  }
  void zeros(size_t n) { v.insert(v.end(), n, 0); }
  // text in a field of fixed size, padded with `pad`
  void text(const std::string& s, size_t len, char pad = 0) {
    for (size_t i = 0; i < len; ++i) u8(i < s.size() ? static_cast<uint8_t>(s[i]) : pad);
  }
  void put16(size_t at, unsigned x) { v.at(at) = x & 255; v.at(at + 1) = (x >> 8) & 255; }
  void put32(size_t at, uint32_t x) { put16(at, x); put16(at + 2, x >> 16); }
};

}  // namespace pcbytes

#endif
