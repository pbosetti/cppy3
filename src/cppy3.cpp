#include <cppy3/cppy3.hpp>

#include <cassert>
#include <cstdlib>
#include <sstream>
#include <iostream>
#include <fstream>
#include <streambuf>

#include <cppy3/utils.hpp>

#ifdef _WIN32
int setenv(const char *name, const char *value, int overwrite)
{
    int errcode = 0;
    if(!overwrite) {
        size_t envsize = 0;
        errcode = getenv_s(&envsize, NULL, 0, name);
        if(errcode || envsize) return errcode;
    }
    return _putenv_s(name, value);
}
#endif

namespace cppy3
{

  PythonVM::PythonVM()
  {

    setenv("PYTHONDONTWRITEBYTECODE", "1", 0);

#ifdef _WIN32
    // force utf-8 on windows
    setenv("PYTHONIOENCODING", "UTF-8", 0);
#endif

    // create CPython instance without registering signal handlers
    Py_InitializeEx(0);
  }

  PythonVM::PythonVM(const std::string &name, ModuleInitializer module)
  {

    setenv("PYTHONDONTWRITEBYTECODE", "1", 0);

#ifdef _WIN32
    // force utf-8 on windows
    setenv("PYTHONIOENCODING", "UTF-8", 0);
#endif

    // register the module
    PyImport_AppendInittab(name.c_str(), module);

    // create CPython instance without registering signal handlers
    Py_InitializeEx(0);
  }

  PythonVM::~PythonVM()
  {
    if (!PyImport_AddModule("dummy_threading"))
    {
      PyErr_Clear();
    }
    Py_Finalize();
  }

  void setArgv(const std::list<std::wstring> &argv)
  {
    GILLocker lock;
    PyConfig *config = NULL;
    PyConfig_InitPythonConfig(config);

    std::vector<const wchar_t *> cargv;
    for (std::list<std::wstring>::const_iterator it = argv.begin(); it != argv.end(); ++it)
    {
      cargv.push_back(it->data());
    }
    PyConfig_SetArgv(config, argv.size(), (wchar_t **)(&cargv[0]));
    PyConfig_Read(config);
    // PySys_SetArgvEx(argv.size(), (wchar_t **)(&cargv[0]), 0);
  }

  Var createClassInstance(const std::wstring &callable)
  {
    GILLocker lock;
    Var instance = Var::steal(call(lookupCallable(getMainModule(), callable).get()));
    rethrowPythonException();
    if (instance.is_none())
    {
      std::wstringstream ss;
      ss << L"error instantiating '" << callable << "': " << getErrorObject().toString();
      throw PythonException(ss.str());
    }
    return instance;
  }

  void appendToSysPath(const std::vector<std::wstring> &paths)
  {
    GILLocker lock;

    Var sys = import("sys");
    List sysPath{lookupObject(sys.get(), L"path")};
    for (auto path : paths)
    {
      Var pyPath = to_var(path);
      if (!sysPath.contains(pyPath))
      {
        // append into the 'sys.path'
        sysPath.append(pyPath);
      }
    }
  }

  void interrupt()
  {
    PyErr_SetInterrupt();
  }

  LIB_API Var exec(const char *pythonScript)
  {
    GILLocker lock;
    PyObject *mainDict = getMainDict();
    Var result = Var::steal(PyRun_String(pythonScript, Py_file_input, mainDict, mainDict));
    if (!result)
    {
      rethrowPythonException();
    }
    return result;
  }

  LIB_API Var eval(const char *pythonScript)
  {
    GILLocker lock;
    PyObject *mainDict = getMainDict();
    Var result = Var::steal(PyRun_String(pythonScript, Py_eval_input, mainDict, mainDict));
    if (!result)
    {
      const PyExceptionData excData = getErrorObject(false);
      if (excData.type == L"<class 'SyntaxError'>")
      {
        // eval() throws SyntaxError when called with expressions
        // use exec() for expressions
        getErrorObject(true);
        return exec(pythonScript);
      }
      else
      {
        rethrowPythonException();
      }
    }
    return result;
  }

  LIB_API Var exec(const std::string &pythonScript)
  {
    return exec(pythonScript.data());
  }

  LIB_API Var exec(const std::wstring &pythonScript)
  {
    ScopedGILLock lock;

    // encode unicode std::wstring to utf8
    std::wstring script = L"# -*- coding: utf-8 -*-\n";
    script += pythonScript;
    return exec(WideToUTF8(script).c_str());
  }

  LIB_API Var execScriptFile(const std::string &path)
  {
    std::ifstream t(path);

    if (!t.is_open())
    {
      throw PythonException(L"cannot open file " + UTF8ToWide(path));
    }

    std::string script((std::istreambuf_iterator<char>(t)),
                       std::istreambuf_iterator<char>());
    return exec(script.c_str());
  }

  LIB_API bool error()
  {
    GILLocker lock;
    return Py_IsInitialized() && PyErr_Occurred();
  }

  LIB_API void rethrowPythonException()
  {
    if (error())
    {
      const PyExceptionData excData = getErrorObject(true);
      throw PythonException(excData);
    }
  }

  std::wstring pyUnicodeToWstring(PyObject *object)
  {
    std::wstring result;
    if (PyUnicode_Check(object))
    {
      PyObject *bytes = PyUnicode_AsEncodedString(object, "UTF-8", "strict"); // Owned reference
      if (bytes != NULL)
      {
        char *utf8String = PyBytes_AS_STRING(bytes); // Borrowed pointer
        result = UTF8ToWide(std::string(utf8String));
        Py_DECREF(bytes);
      }
    }
    return result;
  }

  LIB_API PyExceptionData getErrorObject(const bool clearError)
  {
    GILLocker lock;
    std::wstring exceptionType;
    std::wstring exceptionMessage;
    std::vector<std::wstring> exceptionTrace;
    if (PyErr_Occurred())
    {
      // get error context
      PyObject *excType = NULL;
      PyObject *excValue = NULL;
      PyObject *excTraceback = NULL;
      PyErr_Fetch(&excType, &excValue, &excTraceback);
      PyErr_NormalizeException(&excType, &excValue, &excTraceback);

      // get traceback module
      PyObject *name = PyUnicode_FromString("traceback");
      PyObject *tracebackModule = PyImport_Import(name);
      Py_DECREF(name);

      // write text type of exception
      exceptionType = pyUnicodeToWstring(PyObject_Str(excType));

      // write text message of exception
      exceptionMessage = pyUnicodeToWstring(PyObject_Str(excValue));

      if (excTraceback != NULL && tracebackModule != NULL)
      {
        // get traceback.format_tb() function ptr
        PyObject *tbDict = PyModule_GetDict(tracebackModule);
        PyObject *format_tbFunc = PyDict_GetItemString(tbDict, "format_tb");
        if (format_tbFunc && PyCallable_Check(format_tbFunc))
        {
          // build argument
          PyObject *excTbTupleArg = PyTuple_New(1);
          PyTuple_SetItem(excTbTupleArg, 0, excTraceback);
          Py_INCREF(excTraceback); // because PyTuple_SetItem() steals reference
          // call traceback.format_tb(excTraceback)
          PyObject *list = PyObject_CallObject(format_tbFunc, excTbTupleArg);
          if (list != NULL)
          {
            // parse list and extract traceback text lines
            const int len = PyList_Size(list);
            for (int i = 0; i < len; i++)
            {
              PyObject *tt = PyList_GetItem(list, i);
              PyObject *t = Py_BuildValue("(O)", tt);
              char *buffer = NULL;
              if (PyArg_ParseTuple(t, "s", &buffer))
              {
                exceptionTrace.push_back(UTF8ToWide(buffer));
              }
              Py_XDECREF(t);
            }
            Py_DECREF(list);
          }
          Py_XDECREF(excTbTupleArg);
        }
      }
      Py_XDECREF(tracebackModule);

      if (clearError)
      {
        Py_XDECREF(excType);
        Py_XDECREF(excValue);
        Py_XDECREF(excTraceback);
      }
      else
      {
        PyErr_Restore(excType, excValue, excTraceback);
      }
    }
    return PyExceptionData(exceptionType, exceptionMessage, exceptionTrace);
  }

  LIB_API Var import(const char *moduleName, PyObject *globals, PyObject *locals)
  {
    Var module = Var::steal(PyImport_ImportModuleEx(const_cast<char *>(moduleName), globals, locals, NULL));
    if (!module)
    {
      const PyExceptionData excData = getErrorObject();
      throw PythonException(excData);
    }
    return module;
  }

  LIB_API Var lookupObject(PyObject *module, const std::wstring &name)
  {
    std::wstring temp;
    std::vector<std::wstring> items;
    std::wstringstream wss(name);
    while (std::getline(wss, temp, L'.'))
      items.push_back(temp);

    Var p = Var::borrow(module);
    std::string itemName;
    for (auto it = items.begin(); it != items.end() && p; ++it)
    {
      itemName = WideToUTF8(*it);
      if (PyDict_Check(p.get()))
      {
        p = Var::borrow(PyDict_GetItemString(p.get(), itemName.data()));
      }
      else
      {
        // PyObject_GetAttrString returns new reference
        p = Var::steal(PyObject_GetAttrString(p.get(), itemName.data()));
      }

      if (!p)
      {
        std::wstringstream wss;
        wss << L"lookup " << name << L" failed: no item " << UTF8ToWide(itemName);
        throw PythonException(wss.str());
      }
    }
    return p;
  }

  LIB_API Var lookupCallable(PyObject *module, const std::wstring &name)
  {
    Var p = lookupObject(module, name);

    if (!p.callable())
    {
      std::wstringstream wss;
      wss << L"PyObject " << name << L" is not callable";
      throw PythonException(wss.str());
    }

    return p;
  }

  LIB_API PyObject *call(PyObject *callable, const arguments &args)
  {
    assert(callable);
    if (!PyCallable_Check(callable))
    {
      std::wstringstream wss;
      wss << L"PyObject " << callable << L" is not callable";
      throw PythonException(wss.str());
    }

    PyObject *result = NULL;
    Var argsTuple;
    const size_t argsCount = args.size();
    if (argsCount > 0)
    {
      argsTuple = Var::steal(PyTuple_New(argsCount));
      for (size_t i = 0; i < argsCount; i++)
      {
        // PyTuple_SetItem steals the reference it's given; args[i].get() is
        // a borrowed pointer (the caller's `arguments` vector still owns
        // it), so it must be incref'd first. v1 passed the borrowed
        // pointer straight through, silently double-decref'ing every
        // argument once the tuple and the caller's vector both released it
        // (plan bug #2).
        PyObject *item = args[i].get();
        Py_XINCREF(item);
        PyTuple_SetItem(argsTuple.get(), static_cast<Py_ssize_t>(i), item);
      }
    }

    PyErr_Clear();
    result = PyObject_CallObject(callable, argsTuple.get());
    rethrowPythonException();

    return result;
  }

  LIB_API PyObject *call(const char *callable, const arguments &args)
  {
    return call(lookupCallable(getMainModule(), UTF8ToWide(callable)).get(), args);
  }

  LIB_API PyObject *call(PyObject *callable) { return call(callable, arguments()); }

  LIB_API PyObject *call(const char *callable) { return call(callable, arguments()); }

  LIB_API GILLocker::GILLocker() : _locked(false)
  {
    // autolock GIL in scoped_lock style
    lock();
  }

  LIB_API GILLocker::~GILLocker()
  {
    release();
  }

  LIB_API void GILLocker::release()
  {
    if (_locked)
    {
      assert(Py_IsInitialized());
      PyGILState_Release(_pyGILState);
      _locked = false;
    }
  }

  LIB_API void GILLocker::lock()
  {
    if (!_locked)
    {
      assert(Py_IsInitialized());
      _pyGILState = PyGILState_Ensure();
      _locked = true;
    }
  }

  LIB_API bool GILLocker::isLocked() {
    return PyGILState_Check() == 1;
  }


  LIB_API PyObject *getMainModule()
  {
    PyObject *mainModule = PyImport_AddModule("__main__");
    assert(mainModule);
    return mainModule;
  }

  LIB_API PyObject *getMainDict()
  {
    PyObject *mainDict = PyModule_GetDict(getMainModule());
    assert(mainDict);
    return mainDict;
  }

}
