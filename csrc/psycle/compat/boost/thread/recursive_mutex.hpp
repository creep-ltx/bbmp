// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
namespace boost {
using std::thread; using std::mutex; using std::recursive_mutex; using std::lock_guard;
using std::unique_lock; using std::condition_variable; using std::once_flag; using std::call_once;
}
