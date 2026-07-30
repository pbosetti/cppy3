#include <cppy3/cppy3.hpp>

#define INCLUDED_FROM_CPPY3_NUMPY_CPP
#include <cppy3/cppy3_numpy.hpp>
#undef INCLUDED_FROM_CPPY3_NUMPY_CPP

namespace cppy3
{
  namespace
  {
    // workaround numpy & python3 https://github.com/boostorg/python/issues/214
    // return NULL to avoid UB https://wanzenbug.xyz/boost-numpy/
    void *wrap_import_array()
    {
      import_array();
      return nullptr;
    }

    bool is_ndarray(PyObject *o) { return PyArray_Check(o); }
  }

  void importNumpy()
  {
    static bool imported = false;
    if (!imported)
    {
      // @todo double-lock-singleton pattern against multithreaded race condition
      imported = true;
      wrap_import_array();
      throw_if_error();
      register_is_ndarray_hook(is_ndarray);
    }
  }
}
