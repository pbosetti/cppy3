#include <cppy3/cppy3.hpp>
#include <iostream>

// Bug #11 (plan Part 1): Var::injectVar<T>() converts T to a new PyObject*
// via convert(), then calls inject() -> PyDict_SetItemString, which takes
// its OWN reference. The object convert() returned is never decref'd, so
// every injectVar() call leaks one object. Detected via
// sys.getallocatedblocks(), which sees pymalloc allocations that ASan's
// malloc interceptor does not.
int main()
{
  cppy3::PythonVM vm;
  cppy3::exec("import sys");
  cppy3::Main main;

  const long before = cppy3::eval("sys.getallocatedblocks()").toLong();
  const int N = 20000;
  for (int i = 0; i < N; i++)
  {
    // value must be outside the immortal small-int cache (-5..256), see
    // bug01, or CPython won't allocate a new object per call at all.
    main.injectVar<int>("v", 987654321 + i);
  }
  const long after = cppy3::eval("sys.getallocatedblocks()").toLong();
  const long delta = after - before;

  std::cout << "bug11: getallocatedblocks before=" << before << " after=" << after
            << " delta=" << delta << " over " << N << " injectVar() calls" << std::endl;

  if (delta > N / 2)
  {
    std::cerr << "bug11: CONFIRMED - injectVar() leaks approximately one object per call" << std::endl;
    return 1;
  }
  std::cout << "bug11: not reproduced (already fixed?)" << std::endl;
  return 0;
}
