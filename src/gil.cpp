#include <cppy3/gil.hpp>

namespace cppy3
{
  bool gil_held() noexcept
  {
    return PyGILState_Check() == 1;
  }

  GilLock::GilLock()
  {
    _state = PyGILState_Ensure();
  }

  GilLock::~GilLock()
  {
    PyGILState_Release(_state);
  }

  GilRelease::GilRelease()
  {
    if (gil_held())
      _threadState = PyEval_SaveThread();
  }

  GilRelease::~GilRelease()
  {
    if (_threadState)
      PyEval_RestoreThread(_threadState);
  }
}
