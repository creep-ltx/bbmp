// SPDX-License-Identifier: GPL-3.0-or-later
// Where this program is, so an engine can find the files beside it (<prefix>/share):
// Linux says it in /proc, macOS through _NSGetExecutablePath. "" when it can't be told.
#pragma once
#include <climits>
#include <cstdlib>
#include <string>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

inline std::string self_path() {
  char buf[PATH_MAX];
#ifdef __APPLE__
  uint32_t size = sizeof(buf);
  char real[PATH_MAX];
  if (_NSGetExecutablePath(buf, &size) != 0 || !realpath(buf, real)) return "";
  return real;
#else
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "";
  buf[n] = 0;
  return buf;
#endif
}
