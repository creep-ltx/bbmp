// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in for <windows.h>, <mmreg.h> and <msacm.h> when ProTrekkr's sample
// unpacker is built outside Windows. It declares just what samples_unpack.cpp
// uses of the Audio Compression Manager; ptk_engine.cpp implements those calls.
#ifndef BBMP_PTK_WINDOWS_H
#define BBMP_PTK_WINDOWS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef int MMRESULT;
typedef BYTE* LPBYTE;
typedef uintptr_t DWORD_PTR;

#pragma pack(push, 1)
typedef struct {
  WORD wFormatTag;
  WORD nChannels;
  DWORD nSamplesPerSec;
  DWORD nAvgBytesPerSec;
  WORD nBlockAlign;
  WORD wBitsPerSample;
  WORD cbSize;
} WAVEFORMATEX, *LPWAVEFORMATEX;

typedef struct {
  WAVEFORMATEX wfx;
  WORD wSamplesPerBlock;
} GSM610WAVEFORMAT;

typedef struct {
  WAVEFORMATEX wfx;
  WORD wRevision;
  WORD nSamplesPerBlock;
  BYTE abReserved[28];
} TRUESPEECHWAVEFORMAT;

typedef struct {
  WAVEFORMATEX wfx;
  WORD wID;
  DWORD fdwFlags;
  WORD nBlockSize;
  WORD nFramesPerBlock;
  WORD nCodecDelay;
} MPEGLAYER3WAVEFORMAT;
#pragma pack(pop)

typedef struct {
  DWORD cbStruct;
  DWORD fdwStatus;
  DWORD_PTR dwUser;
  LPBYTE pbSrc;
  DWORD cbSrcLength;
  DWORD cbSrcLengthUsed;
  DWORD_PTR dwSrcUser;
  LPBYTE pbDst;
  DWORD cbDstLength;
  DWORD cbDstLengthUsed;
  DWORD_PTR dwDstUser;
} ACMSTREAMHEADER;

typedef struct ptk_acm_stream* HACMSTREAM;

#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_GSM610 0x0031
#define WAVE_FORMAT_MPEGLAYER3 0x0055
#define MPEGLAYER3_WFX_EXTRA_BYTES 12
#define MPEGLAYER3_ID_MPEG 1
#define MPEGLAYER3_FLAG_PADDING_OFF 2
#define ACM_FORMATSUGGESTF_WFORMATTAG 0x00010000
#define ACM_STREAMOPENF_NONREALTIME 4
#define ACM_STREAMSIZEF_SOURCE 0

MMRESULT acmFormatSuggest(void* driver, LPWAVEFORMATEX src, LPWAVEFORMATEX dst, DWORD size, DWORD flags);
MMRESULT acmStreamOpen(HACMSTREAM* stream, void* driver, LPWAVEFORMATEX src, LPWAVEFORMATEX dst,
                       void* filter, DWORD_PTR callback, DWORD_PTR instance, DWORD flags);
MMRESULT acmStreamSize(HACMSTREAM stream, DWORD input, DWORD* output, DWORD flags);
MMRESULT acmStreamPrepareHeader(HACMSTREAM stream, ACMSTREAMHEADER* header, DWORD flags);
MMRESULT acmStreamConvert(HACMSTREAM stream, ACMSTREAMHEADER* header, DWORD flags);
MMRESULT acmStreamUnprepareHeader(HACMSTREAM stream, ACMSTREAMHEADER* header, DWORD flags);
MMRESULT acmStreamClose(HACMSTREAM stream, DWORD flags);

#endif
