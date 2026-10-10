// SPDX-License-Identifier: GPL-3.0-or-later
// No VST plugins here: what is left of the VST host is the time information that
// Psycle's player keeps up to date for it.
#pragma once
#include <cstdint>
typedef std::int32_t VstInt32;
typedef std::intptr_t VstIntPtr;
enum {
  kVstTransportChanged = 1, kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9,
  kVstTempoValid = 1 << 10, kVstBarsValid = 1 << 11
};
struct VstTimeInfo {
  double samplePos = 0, sampleRate = 44100, nanoSeconds = 0, ppqPos = 0, tempo = 125, barStartPos = 0;
  double cycleStartPos = 0, cycleEndPos = 0;
  VstInt32 timeSigNumerator = 4, timeSigDenominator = 4, smpteOffset = 0, smpteFrameRate = 0;
  VstInt32 samplesToNextClock = 0, flags = 0;
};
namespace seib { namespace vst {
class CVSTHost {
 public:
  virtual ~CVSTHost() {}
  virtual void SetSampleRate(float rate) { vstTimeInfo.sampleRate = rate; }
  static void UseJBridge(bool) {}
  static bool UseJBridge() { return false; }
  static void UsePsycleVstBridge(bool) {}
  static bool UsePsycleVstBridge() { return false; }
  static VstTimeInfo vstTimeInfo;
  static CVSTHost* pHost;
};
}}
