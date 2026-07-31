// Fixture covering the migration script's "manual review" patterns -- none
// of these are rewritten; test_migrate.py asserts the script reports each
// one with the right line number and rule id.
#include <cppy3/cppy3.hpp>

void example()
{
  cppy3::Var mainNs(cppy3::Main());
  PyObject *mainModule = cppy3::getMainModule();

  cppy3::Var obj = cppy3::lookupObject(mainModule, L"a.b");
  cppy3::Var fn = cppy3::lookupCallable(mainModule, L"f");

  cppy3::arguments args;
  PyObject *result = cppy3::call(fn.data(), args);

  cppy3::Var instance = cppy3::createClassInstance(L"MyClass");

  std::list<std::wstring> argv;
  cppy3::setArgv(argv);

  cppy3::NDArray<double> nd;
  nd.wrap(nullptr, 0);
  nd.dim1();
  nd.dim2();

  assert(cppy3::error());
}
