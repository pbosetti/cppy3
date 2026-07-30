# cppy3

## Embed Python 3 into your C++ app in a few minutes

#### Minimalistic library for embedding [CPython](https://github.com/python/cpython) 3.x into a C++ application

Lightweight, simple, and clean alternative to heavy [boost.python](https://github.com/boostorg/python).
No additional dependencies. Cross-platform -- Linux, macOS, and Windows are supported.

cppy3 is suited for [embedding Python in a C++ application](https://docs.python.org/3/extending/index.html), while boost.python is evolved around [extending Python with a C++ module](https://docs.python.org/3/extending/index.html) and [its embedding capabilities are somewhat limited](https://www.boost.org/doc/libs/1_63_0/libs/python/doc/html/tutorial/tutorial/embedding.html).

> **Coming from v1?** See [MIGRATION.md](MIGRATION.md) for a spelling-by-spelling map from the old API to this one, and a script (`tools/migrate_v1_to_v2.py`) that applies most of it for you.

### Features

* `Var`: a reference-counted, RAII-managed `PyObject*` handle with attribute/item access (`.attr()`, `operator[]`), iteration over any Python iterable, and calling (`operator()`, `.call_kw()`, `.method()`)
* `Converter<T>`: a single, specializable extension point for converting your own C++ types to and from Python -- built-in support for `bool`, integers, floats, `std::string`/`string_view`, `std::wstring`, `std::vector`, `std::map`, `std::optional`, `std::pair`, `std::tuple`
* `Error`: Python exceptions translated to C++ exceptions, including the `__cause__`/`__context__` chain and a full traceback
* `Interpreter`/`Namespace`: interpreter lifecycle via `PyConfig` (venv/isolated mode/argv/custom C extensions), and isolated namespaces to `exec()`/`eval()` code in -- not just the one shared `__main__`
* `GilLock`/`GilRelease`: scoped GIL management
* `make_function()`: exposes a C++ callable to Python with no `PyMethodDef` boilerplate
* `NDArray<T>`: a thin, validating C++ view over `numpy.ndarray`
* Example [interactive Python console](examples/console.cpp), [venv activation](examples/venv.cpp), and [C++ callback](examples/callback.cpp)

### Quick tour

Code snippets below are drawn from [tests/tests.cpp](tests/tests.cpp), which is the fullest tour of the API.

#### Inject/extract variables C++ &harr; Python

```c++
cppy3::Interpreter interpreter;
cppy3::Namespace ns = interpreter.main();

// inject
ns.set("a", 2);
ns.set("b", 2);
ns.exec("assert a + b == 4");
ns.exec("print('sum is', a + b)");

// extract
const cppy3::Var sum = ns.eval("a + b");
assert(sum.type() == cppy3::Var::Type::Long);
assert(sum.to<long>() == 4);
assert(sum.str() == "4");

// a Python int also converts to a C++ double -- Converter<T> follows
// Python's own numeric tower instead of rejecting the cross-type request
assert(sum.to<double>() == 4.0);
```

#### Attribute/item access and iteration

```c++
ns.exec("class Point:\n  def __init__(self):\n    self.x = 3\npt = Point()");
const cppy3::Var pt = ns.dict()["pt"];
assert(pt.attr("x").str() == "3");

ns.exec("seq = [10, 20, 30]");
for (const cppy3::Var &item : ns.dict()["seq"])
  std::cout << item.str() << std::endl;
```

#### Calling: positional, keyword, and methods

```c++
ns.exec("def greet(name, greeting='Hello'): return f'{greeting}, {name}!'");
const cppy3::Var greet = ns.dict()["greet"];
assert(greet("World").str() == "Hello, World!");
assert(greet.call_kw({{"greeting", cppy3::to_var("Hi")}}, "World").str() == "Hi, World!");

ns.exec("class Counter:\n  def __init__(self): self.n = 0\n  def add(self, x):\n    self.n += x\n    return self.n");
const cppy3::Var counter = ns.dict()["Counter"]();
assert(counter.method("add", 3).to<long>() == 3);
```

#### Forward exceptions Python &rarr; C++

```c++
try {
  ns.exec("raise Exception('test-exception')");
  assert(false && "not supposed to be here");
} catch (const cppy3::Error &e) {
  assert(e.type_name() == "Exception");
  assert(e.message() == "test-exception");
  assert(e.traceback().size() > 0);
  assert(std::string(e.what()).size() > 0);
}
```

Chained exceptions (`raise ... from ...`) carry their `__cause__` too:

```c++
try {
  ns.exec("try:\n  raise RuntimeError('root cause')\nexcept RuntimeError as e:\n  raise ValueError('boom') from e\n");
} catch (const cppy3::Error &e) {
  assert(e.type_name() == "ValueError");
  assert(e.cause()->type_name() == "RuntimeError");
  std::cout << e.format() << std::endl; // full Python-style traceback, including the cause
}
```

#### Expose a C++ callable to Python

```c++
int calls = 0;
ns.set("cpp_greet", cppy3::make_function("cpp_greet", [&calls](std::string name) -> std::string {
  calls++;
  return "Hello, " + name + "!";
}));
assert(ns.eval("cpp_greet('World')").str() == "Hello, World!");
```

#### Configure the interpreter: venvs, isolation, custom C extensions

```c++
cppy3::Config config;
config.executable = "/path/to/venv/bin/python3"; // follows the venv's pyvenv.cfg
config.isolated = true;                          // PEP 432 isolated mode
config.argv = {"--flag", "value"};                // sys.argv[1:]
cppy3::Interpreter interpreter(config);
```

See [examples/venv.cpp](examples/venv.cpp) for a complete, runnable version.

#### Support numpy ndarray

```c++
cppy3::Interpreter interpreter;
cppy3::importNumpy();
cppy3::Namespace ns = interpreter.main();

double cData[2] = {3.14, 42};

// create a numpy-owned copy
cppy3::NDArray<double> a = cppy3::NDArray<double>::copy(cData, 2, 1);

// wrap cData without copying -- shares memory with the C++ array
cppy3::NDArray<double> b = cppy3::NDArray<double>::wrap(cData, 2, 1);

assert(a(1, 0) == cData[1]);
assert(b(1, 0) == cData[1]);

ns.dict().set_item("a", a);
ns.dict().set_item("b", b);
ns.exec("import numpy");
ns.exec("assert numpy.all(a == b), 'expect cData'");

// modify b from python (b is a shared ndarray over cData)
ns.exec("b[0] = 100500");
assert(b(0, 0) == 100500);
assert(cData[0] == 100500);
```

#### Scoped GIL Lock / Release management

```c++
// initially the GIL is held
assert(cppy3::gil_held());

ns.exec("a = []");
cppy3::List a{ns.dict()["a"]};
assert(a.size() == 0);

// a thread that appends to `a` from Python
const std::string threadScript = R"(
import threading
def thread_main():
  global a
  a.append(42)

t = threading.Thread(target=thread_main, daemon=True)
t.start()
)";
ns.exec(threadScript);

{
  cppy3::GilRelease gilRelease; // let the other thread run
  assert(!cppy3::gil_held());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  {
    cppy3::GilLock locker; // reacquire before touching Python objects
    assert(cppy3::gil_held());
    ns.exec("assert a == [42], a");
    assert(a.size() == 1);
    assert(a[0].str() == "42");
  }
  assert(!cppy3::gil_held());
}
```

### Requirements

* C++20 compiler (Clang, GCC, or MSVC)
* CMake 3.18+
* Python 3.10+ dev package (with NumPy recommended)

#### Build

##### Prerequisites
###### macOS

Homebrew's python package includes dev headers and NumPy:
```bash
brew install cmake python3
```

###### Linux (Debian/Ubuntu)

```bash
sudo apt-get install cmake g++ python3-dev
```

NumPy is optional but recommended:
```bash
sudo apt-get install python3-numpy
```

##### Windows

CMake, Visual Studio (or clang-cl), and Python with NumPy are recommended.

#### Configure, build, test

```bash
cmake -Bbuild -GNinja -DCPPY3_BUILD_TESTS=ON -DCPPY3_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful CMake options:

| Option | Default | Effect |
| --- | --- | --- |
| `CPPY3_BUILD_TESTS` | `OFF` | Build the Catch2-based test suite |
| `CPPY3_BUILD_EXAMPLES` | `OFF` | Build `console`, `venv`, `callback` |
| `CPPY3_SANITIZE` | `OFF` | Build with AddressSanitizer + UndefinedBehaviorSanitizer |
| `BUILD_SHARED_LIBS` | `OFF` | Build cppy3 as a shared library (works on Windows too, unlike v1) |

#### Run the examples

```bash
./build/examples/console                    # interactive Python console
./build/examples/callback                   # C++ callable exposed to Python
python3 -m venv /tmp/demo-venv && ./build/examples/venv /tmp/demo-venv
```

#### Use cppy3 in your own project

Installed and exported via CMake's standard package mechanism:

```bash
cmake --install build --prefix /path/to/install
```

```cmake
find_package(cppy3 REQUIRED)
target_link_libraries(your_target PRIVATE cppy3::cppy3)
```

Or via `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(cppy3 GIT_REPOSITORY <this-repo-url> GIT_TAG main)
FetchContent_MakeAvailable(cppy3)
target_link_libraries(your_target PRIVATE cppy3::cppy3)
```

### License

[MIT License](LICENSE). Feel free to use.
