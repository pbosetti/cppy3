#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <thread>
#include <tuple>
#include <utility>

#include <cppy3/cppy3.hpp>
#if CPPY3_BUILT_WITH_NUMPY
#include <cppy3/cppy3_numpy.hpp>
#endif

#include <catch2/catch_test_macros.hpp>

#ifndef TEST_UNICODE_CONVERTERS
#define TEST_UNICODE_CONVERTERS 1
#endif

namespace
{
  // A minimal custom C extension module, registered via
  // Config::builtin_modules, mirroring examples/console.cpp's "emb" module.
  PyObject *test_ext_double_it(PyObject *, PyObject *arg)
  {
    const long v = PyLong_AsLong(arg);
    return PyLong_FromLong(v * 2);
  }

  PyMethodDef test_ext_methods[] = {
      {"double_it", test_ext_double_it, METH_O, nullptr},
      {nullptr, nullptr, 0, nullptr},
  };

  PyModuleDef test_ext_module = {
      PyModuleDef_HEAD_INIT, "test_ext", nullptr, -1, test_ext_methods,
  };

  PyObject *PyInit_test_ext() { return PyModule_Create(&test_ext_module); }
}

TEST_CASE( "Utils", "" ) {
#if TEST_UNICODE_CONVERTERS
  SECTION( "unicode converters" ) {
    // Deliberately no setlocale()/std::locale::global() here: UTF8ToWide and
    // WideToUTF8 are locale-independent hand-rolled codecs (see plan bug #5)
    // and must round-trip correctly under the default "C" locale too -- the
    // v1 implementation only passed this test because it forced en_US.UTF-8.
    const std::string utf8Str("зачем вы посетили нас в глуши забытого селенья");
    const std::wstring unicodeStr(L"зачем вы посетили нас в глуши забытого селенья");

    REQUIRE(cppy3::WideToUTF8(unicodeStr) == utf8Str);
    REQUIRE(cppy3::UTF8ToWide(utf8Str) == unicodeStr);

    // CJK: 3-byte UTF-8 sequences.
    const std::string cjkUtf8("日本語のテスト");
    const std::wstring cjkWide(L"日本語のテスト");
    REQUIRE(cppy3::WideToUTF8(cjkWide) == cjkUtf8);
    REQUIRE(cppy3::UTF8ToWide(cjkUtf8) == cjkWide);

    // Astral codepoints (4-byte UTF-8; surrogate pairs on platforms where
    // wchar_t is UTF-16). \U escapes denote the Unicode scalar value
    // directly regardless of source encoding. This is exactly the path
    // bug #5's WideToUTF8 heap-buffer-overflowed on.
    const std::wstring emojiWide(L"\U0001F600\U0001F601\U0001F602");
    const std::string emojiUtf8 = cppy3::WideToUTF8(emojiWide);
    REQUIRE(emojiUtf8.size() == 12); // 3 codepoints x 4 UTF-8 bytes each
    REQUIRE(cppy3::UTF8ToWide(emojiUtf8) == emojiWide);
  }
#endif
}

TEST_CASE( "cppy3: Embedding Python into C++ code", "main funcs" ) {
  // create interpreter and get the __main__ namespace, the closest
  // equivalent to where v1's free exec()/eval() always ran
  cppy3::Interpreter interpreter;
  cppy3::Namespace mainNs = interpreter.main();

  // v1's Main/injectVar<T>()/getVar<T>() are gone -- Var now handles data
  // access through attr()/operator[]/str()/type()/iteration, and value
  // conversion through to_var()/Var::to<T>()/try_to<T>() (Converter<T>).
  SECTION("c++ -> python -> c++ variable access (Var attr/[]/str/type/iteration/to<T>)") {
    // inject C++ -> python via Namespace::set<T>()
    mainNs.set("a", 2);
    mainNs.set("b", 2);
    mainNs.exec("assert a + b == 4");
    mainNs.exec("print('sum is', a + b)");

    // extract python -> C++
    const cppy3::Var sum = mainNs.eval("a + b");
    REQUIRE(sum.type() == cppy3::Var::Type::Long);
    REQUIRE(sum.str() == "4");
    REQUIRE(sum.to<long>() == 4);
    REQUIRE(!cppy3::error());

    // bug #32 fix: a Python int now converts to a C++ double too (v1's
    // extract(o, double&) rejected this outright)
    REQUIRE(sum.to<double>() == 4.0);

    // python var assign name from c++ -> python
    mainNs.set("sum_var", sum);
    mainNs.exec("assert sum_var == 4");

    // cast to float in python, read back via Namespace::get<double>()
    mainNs.eval("sum_var = float(sum_var)");
    REQUIRE(std::abs(mainNs.get<double>("sum_var") - 4.0) < 1e-10);
    REQUIRE(mainNs.dict()["sum_var"].type() == cppy3::Var::Type::Float);

    // a type mismatch throws via to<T>(), and is reported via try_to<T>()
    // instead of throwing
    REQUIRE_THROWS_AS(mainNs.get<std::vector<int>>("sum_var"), cppy3::Error);
    REQUIRE(!mainNs.dict()["sum_var"].try_to<std::vector<int>>().has_value());
    REQUIRE(sum.try_to<long>().value() == 4);

    // unicode strings round-trip via exec/eval; Var::str()/to<string>() are
    // UTF-8 native
    const std::wstring unicodeStr = L"юникод smile ☺";
    mainNs.exec("uu = '" + cppy3::WideToUTF8(unicodeStr) + "'");
    const cppy3::Var uVar = mainNs.eval("uu");
    REQUIRE(uVar.str() == cppy3::WideToUTF8(unicodeStr));
    REQUIRE(uVar.to<std::wstring>() == unicodeStr);

    // unicode string inject / extract via Converter<T>, not just exec/eval
    mainNs.set("uVar2", unicodeStr);
    mainNs.exec("print('uVar2:', uVar2)");
    REQUIRE(mainNs.get<std::wstring>("uVar2") == unicodeStr);

    // attribute access on an arbitrary object
    mainNs.exec("class Point:\n  def __init__(self):\n    self.x = 3\npt = Point()");
    const cppy3::Var pt = mainNs.dict()["pt"];
    REQUIRE(pt.attr("x").str() == "3");
    REQUIRE(pt.has_attr("x"));
    REQUIRE(!pt.has_attr("y"));

    // iteration over an arbitrary Python iterable (PyObject_GetIter-based,
    // not list-specific)
    mainNs.exec("seq = [10, 20, 30]");
    const cppy3::Var seq = mainNs.dict()["seq"];
    std::vector<std::string> seen;
    for (const cppy3::Var &item : seq)
      seen.push_back(item.str());
    REQUIRE(seen == std::vector<std::string>{"10", "20", "30"});
  }

  SECTION("Converter<T> / to_var() / to<T>() round-trips") {
    REQUIRE(cppy3::to_var(true).to<bool>() == true);
    REQUIRE(cppy3::to_var(0).to<bool>() == false); // Python truthiness, not just an actual bool
    REQUIRE(cppy3::to_var(std::string("hi")).to<std::string>() == "hi");
    REQUIRE(cppy3::to_var("hi").to<std::string>() == "hi"); // const char* overload

    const std::vector<int> vec{1, 2, 3};
    REQUIRE(cppy3::to_var(vec).to<std::vector<int>>() == vec);

    const std::map<std::string, int> m{{"a", 1}, {"b", 2}};
    REQUIRE(cppy3::to_var(m).to<std::map<std::string, int>>() == m);

    REQUIRE(cppy3::to_var(std::optional<int>(42)).to<std::optional<int>>() == std::optional<int>(42));
    REQUIRE(cppy3::to_var(std::optional<int>(std::nullopt)).is_none());
    REQUIRE(cppy3::Var::steal(Py_NewRef(Py_None)).to<std::optional<int>>() == std::nullopt);

    const auto pr = std::make_pair(1, std::string("one"));
    REQUIRE((cppy3::to_var(pr).to<std::pair<int, std::string>>() == pr));

    const auto tup = std::make_tuple(1, std::string("two"), 3.0);
    REQUIRE(cppy3::to_var(tup).to<std::tuple<int, std::string, double>>() == tup);
  }

  SECTION("Var::operator()/call_kw/method") {
    mainNs.exec("def add(a, b): return a + b");
    REQUIRE(mainNs.dict()["add"](2, 3).to<long>() == 5);

    mainNs.exec("def greet(name, greeting='Hello'): return f'{greeting}, {name}!'");
    const cppy3::Var greet = mainNs.dict()["greet"];
    REQUIRE(greet("World").str() == "Hello, World!");
    REQUIRE(greet.call_kw({{"greeting", cppy3::to_var("Hi")}}, "World").str() == "Hi, World!");

    mainNs.exec(
        "class Counter:\n"
        "  def __init__(self):\n"
        "    self.n = 0\n"
        "  def add(self, x):\n"
        "    self.n += x\n"
        "    return self.n\n");
    const cppy3::Var counter = mainNs.dict()["Counter"]();
    REQUIRE(counter.method("add", 3).to<long>() == 3);
    REQUIRE(counter.method("add", 4).to<long>() == 7);

    // calling a non-callable throws Error rather than crashing
    REQUIRE_THROWS_AS(cppy3::to_var(42)(), cppy3::Error);

    // a Python-side exception raised during the call propagates
    mainNs.exec("def boom(): raise ValueError('kaboom')");
    REQUIRE_THROWS_AS(mainNs.dict()["boom"](), cppy3::Error);
    REQUIRE(!cppy3::error()); // and the interpreter's error state was consumed
  }

  SECTION("make_function() exposes a C++ callable to Python (bug #36)") {
    // a plain function pointer
    mainNs.set("cpp_add", cppy3::make_function("cpp_add", +[](long a, long b) -> long { return a + b; }));
    REQUIRE(mainNs.eval("cpp_add(2, 3)").to<long>() == 5);
    mainNs.exec("assert cpp_add(2, 3) == 5"); // callable from Python source too, not just eval()

    // a capturing lambda
    int calls = 0;
    mainNs.set("cpp_greet",
               cppy3::make_function("cpp_greet", [&calls](std::string name) -> std::string {
                 calls++;
                 return "Hello, " + name + "!";
               }));
    REQUIRE(mainNs.eval("cpp_greet('World')").str() == "Hello, World!");
    REQUIRE(calls == 1);

    // void return -> None
    mainNs.set("cpp_noop", cppy3::make_function("cpp_noop", []() -> void {}));
    REQUIRE(mainNs.eval("cpp_noop()").is_none());

    // wrong arity raises TypeError, not a crash
    REQUIRE_THROWS_AS(mainNs.eval("cpp_add(1)"), cppy3::Error);
    try {
      mainNs.eval("cpp_add(1)");
      REQUIRE(false);
    } catch (const cppy3::Error &e) {
      REQUIRE(e.type_name() == "TypeError");
    }

    // a C++ exception thrown during the call surfaces as a Python exception
    mainNs.set("cpp_throws", cppy3::make_function("cpp_throws", []() -> long {
                  throw std::runtime_error("boom from C++");
                }));
    REQUIRE_THROWS_AS(mainNs.eval("cpp_throws()"), cppy3::Error);
  }

  // The v1 audit (see the plan's Phase 0 bug_repro/ diagnostics, preserved
  // in git history) found three refcount/leak bugs reachable through the
  // old Var API: a broken copy-assignment operator (#1), call() silently
  // double-decref'ing its arguments (#2), and toString() leaking its
  // PyObject_Str() result every call (#9). That API no longer exists (Var
  // was redesigned, not patched in place), so those repro scripts can't
  // even compile anymore -- these sections are their replacement: proving
  // the equivalent properties hold for the current Var/operator()/str().
  SECTION("Var copy-assignment does not corrupt refcounts (regression for v1 bug #1)") {
    // must be outside CPython's immortal small-int cache (-5..256), else
    // Py_INCREF/DECREF are no-ops and the check is vacuous.
    PyObject *obj = PyLong_FromLong(987654321);
    cppy3::Var keepAlive = cppy3::Var::steal(obj);
    const Py_ssize_t before = Py_REFCNT(obj);

    {
      cppy3::Var a = cppy3::Var::borrow(obj);
      cppy3::Var b;
      b = a; // copy-assignment
    }

    REQUIRE(Py_REFCNT(obj) == before);
  }

  SECTION("Var::operator() does not corrupt argument refcounts (regression for v1 bug #2)") {
    mainNs.exec("def identity(x): return x");
    PyObject *arg = PyLong_FromLong(987654322);
    cppy3::Var argVar = cppy3::Var::steal(arg);

    cppy3::Var result;
    {
      const cppy3::Var identity = mainNs.dict()["identity"];
      result = identity(argVar);
    }

    // argVar and result are the only two live owners at this point.
    REQUIRE(Py_REFCNT(arg) == 2);
  }

  SECTION("Var::str() does not leak (regression for v1 bug #9)") {
    mainNs.exec("import sys");
    // a non-cached int: str() must allocate a fresh string every call
    const cppy3::Var val = mainNs.eval("123456789");

    mainNs.exec("_before = sys.getallocatedblocks()");
    for (int i = 0; i < 20000; i++)
      (void)val.str();
    mainNs.exec("_after = sys.getallocatedblocks()");
    mainNs.exec("assert (_after - _before) < 10000, (_before, _after)");
    REQUIRE(!cppy3::error());
  }

  SECTION("python -> c++ exception forwarding") {
    try {
      // throw exception in python
      mainNs.exec("raise Exception('test-exception')");
      REQUIRE( false );  // unreachable code

    } catch (const cppy3::Error& e) {
      // catch in c++
      REQUIRE(e.type_name() == "Exception");
      REQUIRE(e.message() == "test-exception");
      REQUIRE(e.traceback().size() > 0);
      REQUIRE(std::string(e.what()).size() > 0);
    }
    // exception has been popped from python layer
    REQUIRE(!cppy3::error());
  }

  SECTION("chained exception (__cause__) via a real raise ... from ...") {
    try {
      mainNs.exec(
          "try:\n"
          "  raise RuntimeError('root cause')\n"
          "except RuntimeError as e:\n"
          "  raise ValueError('boom') from e\n");
      REQUIRE(false); // unreachable
    } catch (const cppy3::Error &e) {
      REQUIRE(e.type_name() == "ValueError");
      REQUIRE(e.message() == "boom");
      REQUIRE(e.cause() != nullptr);
      REQUIRE(e.cause()->type_name() == "RuntimeError");
      REQUIRE(e.cause()->message() == "root cause");

      const std::string formatted = e.format();
      REQUIRE(formatted.find("ValueError: boom") != std::string::npos);
      REQUIRE(formatted.find("RuntimeError: root cause") != std::string::npos);

      // copy must deep-copy the cause chain, not alias it
      cppy3::Error copy = e;
      REQUIRE(copy.cause() != nullptr);
      REQUIRE(copy.cause() != e.cause());
      REQUIRE(copy.cause()->message() == "root cause");
    }
  }

#if CPPY3_BUILT_WITH_NUMPY
  SECTION("numpy ndarray support") {

    cppy3::importNumpy();
    mainNs.exec("import numpy");
    mainNs.exec("print('numpy version {}'.format(numpy.version.full_version))");

    // create numpy ndarray in C
    double cData[2] = {3.14, 42};
    // create copy
    cppy3::NDArray<double> a = cppy3::NDArray<double>::copy(cData, 2, 1);
    // wrap cData without copying
    cppy3::NDArray<double> b = cppy3::NDArray<double>::wrap(cData, 2, 1);
    REQUIRE(a(1, 0) == cData[1]);
    REQUIRE(b(1, 0) == cData[1]);

    // dim1()/dim2() are 0/1-indexed now, not 1/2 (regression for bug #7)
    REQUIRE(a.dim1() == 2);
    REQUIRE(a.dim2() == 1);

    // inject into python __main__ namespace -- NDArray now inherits Var's
    // (already-correct) copy semantics directly, no more borrow() dance
    mainNs.dict().set_item("a", a);
    mainNs.dict().set_item("b", b);
    mainNs.exec("print('a: {} {}'.format(type(a), a))");
    mainNs.exec("print('b: {} {}'.format(type(b), b))");
    mainNs.exec("assert type(a) == numpy.ndarray, 'expect injected instance'");
    mainNs.exec("assert numpy.all(a == b), 'expect cData'");

    // modify b from python (b is a shared ndarray over cData)
    mainNs.exec("b[0] = 100500");
    REQUIRE(b(0, 0) == 100500);
    REQUIRE(cData[0] == 100500);

    // out-of-range access throws Error instead of asserting (bug #19 spirit)
    REQUIRE_THROWS_AS(a(5, 0), cppy3::Error);
    REQUIRE_THROWS_AS(a.dim(2), cppy3::Error);

    // create({...}) has no create(int,bool)/create(size_t,size_t) overload
    // ambiguity to fall into (regression for bug #8b)
    cppy3::NDArray<double> c = cppy3::NDArray<double>::create({3, 5});
    REQUIRE(c.ndim() == 2);
    REQUIRE(c.dim1() == 3);
    REQUIRE(c.dim2() == 5);

    // Var::type() reports NumpyNdarray via the is_ndarray hook, not
    // dead #ifdef'd-out code that could never match (bug #14)
    REQUIRE(a.type() == cppy3::Var::Type::NumpyNdarray);

    // 1D wrap() must alias the caller's own buffer, not the address of a
    // local pointer parameter inside wrap() itself (regression for v1's
    // bug #6 -- its 1D overload passed (void*)&data, the 2D one correctly
    // passed (void*)data; both now share one shape-span-taking
    // implementation, so there is no separate 1D code path to regress).
    double oneDData[4] = {10.0, 20.0, 30.0, 40.0};
    cppy3::NDArray<double> d = cppy3::NDArray<double>::wrap(std::span(oneDData));
    for (int i = 0; i < 4; i++)
      REQUIRE(d(i) == oneDData[i]);
    d(0) = 99.0;
    REQUIRE(oneDData[0] == 99.0); // shares memory with oneDData, not a copy
  }
#endif

  SECTION("test Scoped GIL Lock / Release") {

    // initially Python GIL is locked
    REQUIRE(cppy3::gil_held());

    // add variable
    mainNs.exec("a = []");
    cppy3::List a{mainNs.dict()["a"]};
    REQUIRE(a.size() == 0);

    // create thread that changes the variable a in a different thread
    const std::string threadScript = R"(
import threading
def thread_main():
  global a
  a.append(42)

t = threading.Thread(target=thread_main, daemon=True)
t.start()
)";
    std::cout << threadScript << std::endl;
    mainNs.exec(threadScript);

    {
      // release GIL on this thread
      cppy3::GilRelease gilRelease;
      REQUIRE(!cppy3::gil_held());
      // and wait thread changes the variable
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      {
        // lock GIL again before accessing python objects
        cppy3::GilLock locker;
        REQUIRE(cppy3::gil_held());

        // ensure that variable has been changed
        mainNs.exec("assert a == [42], a");
        REQUIRE(a.size() == 1);
        REQUIRE(a[0].str() == "42");
      }

      // GIL is released again
      REQUIRE(!cppy3::gil_held());
    }
  }
}

TEST_CASE("cppy3::Interpreter / Namespace features", "interpreter") {
  SECTION("multiple isolated namespaces (bug #25)") {
    cppy3::Interpreter interp;
    cppy3::Namespace ns1 = interp.new_namespace();
    cppy3::Namespace ns2 = interp.new_namespace();
    ns1.set("x", 1);
    ns2.set("x", 2);
    REQUIRE(ns1.get<long>("x") == 1);
    REQUIRE(ns2.get<long>("x") == 2);
    // neither leaks into __main__
    REQUIRE(!interp.main().dict().has_attr("x"));
  }

  SECTION("eval() uses PyErr_ExceptionMatches, not string comparison (bug #20)") {
    cppy3::Interpreter interp;
    cppy3::Namespace ns = interp.main();

    // a genuine syntax error is reported as such, not silently retried
    REQUIRE_THROWS_AS(ns.eval("1 +"), cppy3::Error);
    try {
      ns.eval("1 +");
      REQUIRE(false);
    } catch (const cppy3::Error &e) {
      REQUIRE(e.type_name() == "SyntaxError");
    }

    // a statement (not a valid expression) falls back to exec() correctly
    ns.eval("x = 5");
    REQUIRE(ns.get<long>("x") == 5);
  }

  SECTION("exec_file compiles with the real filename, not \"<string>\" (bug #24)") {
    const auto tmpPath = std::filesystem::temp_directory_path() / "cppy3_test_exec_file.py";
    {
      std::ofstream f(tmpPath);
      f << "def boom():\n    raise ValueError('from file')\n";
    }

    cppy3::Interpreter interp;
    cppy3::Namespace ns = interp.main();
    ns.exec_file(tmpPath);
    REQUIRE(ns.get<std::string>("__file__") == tmpPath.string());

    try {
      ns.eval("boom()");
      REQUIRE(false);
    } catch (const cppy3::Error &e) {
      REQUIRE(e.traceback().find(tmpPath.string()) != std::string::npos);
    }

    std::filesystem::remove(tmpPath);
  }

  SECTION("stdout/stderr redirection (bug #27)") {
    cppy3::Interpreter interp;
    std::string captured;
    interp.set_stdout_hook([&captured](std::string_view s) { captured += s; });
    interp.main().exec("print('hello from python', end='')");
    REQUIRE(captured == "hello from python");
  }

  SECTION("Config::builtin_modules registers a custom C extension before init") {
    cppy3::Config config;
    config.builtin_modules.push_back({"test_ext", PyInit_test_ext});
    cppy3::Interpreter interp(config);
    cppy3::Namespace ns = interp.main();
    ns.exec("import test_ext");
    REQUIRE(ns.eval("test_ext.double_it(21)").to<long>() == 42);
  }

  SECTION("Config::argv populates sys.argv (bug #3 -- v1's setArgv() dereferenced a null PyConfig*)") {
    cppy3::Config config;
    config.program_name = "cppy3_test";
    config.argv = {"--flag", "value"};
    cppy3::Interpreter interp(config);
    cppy3::Namespace ns = interp.main();
    ns.exec("import sys");
    REQUIRE(ns.eval("sys.argv[0]").str() == "cppy3_test");
    REQUIRE(ns.eval("sys.argv[1]").str() == "--flag");
    REQUIRE(ns.eval("sys.argv[2]").str() == "value");
  }
}
