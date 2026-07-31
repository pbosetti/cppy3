// Fixture covering the migration script's "auto" and "assisted" rules.
// See ../v2_expected/basic.cpp for the expected rewrite; test_migrate.py
// asserts the script actually produces it.
#include <cppy3/cppy3.hpp>

void example(PyObject *somePyObject)
{
  cppy3::PythonVM instance;

  cppy3::exec("a = 1");
  cppy3::eval("a + 1");
  cppy3::execScriptFile("script.py");

  cppy3::Main().injectVar<int>("a", 2);
  cppy3::Main().inject("b", somePyObject);

  long outValue;
  cppy3::Main().getVar<long>("a", outValue);

  PyObject *mainDict = cppy3::getMainDict();

  cppy3::Var result;
  result.newRef(cppy3::eval("a").data());
  result.reset(somePyObject);

  if (cppy3::GILLocker::isLocked())
  {
    cppy3::GILLocker locker;
    cppy3::ScopedGILLock lock2;
    cppy3::ScopedGILRelease release;
  }

  try
  {
    cppy3::exec("raise Exception('x')");
  }
  catch (const cppy3::PythonException &e)
  {
    auto t = e.info.type;
    auto r = e.info.reason;
    auto trace = e.info.trace;
  }

  std::string s = result.toUTF8String();
  long l = result.toLong();
  double d = result.toDouble();
  const char *tn = result.typeName();

  cppy3::Var v = cppy3::Var::from(somePyObject);
}
