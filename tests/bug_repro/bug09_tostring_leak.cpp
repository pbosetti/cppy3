#include <cppy3/cppy3.hpp>
#include <iostream>

// Bug #9 (plan Part 1): Var::toString(PyObject*) calls PyObject_Str(val) (or
// PyObject_Repr as fallback) and never decrefs the result -- every call to
// toString()/toUTF8String() leaks one new string object. Detected via
// sys.getallocatedblocks(), since these are pymalloc allocations invisible
// to ASan's malloc interceptor.
//
// NOTE: must call toString() on something whose str() allocates a genuinely
// NEW object each time. A `str` instance won't do -- CPython special-cases
// str.__str__ to return `self` (just an incref, no new block), so the leak
// would still grow refcounts unboundedly but never move
// sys.getallocatedblocks(). An int's __str__ always builds a fresh string.
int main()
{
  cppy3::PythonVM vm;
  cppy3::exec("import sys");

  const cppy3::Var val = cppy3::eval("123456789");

  const long before = cppy3::eval("sys.getallocatedblocks()").toLong();
  const int N = 20000;
  for (int i = 0; i < N; i++)
  {
    val.toString();
  }
  const long after = cppy3::eval("sys.getallocatedblocks()").toLong();
  const long delta = after - before;

  std::cout << "bug09: getallocatedblocks before=" << before << " after=" << after
            << " delta=" << delta << " over " << N << " toString() calls" << std::endl;

  // a non-leaking implementation should settle to a small, bounded delta
  // (interpreter bookkeeping noise), nowhere near one leaked block per call.
  if (delta > N / 2)
  {
    std::cerr << "bug09: CONFIRMED - toString() leaks approximately one object per call" << std::endl;
    return 1;
  }
  std::cout << "bug09: not reproduced (already fixed?)" << std::endl;
  return 0;
}
