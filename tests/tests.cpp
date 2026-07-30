#include <iostream>
#include <thread>

#include <cppy3/cppy3.hpp>
#if CPPY3_BUILT_WITH_NUMPY
#include <cppy3/cppy3_numpy.hpp>
#endif

#include <catch2/catch_test_macros.hpp>

#ifndef TEST_UNICODE_CONVERTERS
#define TEST_UNICODE_CONVERTERS 1
#endif


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
  // create interpreter
  cppy3::PythonVM instance;

  // v1's Main/injectVar<T>()/getVar<T>() are gone (Var now handles data
  // access through attr()/operator[]/str()/type()/iteration instead, and
  // to<T>()-style value conversion returns with the Converter<T> traits in
  // a later phase). This section exercises what the new Var provides today.
  SECTION("c++ -> python -> c++ variable access (Var attr/[]/str/type/iteration)") {
    cppy3::Var mainNs = cppy3::Var::borrow(cppy3::getMainDict());

    // inject C++ -> python via the (still-present, soon-superseded)
    // convert() free function + Var::set_item()
    mainNs.set_item("a", cppy3::Var::steal(cppy3::convert(2)));
    mainNs.set_item("b", cppy3::Var::steal(cppy3::convert(2)));
    cppy3::exec("assert a + b == 4");
    cppy3::exec("print('sum is', a + b)");

    // extract python -> C++
    const cppy3::Var sum = cppy3::eval("a + b");
    REQUIRE(sum.type() == cppy3::Var::Type::Long);
    REQUIRE(sum.str() == "4");
    REQUIRE(!cppy3::error());

    // python var assign name from c++ -> python
    mainNs.set_item("sum_var", sum);
    cppy3::exec("assert sum_var == 4");

    // cast to float in python, read back via Var
    cppy3::eval("sum_var = float(sum_var)");
    const cppy3::Var sumVar = mainNs["sum_var"];
    REQUIRE(sumVar.type() == cppy3::Var::Type::Float);
    REQUIRE(sumVar.str() == "4.0");

    // unicode strings round-trip via exec/eval; Var::str() is UTF-8 native
    const std::wstring unicodeStr = L"юникод smile ☺";
    cppy3::exec(L"uu = '" + unicodeStr + L"'");
    const cppy3::Var uVar = cppy3::eval("uu");
    REQUIRE(uVar.str() == cppy3::WideToUTF8(unicodeStr));

    // attribute access on an arbitrary object
    cppy3::exec("class Point:\n  def __init__(self):\n    self.x = 3\npt = Point()");
    const cppy3::Var pt = mainNs["pt"];
    REQUIRE(pt.attr("x").str() == "3");
    REQUIRE(pt.has_attr("x"));
    REQUIRE(!pt.has_attr("y"));

    // iteration over an arbitrary Python iterable (PyObject_GetIter-based,
    // not list-specific)
    cppy3::exec("seq = [10, 20, 30]");
    const cppy3::Var seq = mainNs["seq"];
    std::vector<std::string> seen;
    for (const cppy3::Var &item : seq)
      seen.push_back(item.str());
    REQUIRE(seen == std::vector<std::string>{"10", "20", "30"});
  }

  // The v1 audit (see the plan's Phase 0 bug_repro/ diagnostics, preserved
  // in git history) found three refcount/leak bugs reachable through the
  // old Var API: a broken copy-assignment operator (#1), call() silently
  // double-decref'ing its arguments (#2), and toString() leaking its
  // PyObject_Str() result every call (#9). That API no longer exists (Var
  // was redesigned, not patched in place), so those repro scripts can't
  // even compile anymore -- these sections are their replacement: proving
  // the equivalent properties hold for the current Var/call()/str().
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

  SECTION("call() does not corrupt argument refcounts (regression for v1 bug #2)") {
    cppy3::exec("def identity(x): return x");
    PyObject *arg = PyLong_FromLong(987654322);
    cppy3::Var argVar = cppy3::Var::steal(arg);

    cppy3::Var result;
    {
      cppy3::arguments args;
      args.push_back(argVar);
      result = cppy3::Var::steal(
          cppy3::call(cppy3::lookupCallable(cppy3::getMainModule(), L"identity").get(), args));
    }

    // argVar and result are the only two live owners at this point.
    REQUIRE(Py_REFCNT(arg) == 2);
  }

  SECTION("Var::str() does not leak (regression for v1 bug #9)") {
    cppy3::exec("import sys");
    // a non-cached int: str() must allocate a fresh string every call
    const cppy3::Var val = cppy3::eval("123456789");

    cppy3::exec("_before = sys.getallocatedblocks()");
    for (int i = 0; i < 20000; i++)
      (void)val.str();
    cppy3::exec("_after = sys.getallocatedblocks()");
    cppy3::exec("assert (_after - _before) < 10000, (_before, _after)");
    REQUIRE(!cppy3::error());
  }

  SECTION("python -> c++ exception forwarding") {
    try {
      // throw excepton in python
      cppy3::exec("raise Exception('test-exception')");
      REQUIRE( false );  // unreachable code

    } catch (const cppy3::PythonException& e) {
      // catch in c++
      REQUIRE(e.info.type == L"<class 'Exception'>");
      REQUIRE(e.info.reason == L"test-exception");
      REQUIRE(e.info.trace.size() > 0);
      REQUIRE(std::string(e.what()).size() > 0);
    }
    // exception has been poped from python layer
    REQUIRE(!cppy3::error());
  }

#if CPPY3_BUILT_WITH_NUMPY
  SECTION("numpy ndarray support") {

    cppy3::importNumpy();
    cppy3::exec("import numpy");
    cppy3::exec("print('numpy version {}'.format(numpy.version.full_version))");

    // create numpy ndarray in C
    double cData[2] = {3.14, 42};
    // create copy
    cppy3::NDArray<double> a(cData, 2, 1);
    // wrap cData without copying
    cppy3::NDArray<double> b;
    b.wrap(cData, 2, 1);
    REQUIRE(a(1, 0) == cData[1]);
    REQUIRE(b(1, 0) == cData[1]);

    // inject into python __main__ namespace
    cppy3::Var mainNs = cppy3::Var::borrow(cppy3::getMainDict());
    mainNs.set_item("a", cppy3::Var::borrow(a));
    mainNs.set_item("b", cppy3::Var::borrow(b));
    cppy3::exec("print('a: {} {}'.format(type(a), a))");
    cppy3::exec("print('b: {} {}'.format(type(b), b))");
    cppy3::exec("assert type(a) == numpy.ndarray, 'expect injected instance'");
    cppy3::exec("assert numpy.all(a == b), 'expect cData'");

    // modify b from python (b is a shared ndarray over cData)
    cppy3::exec("b[0] = 100500");
    REQUIRE(b(0, 0) == 100500);
    REQUIRE(cData[0] == 100500);
  }
#endif


  SECTION("test Scoped GIL Lock / Release") {

    // initially Python GIL is locked
    REQUIRE(cppy3::GILLocker::isLocked());

    // add variable
    cppy3::exec("a = []");
    cppy3::List a = cppy3::List(cppy3::lookupObject(cppy3::getMainModule(), L"a"));
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
    cppy3::exec(threadScript);

    {
      // release GIL on this thread
      cppy3::ScopedGILRelease gilRelease;
      REQUIRE(!cppy3::GILLocker::isLocked());
      // and wait thread changes the variable
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      {
        // lock GIL again before accessing python objects
        cppy3::GILLocker locker;
        REQUIRE(cppy3::GILLocker::isLocked());

        // ensure that variable has been changed
        cppy3::exec("assert a == [42], a");
        REQUIRE(a.size() == 1);
        REQUIRE(a[0].str() == "42");
      }

      // GIL is released again
      REQUIRE(!cppy3::GILLocker::isLocked());
    }
  }

  SECTION("cppy3::Error / throw_if_error() (v2, not yet wired into exec/eval)") {
    PyObject *mainDict = cppy3::getMainDict();

    SECTION("simple exception") {
      PyObject *result = PyRun_String("raise ValueError('boom')", Py_file_input, mainDict, mainDict);
      REQUIRE(result == nullptr);

      try {
        cppy3::throw_if_error();
        REQUIRE(false); // unreachable
      } catch (const cppy3::Error &e) {
        REQUIRE(e.type_name() == "ValueError");
        REQUIRE(e.message() == "boom");
        REQUIRE(e.traceback().size() > 0);
        REQUIRE(e.cause() == nullptr);
      }
      // exception has been fetched and cleared
      REQUIRE(!cppy3::error());
    }

    SECTION("chained exception (__cause__)") {
      PyObject *result = PyRun_String(
          "try:\n"
          "  raise RuntimeError('root cause')\n"
          "except RuntimeError as e:\n"
          "  raise ValueError('boom') from e\n",
          Py_file_input, mainDict, mainDict);
      REQUIRE(result == nullptr);

      try {
        cppy3::throw_if_error();
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
  }
}
