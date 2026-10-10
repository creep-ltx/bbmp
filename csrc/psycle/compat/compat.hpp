// SPDX-License-Identifier: GPL-3.0-or-later
// What Psycle's host code expects of Windows, as far as its playing parts use it.
#pragma once
#include "plugin.hpp"
#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cmath>
#include <string>
#include <functional>
#include <cstdarg>
#include <cctype>
#include <memory>
#include <typeinfo>
#include <stdexcept>
#include <map>
#include <set>
#include <vector>
#include <list>
#include <algorithm>
#include <cassert>
#include <sstream>
#include <iostream>
#include <strings.h>
#include <unistd.h>

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef std::uint32_t DWORD;
typedef std::uint64_t DWORDLONG;
typedef unsigned int UINT;
typedef std::int32_t LONG;
typedef std::uint32_t ULONG;
typedef std::int64_t LONGLONG;
typedef std::int64_t __int64;
typedef std::uintptr_t UINT_PTR;
typedef std::intptr_t INT_PTR;
typedef std::intptr_t LONG_PTR;
typedef std::uintptr_t ULONG_PTR;
typedef std::uintptr_t DWORD_PTR;
typedef std::uintptr_t WPARAM;
typedef std::intptr_t LPARAM;
typedef std::intptr_t LRESULT;
typedef std::int32_t HRESULT;
typedef char TCHAR;
typedef char CHAR;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef char* LPTSTR;
typedef const char* LPCTSTR;
typedef DWORD* LPDWORD;
typedef void* LPVOID;
typedef void* HANDLE;
typedef void* HWND;
typedef void* HINSTANCE;
typedef void* HMODULE;
typedef void* HICON;
typedef void* HKEY;
typedef DWORD COLORREF;
class CWnd;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define WINAPI
#define CALLBACK
#define MAX_PATH 4096
#define _MAX_PATH 4096
#define _T(x) x
#define TEXT(x) x
#define ZeroMemory(p, n) std::memset((p), 0, (n))
#define CopyMemory(d, s, n) std::memcpy((d), (s), (n))
#define stricmp strcasecmp
#define _stricmp strcasecmp
#define strnicmp strncasecmp
#define _strnicmp strncasecmp
#define _snprintf snprintf
#define __forceinline inline __attribute__((always_inline))
#define ASSERT(x) assert(x)
#define VERIFY(x) ((void)(x))
#define TRACE(...) ((void)0)
#define MB_OK 0
#define MB_ICONERROR 0
#define MB_ICONWARNING 0
#define MB_YESNO 0
#define IDYES 6
#define IDNO 7
// a message box is a line on stderr, and nobody answers
inline int MessageBox(void*, const char* text, const char* title, unsigned) {
  std::fprintf(stderr, "psycle: %s: %s\n", title ? title : "", text ? text : "");
  return IDNO;
}
struct SYSTEM_INFO { int wProcessorArchitecture; };
#define PROCESSOR_ARCHITECTURE_AMD64 9
#define OS_WOW6432 30
inline void GetSystemInfo(SYSTEM_INFO* si) { si->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_AMD64; }
inline BOOL IsOS(int) { return FALSE; }
inline char* _strlwr(char* s) { for (char* p = s; *p; ++p) *p = static_cast<char>(std::tolower(static_cast<unsigned char>(*p))); return s; }
inline char* strlwr(char* s) { return _strlwr(s); }
typedef unsigned char byte;
union ULARGE_INTEGER {
  struct { DWORD LowPart; DWORD HighPart; };
  struct { DWORD LowPart; DWORD HighPart; } u;
  std::uint64_t QuadPart;
};
union LARGE_INTEGER {
  struct { DWORD LowPart; LONG HighPart; };
  std::int64_t QuadPart;
};
struct GUID { DWORD Data1; WORD Data2, Data3; BYTE Data4[8]; };
#pragma pack(push, 1)
struct WAVEFORMATEX {
  WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize;
};
struct WAVEFORMATEXTENSIBLE {
  WAVEFORMATEX Format;
  union { WORD wValidBitsPerSample, wSamplesPerBlock, wReserved; } Samples;
  DWORD dwChannelMask; GUID SubFormat;
};
#pragma pack(pop)
struct CPoint {
  LONG x, y;
  CPoint() : x(0), y(0) {}
  CPoint(int x_, int y_) : x(x_), y(y_) {}
};
struct CSize { LONG cx, cy; CSize() : cx(0), cy(0) {} CSize(int x, int y) : cx(x), cy(y) {} };
struct CRect { LONG left, top, right, bottom; CRect() : left(0), top(0), right(0), bottom(0) {} };
// The player is one thread, so that nothing has to wait for anything.
struct CSyncObject {
  virtual ~CSyncObject() {}
  BOOL Lock(DWORD = 0xFFFFFFFF) { return TRUE; }
  BOOL Unlock() { return TRUE; }
  BOOL Unlock(LONG, LONG* = 0) { return TRUE; }
};
struct CSemaphore : CSyncObject { CSemaphore(LONG = 1, LONG = 1, const char* = 0, void* = 0) {} };
struct CCriticalSection : CSyncObject {};
struct CMutex : CSyncObject {};
struct CEvent : CSyncObject {
  CEvent(BOOL = FALSE, BOOL = FALSE, const char* = 0, void* = 0) {}
  BOOL SetEvent() { return TRUE; }
  BOOL ResetEvent() { return TRUE; }
};
struct CSingleLock {
  explicit CSingleLock(CSyncObject*, BOOL = FALSE) {}
  BOOL Lock(DWORD = 0xFFFFFFFF) { return TRUE; }
  BOOL Unlock() { return TRUE; }
  BOOL IsLocked() { return TRUE; }
};
// MFC's string, as much of it as the player's code uses
class CString {
 public:
  CString() {}
  CString(const char* s) : s_(s ? s : "") {}
  CString(const std::string& s) : s_(s) {}
  operator const char*() const { return s_.c_str(); }
  const char* GetString() const { return s_.c_str(); }
  bool IsEmpty() const { return s_.empty(); }
  int GetLength() const { return static_cast<int>(s_.size()); }
  int ReverseFind(char c) const { const size_t p = s_.rfind(c); return p == std::string::npos ? -1 : static_cast<int>(p); }
  int Find(char c) const { const size_t p = s_.find(c); return p == std::string::npos ? -1 : static_cast<int>(p); }
  int Find(const char* t) const { const size_t p = s_.find(t); return p == std::string::npos ? -1 : static_cast<int>(p); }
  CString Mid(int first) const { return first < GetLength() ? CString(s_.substr(first)) : CString(); }
  CString Mid(int first, int count) const { return first < GetLength() ? CString(s_.substr(first, count)) : CString(); }
  CString Left(int count) const { return CString(s_.substr(0, count)); }
  CString Right(int count) const { return count >= GetLength() ? *this : CString(s_.substr(s_.size() - count)); }
  CString& MakeLower() { for (size_t i = 0; i < s_.size(); ++i) s_[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s_[i]))); return *this; }
  CString& operator+=(const CString& o) { s_ += o.s_; return *this; }
  CString& operator+=(char c) { s_ += c; return *this; }
  friend CString operator+(CString a, const CString& b) { a += b; return a; }
  friend bool operator==(const CString& a, const CString& b) { return a.s_ == b.s_; }
  friend bool operator!=(const CString& a, const CString& b) { return a.s_ != b.s_; }
  int Compare(const char* t) const { return s_.compare(t); }
  int CompareNoCase(const char* t) const { return strcasecmp(s_.c_str(), t); }
  void Format(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    s_ = buf;
  }
 private:
  std::string s_;
};
// files, for the one class of Psycle that reads them the Windows way
#include <fcntl.h>
#define GENERIC_READ 1u
#define GENERIC_WRITE 2u
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define CREATE_NEW 1
#define FILE_ATTRIBUTE_NORMAL 0
#define FILE_BEGIN SEEK_SET
#define FILE_CURRENT SEEK_CUR
#define FILE_END SEEK_END
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1)))
inline HANDLE CreateFile(const char* name, DWORD access, DWORD, void*, DWORD how, DWORD, void*) {
  int flags = (access & GENERIC_WRITE) && how != OPEN_EXISTING ? O_RDWR : O_RDONLY;
  if (how == CREATE_ALWAYS) flags |= O_CREAT | O_TRUNC;
  if (how == CREATE_NEW) flags |= O_CREAT | O_EXCL;
  const int fd = ::open(name, flags, 0644);
  return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(fd));
}
inline int fd_of(HANDLE h) { return static_cast<int>(reinterpret_cast<std::intptr_t>(h)); }
inline BOOL ReadFile(HANDLE h, void* data, DWORD bytes, DWORD* done, void*) {
  const ssize_t n = ::read(fd_of(h), data, bytes);
  if (done) *done = n < 0 ? 0 : static_cast<DWORD>(n);
  return n >= 0;
}
inline BOOL WriteFile(HANDLE h, const void* data, DWORD bytes, DWORD* done, void*) {
  const ssize_t n = ::write(fd_of(h), data, bytes);
  if (done) *done = n < 0 ? 0 : static_cast<DWORD>(n);
  return n >= 0;
}
inline DWORD SetFilePointer(HANDLE h, LONG offset, LONG*, DWORD whence) {
  return static_cast<DWORD>(::lseek(fd_of(h), offset, static_cast<int>(whence)));
}
inline BOOL CloseHandle(HANDLE h) { return ::close(fd_of(h)) == 0; }
// wave formats
#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_IEEE_FLOAT 3
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#define SPEAKER_FRONT_LEFT 1
#define SPEAKER_FRONT_RIGHT 2
const GUID KSDATAFORMAT_SUBTYPE_PCM = {1, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {3, 0, 0x10, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
// libraries
#include <dlfcn.h>
inline HMODULE LoadLibrary(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
inline void* GetProcAddress(HMODULE lib, const char* name) { return dlsym(lib, name); }
inline BOOL FreeLibrary(HMODULE lib) { return dlclose(lib) == 0; }
#define IDOK 1
inline void Sleep(unsigned ms) { usleep(ms * 1000u); }
#endif

#ifdef __cplusplus
#include <boost/bind.hpp>
#endif
