// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
namespace boost {
inline void to_lower(std::string& s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
inline void to_upper(std::string& s) { for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
inline std::string to_lower_copy(std::string s) { to_lower(s); return s; }
inline void trim(std::string& s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  s.erase(0, i);
}
}
