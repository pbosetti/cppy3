# Migrating from cppy3 v1 to v2

v2 is a deliberate breaking rewrite, not an incremental patch: nearly every
class was replaced rather than fixed in place, because several v1 bugs
(refcount corruption, a double-decref in `call()`, silent leaks) were
consequences of the *design*, not just its implementation. There is no
compatibility shim -- v1 and v2 cannot be mixed in the same translation
unit.

A companion script, `tools/migrate_v1_to_v2.py`, applies the mechanical
renames below automatically and flags the rest for manual review. Run it
with `--apply` to rewrite files in place, or without to preview a diff:

```bash
python3 tools/migrate_v1_to_v2.py --apply path/to/your/code
```

The tables below are the same rule set the script uses (`--json` dumps it
verbatim), so this document and the script cannot drift apart.

A few v1 spellings (`exec()`, `eval()`, `execScriptFile()`, `Main().inject*`,
`getMainDict()`) became methods on a `cppy3::Namespace` in v2, but the
script cannot know what you named your `Namespace` variable. For those, it
rewrites the call with a literal placeholder receiver,
`/*YOUR_NAMESPACE*/`, e.g. `cppy3::exec("...")` becomes
`/*YOUR_NAMESPACE*/.exec("...")`. After running with `--apply`, grep for
`YOUR_NAMESPACE` and replace each occurrence with your actual `Namespace`
(e.g. `interpreter.main()`, or a variable holding it) -- the placeholder is
deliberately not valid C++, so these sites fail to compile until you do,
rather than silently referencing the wrong thing.

## Automatic renames

These are one-to-one token substitutions; the script applies them without
supervision.

| v1 | v2 |
| --- | --- |
| `PythonVM` | `Interpreter` |
| `PythonException` | `Error` |
| `GILLocker` | `GilLock` |
| `ScopedGILLock` | `GilLock` |
| `ScopedGILRelease` | `GilRelease` |
| `cppy3::GILLocker::isLocked()` | `cppy3::gil_held()` |
| `Var::from(x)` | `Var::steal(x)` |
| `.newRef(x)` (on a live `Var`) | `= Var::steal(x)` (an assignment) |
| `.toUTF8String()` | `.str()` |
| `.toLong()` | `.to<long>()` |
| `.toDouble()` | `.to<double>()` |
| `.typeName()` | `.type_name()` |
| `e.info.reason` | `e.message()` |
| `e.info.type` | `e.type_name()` |
| `e.info.trace` | `e.traceback()` |

## Assisted renames

The script rewrites these too, but either the change alters behavior, or
it needs the `/*YOUR_NAMESPACE*/` placeholder above -- verify the
surrounding code (and resolve the placeholder) after applying it.

| v1 | v2 | What changes |
| --- | --- | --- |
| `cppy3::exec(code)` | `ns.exec(code)` | Namespace receiver needed. |
| `cppy3::eval(expr)` | `ns.eval(expr)` | Namespace receiver needed. |
| `cppy3::execScriptFile(path)` | `ns.exec_file(path)` | Namespace receiver needed; tracebacks and `__file__` now show the real path instead of `"<string>"`. |
| `cppy3::Main().injectVar<T>(name, value)` | `ns.set(name, value)` | Namespace receiver needed. |
| `cppy3::Main().inject(name, value)` | `ns.set(name, value)` | Namespace receiver needed. |
| `cppy3::Main().getVar<T>(name, out)` | `out = ns.get<T>(name)` | Namespace receiver needed; also returns the value instead of writing through an out-parameter. |
| `cppy3::getMainDict()` | `ns.dict().get()` | Namespace receiver needed. |
| `.reset(x)` (on a `Var`) | `= Var::borrow(x)` (an assignment) | **Caution:** `.reset(` is also a common `std::unique_ptr`/`std::optional` method name unrelated to cppy3 -- the script rewrites any `IDENT.reset(x)` it sees, so verify the receiver is actually a `cppy3::Var` before keeping each one. |

## Manual review required

### Flagged by the script

These have no mechanical rewrite (the target needs real restructuring, or
the pattern is ambiguous without type information); the script reports
each occurrence's file/line and a note, but never rewrites them.

* **`cppy3::Main()`**, used any way other than `.injectVar<T>(...)`/
  `.inject(...)`/`.getVar<T>(...)` (those three are rewritten
  automatically -- see above). Replace with your `cppy3::Namespace`.
* **`cppy3::getMainModule()`** -- returned the `__main__` module object
  itself; `Namespace` only wraps its dict. Use
  `PyImport_AddModule("__main__")` directly if you need the module object.
* **`cppy3::lookupObject(m, name)` / `cppy3::lookupCallable(m, name)`** --
  use `Var::attr()`/`operator[]` directly, e.g.
  `lookupObject(m, L"a.b")` becomes `Var(m).attr("a").attr("b")`.
* **`cppy3::call(f, args)`** -- becomes `f(args...)` (`Var::operator()`),
  which returns an owning `Var`. If you manually `Py_DECREF`'d the raw
  `PyObject*` `call()` returned, **delete that decref** -- it would now
  double-free.
* **`cppy3::arguments`** -- was `call()`'s `std::vector<Var>` argument-list
  type; `Var::operator()` takes a variadic argument pack directly instead.
* **`cppy3::createClassInstance(name)`** -- becomes
  `ns.dict()["ClassName"](...)`, an ordinary `Var` call.
* **`cppy3::setArgv(...)`** -- removed outright (it never worked: it
  dereferenced a null `PyConfig*`). Use `Config::argv` when constructing
  the `Interpreter`.
* **`.wrap(`/`.dim1(`/`.dim2(`** on an `NDArray` -- `wrap()` is now a
  static factory (`NDArray<T>::wrap(...)`), and `dim1()`/`dim2()` are
  0/1-indexed (v1's were off by one). Not necessarily cppy3-related if
  it's an unrelated type's method of the same name -- the script flags
  every occurrence for you to check.
* **`assert(...)` calls mentioning `cppy3::`** -- v1 used `assert()` for
  invalid input (compiled out under `NDEBUG`); v2 throws `cppy3::Error`
  instead. Replace with a `try`/`catch` if you need to handle the failure,
  or simply remove the assert if letting the exception propagate is fine.

### Not detectable by the script

These have no regex signature reliable enough to flag automatically; be
aware of them regardless when migrating by hand.

* **`Var::operator PyObject*()`** -- removed. Anywhere v1 code passed a
  `Var` directly to a `PyObject*`-typed parameter (implicitly), call
  `.get()` explicitly now. This is deliberate: that implicit conversion is
  what let v1's `call()` silently double-decref its arguments.
* **Custom `Var` copy-assignment workarounds** -- v1's `Var` had no
  `operator=`, so some v1 code may have hand-rolled ways to avoid
  triggering the compiler-generated (buggy) one. v2's `Var::operator=` is
  correct; remove the workaround.
* **Raw `PyObject*` handling adjacent to a rewritten call** -- anywhere a
  rule above rewrote a call that used to hand back or accept a raw
  `PyObject*`, double-check neighboring code that manually
  incref'd/decref'd it.

## API map by area

### Interpreter lifecycle

```c++
// v1
cppy3::PythonVM instance;
cppy3::exec("...");

// v2
cppy3::Interpreter interpreter;
cppy3::Namespace ns = interpreter.main();
ns.exec("...");
```

```c++
// v1: register a custom C extension module
cppy3::PythonVM instance("emb", PyInit_emb);

// v2
cppy3::Config config;
config.builtin_modules.push_back({"emb", PyInit_emb});
cppy3::Interpreter interpreter(config);
```

`exec()`/`eval()`/`execScriptFile()`/`import()`/`lookupObject()`/
`lookupCallable()`/`call()`/`createClassInstance()`/`getMainModule()`/
`getMainDict()` are gone as free functions. `exec()`/`eval()`/`exec_file()`
are now `Namespace` methods (`interpreter.main()` for what used to be the
implicit, single, shared namespace); calling and attribute/item lookup are
now plain `Var` operations (`ns.dict()["name"]`, `.attr()`, `operator()`),
so the lookup helpers have no direct replacement -- they're simply no
longer needed.

### Variables

```c++
// v1
cppy3::Main().injectVar<int>("a", 2);
long a;
cppy3::Main().getVar<long>("a", a);

// v2
ns.set("a", 2);
long a = ns.get<long>("a");
```

### Exceptions

```c++
// v1
catch (const cppy3::PythonException &e) {
  e.info.type; e.info.reason; e.info.trace; e.what();
}

// v2
catch (const cppy3::Error &e) {
  e.type_name(); e.message(); e.traceback(); e.what(); e.format();
  e.cause(); // new: the __cause__/__context__ chain, absent from v1 entirely
}
```

### NumPy

```c++
// v1
cppy3::NDArray<double> a(cData, 2, 1);   // copy
cppy3::NDArray<double> b;
b.wrap(cData, 2, 1);                      // wrap

// v2
auto a = cppy3::NDArray<double>::copy(cData, 2, 1);
auto b = cppy3::NDArray<double>::wrap(cData, 2, 1);
```

`create(n)`/`create(n1, n2)` become `create({n})`/`create({n1, n2})` (a
single `initializer_list<Py_ssize_t>` shape, not overloaded int/bool
parameters -- v1's `NDArray(int, int)` constructor was ambiguous with
`create(int, bool)` and silently picked the wrong one).

### Not carried forward

* **`Main`** -- superseded by `Interpreter::main()` returning a
  `Namespace`.
* **`convert()`/`extract()`** -- superseded by `Converter<T>` (specialize
  it for your own types) and `to_var()`/`Var::to<T>()`/`try_to<T>()`.
