#include <filesystem>
#include <iostream>

#include <cppy3/cppy3.hpp>

// Demonstrates pointing the embedded interpreter at a virtualenv via
// Config::executable. Since CPython 3.12 removed Py_SetPythonHome(), this
// is the only way left to embed against a venv instead of the interpreter
// this program happened to link against.
//
// Config::executable, not Config::home: a venv directory does not bundle
// its own copy of the standard library (only site-packages) -- it relies
// on its pyvenv.cfg's "home" key to point back at the base installation
// that actually has it. Pointing config.executable at the venv's own
// python binary makes CPython follow that pyvenv.cfg, exactly as it would
// if you'd run <venv>/bin/python directly, picking up both the venv's
// site-packages and the base installation's stdlib. Pointing config.home
// at the venv directory itself skips that lookup and fails to find even
// the "encodings" module.
int main(int argc, char *argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0] << " <path-to-venv>\n"
              << "Create one first, e.g.:\n"
              << "  python3 -m venv /tmp/demo-venv\n"
              << "  /tmp/demo-venv/bin/pip install requests\n"
              << "  " << argv[0] << " /tmp/demo-venv\n";
    return 1;
  }

  cppy3::Config config;
  config.executable = std::filesystem::path(argv[1]) / "bin" / "python3";

  cppy3::Interpreter interpreter(config);
  cppy3::Namespace ns = interpreter.main();

  ns.exec("import sys");
  std::cout << "sys.prefix:     " << ns.eval("sys.prefix").str() << std::endl;
  std::cout << "sys.executable: " << ns.eval("sys.executable").str() << std::endl;

  // succeeds only if a package was pip-installed into that venv
  try
  {
    ns.exec("import requests");
    std::cout << "requests " << ns.eval("requests.__version__").str() << " imported from the venv" << std::endl;
  }
  catch (const cppy3::Error &e)
  {
    std::cout << "(import requests failed -- pip install requests into the venv to see this succeed)\n"
              << e.format() << std::endl;
  }

  return 0;
}
