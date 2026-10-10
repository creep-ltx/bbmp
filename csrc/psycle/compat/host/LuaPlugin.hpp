// SPDX-License-Identifier: GPL-3.0-or-later
// No Lua machines here: making one fails, and the song gets a Dummy in its place.
#pragma once
#include <psycle/host/detail/project.hpp>
#include <psycle/host/Machine.hpp>
#include <stdexcept>
namespace psycle { namespace host {
const int AUTOID = -1;
class LuaPlugin : public Machine {
 public:
  LuaPlugin(const char*, int, bool = true) { throw std::runtime_error("Lua machines are not part of this player"); }
  virtual const char* const GetName(void) const { return "Lua"; }
};
}}
