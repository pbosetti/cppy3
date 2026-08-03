#include <cppy3/error.hpp>

namespace cppy3
{
  namespace
  {
    std::string pyUnicodeToUtf8(PyObject *obj)
    {
      if (!obj)
        return {};
      Py_ssize_t size = 0;
      const char *data = PyUnicode_AsUTF8AndSize(obj, &size);
      if (!data)
      {
        PyErr_Clear();
        return {};
      }
      return std::string(data, static_cast<size_t>(size));
    }

    std::string formatTraceback(PyObject *tracebackObj)
    {
      if (!tracebackObj)
        return {};

      PyObject *tbModuleName = PyUnicode_FromString("traceback");
      PyObject *tbModule = tbModuleName ? PyImport_Import(tbModuleName) : nullptr;
      Py_XDECREF(tbModuleName);
      if (!tbModule)
      {
        PyErr_Clear();
        return {};
      }

      PyObject *lines = PyObject_CallMethod(tbModule, "format_tb", "O", tracebackObj);
      Py_DECREF(tbModule);
      if (!lines)
      {
        PyErr_Clear();
        return {};
      }

      std::string text;
      const Py_ssize_t n = PyList_Size(lines);
      for (Py_ssize_t i = 0; i < n; i++)
      {
        text += pyUnicodeToUtf8(PyList_GetItem(lines, i)); // borrowed ref
      }
      Py_DECREF(lines);
      return text;
    }

    std::string exceptionTypeName(PyObject *excValue)
    {
      PyObject *type = PyObject_Type(excValue); // new ref
      PyObject *nameObj = type ? PyObject_GetAttrString(type, "__name__") : nullptr;
      std::string name = nameObj ? pyUnicodeToUtf8(nameObj) : std::string("Exception");
      Py_XDECREF(nameObj);
      Py_XDECREF(type);
      return name;
    }

    std::string exceptionMessage(PyObject *excValue)
    {
      PyObject *str = PyObject_Str(excValue); // new ref
      std::string msg = str ? pyUnicodeToUtf8(str) : std::string();
      Py_XDECREF(str);
      return msg;
    }

    // Builds an Error (with its __cause__/__context__ chain) from a
    // normalized, non-null exception instance. Does not consume or clear
    // any interpreter-global error state; the caller retains ownership of
    // `excValue` and `tracebackObj`.
    Error buildError(PyObject *excValue, PyObject *tracebackObj)
    {
      PyObject *cause = PyException_GetCause(excValue);                       // new ref, may be NULL
      PyObject *context = cause ? nullptr : PyException_GetContext(excValue); // new ref, may be NULL
      PyObject *chained = cause ? cause : context;

      std::unique_ptr<Error> chainedErr;
      if (chained && chained != Py_None)
      {
        PyObject *chainedTb = PyException_GetTraceback(chained); // new ref, may be NULL
        chainedErr = std::make_unique<Error>(buildError(chained, chainedTb));
        Py_XDECREF(chainedTb);
      }
      Py_XDECREF(cause);
      Py_XDECREF(context);

      return Error(exceptionTypeName(excValue), exceptionMessage(excValue),
                   formatTraceback(tracebackObj), std::move(chainedErr));
    }
  }

  Error::Error(std::string type_name, std::string message, std::string traceback, std::unique_ptr<Error> cause)
      : std::runtime_error(type_name.empty() ? message : type_name + ": " + message),
        _type_name(std::move(type_name)),
        _message(std::move(message)),
        _traceback(std::move(traceback)),
        _cause(std::move(cause))
  {
  }

  Error::Error(std::string message)
      : std::runtime_error(message),
        _message(std::move(message))
  {
  }

  Error::Error(const Error &other)
      : std::runtime_error(other),
        _type_name(other._type_name),
        _message(other._message),
        _traceback(other._traceback),
        _cause(other._cause ? std::make_unique<Error>(*other._cause) : nullptr)
  {
  }

  Error &Error::operator=(const Error &other)
  {
    if (this != &other)
    {
      std::runtime_error::operator=(other);
      _type_name = other._type_name;
      _message = other._message;
      _traceback = other._traceback;
      _cause = other._cause ? std::make_unique<Error>(*other._cause) : nullptr;
    }
    return *this;
  }

  std::string Error::format() const
  {
    std::string result;
    if (!_traceback.empty())
    {
      result += "Traceback (most recent call last):\n";
      result += _traceback;
    }
    result += _type_name.empty() ? _message : _type_name + ": " + _message;
    if (_cause)
    {
      result += "\n\nThe above exception was the direct cause of the following exception:\n\n" +
                _cause->format();
    }
    return result;
  }

  void throw_if_error()
  {
    if (!PyErr_Occurred())
      return;

    PyObject *excValue;
    PyObject *tracebackObj;

#if PY_VERSION_HEX >= 0x030C0000
    excValue = PyErr_GetRaisedException(); // new ref, normalized
    tracebackObj = excValue ? PyException_GetTraceback(excValue) : nullptr; // new ref
#else
    PyObject *excType = nullptr;
    excValue = nullptr;
    tracebackObj = nullptr;
    PyErr_Fetch(&excType, &excValue, &tracebackObj);
    PyErr_NormalizeException(&excType, &excValue, &tracebackObj);
    if (tracebackObj)
      PyException_SetTraceback(excValue, tracebackObj);
    Py_XDECREF(excType);
#endif

    Error err = buildError(excValue, tracebackObj);
    Py_XDECREF(excValue);
    Py_XDECREF(tracebackObj);
    throw err;
  }
}
