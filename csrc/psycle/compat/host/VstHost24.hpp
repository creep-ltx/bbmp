// SPDX-License-Identifier: GPL-3.0-or-later
// No VST plugins here. What is kept of Psycle's VST host is what the song loaders
// need to read past a VST machine in a file; a machine that is one becomes a Dummy.
#pragma once
#include <psycle/host/detail/project.hpp>
#include <psycle/host/Global.hpp>
#include <psycle/host/Machine.hpp>
#include <seib-vsthost/CVSTHost.Seib.hpp>
#include <cstring>
#include <string>

struct AEffect;

namespace psycle { namespace host { namespace vst {

class Plugin;

class Host : public seib::vst::CVSTHost {
 public:
  Host() { pHost = this; }
  Machine* LoadPlugin(const char*, int = 0) { return 0; }
};

class Plugin : public Machine {
 public:
  explicit Plugin(AEffect*) { _nCols = 0; _type = MACH_VST; InitializeSamplesVector(); }
  virtual const char* const GetName(void) const { return "VST"; }
  virtual const char* const GetDllName() const throw() { return ""; }
  bool ProgramIsChunk() const { return false; }
  void SetProgram(int) {}
  virtual void SetParameter(int, float) {}
  using Machine::SetParameter;

  // the machine as PSY2 files have it, up to where the VST's own data begins
  bool PreLoad(RiffFile* file, unsigned char& program, int& instance) {
    Machine::Init();
    file->Read(&_editName, 16);
    _editName[15] = '\0';
    legacyWires.resize(MAX_CONNECTIONS);
    for (int i = 0; i < MAX_CONNECTIONS; ++i) file->Read(&legacyWires[i]._inputMachine, sizeof legacyWires[i]._inputMachine);
    for (int i = 0; i < MAX_CONNECTIONS; ++i) file->Read(&legacyWires[i]._outputMachine, sizeof legacyWires[i]._outputMachine);
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
      file->Read(&legacyWires[i]._inputConVol, sizeof legacyWires[i]._inputConVol);
      legacyWires[i]._wireMultiplier = 1.0f;
    }
    for (int i = 0; i < MAX_CONNECTIONS; ++i) file->Read(&legacyWires[i]._connection, sizeof legacyWires[i]._connection);
    for (int i = 0; i < MAX_CONNECTIONS; ++i) file->Read(&legacyWires[i]._inputCon, sizeof legacyWires[i]._inputCon);
    file->Read(&_connectionPoint[0], sizeof _connectionPoint);
    file->Skip(2 * sizeof(int));
    file->Read(&_panning, sizeof _panning);
    Machine::SetPan(_panning);
    file->Skip(109);
    bool old;
    file->Read(&old, sizeof old);
    file->Read(&instance, sizeof instance);
    if (old) {
      char channel;
      file->Read(&channel, sizeof channel);
      program = 0;
    } else {
      file->Read(&program, sizeof program);
    }
    return true;
  }
  bool LoadFromMac(Plugin*) { return false; }
  bool LoadChunk(RiffFile*) { return false; }
};

}}}
