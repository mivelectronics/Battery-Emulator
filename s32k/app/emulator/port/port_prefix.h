/*
 * Force-included before every upstream source of the core.
 *
 * The toolchain's libstdc++ is built without threads, so it has no std::mutex. eModBus only takes
 * locks around its own server tasks, and those do not run here (RS485 inverters are not ported yet).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef __cplusplus
#include <mutex>
#ifndef _GLIBCXX_HAS_GTHREADS
namespace std {
class mutex {
 public:
  void lock() {}
  void unlock() {}
  bool try_lock() { return true; }
};
}  // namespace std
#endif
#endif
