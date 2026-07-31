// Fixture covering the migration script's "auto" and "assisted" rules.
// See ../v2_expected/basic.cpp for the expected rewrite; test_migrate.py
// asserts the script actually produces it.
#include <cppy3/cppy3.hpp>

void example(PyObject *somePyObject)
{
  cppy3::Interpreter instance;

  /*YOUR_NAMESPACE*/.exec("a = 1");
  /*YOUR_NAMESPACE*/.eval("a + 1");
  /*YOUR_NAMESPACE*/.exec_file("script.py");

  /*YOUR_NAMESPACE*/.set("a", 2);
  /*YOUR_NAMESPACE*/.set("b", somePyObject);

  long outValue;
  outValue = /*YOUR_NAMESPACE*/.get<long>("a");

  PyObject *mainDict = /*YOUR_NAMESPACE*/.dict().get();

  cppy3::Var result;
  result = Var::steal(/*YOUR_NAMESPACE*/.eval("a").data());
  result = Var::borrow(somePyObject);

  if (cppy3::gil_held())
  {
    cppy3::GilLock locker;
    cppy3::GilLock lock2;
    cppy3::GilRelease release;
  }

  try
  {
    /*YOUR_NAMESPACE*/.exec("raise Exception('x')");
  }
  catch (const cppy3::Error &e)
  {
    auto t = e.type_name();
    auto r = e.message();
    auto trace = e.traceback();
  }

  std::string s = result.str();
  long l = result.to<long>();
  double d = result.to<double>();
  const char *tn = result.type_name();

  cppy3::Var v = cppy3::Var::steal(somePyObject);
}
