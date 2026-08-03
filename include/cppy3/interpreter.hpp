#pragma once

#include <cppy3/pycompat.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cppy3/convert.hpp>
#include <cppy3/error.hpp>
#include <cppy3/libdefs.hpp>
#include <cppy3/var.hpp>

namespace cppy3
{
  // A Python namespace (a dict used as globals/locals) that C++ code can
  // exec()/eval() code against and set()/get() variables in. Replaces v1's
  // free exec()/eval()/execScriptFile() functions, which always ran in the
  // single, shared __main__ dict with no way to isolate one script's
  // globals from another's (bug #25).
  class LIB_API Namespace
  {
  public:
    explicit Namespace(Var dict) : _dict(std::move(dict)) {}

    // Executes statements. filename shows up in tracebacks; v1's exec()
    // always compiled under the literal name "<string>" with no way to
    // override it (bug #24).
    Var exec(std::string_view code, std::string_view filename = "<string>") const;

    // Evaluates a single expression, falling back to exec() for statements
    // (detected via PyErr_ExceptionMatches(PyExc_SyntaxError) on the
    // compile step). v1 detected this by string-comparing the formatted
    // exception type name against "<class 'SyntaxError'>" (bug #20), which
    // cannot distinguish "this looks like a statement" from a genuine
    // SyntaxError in the user's expression -- it silently re-ran the
    // latter through exec() too, hiding the real error behind a
    // differently-worded one.
    Var eval(std::string_view expr) const;

    // Executes a script file, compiled with its real path so tracebacks
    // and __file__ show it instead of "<string>" (bug #24), unlike v1's
    // execScriptFile().
    Var exec_file(const std::filesystem::path &path) const;

    // Converts value via Converter<T> and binds it to `name` in this
    // namespace.
    template <typename T>
    void set(std::string_view name, T &&value) const
    {
      _dict.set_item(name, to_var(std::forward<T>(value)));
    }

    // Looks up `name` in this namespace and converts it via Converter<T>.
    template <typename T>
    [[nodiscard]] T get(std::string_view name) const
    {
      return _dict[name].to<T>();
    }

    [[nodiscard]] const Var &dict() const noexcept { return _dict; }

  private:
    Var _dict;
  };

  // Interpreter startup configuration, built on PyConfig
  // (Py_InitializeFromConfig) rather than environment variables +
  // Py_InitializeEx(0) (v1's PythonVM) -- PyConfig is the only mechanism
  // left for configuring the home directory/program name since CPython
  // 3.12 removed Py_SetPythonHome()/Py_SetProgramName() (bug #26).
  struct Config
  {
    // PYTHONHOME equivalent: the prefix of the Python installation/venv to
    // embed. Leave unset to use the interpreter this program was linked
    // against -- on Windows, where CPython's own auto-detection can come up
    // empty for a host binary that is not python.exe, cppy3 falls back to
    // detect_python_home() (below) rather than failing with "Failed to
    // import encodings module".
    std::optional<std::filesystem::path> home;

    // sys.executable. Leave unset to let CPython calculate it.
    std::optional<std::filesystem::path> executable;

    // argv[0] / sys.argv[0] equivalent, shown in tracebacks etc.
    std::string program_name = "cppy3";

    // Extra directories appended to sys.path after startup -- not a
    // replacement for CPython's own calculated default path list (which is
    // nontrivial to reproduce correctly by hand), just additions to it.
    std::vector<std::filesystem::path> extra_sys_path;

    // sys.argv[1:]. Replaces v1's setArgv(), which dereferenced a null
    // PyConfig* (an immediate crash) and never called
    // Py_InitializeFromConfig() even when fixed to not crash, making it a
    // no-op either way (bug #3).
    std::vector<std::string> argv;

    // Custom C extension modules to register before the interpreter starts
    // (PyImport_AppendInittab), e.g. for exposing C++ callables -- see
    // make_function() in function.hpp.
    std::vector<std::pair<std::string, PyObject *(*)()>> builtin_modules;

    bool isolated = false;             // PEP 432 isolated mode
    bool install_signal_handlers = false;
    bool write_bytecode = false;       // .pyc generation
  };

  // Owns the CPython interpreter's lifetime: constructing it calls
  // Py_InitializeFromConfig(), destructing it calls Py_Finalize(). Every
  // Var/Namespace obtained from it must not outlive it. Replaces v1's
  // PythonVM, which:
  //   - configured almost nothing (setenv() + Py_InitializeEx(0) -- #26),
  //   - forced UTF-8 stdio only on Windows, leaving POSIX to whatever the
  //     ambient locale happened to be -- found auditing this rewrite, a
  //     Unicode print() throws UnicodeEncodeError under LC_ALL=C (#27b),
  //   - was silently copyable, risking a double Py_Finalize() (#28),
  //   - tried to import the long-removed "dummy_threading" module in its
  //     destructor, for no discernible reason, swallowing the resulting
  //     ImportError either way (#17).
  //
  // Non-copyable and non-movable: there is exactly one CPython interpreter
  // per process in the normal (non-subinterpreter) embedding model.
  class LIB_API Interpreter
  {
  public:
    explicit Interpreter(const Config &config = {});
    ~Interpreter();

    Interpreter(const Interpreter &) = delete;
    Interpreter &operator=(const Interpreter &) = delete;
    Interpreter(Interpreter &&) = delete;
    Interpreter &operator=(Interpreter &&) = delete;

    [[nodiscard]] static bool is_initialized() noexcept;

    // The __main__ module's namespace -- the closest equivalent to where
    // v1's free exec()/eval() ran.
    [[nodiscard]] Namespace main() const;

    // A fresh namespace (an empty dict seeded with __builtins__), not tied
    // to any real module and isolated from __main__ and every other
    // namespace (bug #25).
    [[nodiscard]] Namespace new_namespace() const;

    // Imports a module and returns it as a Var.
    [[nodiscard]] Var import(std::string_view module_name) const;

    // Appends to sys.path (skipping paths already present). v1 exposed
    // this as appendToSysPath(); ported here since it operates on the
    // interpreter, not any particular namespace.
    void append_sys_path(const std::vector<std::filesystem::path> &paths) const;

    // Redirects sys.stdout/sys.stderr to the given callback, invoked once
    // per write() with the text written. Pass an empty std::function to
    // restore the real stream. v1 had no way to intercept output at all
    // (bug #27) -- print() and friends went straight to the process's own
    // stdio.
    void set_stdout_hook(std::function<void(std::string_view)> hook) const;
    void set_stderr_hook(std::function<void(std::string_view)> hook) const;
  };

  // Best-effort location of the Python installation prefix (the directory
  // whose Lib/ holds the standard library), determined without starting the
  // interpreter. Tried in order: the directory of the pythonXY library
  // loaded into this process, this process's own executable directory, the
  // installation cppy3 was built against, and -- on Windows -- the X.Y
  // install path recorded in the registry. Each candidate is searched
  // towards the filesystem root for CPython's own stdlib landmarks, and
  // returned only if one is found; std::nullopt means none of them held a
  // usable standard library.
  //
  // Interpreter calls this on Windows when Config::home is unset, since
  // CPython's equivalent search has no compile-time prefix to fall back on
  // there. Exposed because embedders that need the answer for their own
  // reasons (bundling, diagnostics, spawning a matching python.exe) would
  // otherwise have to reimplement it.
  [[nodiscard]] LIB_API std::optional<std::filesystem::path> detect_python_home();

  // True if the interpreter is running and a Python exception is currently
  // set (but not yet fetched/thrown). Mostly useful in tests and to check
  // that a caught cppy3::Error actually consumed the interpreter's error
  // state, since throw_if_error() clears it as a side effect.
  LIB_API bool error() noexcept;

  // Sends a KeyboardInterrupt to the running interpreter.
  LIB_API void interrupt();
}
