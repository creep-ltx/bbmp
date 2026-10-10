// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// one thread only: nothing to lock
namespace Common {
class Mutex {};
class StackLock {
public:
	explicit StackLock(Mutex &) {}
};
}
