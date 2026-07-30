#include <iostream>

#include <cppy3/cppy3.hpp>

// Demonstrates make_function(): exposing a C++ callable to Python without
// hand-writing a PyMethodDef/PyCFunction table (compare to console.cpp's
// "emb" module, which still needs one for its METH_KEYWORDS argument).
int main()
{
  cppy3::Interpreter interpreter;
  cppy3::Namespace ns = interpreter.main();

  int callCount = 0;
  ns.set("cpp_log", cppy3::make_function("cpp_log", [&callCount](std::string message) -> void {
                       callCount++;
                       std::cout << "[C++] " << message << std::endl;
                     }));

  ns.exec(
      "cpp_log('hello from Python')\n"
      "for i in range(3):\n"
      "    cpp_log(f'iteration {i}')\n");

  std::cout << "cpp_log was called " << callCount << " times" << std::endl;
  return 0;
}
