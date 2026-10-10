// SPDX-License-Identifier: GPL-3.0-or-later
// No LADSPA machines here.
#pragma once
#include <psycle/host/detail/project.hpp>
#include <psycle/host/Machine.hpp>
namespace psycle { namespace host {
class LADSPAMachine : public Machine {
 public:
  virtual const char* const GetName(void) const { return "LADSPA"; }
};
}}
