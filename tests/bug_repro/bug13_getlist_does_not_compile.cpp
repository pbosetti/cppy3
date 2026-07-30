#include <cppy3/cppy3.hpp>

// Bug #13 (plan Part 1): extract(PyObject*, std::vector<T>&) calls
// `value.reset(0)` -- std::vector has no `reset` member, so Var::getList<T>
// (and this extract<T> overload) fails to compile the moment it is
// instantiated. This target is EXCLUDE_FROM_ALL and is not part of the
// normal build or ctest -- build it explicitly to observe the error:
//   cmake --build . --target bug13_getlist_does_not_compile
int main()
{
  cppy3::PythonVM vm;
  cppy3::Main main;
  std::vector<int> values;
  main.getList<int>(L"v", values); // instantiates extract(PyObject*, std::vector<int>&) -> compile error
  return 0;
}
