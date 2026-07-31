#pragma once

#include <cppy3/pycompat.hpp>

#include <concepts>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <cppy3/error.hpp>
#include <cppy3/utils.hpp>
#include <cppy3/var.hpp>

namespace cppy3
{
  // Converter<T> is the sole extension point for crossing the C++/Python
  // boundary -- specialize it for your own types to make them work
  // everywhere to_var()/Var::to<T>()/try_to<T>() accept a T. This
  // supersedes v1's convert()/extract() free functions, which:
  //   - leaked their result when injectVar() forgot to release it (#11),
  //   - didn't compile for std::vector<T> at all (value.reset(0) on a
  //     vector -- #13),
  //   - rejected a Python int where a C++ double was requested, i.e. no
  //     numeric coercion at all (#32).
  //
  // A specialization provides some subset of:
  //   static PyObject *to_python(const T &value);   // returns a new reference
  //   static T from_python(const Var &value);        // throws Error on mismatch
  // (only one direction is required for write-only or read-only types, e.g.
  // Converter<const char*> has no from_python -- there is no C++ owner to
  // extract into.)
  template <typename T>
  struct Converter; // no generic definition -- specialize per type

  template <typename T>
  concept ConvertibleToPython = requires(const T &v) {
    { Converter<std::remove_cvref_t<T>>::to_python(v) } -> std::same_as<PyObject *>;
  };

  template <typename T>
  concept ConvertibleFromPython = requires(const Var &v) {
    { Converter<T>::from_python(v) } -> std::same_as<T>;
  };

  // Converts a C++ value into an owned Var. Injection primitive underneath
  // Namespace::set<T>() (see the interpreter rework); usable standalone
  // today via Var::set_item()/set_attr(), e.g.
  // ns.set_item("a", cppy3::to_var(2)).
  template <ConvertibleToPython T>
  [[nodiscard]] Var to_var(const T &value)
  {
    return Var::steal(Converter<std::remove_cvref_t<T>>::to_python(value));
  }

  // Non-template overload for `const char*`, defined after
  // Converter<const char*> below (it must be visible first). Needed so
  // string literals bind here directly instead of the generic template
  // deducing T = char[N]: for a deduced-by-reference template parameter,
  // array-to-pointer decay never happens, so to_var("hi") would otherwise
  // look for a nonexistent Converter<char[3]>.
  [[nodiscard]] Var to_var(const char *value);

  // --- Var passthrough ---
  template <>
  struct Converter<Var>
  {
    static PyObject *to_python(const Var &value)
    {
      PyObject *o = value.get();
      Py_XINCREF(o);
      return o;
    }
    static Var from_python(const Var &value) { return value; }
  };

  // --- None ---
  template <>
  struct Converter<std::monostate>
  {
    static PyObject *to_python(std::monostate) { Py_RETURN_NONE; }
    static std::monostate from_python(const Var &value)
    {
      if (!value.is_none())
        throw Error("expected None, got " + value.type_name());
      return {};
    }
  };

  // --- bool: uses Python's own truthiness protocol on the way in, so any
  // object works, not just an actual bool (relaxed coercion, same spirit as
  // the numeric fix below) ---
  template <>
  struct Converter<bool>
  {
    static PyObject *to_python(bool value) { return PyBool_FromLong(value ? 1 : 0); }
    static bool from_python(const Var &value)
    {
      if (!value)
        throw Error("cannot convert a null Var to bool");
      const int result = PyObject_IsTrue(value.get());
      if (result < 0)
        throw_if_error();
      return result == 1;
    }
  };

  // --- integral types (bool handled separately above) ---
  template <typename T>
    requires std::integral<T> && (!std::same_as<T, bool>)
  struct Converter<T>
  {
    static PyObject *to_python(T value) { return PyLong_FromLongLong(static_cast<long long>(value)); }
    static T from_python(const Var &value)
    {
      if (!value || !PyLong_Check(value.get()))
        throw Error("expected an int, got " + (value ? value.type_name() : std::string("None")));
      const long long v = PyLong_AsLongLong(value.get());
      if (v == -1 && PyErr_Occurred())
        throw_if_error();
      return static_cast<T>(v);
    }
  };

  // --- floating-point types: bug #32 fix -- a Python int is also accepted
  // (widened via PyLong_AsDouble), matching Python's own numeric tower
  // instead of v1's PyFloat_Check-only rejection of ints ---
  template <std::floating_point T>
  struct Converter<T>
  {
    static PyObject *to_python(T value) { return PyFloat_FromDouble(static_cast<double>(value)); }
    static T from_python(const Var &value)
    {
      if (!value)
        throw Error("cannot convert a null Var to a floating-point type");
      PyObject *o = value.get();
      if (PyFloat_Check(o))
        return static_cast<T>(PyFloat_AsDouble(o));
      if (PyLong_Check(o))
      {
        const double d = PyLong_AsDouble(o);
        if (d == -1.0 && PyErr_Occurred())
          throw_if_error();
        return static_cast<T>(d);
      }
      throw Error("expected a real number, got " + value.type_name());
    }
  };

  // --- strings ---
  template <>
  struct Converter<std::string>
  {
    static PyObject *to_python(std::string_view value)
    {
      PyObject *o = PyUnicode_FromStringAndSize(value.data(), static_cast<Py_ssize_t>(value.size()));
      if (!o)
        throw_if_error();
      return o;
    }
    static std::string from_python(const Var &value)
    {
      if (!value)
        throw Error("cannot convert a null Var to std::string");
      return value.str(); // Var::str() already applies Python's str() rules
    }
  };

  template <>
  struct Converter<std::string_view>
  {
    static PyObject *to_python(std::string_view value) { return Converter<std::string>::to_python(value); }
  };

  template <>
  struct Converter<const char *>
  {
    static PyObject *to_python(const char *value)
    {
      PyObject *o = PyUnicode_FromString(value);
      if (!o)
        throw_if_error();
      return o;
    }
  };

  [[nodiscard]] inline Var to_var(const char *value) { return Var::steal(Converter<const char *>::to_python(value)); }

  template <>
  struct Converter<std::wstring>
  {
    static PyObject *to_python(const std::wstring &value)
    {
      PyObject *o = PyUnicode_FromWideChar(value.data(), static_cast<Py_ssize_t>(value.size()));
      if (!o)
        throw_if_error();
      return o;
    }
    static std::wstring from_python(const Var &value)
    {
      if (!value)
        throw Error("cannot convert a null Var to std::wstring");
      return UTF8ToWide(value.str());
    }
  };

  // --- containers: unconstrained partial specializations -- errors surface
  // at the point a direction is actually used for an element type that
  // doesn't support it, same approach as most trait-based (de)serializers ---
  template <typename T>
  struct Converter<std::vector<T>>
  {
    static PyObject *to_python(const std::vector<T> &value)
    {
      PyObject *list = PyList_New(static_cast<Py_ssize_t>(value.size()));
      if (!list)
        throw_if_error();
      for (size_t i = 0; i < value.size(); i++)
      {
        PyObject *item = Converter<T>::to_python(value[i]);
        if (!item)
        {
          Py_DECREF(list);
          throw_if_error();
        }
        PyList_SetItem(list, static_cast<Py_ssize_t>(i), item); // steals
      }
      return list;
    }

    static std::vector<T> from_python(const Var &value)
    {
      if (!value || (value.type() != Var::Type::List && value.type() != Var::Type::Tuple))
        throw Error("expected a list or tuple, got " + (value ? value.type_name() : std::string("None")));
      std::vector<T> result;
      const Py_ssize_t n = value.size();
      result.reserve(static_cast<size_t>(n));
      for (Py_ssize_t i = 0; i < n; i++)
        result.push_back(Converter<T>::from_python(value[i]));
      return result;
    }
  };

  template <typename T>
  struct Converter<std::map<std::string, T>>
  {
    static PyObject *to_python(const std::map<std::string, T> &value)
    {
      PyObject *dict = PyDict_New();
      if (!dict)
        throw_if_error();
      for (const auto &[k, v] : value)
      {
        PyObject *item = Converter<T>::to_python(v);
        if (!item || PyDict_SetItemString(dict, k.c_str(), item) != 0)
        {
          Py_XDECREF(item);
          Py_DECREF(dict);
          throw_if_error();
        }
        Py_DECREF(item); // PyDict_SetItemString increfs its own copy
      }
      return dict;
    }

    static std::map<std::string, T> from_python(const Var &value)
    {
      if (!value || value.type() != Var::Type::Dict)
        throw Error("expected a dict, got " + (value ? value.type_name() : std::string("None")));
      std::map<std::string, T> result;
      for (const Var &key : value) // Var's iterator over a dict yields keys
      {
        std::string k = key.str();
        Var v = value[k];
        result.emplace(std::move(k), Converter<T>::from_python(v));
      }
      return result;
    }
  };

  template <typename T>
  struct Converter<std::optional<T>>
  {
    static PyObject *to_python(const std::optional<T> &value)
    {
      if (!value)
        Py_RETURN_NONE;
      return Converter<T>::to_python(*value);
    }
    static std::optional<T> from_python(const Var &value)
    {
      if (!value || value.is_none())
        return std::nullopt;
      return Converter<T>::from_python(value);
    }
  };

  template <typename A, typename B>
  struct Converter<std::pair<A, B>>
  {
    static PyObject *to_python(const std::pair<A, B> &value)
    {
      PyObject *tup = PyTuple_New(2);
      if (!tup)
        throw_if_error();
      PyTuple_SetItem(tup, 0, Converter<A>::to_python(value.first));
      PyTuple_SetItem(tup, 1, Converter<B>::to_python(value.second));
      return tup;
    }
    static std::pair<A, B> from_python(const Var &value)
    {
      if (!value || value.size() != 2)
        throw Error("expected a 2-tuple, got " + (value ? value.type_name() : std::string("None")));
      return {Converter<A>::from_python(value[Py_ssize_t{0}]), Converter<B>::from_python(value[Py_ssize_t{1}])};
    }
  };

  template <typename T>
  T Var::to() const
  {
    return Converter<T>::from_python(*this);
  }

  template <typename T>
  std::optional<T> Var::try_to() const
  {
    try
    {
      return to<T>();
    }
    catch (const Error &)
    {
      return std::nullopt;
    }
  }

  template <typename... Ts>
  struct Converter<std::tuple<Ts...>>
  {
    static PyObject *to_python(const std::tuple<Ts...> &value)
    {
      PyObject *tup = PyTuple_New(sizeof...(Ts));
      if (!tup)
        throw_if_error();
      to_python_impl(value, tup, std::index_sequence_for<Ts...>{});
      return tup;
    }

    static std::tuple<Ts...> from_python(const Var &value)
    {
      if (!value || value.size() != static_cast<Py_ssize_t>(sizeof...(Ts)))
        throw Error("expected a tuple of size " + std::to_string(sizeof...(Ts)));
      return from_python_impl(value, std::index_sequence_for<Ts...>{});
    }

  private:
    template <size_t... Is>
    static void to_python_impl(const std::tuple<Ts...> &value, PyObject *tup, std::index_sequence<Is...>)
    {
      (PyTuple_SetItem(tup, static_cast<Py_ssize_t>(Is), Converter<Ts>::to_python(std::get<Is>(value))), ...);
    }

    template <size_t... Is>
    static std::tuple<Ts...> from_python_impl(const Var &value, std::index_sequence<Is...>)
    {
      return std::make_tuple(Converter<Ts>::from_python(value[static_cast<Py_ssize_t>(Is)])...);
    }
  };

  // Keyword arguments for Var::call_kw(). Values must already be Vars
  // (call to_var() explicitly at the call site, e.g.
  // obj.call_kw({{"x", to_var(1)}})) -- consistent with the rest of cppy3
  // deliberately not offering an implicit T->Var conversion.
  class Kwargs
  {
  public:
    Kwargs() = default;
    Kwargs(std::initializer_list<std::pair<std::string_view, Var>> items)
    {
      _items.reserve(items.size());
      for (const auto &item : items)
        _items.emplace_back(item.first, item.second);
    }

    [[nodiscard]] auto begin() const noexcept { return _items.begin(); }
    [[nodiscard]] auto end() const noexcept { return _items.end(); }

  private:
    std::vector<std::pair<std::string_view, Var>> _items;
  };

  template <typename... A>
  Var Var::operator()(A &&...args) const
  {
    if (!*this)
      throw Error("cannot call a null Var");
    if (!callable())
      throw Error("object of type '" + type_name() + "' is not callable");

    Var argsTuple = Var::steal(PyTuple_New(static_cast<Py_ssize_t>(sizeof...(A))));
    if (!argsTuple)
      throw_if_error();
    Py_ssize_t i = 0;
    // PyTuple_SetItem steals; to_var(...).release() hands it a reference
    // with nothing else still owning it, unlike v1's call() which handed
    // over a borrowed pointer while the caller's vector kept its own claim
    // on it too (bug #2).
    (PyTuple_SetItem(argsTuple.get(), i++, to_var(std::forward<A>(args)).release()), ...);

    Var result = Var::steal(PyObject_CallObject(_o, argsTuple.get()));
    if (!result)
      throw_if_error();
    return result;
  }

  template <typename... A>
  Var Var::call_kw(const Kwargs &kwargs, A &&...args) const
  {
    if (!*this)
      throw Error("cannot call a null Var");
    if (!callable())
      throw Error("object of type '" + type_name() + "' is not callable");

    Var argsTuple = Var::steal(PyTuple_New(static_cast<Py_ssize_t>(sizeof...(A))));
    if (!argsTuple)
      throw_if_error();
    Py_ssize_t i = 0;
    (PyTuple_SetItem(argsTuple.get(), i++, to_var(std::forward<A>(args)).release()), ...);

    Var kwargsDict = Var::steal(PyDict_New());
    if (!kwargsDict)
      throw_if_error();
    for (const auto &[name, value] : kwargs)
    {
      const std::string key(name);
      if (PyDict_SetItemString(kwargsDict.get(), key.c_str(), value.get()) != 0)
        throw_if_error();
    }

    Var result = Var::steal(PyObject_Call(_o, argsTuple.get(), kwargsDict.get()));
    if (!result)
      throw_if_error();
    return result;
  }

  template <typename... A>
  Var Var::method(std::string_view name, A &&...args) const
  {
    return attr(name)(std::forward<A>(args)...);
  }
}
