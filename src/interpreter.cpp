#include <cppy3/interpreter.hpp>

#include <deque>
#include <fstream>
#include <iterator>

#include <cppy3/utils.hpp>

namespace cppy3
{
  namespace
  {
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

    if (config.home)
    {
      status = PyConfig_SetString(&pyConfig, &pyConfig.home, config.home->wstring().c_str());
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
      throw_status(status);

    if (!config.extra_sys_path.empty())
      append_sys_path(config.extra_sys_path);
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
