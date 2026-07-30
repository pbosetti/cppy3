#include <cppy3/cppy3.hpp>
#include <iostream>

// Bug #1 (plan Part 1): cppy3::Var declares a copy constructor and destructor
// but no operator=. The compiler-generated copy-assignment memberwise-copies
// the raw PyObject* without touching refcounts, so `b = a;` never increfs the
// object on b's behalf while both a's and b's destructors still decref it.
//
// keepAlive holds an independent owning reference for the whole test so the
// object survives the bug and Py_REFCNT() can be read safely afterward.
int main()
{
  cppy3::PythonVM vm;

  // NOTE: must be outside CPython's small-int cache (-5..256): those ints are
  // immortal since 3.12 and Py_INCREF/Py_DECREF on them are no-ops, which
  // would mask this bug entirely.
  PyObject *obj = PyLong_FromLong(987654321); // refcnt 1 (unowned by any Var yet)
  cppy3::Var keepAlive(obj);            // refcnt 2
  const Py_ssize_t before = Py_REFCNT(obj);

  {
    cppy3::Var a(obj); // refcnt 3
    cppy3::Var b;
    b = a; // BUG: should incref for b's new ownership; does not
  }        // ~b decref (3->2), ~a decref (2->1): one decref too many

  const Py_ssize_t after = Py_REFCNT(obj);
  std::cout << "bug01: before=" << before << " after=" << after << std::endl;

  if (after != before)
  {
    std::cerr << "bug01: CONFIRMED - Var::operator= corrupts refcount ("
               << before << " -> " << after << ")" << std::endl;
    return 1;
  }
  std::cout << "bug01: not reproduced (already fixed?)" << std::endl;
  return 0;
}
