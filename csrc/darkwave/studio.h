/* SPDX-License-Identifier: GPL-3.0-or-later */
/*  bbmp-darkwave: DarkWave Studio's studio without its windows: a project's machines,
    their cables, patterns and sequence, and the loop that makes the sound.

    Ported from ExperimentalScene DarkWave Studio 4.1.6 (Studio/Core.cpp, Studio.cpp,
    MachineContainer.cpp, Connector.cpp, Pattern.cpp, SequenceEditor.cpp, PatternEditor.cpp,
    Host.cpp, Master.cpp), Copyright (C) 2004-2012 Daniel Werner,
    http://www.experimentalscene.com/ ; changed for bbmp in 2026 (no windows, no audio
    devices, no VST, and a damaged file can't make it read outside the file).

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
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "DarkPlug.h"

namespace darkwave {

#define DW_MAX_NOTE 127

class Host : public CHost {
 public:
  Host(int sampleRate, int blockSamples);
  EError DARKCALL RegisterMachineInfo(CMachineInfo* machineInfo) override;
  EError DARKCALL GetNoteFrequency(unsigned int note, double* frequency) override;
  EError DARKCALL GetBankServer(CBankServer** bankServer) override;
  void DARKCALL SetPropertyAutomated(CMachine* machine, int index, double value) override;
  int DARKCALL GetSamplePosition() override { return SamplePosition; }
  double DARKCALL GetBPM() override { return BPM; }
  double DARKCALL GetTPB() override { return TPB; }
  int DARKCALL GetSamplesPerTick() override;
  int DARKCALL GetSampleRate() override { return SampleRate; }
  int DARKCALL GetBlockSamples() override { return BlockSamples; }
  bool DARKCALL Supports(ESupports code) override { return false; }
  int DARKCALL GetCPUFeatures() override;

  double BPM = 140.0, TPB = 16.0;
  int SampleRate, BlockSamples, SamplePosition = 0;
  std::vector<CMachineInfo*> Machines;   // what the studio can make, as registered

 private:
  double NoteTable[DW_MAX_NOTE + 1];
};

struct Container;
struct Connection;

struct Connector {
  CPort* Port = nullptr;
  Container* Owner = nullptr;
  std::vector<Connection*> Connections;   // cables into this input: from Connection::Input
  std::unique_ptr<CControlBuffer> Future;  // control input: messages for later blocks
};

struct Connection {
  Connector* Input;    // where the sound comes from (an output port)
  Connector* Output;   // where it goes (this input port)
  double PowerRatio, PowerRatioFollow;
};

struct Container {
  std::string Name, UniqueID;
  CMachine* Machine = nullptr;
  std::vector<std::unique_ptr<Connector>> Connectors;
  bool WorkDone = true;
  void PrepareToWork();
  void Work(int blockSamples);
};

struct Note {
  int Start, Length, Value;
};

struct ControlEntry {
  bool Used;
  double Value;
};

struct ControlTrack {
  int Property;
  std::vector<ControlEntry> Entries;
};

#define DW_PROPERTY_VELOCITY (-1)

struct Pattern {
  int Length = 0, Index = 0;
  std::string Name;
  Connector* Port = nullptr;
  std::vector<Note> Notes;
  std::vector<ControlTrack> ControlTracks;
  int VelocityTrack = -1;
  void ProcessBlock(Host& host, int offset, int startSample, int samples, double samplesPerTick);
};

struct SequenceEvent {
  Pattern* P;
  int Start;
};

struct SequenceTrack {
  Connector* Port;
  std::vector<SequenceEvent> Events;
};

class Studio {
 public:
  Studio(int sampleRate, int blockSamples);
  ~Studio();
  // DarkWave Studio's CCore::ReadProject; "" or why the project can't be played
  std::string Load(const std::vector<uint8_t>& file);
  // one block of BlockSamples stereo frames into left/right; false once the sequence has
  // ended (the machines still sound after that: their echoes and releases)
  bool Work(double* left, double* right);
  // blocks of the studio with the sequence stopped, as before Play is pressed
  void Idle(int blocks, double* left, double* right);

  Host TheHost;
  int MajorVersion = 0, MinorVersion = 0;
  int SequenceLength = 0;          // in steps of 4 ticks
  std::string Comment;
  std::vector<std::string> MissingMachines;   // machines of the project this studio hasn't
  std::vector<std::string> MachineNames;      // "instance (machine)" of what it has
  long long SongSamples() const;   // the sequence's length in samples
  int Passes = 1;                  // times the sequence is played (the studio's Loop button)

 private:
  Container* AddMachine(const std::string& instanceName, const std::string& machineName,
                        const std::string& uniqueID, const char* properties, int propertyBytes,
                        bool oldUniqueID);
  Connector* GetConnector(int machine, int connector);
  void ConnectConnectors(int m1, int c1, int m2, int c2, double volume);
  void SequencerWork();

  std::vector<std::unique_ptr<Container>> Containers;
  std::vector<int> ContainerIndex;          // Machine->Index of each container
  Container* Master = nullptr;
  std::vector<std::unique_ptr<Pattern>> Patterns;
  std::vector<SequenceTrack> Tracks;
  std::vector<std::unique_ptr<CMachineInfo>> Infos;
  bool Playing = true;
  double PlayOffset = 0.0;
  // the studio fades its output in from 0 when the program starts; by the time a song is
  // played that is long over
  double WorkingVolume = 1.0;
};

}  // namespace darkwave
