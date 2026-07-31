#pragma once

#include <cppy3/pycompat.hpp>

#include <cppy3/libdefs.hpp>

namespace cppy3
{
  // True if the calling thread currently holds the GIL.
  LIB_API bool gil_held() noexcept;

  // Recursive scoped GIL acquisition -- safe to nest arbitrarily, since it
  // is a thin wrapper over PyGILState_Ensure()/Release(), which are
  // explicitly designed to nest. Merges v1's GILLocker and ScopedGILLock
  // (which were two names for the same thing) into one type.
  class LIB_API GilLock
  {
  public:
    GilLock();
    ~GilLock();
    GilLock(const GilLock &) = delete;
    GilLock &operator=(const GilLock &) = delete;

  private:
    PyGILState_STATE _state;
  };

  // Releases the GIL for the scope's duration so another thread can run,
  // e.g. around a long-running non-Python computation. A no-op if the
  // calling thread does not currently hold the GIL: v1's ScopedGILRelease
  // called PyEval_SaveThread() unconditionally, which is undefined
  // behavior (typically a crash) if the GIL isn't held (bug #29).
  class LIB_API GilRelease
  {
  public:
    GilRelease();
    ~GilRelease();
    GilRelease(const GilRelease &) = delete;
    GilRelease &operator=(const GilRelease &) = delete;

  private:
    PyThreadState *_threadState = nullptr;
  };
}
