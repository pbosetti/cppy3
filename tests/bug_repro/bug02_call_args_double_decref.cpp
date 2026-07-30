#include <cppy3/cppy3.hpp>
#include <iostream>

// Bug #2 (plan Part 1): cppy3::call() builds a PyTuple with PyTuple_SetItem,
// which STEALS the reference passed to it. call() steals from `args[i]`
// (a borrowed PyObject* via Var::operator PyObject*()) without incref'ing
// first, so the tuple and the caller's `arguments` vector both end up
// believing they own the same reference. That is a latent one-unit deficit
// that surfaces as an extra decref once every legitimate owner unwinds --
// here, a genuine double-free / use-after-free once argVar and result (both
// still alive after this function returns to its own caller) are destroyed.
//
// Ledger (f(x): return x is the identity, so result IS arg):
//   create                    refcnt=1  (owner: none yet, latent)
//   argVar.newRef(arg)        refcnt=1  (owner: argVar)
//   args.push_back(argVar)    refcnt=2  (owners: argVar, args[0])
//   call()'s PyTuple_SetItem  refcnt=2  (owners: argVar, args[0] *and* argsTuple -- 3 claims, 2 units: deficit)
//   PyObject_CallObject       refcnt=3  (+1 for the returned reference; owners: argVar, args[0]/argsTuple, result)
//   ~argsTuple (call returns) refcnt=2  (owners: argVar, args[0], result -- 3 claims, 2 units: deficit persists)
//   ~args (vector goes out)   refcnt=1  (owners: argVar, result -- 2 claims, 1 unit: deficit persists)
// So immediately after the `args` vector is destroyed, refcnt should be 2
// (matching its 2 live owners, argVar and result) if call() were correct,
// but is actually 1.
int main()
{
  cppy3::PythonVM vm;
  cppy3::exec("def f(x): return x");

  // must be outside the immortal small-int range (-5..256), see bug01.
  PyObject *arg = PyLong_FromLong(987654322); // refcnt 1
  cppy3::Var argVar;
  argVar.newRef(arg); // owns it, refcnt still 1

  cppy3::Var result;
  {
    cppy3::arguments args;
    args.push_back(argVar); // copy ctor increfs -> refcnt 2 (argVar + args[0])

    result.newRef(cppy3::call(cppy3::lookupCallable(cppy3::getMainModule(), L"f"), args));
    // `args` destructs here (end of block).
  }

  const Py_ssize_t refcnt = Py_REFCNT(arg);
  std::cout << "bug02: refcnt after args vector destructs = " << refcnt
            << " (argVar and result are both still alive; correct value is 2)" << std::endl;

  if (refcnt != 2)
  {
    std::cerr << "bug02: CONFIRMED - call() double-decrefs its arguments (refcnt=" << refcnt << ", expected 2)."
               << " argVar and result will each decref once more at scope exit -> use-after-free / double-free." << std::endl;
    return 1;
  }
  std::cout << "bug02: not reproduced (already fixed?)" << std::endl;
  return 0;
}
