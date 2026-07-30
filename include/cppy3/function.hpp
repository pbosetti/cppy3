#pragma once

#include <Python.h>

#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include <cppy3/convert.hpp>
#include <cppy3/error.hpp>
#include <cppy3/var.hpp>

namespace cppy3
{
  namespace detail
  {
    // Owns everything a PyCFunction object needs for its entire lifetime:
    // the C++ callable, and the PyMethodDef pointing back at it (whose
    // ml_name must remain valid for as long as the function object does --
    // this is the same "who keeps this string alive" hazard as
    // PyImport_AppendInittab's name parameter, see Phase 5's Config::
    // builtin_modules fix). A PyCapsule storing this struct is passed as
    // the PyCFunction's `self`; PyCFunction_New() increfs it, so it (and
    // everything it owns) stays alive exactly as long as the Python
    // function object does, and the capsule's destructor below frees it
    // when that function object is finally garbage-collected.
    template <typename R, typename... Args>
    struct FunctionCapsuleData
    {
      std::string name;
      std::function<R(Args...)> fn;
      PyMethodDef method_def{};
    };

    template <typename R, typename... Args, size_t... Is>
    PyObject *invoke_converted(const std::function<R(Args...)> &fn, PyObject *argsTuple, std::index_sequence<Is...>)
    {
      if constexpr (std::is_void_v<R>)
      {
        fn(Converter<std::remove_cvref_t<Args>>::from_python(Var::borrow(PyTuple_GET_ITEM(argsTuple, Is)))...);
        Py_RETURN_NONE;
      }
      else
      {
        PyObject *result = Converter<R>::to_python(
            fn(Converter<std::remove_cvref_t<Args>>::from_python(Var::borrow(PyTuple_GET_ITEM(argsTuple, Is)))...));
        if (!result)
          throw_if_error();
        return result;
      }
    }

    template <typename R, typename... Args>
    PyObject *function_trampoline(PyObject *self, PyObject *args)
    {
      auto *data = static_cast<FunctionCapsuleData<R, Args...> *>(PyCapsule_GetPointer(self, "cppy3::function"));
      if (!data)
        return nullptr; // PyCapsule_GetPointer already set an error

      const auto expected = static_cast<Py_ssize_t>(sizeof...(Args));
      if (PyTuple_GET_SIZE(args) != expected)
      {
        PyErr_Format(PyExc_TypeError, "%s() takes %zd positional argument(s) but %zd were given", data->name.c_str(),
                     expected, PyTuple_GET_SIZE(args));
        return nullptr;
      }

      try
      {
        return invoke_converted<R, Args...>(data->fn, args, std::index_sequence_for<Args...>{});
      }
      catch (const std::exception &e)
      {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
      }
    }

    template <typename R, typename... Args>
    void destroy_function_capsule(PyObject *capsule)
    {
      delete static_cast<FunctionCapsuleData<R, Args...> *>(PyCapsule_GetPointer(capsule, "cppy3::function"));
    }
  }

  // Wraps a C++ callable as a Python callable, converting arguments and the
  // return value via Converter<T> -- v1 had no way to expose a C++
  // function to Python short of hand-writing a PyMethodDef/PyCFunction
  // table (bug #36; examples/console.cpp's "emb" module still does this by
  // hand, for METH_KEYWORDS and module-init cases this function doesn't
  // cover).
  template <typename R, typename... Args>
  [[nodiscard]] Var make_function(std::string name, std::function<R(Args...)> fn)
  {
    auto *data = new detail::FunctionCapsuleData<R, Args...>{std::move(name), std::move(fn), {}};
    data->method_def = PyMethodDef{data->name.c_str(), detail::function_trampoline<R, Args...>, METH_VARARGS, nullptr};

    Var capsule = Var::steal(PyCapsule_New(data, "cppy3::function", detail::destroy_function_capsule<R, Args...>));
    if (!capsule)
    {
      delete data;
      throw_if_error();
    }

    PyObject *func = PyCFunction_New(&data->method_def, capsule.get());
    if (!func)
      throw_if_error();
    return Var::steal(func);
  }

  // Overload for anything convertible to std::function (function pointers,
  // non-generic lambdas, functors with a single operator()) -- deduces
  // R/Args... via std::function's class template argument deduction guide.
  template <typename F>
  [[nodiscard]] auto make_function(std::string name, F &&fn)
      -> decltype(make_function(std::move(name), std::function(std::forward<F>(fn))))
  {
    return make_function(std::move(name), std::function(std::forward<F>(fn)));
  }
}
