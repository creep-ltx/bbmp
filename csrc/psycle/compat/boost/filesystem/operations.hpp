// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A path as far as Psycle's player needs one: a string with a few questions to ask.
#include <string>
#include <sys/stat.h>
namespace boost { namespace filesystem {
class path {
 public:
  path() {}
  path(const char* s) : s_(s ? s : "") {}
  path(const std::string& s) : s_(s) {}
  const std::string& string() const { return s_; }
  const char* c_str() const { return s_.c_str(); }
  std::string native() const { return s_; }
  std::string file_string() const { return s_; }
  std::string native_file_string() const { return s_; }
  std::string directory_string() const { return s_; }
  std::string native_directory_string() const { return s_; }
  bool empty() const { return s_.empty(); }
  path filename() const { const size_t p = s_.rfind('/'); return path(p == std::string::npos ? s_ : s_.substr(p + 1)); }
  path leaf() const { return filename(); }
  path parent_path() const { const size_t p = s_.rfind('/'); return path(p == std::string::npos ? std::string() : s_.substr(0, p)); }
  path branch_path() const { return parent_path(); }
  path extension() const { const std::string f = filename().s_; const size_t p = f.rfind('.'); return path(p == std::string::npos ? std::string() : f.substr(p)); }
  path stem() const { const std::string f = filename().s_; const size_t p = f.rfind('.'); return path(p == std::string::npos ? f : f.substr(0, p)); }
  path& operator/=(const path& o) { if (!s_.empty() && s_.back() != '/') s_ += '/'; s_ += o.s_; return *this; }
  friend path operator/(path a, const path& b) { a /= b; return a; }
  friend bool operator==(const path& a, const path& b) { return a.s_ == b.s_; }
  friend bool operator!=(const path& a, const path& b) { return a.s_ != b.s_; }
  friend bool operator<(const path& a, const path& b) { return a.s_ < b.s_; }
 private:
  std::string s_;
};
inline bool exists(const path& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
inline bool is_directory(const path& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
template<class Out> Out& operator<<(Out& out, const path& p) { out << p.string(); return out; }
}}
