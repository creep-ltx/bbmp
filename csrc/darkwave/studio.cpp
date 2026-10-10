/* SPDX-License-Identifier: GPL-3.0-or-later */
/*  bbmp-darkwave: DarkWave Studio's studio without its windows.

    Ported from ExperimentalScene DarkWave Studio 4.1.6 (Studio/Core.cpp, Studio.cpp,
    MachineContainer.cpp, Connector.cpp, Pattern.cpp, SequenceEditor.cpp, PatternEditor.cpp,
    Host.cpp, Master.cpp), Copyright (C) 2004-2012 Daniel Werner,
    http://www.experimentalscene.com/ ; changed for bbmp in 2026. The comments "as the
    studio does" mark where its behaviour is kept although it looks odd.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "studio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "BassHead.h"
#include "Core.h"
#include "DrumKit.h"
#include "Echo.h"
#include "Kick.h"
#include "Mixer.h"
#include "SpatialVerb.h"
#include "Util.h"
#undef min   // Util.h's macros
#undef max

namespace darkwave {

// ---------------------------------------------------------------- the host (Host.cpp)

Host::Host(int sampleRate, int blockSamples) : SampleRate(sampleRate), BlockSamples(blockSamples) {
  const double k = 1.059463094359;   // 12th root of 2
  double a = 6.875;                  // a
  a *= k;                            // b
  a *= k;                            // bb
  a *= k;                            // c, frequency of midi note 0
  for (int i = 0; i <= DW_MAX_NOTE; i++) {   // the studio's table ends at 126; 127 goes on
    NoteTable[i] = a;
    a *= k;
  }
}

EError Host::RegisterMachineInfo(CMachineInfo* machineInfo) {
  Machines.push_back(machineInfo);
  return errorOK;
}

EError Host::GetNoteFrequency(unsigned int note, double* frequency) {
  if (note > DW_MAX_NOTE) {
    *frequency = 0.0;
    return errorInvalidParameter;
  }
  *frequency = NoteTable[note];
  return errorOK;
}

EError Host::GetBankServer(CBankServer** bankServer) {
  *bankServer = nullptr;
  return errorFailed;
}

void Host::SetPropertyAutomated(CMachine* machine, int index, double value) {
  // the container passes it on to the machine (and to the property window it hasn't here)
  machine->SetProperty(index, value);
}

int Host::GetSamplesPerTick() {
  return (int)((60.0 / (GetBPM() * GetTPB())) * (double)SampleRate);
}

int Host::GetCPUFeatures() {
  // what DarkWave Studio found on any x86-64 processor; the machines' SSE2 paths differ
  // from their plain ones (Util.cpp's AddValueToBlock adds to half the samples), and the
  // program used the SSE2 ones
  return CPU_FEATURE_MMX | CPU_FEATURE_SSE | CPU_FEATURE_SSE2;
}

// ---------------------------------------------------------------- the master (Master.cpp)

namespace {

class MasterMachine : public CMachine {
 public:
  explicit MasterMachine(CHost* host) : CMachine(host) {
    Ports = new CPort*[1];
    Ports[0] = new CPort(host, this, PT_AUDIO | PT_INPUT | PT_STEREO, (char*)"Line In");
    PortCount = 1;
  }
  ~MasterMachine() override {
    delete Ports[0];
    delete[] Ports;
  }
  void DARKCALL Work() override {}
  void DARKCALL SetSampleRate(int) override {}
  void DARKCALL SetBlockSamples(int) override {}
  bool DARKCALL IsSynth() override { return false; }
};

class MasterInfo : public CMachineInfo {
 public:
  explicit MasterInfo(CHost* host) : Host(host) {
    Name = (char*)"Master";
    UniqueID = (char*)"Master";
  }
  CMachine* DARKCALL CreateMachine() override { return new MasterMachine(Host); }

 private:
  CHost* Host;
};

// A reader of the project file that never reads past its end.
struct Reader {
  const uint8_t* p;
  size_t left;
  bool ok = true;
  bool take(void* out, size_t n) {
    if (!ok || n > left) {
      ok = false;
      return false;
    }
    std::memcpy(out, p, n);
    p += n;
    left -= n;
    return true;
  }
  int32_t i32() {
    int32_t v = 0;
    take(&v, 4);
    return v;
  }
  float f32() {
    float v = 0;
    take(&v, 4);
    return v;
  }
  // a block the file says the size of: its bytes, or empty when it isn't all there
  std::vector<uint8_t> block() {
    const int32_t n = i32();
    if (!ok || n < 0 || (size_t)n > left) {
      ok = false;
      return {};
    }
    std::vector<uint8_t> out(p, p + n);
    p += n;
    left -= n;
    return out;
  }
};

std::string cstring(const char* p, size_t n) {
  size_t k = 0;
  while (k < n && p[k]) k++;
  return std::string(p, k);
}

// A property as the studio's sliders and control tracks make it: 0 to 1. A damaged file
// can hold anything, and some machines index their tables with what they are given.
inline double Unit(double v) {
  return v >= 0.0 ? (v <= 1.0 ? v : 1.0) : 0.0;   // and not a number is 0
}

inline int32_t at32(const std::vector<uint8_t>& d, size_t pos) {
  int32_t v;
  std::memcpy(&v, d.data() + pos, 4);
  return v;
}

}  // namespace

// ---------------------------------------------------------------- the studio (Studio.cpp)

Studio::Studio(int sampleRate, int blockSamples) : TheHost(sampleRate, blockSamples) {
  // the machines of ES-CoreMachines.dll (CoreMachines/Main.cpp) that the port has, after the
  // studio's own Master
  Core = new CCore(nullptr, &TheHost, TheHost.GetCPUFeatures());
  Infos.emplace_back(new MasterInfo(&TheHost));
  Infos.emplace_back(new CBassHeadInfo(&TheHost));
  Infos.emplace_back(new CBassHeadPolyphonicInfo(&TheHost));
  Infos.emplace_back(new CDrumKitInfo(&TheHost));
  Infos.emplace_back(new CEchoInfo(&TheHost));
  Infos.emplace_back(new CKickInfo(&TheHost));
  Infos.emplace_back(new CMixerInfo(&TheHost));
  Infos.emplace_back(new CSpatialVerbInfo(&TheHost));
}

Studio::~Studio() {
  for (auto& c : Containers) {
    for (auto& k : c->Connectors)
      for (Connection* n : k->Connections) delete n;
    if (c->Machine) c->Machine->Delete();
  }
}

long long Studio::SongSamples() const {
  Host& h = const_cast<Host&>(TheHost);
  return (long long)SequenceLength * 4 * h.GetSamplesPerTick();
}

Container* Studio::AddMachine(const std::string& instanceName, const std::string& machineName,
                              const std::string& uniqueID, const char* properties,
                              int propertyBytes, bool oldUniqueID) {
  (void)machineName;   // the studio uses it only to choose among VST plug-ins
  for (auto& info : Infos) {
    const char* id = oldUniqueID ? (info->OldUniqueID ? info->OldUniqueID : info->UniqueID)
                                 : info->UniqueID;
    if (uniqueID != id) continue;
    auto c = std::make_unique<Container>();
    c->Name = instanceName;
    c->UniqueID = info->UniqueID;
    c->Machine = info->CreateMachine();
    // CMachineContainer's constructor: the properties first, then the name and the ports
    if (properties && propertyBytes >= 8) {
      // CMachine::ReadProperties: floats after the magic 'DwPh', doubles after 'PH64'
      int32_t magic, count;
      std::memcpy(&magic, properties, 4);
      std::memcpy(&count, properties + 4, 4);
      const bool floats = magic == 'DwPh';
      const int size = floats ? 4 : 8;
      if (count < 0) count = 0;
      count = std::min(count, (propertyBytes - 8) / size);   // no further than the file has
      for (int i = 0; i < count; i++) {
        double v;
        if (floats) {
          float f;
          std::memcpy(&f, properties + 8 + 4 * i, 4);
          v = f;
        } else {
          std::memcpy(&v, properties + 8 + 8 * i, 8);
        }
        c->Machine->SetPropertyAutomated(i, Unit(v));
      }
    }
    c->Machine->HostData = c.get();
    c->Machine->SetName((char*)instanceName.c_str());
    for (int i = 0; i < c->Machine->PortCount; i++) {
      auto k = std::make_unique<Connector>();
      k->Port = c->Machine->Ports[i];
      k->Owner = c.get();
      if ((k->Port->Type & PT_CONTROL) && (k->Port->Type & PT_INPUT)) k->Future.reset(new CControlBuffer());
      c->Connectors.push_back(std::move(k));
    }
    // the pattern editor's AddMachine: a track of the sequence for each control input
    for (auto& k : c->Connectors)
      if ((k->Port->Type & (PT_CONTROL | PT_OUTPUT)) == PT_CONTROL) Tracks.push_back({k.get(), {}});
    Container* made = c.get();
    Containers.push_back(std::move(c));
    if (instanceName == "Master") Master = made;
    return made;
  }
  return nullptr;
}

Connector* Studio::GetConnector(int machine, int connector) {
  for (size_t i = 0; i < Containers.size(); i++)
    if (ContainerIndex[i] == machine) {
      Container* c = Containers[i].get();
      if (connector >= 0 && connector < (int)c->Connectors.size()) return c->Connectors[connector].get();
      return nullptr;
    }
  return nullptr;
}

void Studio::ConnectConnectors(int m1, int c1, int m2, int c2, double volume) {
  Connector* a = GetConnector(m1, c1);
  Connector* b = GetConnector(m2, c2);
  if (!a || !b) return;
  // CStudio::CanConnect
  CPort* p1 = a->Port;
  CPort* p2 = b->Port;
  if (p1->Machine == p2->Machine) return;
  if ((p1->Type & (PT_AUDIO | PT_CONTROL)) != (p2->Type & (PT_AUDIO | PT_CONTROL))) return;
  if ((p1->Type & PT_OUTPUT) == (p2->Type & PT_OUTPUT)) return;
  Connector* from = (p1->Type & PT_OUTPUT) ? a : b;
  Connector* to = (p1->Type & PT_OUTPUT) ? b : a;
  for (Connection* n : to->Connections)
    if (n->Input == from) return;
  to->Connections.push_back(new Connection{from, to, volume, volume});
}

std::string Studio::Load(const std::vector<uint8_t>& file) {
  Reader r{file.data(), file.size()};
  // DWP_HEADER, packed: int Magic; char MajorVersion, MinorVersion; float BPM
  int32_t magic = r.i32();
  int8_t major = 0, minor = 0;
  r.take(&major, 1);
  r.take(&minor, 1);
  float bpm = r.f32();
  if (!r.ok || magic != 'DWP ') return "not a DarkWave Studio project";
  MajorVersion = major;
  MinorVersion = minor;
  if (major > 6 || major < 1) return "a DarkWave Studio project of a version this player doesn't know";
  if (!(bpm >= 1.0f && bpm <= 10000.0f)) return "the project's tempo is damaged";
  TheHost.BPM = bpm;
  TheHost.TPB = 16;
  if (major > 3) {
    const float tpb = r.f32();
    if (!r.ok || !(tpb >= 1.0f && tpb <= 1000.0f)) return "the project's ticks per beat are damaged";
    TheHost.TPB = tpb;
  }
  if (TheHost.GetSamplesPerTick() < 1) return "the project's tempo is too fast to play";

  const int32_t machineCount = r.i32();
  if (!r.ok || machineCount < 0) return "the project's machines are damaged";
  if (machineCount > 256) return "the project has more machines than this player plays (256)";
  std::vector<Container*> mc(machineCount, nullptr);
  for (int i = 0; i < machineCount; i++) {
    // DWP_MACHINE: char Name[1024], UniqueID[1024]; int X, Y, VisibleControl, PropertyBytes;
    // bool IsInner; then (version 5 on) DWP_MACHINE_2: char MachineName[1024]
    char name[1024], id[1024], machineName[1024];
    int32_t xyv[3], propertyBytes;
    uint8_t inner;
    r.take(name, sizeof name);
    r.take(id, sizeof id);
    r.take(xyv, sizeof xyv);
    propertyBytes = r.i32();
    r.take(&inner, 1);
    if (major > 4) r.take(machineName, sizeof machineName);
    if (!r.ok || propertyBytes < 0 || (size_t)propertyBytes > r.left) return "the project's machines are damaged";
    std::vector<char> properties(r.p, r.p + propertyBytes);
    r.p += propertyBytes;
    r.left -= propertyBytes;
    std::string instance = cstring(name, sizeof name);
    if (instance == "DarkPlug::ES BrainSynth") instance = "DarkPlug::ES BassHead";   // as the studio does
    const std::string uid = cstring(id, sizeof id);
    const std::string mname = major > 4 ? cstring(machineName, sizeof machineName) : instance;
    mc[i] = AddMachine(instance, mname, uid, propertyBytes ? properties.data() : nullptr, propertyBytes,
                       major < 6);
    if (mc[i]) {
      ContainerIndex.push_back(i);
      mc[i]->Machine->Index = i;
      for (int j = 0; j < mc[i]->Machine->PortCount; j++) mc[i]->Machine->Ports[j]->Index = j;
      MachineNames.push_back(instance);
    } else {
      std::string what = uid.rfind("DarkPlug::", 0) == 0 ? uid.substr(10) : uid;
      if (what.rfind("VST::", 0) == 0) what = "the VST plug-in " + what.substr(5);
      MissingMachines.push_back(what + " (" + instance + ")");
    }
  }

  for (int i = 0; i < machineCount; i++) {
    const std::vector<uint8_t> connections = r.block();
    if (!r.ok) return "the project's cables are damaged";
    if (mc[i] && connections.size() >= 4) {
      // CMachineContainer::ReadConnections: int m1, c1, m2, c2 and a volume: none before
      // version 2, a float in version 2, a double since
      const bool hasVolume = major > 1, isFloat = major < 3;
      const size_t size = 16 + (hasVolume ? (isFloat ? 4 : 8) : 0);
      const int32_t count = at32(connections, 0);
      for (int32_t k = 0; k >= 0 && k < count && 4 + (k + 1) * size <= connections.size(); k++) {
        const size_t at = 4 + k * size;
        double volume = 1.0;
        if (hasVolume && isFloat) {
          float f;
          std::memcpy(&f, connections.data() + at + 16, 4);
          volume = f;
        } else if (hasVolume) {
          std::memcpy(&volume, connections.data() + at + 16, 8);
        }
        if (!std::isfinite(volume)) volume = 0.0;
        ConnectConnectors(at32(connections, at), at32(connections, at + 4), at32(connections, at + 8),
                          at32(connections, at + 12), volume);
      }
    }
    const std::vector<uint8_t> patterns = r.block();
    if (!r.ok) return "the project's patterns are damaged";
    if (!mc[i] || patterns.size() < 4) continue;
    // CPatternEditor::ReadPatterns, packed by 2: Pattern { int Index, Length; char Name[1024];
    // int NoteCount, ControlTrackCount, Machine, Connector }, then its notes { int Start,
    // Length, Note } and control tracks { int Index, EventCount } { float Value; bool Used }
    const size_t n = patterns.size();
    size_t at = 4;
    const int32_t count = at32(patterns, 0);
    for (int32_t k = 0; k >= 0 && k < count; k++) {
      if (at + 1048 > n) return "the project's patterns are damaged";
      auto p = std::make_unique<Pattern>();
      p->Index = at32(patterns, at);
      p->Length = at32(patterns, at + 4);
      p->Name = cstring((const char*)patterns.data() + at + 8, 1024);
      const int32_t notes = at32(patterns, at + 1032), tracks = at32(patterns, at + 1036);
      p->Port = GetConnector(at32(patterns, at + 1040), at32(patterns, at + 1044));
      at += 1048;
      // the pattern editor makes patterns of 1 to 1024 steps
      if (p->Length < 0 || p->Length > 1024 || notes < 0 || tracks < 0) return "the project's patterns are damaged";
      if ((size_t)notes > (n - at) / 12) return "the project's patterns are damaged";
      for (int32_t j = 0; j < notes; j++, at += 12)
        p->Notes.push_back({at32(patterns, at), at32(patterns, at + 4), at32(patterns, at + 8)});
      for (int32_t j = 0; j < tracks; j++) {
        if (at + 8 > n) return "the project's patterns are damaged";
        ControlTrack t;
        t.Property = at32(patterns, at);
        const int32_t events = at32(patterns, at + 8 - 4);
        at += 8;
        if (events < 0 || (size_t)events > (n - at) / 6) return "the project's patterns are damaged";
        // CControlTrack: Length * 4 entries, the velocities used and at 1
        const bool velocity = t.Property == DW_PROPERTY_VELOCITY;
        t.Entries.assign((size_t)p->Length * 4, ControlEntry{velocity, velocity ? 1.0 : 0.0});
        for (int32_t e = 0; e < events; e++, at += 6) {
          if (e >= (int32_t)t.Entries.size()) continue;
          float v;
          std::memcpy(&v, patterns.data() + at, 4);
          t.Entries[e].Value = Unit(v);
          t.Entries[e].Used = patterns[at + 4] != 0;
        }
        if (velocity) p->VelocityTrack = (int)p->ControlTracks.size();
        p->ControlTracks.push_back(std::move(t));
      }
      Patterns.push_back(std::move(p));
    }
  }

  // CSequenceEditor::ReadSequence: int Length, Tracks; per track int Port, Size and its
  // events { int Start, Pattern }
  const std::vector<uint8_t> sequence = r.block();
  if (!r.ok || sequence.size() < 8) return "the project's sequence is damaged";
  SequenceLength = at32(sequence, 0);
  if (SequenceLength < 1 || SequenceLength > 4096) return "the project's sequence is damaged";
  const int32_t tracks = at32(sequence, 4);
  size_t at = 8;
  for (int32_t i = 0; i >= 0 && i < tracks; i++) {
    if (at + 8 > sequence.size()) return "the project's sequence is damaged";
    const int32_t size = at32(sequence, at + 4);
    at += 8;
    if (size < 0 || (size_t)size > (sequence.size() - at) / 8) return "the project's sequence is damaged";
    for (int32_t j = 0; j < size; j++, at += 8) {
      const int32_t start = at32(sequence, at), index = at32(sequence, at + 4);
      Pattern* p = nullptr;
      for (auto& q : Patterns)
        if (q->Index == index) {
          p = q.get();
          break;
        }
      if (!p || !p->Port) continue;
      SequenceTrack* track = nullptr;
      for (auto& t : Tracks)
        if (t.Port->Port == p->Port->Port) track = &t;
      if (track) track->Events.push_back({p, start});
    }
  }

  if (major == 2 && minor == 1 || major > 2) {
    const std::vector<uint8_t> text = r.block();
    if (r.ok) Comment = cstring((const char*)text.data(), text.size());
  }
  if (!Master) return "the project has no Master";
  return "";
}

// ---------------------------------------------------------------- playing

// CMachineContainer::Mix: out += in * powerRatio
static void Mix(int samples, const double* in, double* out, double powerRatio) {
  while ((samples--) > 0) *(out++) += *(in++) * powerRatio;
}

void Container::PrepareToWork() {
  for (auto& k : Connectors) {
    CPort* port = k->Port;
    if ((port->Type & PT_INPUT) && (port->Type & PT_CONTROL)) port->ControlBuffer->Clear();
  }
  WorkDone = false;
}

// the sound of the machines that play into an input, mixed into it (CMachineContainer::Work)
static void MixInto(Connector* connector) {
  CAudioBuffer* in = connector->Port->AudioBuffer;
  for (Connection* n : connector->Connections) {
    CAudioBuffer* out = n->Input->Port->AudioBuffer;
    const double powerRatio = n->PowerRatio;
    double follow = n->PowerRatioFollow;
    const int samples = std::min(in->Samples, out->Samples);
    if (in->Stereo) {
      if (out->Stereo) {
        if (follow == powerRatio) {
          Mix(samples, out->LeftData, in->LeftData, powerRatio);
          Mix(samples, out->RightData, in->RightData, powerRatio);
        } else
          for (int l = 0; l < samples; l++) {
            follow = AdjustVolume(powerRatio, follow);
            in->LeftData[l] += out->LeftData[l] * follow;
            in->RightData[l] += out->RightData[l] * follow;
          }
      } else {
        if (follow == powerRatio) {
          Mix(samples, out->MonoData, in->LeftData, powerRatio);
          Mix(samples, out->MonoData, in->RightData, powerRatio);
        } else
          for (int l = 0; l < samples; l++) {
            follow = AdjustVolume(powerRatio, follow);
            const double v = out->MonoData[l] * follow;
            in->LeftData[l] += v;
            in->RightData[l] += v;
          }
      }
    } else {
      if (out->Stereo) {
        if (follow == powerRatio) {
          Mix(samples, out->LeftData, in->MonoData, powerRatio * 0.5);
          Mix(samples, out->RightData, in->MonoData, powerRatio * 0.5);
        } else
          for (int l = 0; l < samples; l++) {
            follow = AdjustVolume(powerRatio, follow);
            in->MonoData[l] += (out->LeftData[l] + out->RightData[l]) * 0.5 * follow;
          }
      } else {
        if (follow == powerRatio)
          Mix(samples, out->MonoData, in->MonoData, powerRatio);
        else
          for (int l = 0; l < samples; l++) {
            follow = AdjustVolume(powerRatio, follow);
            in->MonoData[l] += out->MonoData[l] * follow;
          }
      }
    }
    n->PowerRatioFollow = follow;
  }
}

static void ClearAudio(CAudioBuffer* b) {
  for (int k = 0; k < b->Samples; k++) {
    b->LeftData[k] = 0.0;
    if (b->Stereo) b->RightData[k] = 0.0;
  }
}

void Container::Work(int blockSamples) {
  if (WorkDone) return;
  WorkDone = true;
  bool hasAUX = false;
  // the machines that play into this one first
  for (auto& k : Connectors) {
    if (!(k->Port->Type & PT_INPUT)) continue;
    if (k->Port->Type & PT_AUX)
      hasAUX = true;
    else
      for (Connection* n : k->Connections) n->Input->Owner->Work(blockSamples);
  }
  for (auto& k : Connectors) {
    CPort* port = k->Port;
    if ((port->Type & PT_OUTPUT) && (port->Type & PT_CONTROL)) port->ControlBuffer->Clear();
    if (!(port->Type & PT_INPUT)) continue;
    if (port->Type & PT_CONTROL)
      for (Connection* n : k->Connections) n->Input->Port->ControlBuffer->CopyTo(port->ControlBuffer);
    if (port->Type & PT_AUX)
      hasAUX = true;
    else if (port->Type & PT_AUDIO) {
      ClearAudio(port->AudioBuffer);
      MixInto(k.get());
    }
  }
  for (auto& k : Connectors)
    if ((k->Port->Type & PT_INPUT) && (k->Port->Type & PT_CONTROL))
      k->Future->MoveFuture(k->Port->ControlBuffer, blockSamples);

  Machine->Work();

  if (hasAUX) {
    // the sends: the machines on them now, and what they give back into the AUX inputs,
    // added to what is there (as the studio does, the AUX inputs are not cleared first)
    for (auto& k : Connectors)
      if ((k->Port->Type & PT_INPUT) && (k->Port->Type & PT_AUX))
        for (Connection* n : k->Connections) n->Input->Owner->Work(blockSamples);
    for (auto& k : Connectors)
      if ((k->Port->Type & PT_AUX) && (k->Port->Type & PT_AUDIO) && (k->Port->Type & PT_INPUT)) MixInto(k.get());
    Machine->WorkAUX();
  }
}

void Pattern::ProcessBlock(Host& host, int offset, int startSample, int samples, double samplesPerTick) {
  // CPattern::ProcessBlock
  const bool voiceMode = Port->Owner->Machine->VoiceCount > 0;
  CControlBuffer* buffer = Port->Port->ControlBuffer;
  SControlMessage m;
  std::memset(&m, 0, sizeof m);
  for (int pos = 0; pos < Length * 4; pos++) {
    const int sample = (int)((double)pos * samplesPerTick);
    if (sample >= startSample && sample < startSample + samples) {
      for (auto& track : ControlTracks) {
        if (track.Property == DW_PROPERTY_VELOCITY) continue;
        const ControlEntry& entry = track.Entries[pos];
        if (entry.Used) {
          m.BlockSample = offset + sample - startSample;
          m.Size = sizeof(m);
          m.Type = CONTROL_MESSAGE_CONTROL_CHANGE;
          m.ControlChange.Controller = track.Property;
          m.ControlChange.Value = entry.Value;
          buffer->Write(&m);
        }
      }
    }
  }
  for (const Note& note : Notes) {
    int sample = (int)((double)note.Start * samplesPerTick);
    if (sample >= startSample && sample < startSample + samples) {
      m.BlockSample = offset + sample - startSample;
      m.Size = sizeof(m);
      double velocity = ((1.0 / 127.0) * 100.0);   // DEFAULT_VELOCITY
      if (VelocityTrack >= 0 && note.Start >= 0 && note.Start < (int)ControlTracks[VelocityTrack].Entries.size())
        velocity = ControlTracks[VelocityTrack].Entries[note.Start].Value;
      if (voiceMode) {
        m.Type = CONTROL_MESSAGE_VOICE_ON;
        m.VoiceOn.Voice = note.Value;
        m.VoiceOn.Velocity = velocity;
      } else {
        m.Type = CONTROL_MESSAGE_NOTE_ON;
        m.NoteOn.Note = (unsigned char)note.Value;
        host.GetNoteFrequency(m.NoteOn.Note, &m.NoteOn.Frequency);
        m.NoteOn.Velocity = velocity;
      }
      buffer->Write(&m);

      sample = (int)((double)(note.Start + note.Length) * samplesPerTick);
      m.BlockSample = (offset + sample - startSample) - 1;
      m.Size = sizeof(m);
      if (voiceMode) {
        m.Type = CONTROL_MESSAGE_VOICE_OFF;
        m.VoiceOff.Voice = note.Value;
      } else {
        m.Type = CONTROL_MESSAGE_NOTE_OFF;
        m.NoteOff.Note = (unsigned char)note.Value;
      }
      if (m.BlockSample >= host.GetBlockSamples())
        Port->Future->Write(&m);
      else
        buffer->Write(&m);
    }
  }
}

void Studio::SequencerWork() {
  // CSequenceEditor::Work, as it is: the last block of the song is gone through twice
  if (!Playing) return;
  const double samplesPerTick = TheHost.GetSamplesPerTick();
  int blockSamples = TheHost.GetBlockSamples();
  int offset = 0;
  const double sl4 = SequenceLength * 4.0;
  while (blockSamples) {
    const double offsetInc = blockSamples / samplesPerTick;
    const int s = std::min(blockSamples, std::max(0, (int)(((PlayOffset + offsetInc) - sl4) * samplesPerTick)));
    const int samples = blockSamples;
    for (auto& track : Tracks)
      for (auto& ev : track.Events) {
        const double es = (double)(ev.Start * 4);
        const double ef = (double)((ev.Start + ev.P->Length) * 4);
        if ((PlayOffset >= es && PlayOffset <= ef) || (PlayOffset < es && PlayOffset + offsetInc > es)) {
          const int startSample = (int)((PlayOffset - es) * samplesPerTick);
          ev.P->ProcessBlock(TheHost, offset, startSample, samples, samplesPerTick);
        }
      }
    if (s) {
      if (Passes > 1) {   // the Loop button: the rest of the block from the start again
        Passes--;
        offset += blockSamples - s;
        blockSamples = s;
        if (blockSamples < 0) blockSamples = 0;
        PlayOffset = 0.0;
      } else {
        blockSamples -= s;
        if (blockSamples <= 0) {
          blockSamples = 0;
          Playing = false;
        }
      }
    } else {
      PlayOffset += offsetInc;
      blockSamples = 0;
    }
  }
}

void Studio::Idle(int blocks, double* left, double* right) {
  Playing = false;
  while (blocks-- > 0) Work(left, right);
  Playing = true;
  PlayOffset = 0.0;
}

bool Studio::Work(double* left, double* right) {
  // CStudio::Work
  const int samples = TheHost.GetBlockSamples();
  const double volume = 1.0;   // the master volume of a new installation
  double workingVolume = WorkingVolume;
  for (auto& c : Containers) c->PrepareToWork();
  const bool wasPlaying = Playing;
  SequencerWork();
  Master->Work(samples);
  TheHost.SamplePosition += samples;
  CAudioBuffer* out = Master->Machine->Ports[0]->AudioBuffer;
  for (int i = 0; i < samples; i++) {
    left[i] = out->LeftData[i] * workingVolume;
    right[i] = out->RightData[i] * workingVolume;
    if (workingVolume != volume) {
      if (workingVolume < volume) {
        workingVolume += (0.1f + (volume - workingVolume)) * VOLUME_VELOCITY;
        if (workingVolume > volume) workingVolume = volume;
      } else {
        workingVolume -= (0.1f + (workingVolume - volume)) * VOLUME_VELOCITY;
        if (workingVolume < volume) workingVolume = volume;
      }
    }
  }
  for (auto& c : Containers)
    if (!c->WorkDone) c->Work(samples);
  WorkingVolume = workingVolume;
  return wasPlaying;
}

}  // namespace darkwave
