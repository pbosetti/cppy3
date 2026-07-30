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
| `cppy3::Main()` | `interpreter.main()` |
| `Var::from(x)` / `.newRef(x)` | `Var::steal(x)` |
| `.reset(x)` (on a `Var`) | `Var::borrow(x)` |
| `.toUTF8String()` | `.str()` |
| `.toLong()` | `.to<long>()` |
| `.toDouble()` | `.to<double>()` |
| `.typeName()` | `.type_name()` |
| `e.info.reason` | `e.message()` |
| `e.info.type` | `e.type_name()` |
| `e.info.trace` | `e.traceback()` |
| `cppy3::GILLocker::isLocked()` | `cppy3::gil_held()` |

## Assisted renames

The script rewrites these too, but the change alters behavior -- verify
the surrounding code after applying it.

| v1 | v2 | What changes |
| --- | --- | --- |
| `getVar<T>(name, out)` | `out = ns.get<T>(name)` | Returns the value instead of writing through an out-parameter. |
| `call(f, args)` | `f(args...)` | Now returns an owning `Var`. If your v1 code manually `Py_DECREF`'d the raw `PyObject*` `call()` returned, **delete that decref** -- it would now double-free. |
| `execScriptFile(path)` | `ns.exec_file(path)` | Tracebacks and `__file__` now show the real path instead of `"<string>"`. |
| `L"..."` wide string literals (script source) | UTF-8 `"..."` | `exec()`/`eval()` are UTF-8-only in v2; there's no wide-string overload for source code (there still is for data going through `Converter<std::wstring>`). |

## Manual review required

The script flags these; it does not rewrite them.

* **`Var::operator PyObject*()`** -- removed. Anywhere v1 code passed a
  `Var` directly to a `PyObject*`-typed parameter (implicitly), call
  `.get()` explicitly now. This is deliberate: that implicit conversion is
  what let v1's `call()` silently double-decref its arguments.
* **`setArgv()`** -- removed (it never worked: it dereferenced a null
  `PyConfig*`). Use `Config::argv` when constructing the `Interpreter`.
* **Custom `Var` copy-assignment workarounds** -- v1's `Var` had no
  `operator=`, so some v1 code may have hand-rolled ways to avoid triggering
  the compiler-generated (buggy) one. v2's `Var::operator=` is correct;
  remove the workaround.
* **`assert()` calls guarding cppy3 operations** -- v1 used `assert()` for
  invalid input (compiled out under `NDEBUG`); v2 throws `cppy3::Error`
  instead. Replace `assert(cppy3_call_that_might_fail())`-style guards with
  a `try`/`catch` if you need to handle the failure, or simply remove the
  assert if letting the exception propagate is fine.
* **Direct `NDArray::wrap()`/`dim1()`/`dim2()` use** -- v1's 1D `wrap()`
  aliased the wrong memory entirely, and `dim1()`/`dim2()` were off by one.
  If your code compensated for either bug, remove the compensation; v2's
  versions are correct.
* **Raw `PyObject*` handling adjacent to a rewritten call** -- anywhere the
  script rewrote a call that used to hand back or accept a raw
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
