/**
 * cppy3 -- embed python3 scripting layer into your c++ app in 10 minutes
 *
 * (c) 2018 Dennis Shilko
 *
 * Minimalistic library for embedding python 3 in C++ application.
 * No additional dependencies required.
 * Linux, Windows platforms supported
 *
 * - Convenient rererence-counted holder for PyObject*
 * - Simple wrapper over init/shutdown of interpreter in 1 line of code
 * - Manage GIL with scoped lock/unlock guards
 * - Translate exceptions from Python to to C++ layer
 * - C++ abstractions for list, dict and numpy.ndarray
 *
 */
#pragma once

#include <Python.h>

#include <exception>
#include <string>
#include <list>
#include <vector>

#include "libdefs.hpp"
#include "utils.hpp"
#include "error.hpp"
#include "var.hpp"
#include "convert.hpp"

namespace cppy3
{

  /** forward decl */
  struct PyExceptionData;
  class PythonException;

  /** exec python script from text string */
  LIB_API Var exec(const char *pythonScript);
  LIB_API Var exec(const std::wstring &pythonScript);
  LIB_API Var exec(const std::string &pythonScript);
  LIB_API Var eval(const char *pythonScript);

  /** exec python script file */
  LIB_API Var execScriptFile(const std::string &path);

  /** @return true if python exception occured */
  LIB_API bool error();

  /** Add to sys.path */
  void appendToSysPath(const std::vector<std::wstring> &paths);

  /** Make instance of a class */
  Var createClassInstance(const std::wstring &callable);

  /** Send ctrl-c */
  void interrupt();

  /** Set sys.argv */
  void setArgv(const std::list<std::wstring> &argv);

  /**
   * @returns pointer to object of root python module __main__
   * can be reached with PyImport_AddModule("__main__")
   * or PyDict_GetItemString(PyImport_GetModuleDict(), "__main__")
   * either way is right
   */
  LIB_API PyObject *getMainModule();
  LIB_API PyObject *getMainDict();

  /**
   * writes python error output to object
   * @param clearError - if true function will reset python error status before exit
   * @return python exception info and traceback in object
   */
  LIB_API PyExceptionData getErrorObject(bool clearError = false);

  /** throws c++ exception if python exception occured */
  LIB_API void rethrowPythonException();

  /** import python module into given context */
  LIB_API Var import(const char *moduleName, PyObject *globals = NULL, PyObject *locals = NULL);

  /** call python callable object and return result */
  typedef std::vector<Var> arguments;
  LIB_API PyObject *call(PyObject *callable, const arguments &args);
  LIB_API PyObject *call(PyObject *callable);
  LIB_API PyObject *call(const char *callable, const arguments &args);
  LIB_API PyObject *call(const char *callable);

  /** get reference to an object in python's namespace */
  LIB_API Var lookupObject(PyObject *module, const std::wstring &name);
  LIB_API Var lookupCallable(PyObject *module, const std::wstring &name);

  /**
   * Tiny wrapper over CPython interpreter instance
   * to manage init/shutdown and expose api in most simple way
   */
  class PythonVM
  {
  public:
    typedef PyObject*(*ModuleInitializer)();

    PythonVM();
    PythonVM(const std::string &name, ModuleInitializer module);
    ~PythonVM();
  };

  struct PyExceptionData
  {
    std::wstring type;
    std::wstring reason;
    std::vector<std::wstring> trace;

    explicit PyExceptionData(const std::wstring &reason = std::wstring()) throw() : reason(reason) {}
    explicit PyExceptionData(const std::wstring &type, const std::wstring &reason, const std::vector<std::wstring> &trace) throw() : type(type), reason(reason), trace(trace) {}

    bool isEmpty() const throw()
    {
      return type.empty() && reason.empty();
    }

    std::wstring toString() const
    {
      std::wstring traceText;
      for (auto t : trace)
      {
        traceText += t + L'\n';
      }
      return isEmpty() ? std::wstring() : type + L"\n" + reason + L"\n" + traceText;
    }
  };

  class PythonException : public std::exception
  {
  public:
    PythonException(const PyExceptionData &info_) : info(info_), _what(WideToUTF8(info.toString())) {}
    PythonException(const std::wstring &reason) : info(reason), _what(WideToUTF8(info.toString())) {}
    ~PythonException() throw() {}

    const char *what() const throw()
    {
      return _what.c_str();
    }
    const PyExceptionData info;

  private:
    const std::string _what;
  };

  /**
   * GIL state scoped-lock
   * can be used recursively (like recursive mutex)
   */
  class LIB_API GILLocker
  {
  public:
    GILLocker();
    ~GILLocker();

    /** Check if the current thread is holding the GIL */
    static bool isLocked();

  private:
    void lock();
    void release();
    bool _locked;
    PyGILState_STATE _pyGILState;
  };

  /**
   * @brief The Scoped GIL unlocker
   */
  class ScopedGILRelease
  {
  public:
    ScopedGILRelease()
    {
      _threadState = PyEval_SaveThread();
    }

    ~ScopedGILRelease()
    {
      PyEval_RestoreThread(_threadState);
    }

  private:
    PyThreadState *_threadState;
  };

  /**
   * @brief The Scoped GIL locker
   */
  class ScopedGILLock
  {
  public:
    ScopedGILLock()
    {
      _state = PyGILState_Ensure();
    }

    ~ScopedGILLock()
    {
      PyGILState_Release(_state);
    }

  private:
    PyGILState_STATE _state;
  };

} // namespace
