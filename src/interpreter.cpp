#include <cppy3/interpreter.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <fstream>
#include <iterator>
#include <system_error>

#include <cppy3/cppy3_build_config.h>
#include <cppy3/utils.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cppy3
{
  namespace
  {
    namespace fs = std::filesystem;

    // --- locating the Python installation without starting the interpreter ---
    //
    // These mirror the landmarks CPython's own Modules/getpath.py looks for
    // (STDLIB_LANDMARKS and ZIP_LANDMARK) so that detect_python_home() agrees
    // with CPython wherever CPython manages on its own, and only differs
    // where CPython gives up.
    bool holds_stdlib(const fs::path &dir)
    {
      if (dir.empty())
        return false;
      std::error_code ec;
#ifdef _WIN32
      if (fs::is_regular_file(dir / "Lib" / "os.py", ec))
        return true;
      // Embedded distributions ship the stdlib as pythonXY.zip beside the DLL
      // instead of an unpacked Lib directory.
      std::array<char, 32> zip{};
      std::snprintf(zip.data(), zip.size(), "python%d%d.zip", PY_MAJOR_VERSION, PY_MINOR_VERSION);
      return fs::is_regular_file(dir / zip.data(), ec);
#else
      std::array<char, 32> stdlibDir{};
      std::snprintf(stdlibDir.data(), stdlibDir.size(), "python%d.%d", PY_MAJOR_VERSION, PY_MINOR_VERSION);
      return fs::is_regular_file(dir / "lib" / stdlibDir.data() / "os.py", ec);
#endif
    }

    // getpath.py's search_up(): walk towards the filesystem root looking for
    // the stdlib landmark, as CPython does from its executable's directory.
    std::optional<fs::path> search_up_for_stdlib(fs::path dir)
    {
      while (!dir.empty())
      {
        if (holds_stdlib(dir))
          return dir;
        fs::path parent = dir.parent_path();
        if (parent == dir) // reached the root
          break;
        dir = std::move(parent);
      }
      return std::nullopt;
    }

#ifdef _WIN32
    std::optional<fs::path> module_path(HMODULE module)
    {
      std::wstring buffer(MAX_PATH, L'\0');
      for (;;)
      {
        const DWORD written = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0)
          return std::nullopt;
        if (written < buffer.size())
        {
          buffer.resize(written);
          return fs::path(buffer);
        }
        if (buffer.size() > 32768) // longest path Windows will ever hand back
          return std::nullopt;
        buffer.resize(buffer.size() * 2);
      }
    }
#endif

    // Full path of the pythonXY.dll this process is actually running against
    // -- not necessarily the one CMake found at build time, and (as observed
    // on GitHub's windows-latest runners) not necessarily one that sits in
    // its own installation's prefix.
    std::optional<fs::path> libpython_path()
    {
#ifdef _WIN32
      HMODULE module = nullptr;
      // Any address inside the DLL identifies it; Py_IsInitialized is exported
      // by every CPython build and, unlike a data symbol, is safe to take the
      // address of before initialization.
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&Py_IsInitialized), &module))
        return std::nullopt;
      return module_path(module);
#else
      return std::nullopt; // POSIX gets its prefix from CPython's PREFIX macro
#endif
    }

    // Full path of the process's own executable (CPython's real_executable).
    std::optional<fs::path> host_executable_path()
    {
#ifdef _WIN32
      return module_path(nullptr);
#else
      return std::nullopt;
#endif
    }

#ifdef _WIN32
    // HKCU\...\PythonCore\X.Y\InstallPath, the location the official Windows
    // installer records. Consulted only after the on-disk searches, since a
    // registry entry can outlive the installation it points at.
    std::optional<fs::path> registry_python_home()
    {
      std::array<wchar_t, 96> subkey{};
      std::swprintf(subkey.data(), subkey.size(), L"Software\\Python\\PythonCore\\%d.%d\\InstallPath",
                    PY_MAJOR_VERSION, PY_MINOR_VERSION);

      for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
      {
        DWORD bytes = 0;
        if (RegGetValueW(root, subkey.data(), nullptr, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
          continue;
        std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
        DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        if (RegGetValueW(root, subkey.data(), nullptr, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS)
          continue;
        value.resize(std::wcslen(value.c_str())); // drop the trailing NUL(s)
        if (!value.empty())
          return fs::path(value);
      }
      return std::nullopt;
    }

    // A pythonXY._pth / <exename>._pth file beside the DLL or the executable
    // takes total control of sys.path calculation (and implies isolated mode).
    // Setting PyConfig::home would silently disable that, so when one is
    // present cppy3 keeps its hands off entirely.
    bool pth_file_present()
    {
      for (const auto &binary : {libpython_path(), host_executable_path()})
      {
        if (!binary)
          continue;
        fs::path pth = *binary;
        pth.replace_extension("._pth");
        std::error_code ec;
        if (fs::is_regular_file(pth, ec))
          return true;
      }
      return false;
    }

    // "Set" in the sense getpath.py means it: present *and* non-empty.
    bool env_var_set(const wchar_t *name)
    {
      const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
      if (needed == 0)
        return false;
      std::wstring value(needed, L'\0');
      const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
      return written != 0;
    }

    // CPython treats a directory holding pyvenv.cfg (beside the interpreter,
    // or one level up) as a virtual environment and reads the real
    // installation's location out of it. That path works on Windows already,
    // so an explicitly configured venv executable must be left alone.
    bool is_venv_executable(const fs::path &executable)
    {
      const fs::path dir = executable.parent_path();
      std::error_code ec;
      return fs::is_regular_file(dir / "pyvenv.cfg", ec) ||
             fs::is_regular_file(dir.parent_path() / "pyvenv.cfg", ec);
    }
#endif

    [[noreturn]] void throw_status(const PyStatus &status)
    {
      std::string msg = status.err_msg ? status.err_msg : "unknown CPython initialization error";
      if (status.func)
        msg = std::string(status.func) + ": " + msg;
      throw Error(msg);
    }

    // --- sys.stdout/sys.stderr redirection ---
    //
    // A minimal Python type whose instances just carry a tag saying which of
    // the two process-wide hook slots to forward write()/flush() calls to.
    // There is exactly one Interpreter per process, so one slot per stream
    // is all that's ever needed.
    //
    // The type itself is created fresh via PyType_FromSpec() every time
    // set_stream() runs, rather than kept as a process-wide static
    // PyTypeObject. A static PyTypeObject's tp_dict/tp_mro/tp_subclasses
    // are populated by PyType_Ready() with objects owned by whichever
    // interpreter was live at the time; Py_Finalize() frees those along
    // with everything else in that interpreter, but never clears
    // Py_TPFLAGS_READY on our own static struct, so a later interpreter
    // cycle's PyType_Ready() call sees "already ready" and skips
    // re-initializing them, leaving the static type holding dangling
    // pointers into a dead interpreter. A subsequent, unrelated allocation
    // eventually reuses that freed memory and corrupts it -- this crashed
    // deep inside an unrelated codec lookup during a later Interpreter's
    // startup, several cycles after the dangling type was created. Heap
    // types created per-call have no such cross-cycle staleness: each one
    // lives and dies with the interpreter it was created in.
    std::function<void(std::string_view)> g_stdoutHook;
    std::function<void(std::string_view)> g_stderrHook;

    struct StreamProxy
    {
      PyObject_HEAD;
      bool is_stderr;
    };

    PyObject *StreamProxy_write(PyObject *self, PyObject *args)
    {
      const char *text = nullptr;
      Py_ssize_t len = 0;
      if (!PyArg_ParseTuple(args, "s#", &text, &len))
        return nullptr;
      const auto &hook = reinterpret_cast<StreamProxy *>(self)->is_stderr ? g_stderrHook : g_stdoutHook;
      if (hook)
        hook(std::string_view(text, static_cast<size_t>(len)));
      return PyLong_FromSsize_t(len);
    }

    PyObject *StreamProxy_flush(PyObject *, PyObject *) { Py_RETURN_NONE; }

    PyMethodDef StreamProxy_methods[] = {
        {"write", StreamProxy_write, METH_VARARGS, nullptr},
        {"flush", StreamProxy_flush, METH_NOARGS, nullptr},
        {nullptr, nullptr, 0, nullptr},
    };

    PyType_Slot StreamProxy_slots[] = {
        {Py_tp_methods, StreamProxy_methods},
        {0, nullptr},
    };

    PyType_Spec StreamProxy_spec = {
        "cppy3._StreamProxy",
        sizeof(StreamProxy),
        0,
        Py_TPFLAGS_DEFAULT,
        StreamProxy_slots,
    };

    void set_stream(const char *sysAttr, bool isStderr, std::function<void(std::string_view)> hook)
    {
      (isStderr ? g_stderrHook : g_stdoutHook) = std::move(hook);

      Var type = Var::steal(PyType_FromSpec(&StreamProxy_spec));
      if (!type)
        throw_if_error();
      auto *proxy = PyObject_New(StreamProxy, reinterpret_cast<PyTypeObject *>(type.get()));
      if (!proxy)
        throw_if_error();
      proxy->is_stderr = isStderr;
      if (PySys_SetObject(sysAttr, reinterpret_cast<PyObject *>(proxy)) != 0)
      {
        Py_DECREF(proxy);
        throw_if_error();
      }
      Py_DECREF(proxy); // PySys_SetObject took its own reference
    }
  }

  std::optional<std::filesystem::path> detect_python_home()
  {
    // 1. Beside (or above) the pythonXY.dll actually loaded into this
    //    process. This is CPython's own first choice, and normally succeeds.
    if (const auto library = libpython_path())
      if (auto home = search_up_for_stdlib(library->parent_path()))
        return home;

    // 2. Beside (or above) this process's executable, for applications that
    //    ship a private copy of the stdlib next to their binary.
    if (const auto executable = host_executable_path())
      if (auto home = search_up_for_stdlib(executable->parent_path()))
        return home;

    // 3. The installation this build of cppy3 links against, recorded at
    //    configure time -- the stand-in for the PREFIX macro CPython bakes
    //    into POSIX builds and getpath.py falls back to there. Validated
    //    rather than trusted, since the build machine's layout need not
    //    survive to the machine that runs the binary.
    if (const fs::path built(CPPY3_BUILD_PYTHON_HOME); holds_stdlib(built))
      return built;

#ifdef _WIN32
    // 4. Whatever the official installer registered for this exact X.Y.
    if (const auto registered = registry_python_home(); registered && holds_stdlib(*registered))
      return registered;
#endif

    return std::nullopt;
  }

  Var Namespace::exec(std::string_view code, std::string_view filename) const
  {
    Var codeObj = Var::steal(Py_CompileString(std::string(code).c_str(), std::string(filename).c_str(), Py_file_input));
    if (!codeObj)
      throw_if_error();
    Var result = Var::steal(PyEval_EvalCode(codeObj.get(), _dict.get(), _dict.get()));
    if (!result)
      throw_if_error();
    return result;
  }

  Var Namespace::eval(std::string_view expr) const
  {
    Var codeObj = Var::steal(Py_CompileString(std::string(expr).c_str(), "<string>", Py_eval_input));
    if (!codeObj)
    {
      if (PyErr_ExceptionMatches(PyExc_SyntaxError))
      {
        PyErr_Clear();
        return exec(expr); // eval() rejects statements; exec() accepts them
      }
      throw_if_error();
    }
    Var result = Var::steal(PyEval_EvalCode(codeObj.get(), _dict.get(), _dict.get()));
    if (!result)
      throw_if_error();
    return result;
  }

  Var Namespace::exec_file(const std::filesystem::path &path) const
  {
    std::ifstream file(path);
    if (!file.is_open())
      throw Error("cannot open file: " + path.string());
    std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    set("__file__", path.string());
    return exec(source, path.string());
  }

  Interpreter::Interpreter(const Config &config)
  {
    // PyImport_AppendInittab() stores the *pointer* it's given in CPython's
    // process-global inittab -- it does not copy the string, and there is
    // no corresponding "remove" API to undo it. That's fine for a string
    // literal (as in examples/console.cpp), but Config::builtin_modules'
    // names are ordinary std::strings that die with the Config object;
    // without interning them somewhere durable, a *later* Interpreter's
    // Py_InitializeFromConfig() walks the inittab, finds a name pointer
    // into an already-freed std::string from a previous Interpreter's
    // Config, and reads freed memory. std::deque (unlike std::vector)
    // never invalidates existing elements' addresses on further
    // push_back(), so interning here is permanent for the process, which
    // is exactly what the raw API already assumes.
    static std::deque<std::string> internedModuleNames;
    for (const auto &[name, initFunc] : config.builtin_modules)
    {
      internedModuleNames.push_back(name);
      PyImport_AppendInittab(internedModuleNames.back().c_str(), initFunc);
    }

    // Force UTF-8 mode (PEP 540) regardless of the ambient process locale --
    // fixes bug #27b (v1 only forced UTF-8 stdio on Windows; under a POSIX
    // "C" locale, printing a Unicode string threw UnicodeEncodeError,
    // masked in v1's own test suite by locale state that leaked in from an
    // unrelated, earlier test case). utf8_mode lives on PyPreConfig, not
    // PyConfig, so it must be set in an explicit pre-initialization step.
    PyPreConfig preConfig;
    if (config.isolated)
      PyPreConfig_InitIsolatedConfig(&preConfig);
    else
      PyPreConfig_InitPythonConfig(&preConfig);
    preConfig.utf8_mode = 1;
    PyStatus status = Py_PreInitialize(&preConfig);
    if (PyStatus_Exception(status))
      throw_status(status);

    PyConfig pyConfig;
    if (config.isolated)
      PyConfig_InitIsolatedConfig(&pyConfig);
    else
      PyConfig_InitPythonConfig(&pyConfig);

    pyConfig.write_bytecode = config.write_bytecode ? 1 : 0;
    pyConfig.install_signal_handlers = config.install_signal_handlers ? 1 : 0;

    std::optional<std::filesystem::path> home = config.home;
#ifdef _WIN32
    // Windows has no compile-time PREFIX for getpath.py to fall back on, so
    // when its searches come up empty it does not fail loudly -- it warns
    // ("Could not find platform independent libraries <prefix>"), sets prefix
    // to the current working directory and carries on until the very next
    // step dies with "Failed to import encodings module". Those searches
    // start from the loaded pythonXY.dll and from this process's executable,
    // which for an embedding application is neither python.exe nor anywhere
    // near the stdlib; whether they find anything is a property of how the
    // host happens to have laid Python out, and on GitHub's windows-latest
    // runners they do not. POSIX embedders never see this because CPython
    // hardcodes its own install prefix at build time and getpath.py falls
    // back to it, so supply the equivalent here and let Windows behave the
    // same way.
    //
    // Only ever a fallback: an explicitly configured home, PYTHONHOME, a
    // ._pth file or a venv all still take precedence, and in the common case
    // where CPython's own detection works, detect_python_home() returns the
    // very directory CPython would have found anyway.
    if (!home && !pth_file_present() && (config.isolated || !env_var_set(L"PYTHONHOME")))
    {
      if (config.executable)
      {
        // A configured executable that is not a venv launcher does not steer
        // prefix detection on Windows the way it does on POSIX: getpath.py
        // derives the directory it searches from real_executable (this
        // process's own binary), not from PyConfig::executable. Search from
        // it explicitly so that documented escape hatch actually works.
        if (!is_venv_executable(*config.executable))
          home = search_up_for_stdlib(config.executable->parent_path());
      }
      else
      {
        home = detect_python_home();
      }
    }
#endif
    if (home)
    {
      status = PyConfig_SetString(&pyConfig, &pyConfig.home, home->wstring().c_str());
      if (PyStatus_Exception(status))
      {
        PyConfig_Clear(&pyConfig);
        throw_status(status);
      }
    }
    if (config.executable)
    {
      status = PyConfig_SetString(&pyConfig, &pyConfig.executable, config.executable->wstring().c_str());
      if (PyStatus_Exception(status))
      {
        PyConfig_Clear(&pyConfig);
        throw_status(status);
      }
    }
    status = PyConfig_SetString(&pyConfig, &pyConfig.program_name, UTF8ToWide(config.program_name).c_str());
    if (PyStatus_Exception(status))
    {
      PyConfig_Clear(&pyConfig);
      throw_status(status);
    }

    if (!config.argv.empty())
    {
      std::vector<std::wstring> wideArgv;
      wideArgv.reserve(config.argv.size() + 1);
      wideArgv.push_back(UTF8ToWide(config.program_name));
      for (const auto &a : config.argv)
        wideArgv.push_back(UTF8ToWide(a));
      std::vector<wchar_t *> cArgv;
      cArgv.reserve(wideArgv.size());
      for (auto &w : wideArgv)
        cArgv.push_back(const_cast<wchar_t *>(w.c_str())); // PyConfig_SetArgv only reads these
      // Embedded argv should just populate sys.argv, not be interpreted as
      // Python's own CLI flags (-c/-m/etc.) -- an unrecognized flag like
      // "--flag" would otherwise make CPython print a usage error and exit.
      pyConfig.parse_argv = 0;
      status = PyConfig_SetArgv(&pyConfig, static_cast<Py_ssize_t>(cArgv.size()), cArgv.data());
      if (PyStatus_Exception(status))
      {
        PyConfig_Clear(&pyConfig);
        throw_status(status);
      }
    }

    status = Py_InitializeFromConfig(&pyConfig);
    PyConfig_Clear(&pyConfig);
    if (PyStatus_Exception(status))
    {
      // A failed Py_InitializeFromConfig() can still leave the runtime
      // partially/technically initialized (CPython's own docs recommend
      // Py_ExitStatusException() here, i.e. terminating the process rather
      // than continuing). We throw instead so a single failed Interpreter
      // construction doesn't take down the whole host process -- but that
      // means, without this Py_Finalize(), every later Interpreter
      // construction in the same process would find Py_IsInitialized()
      // still true and fail loudly at PyImport_AppendInittab() ("...may
      // not be called after Py_Initialize()") or crash outright, even
      // though no live Interpreter object owns that state to clean it up
      // (this object's constructor never returns, so its destructor never
      // runs either). Best-effort recovery, not a guarantee: CPython does
      // not promise a partially-initialized runtime finalizes cleanly.
      if (Py_IsInitialized())
        Py_Finalize();
      throw_status(status);
    }

    try
    {
      if (!config.extra_sys_path.empty())
        append_sys_path(config.extra_sys_path);
    }
    catch (...)
    {
      // Same reasoning as above: initialization itself succeeded, so
      // without this the runtime would stay alive and initialized forever
      // -- this object never finishes constructing, so ~Interpreter() will
      // never call Py_Finalize() for it.
      Py_Finalize();
      throw;
    }
  }

  Interpreter::~Interpreter()
  {
    // No dummy_threading import here (v1 tried to import that
    // long-removed module for no discernible reason and swallowed the
    // resulting ImportError either way -- bug #17).
    Py_Finalize();
  }

  bool Interpreter::is_initialized() noexcept { return Py_IsInitialized() != 0; }

  Namespace Interpreter::main() const
  {
    PyObject *mainModule = PyImport_AddModule("__main__");
    if (!mainModule)
      throw_if_error();
    return Namespace(Var::borrow(PyModule_GetDict(mainModule))); // borrowed
  }

  Namespace Interpreter::new_namespace() const
  {
    Var dict = Var::steal(PyDict_New());
    if (!dict)
      throw_if_error();
    PyObject *builtins = PyEval_GetBuiltins(); // borrowed
    if (PyDict_SetItemString(dict.get(), "__builtins__", builtins) != 0)
      throw_if_error();
    return Namespace(std::move(dict));
  }

  Var Interpreter::import(std::string_view module_name) const
  {
    const std::string name(module_name);
    Var mod = Var::steal(PyImport_ImportModule(name.c_str()));
    if (!mod)
      throw_if_error();
    return mod;
  }

  void Interpreter::append_sys_path(const std::vector<std::filesystem::path> &paths) const
  {
    Var sysModule = Var::steal(PyImport_ImportModule("sys"));
    if (!sysModule)
      throw_if_error();
    List sysPath{sysModule.attr("path")};
    for (const auto &path : paths)
    {
      Var pyPath = to_var(path.string());
      if (!sysPath.contains(pyPath))
        sysPath.append(pyPath);
    }
  }

  void Interpreter::set_stdout_hook(std::function<void(std::string_view)> hook) const
  {
    set_stream("stdout", false, std::move(hook));
  }

  void Interpreter::set_stderr_hook(std::function<void(std::string_view)> hook) const
  {
    set_stream("stderr", true, std::move(hook));
  }

  bool error() noexcept { return Py_IsInitialized() && PyErr_Occurred(); }

  void interrupt() { PyErr_SetInterrupt(); }
}
