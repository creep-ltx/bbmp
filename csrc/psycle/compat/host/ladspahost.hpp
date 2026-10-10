// SPDX-License-Identifier: GPL-3.0-or-later
// No LADSPA machines here: loading one gives nothing, and the song gets a Dummy.
#pragma once
#include "ladspamachine.h"
#include <string>
namespace psycle { namespace host {
struct LadspaHost {
  static Machine* LoadPlugin(const std::string&, int, int) { return 0; }
};
}}
