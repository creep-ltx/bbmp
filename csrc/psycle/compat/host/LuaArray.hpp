// SPDX-License-Identifier: GPL-3.0-or-later
// Of the arrays of Psycle's Lua machines, the player uses one thing: a buffer of
// samples for the scopes that nobody looks at here.
#pragma once
#include <vector>
namespace psycle { namespace host {
const int SCOPE_BUF_SIZE = 4096;
class PSArray {
 public:
  PSArray() {}
  PSArray(int len, float value) : v_(len > 0 ? len : 0, value) {}
  int len() const { return static_cast<int>(v_.size()); }
  float* data() { return v_.data(); }
  void set_val(int i, float value) { v_[i] = value; }
  float get_val(int i) const { return v_[i]; }
 private:
  std::vector<float> v_;
};
}}
