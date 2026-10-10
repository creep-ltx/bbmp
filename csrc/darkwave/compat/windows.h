// SPDX-License-Identifier: GPL-3.0-or-later
// bbmp-darkwave: what DarkWave Studio's sound code takes from <windows.h>, for Linux.
// Part of a port of DarkWave Studio 4.1.6 (GPL 3 or later, see csrc/darkwave/README.md).
// Everything runs in one thread, so the mutexes do nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <strings.h>
#include <cstring>
#include <cmath>
#include <algorithm>   // before rand() is renamed below
#include <memory>
#include <string>
#include <vector>
#include <mm_malloc.h>
#include <xmmintrin.h>
#include <emmintrin.h>

#ifndef WIN64
#define WIN64 1          // 64-bit: DarkPlug.h and Util.h choose by it
#endif
#define __cdecl
#define __stdcall
#define __declspec(x)

typedef void* HANDLE;
typedef void* HWND;
typedef void* HINSTANCE;
typedef void* HICON;
typedef void* HGLOBAL;
typedef void* HMENU;
typedef void* HDC;
typedef uint32_t COLORREF;
typedef uint8_t BYTE;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef int64_t __int64;
typedef int32_t __int32;
typedef union { struct { uint32_t LowPart; int32_t HighPart; } u; int64_t QuadPart; } LARGE_INTEGER;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define INFINITE 0xffffffffu

#define RGB(r, g, b) ((COLORREF)(((BYTE)(r)) | (((DWORD)(BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(c) ((BYTE)(c))
#define GetGValue(c) ((BYTE)((c) >> 8))
#define GetBValue(c) ((BYTE)((c) >> 16))

inline HANDLE CreateMutex(void*, BOOL, void*) { return nullptr; }
inline DWORD WaitForSingleObject(HANDLE, DWORD) { return 0; }
inline BOOL ReleaseMutex(HANDLE) { return TRUE; }
inline BOOL CloseHandle(HANDLE) { return TRUE; }
// BassHead seeds rand() with the time; a fixed seed makes every rendering the same
inline DWORD GetTickCount() { return 0; }

// Microsoft's C library's rand(), as the program had it: its noise is this sequence
inline unsigned int& dw_rand_state() { static unsigned int s = 1; return s; }
inline int dw_msvc_rand() {
  dw_rand_state() = dw_rand_state() * 214013u + 2531011u;
  return (int)((dw_rand_state() >> 16) & 0x7fff);
}
inline void dw_msvc_srand(unsigned int seed) { dw_rand_state() = seed; }
#define rand dw_msvc_rand
#define srand dw_msvc_srand
#undef RAND_MAX
#define RAND_MAX 0x7fff

inline int _vscprintf(const char* format, va_list args) {
  va_list copy;
  va_copy(copy, args);
  int n = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  return n;
}
#define _stricmp strcasecmp

#define SW_SHOWNORMAL 1
inline void ShellExecuteA(void*, const char*, const char*, void*, void*, int) {}

// the machines' icons and pictures: nothing is loaded
#define MAKEINTRESOURCE(i) ((const char*)(uintptr_t)(i))
#define MAKEINTRESOURCEA(i) ((const char*)(uintptr_t)(i))
#define IMAGE_ICON 1
#define LR_DEFAULTCOLOR 0
inline HANDLE LoadImage(HINSTANCE, const char*, UINT, int, int, UINT) { return nullptr; }

// messages go to stderr
#define MB_ICONERROR 0x10
inline int MessageBoxA(HWND, const char* text, const char*, UINT) { std::fprintf(stderr, "%s\n", text); return 0; }
inline void OutputDebugStringA(const char*) {}
