#include <cppy3/var.hpp>

namespace cppy3
{
  namespace
  {
    std::string pyStrToUtf8(PyObject *strObj)
    {
      Py_ssize_t size = 0;
      const char *data = PyUnicode_AsUTF8AndSize(strObj, &size);
      if (!data)
      {
        PyErr_Clear();
        return {};
      }
      return std::string(data, static_cast<size_t>(size));
    }

    IsNdarrayHook g_isNdarrayHook = nullptr;
  }

  void register_is_ndarray_hook(IsNdarrayHook hook) noexcept { g_isNdarrayHook = hook; }

  Var::Var(const Var &other) noexcept : _o(other._o)
  {
    Py_XINCREF(_o);
  }

  Var &Var::operator=(const Var &other) noexcept
  {
    if (this != &other)
    {
      PyObject *old = _o;
      _o = other._o;
      Py_XINCREF(_o); // incref the new referent before decref'ing the old
      Py_XDECREF(old);
    }
    return *this;
  }

  Var::Var(Var &&other) noexcept : _o(other._o)
  {
    other._o = nullptr;
  }

  Var &Var::operator=(Var &&other) noexcept
  {
    if (this != &other)
    {
      Py_XDECREF(_o);
      _o = other._o;
      other._o = nullptr;
    }
    return *this;
  }

  Var::~Var()
  {
    Py_XDECREF(_o);
  }

  Var Var::borrow(PyObject *object) noexcept
  {
    Py_XINCREF(object);
    Var v;
    v._o = object;
    return v;
  }

  Var Var::steal(PyObject *object) noexcept
  {
    Var v;
    v._o = object;
    return v;
  }

  PyObject *Var::release() noexcept
  {
    PyObject *o = _o;
    _o = nullptr;
    return o;
  }

  Var Var::attr(std::string_view name) const
  {
    if (!*this)
      throw Error("cannot get attribute '" + std::string(name) + "' of a null Var");
    const std::string key(name);
    PyObject *result = PyObject_GetAttrString(_o, key.c_str());
    if (!result)
      throw_if_error();
    return Var::steal(result);
  }

  bool Var::has_attr(std::string_view name) const noexcept
  {
    if (!*this)
      return false;
    const std::string key(name);
    const int result = PyObject_HasAttrString(_o, key.c_str());
    PyErr_Clear(); // HasAttrString can set (and swallow) an error internally
    return result == 1;
  }

  void Var::set_attr(std::string_view name, const Var &value) const
  {
    if (!*this)
      throw Error("cannot set attribute '" + std::string(name) + "' of a null Var");
    const std::string key(name);
    if (PyObject_SetAttrString(_o, key.c_str(), value.get()) != 0)
      throw_if_error();
  }

  Var Var::operator[](std::string_view key) const
  {
    if (!*this)
      throw Error("cannot subscript a null Var");
    const std::string keyStr(key);
    PyObject *result = PyMapping_GetItemString(_o, keyStr.c_str());
    if (!result)
      throw_if_error();
    return Var::steal(result);
  }

  void Var::set_item(std::string_view key, const Var &value) const
  {
    if (!*this)
      throw Error("cannot subscript a null Var");
    const std::string keyStr(key);
    if (PyMapping_SetItemString(_o, keyStr.c_str(), value.get()) != 0)
      throw_if_error();
  }

  Var Var::operator[](Py_ssize_t index) const
  {
    if (!*this)
      throw Error("cannot index a null Var");
    PyObject *result = PySequence_GetItem(_o, index);
    if (!result)
      throw_if_error();
    return Var::steal(result);
  }

  void Var::set_item(Py_ssize_t index, const Var &value) const
  {
    if (!*this)
      throw Error("cannot index a null Var");
    if (PySequence_SetItem(_o, index, value.get()) != 0)
      throw_if_error();
  }

  Py_ssize_t Var::size() const
  {
    if (!*this)
      throw Error("cannot get size of a null Var");
    const Py_ssize_t n = PyObject_Size(_o);
    if (n < 0)
      throw_if_error();
    return n;
  }

  bool Var::callable() const noexcept
  {
    return _o && PyCallable_Check(_o);
  }

  std::string Var::str() const
  {
    if (!*this)
      throw Error("cannot stringify a null Var");
    PyObject *s = PyObject_Str(_o);
    if (!s)
      throw_if_error();
    std::string result = pyStrToUtf8(s);
    Py_DECREF(s);
    return result;
  }

  std::string Var::repr() const
  {
    if (!*this)
      throw Error("cannot repr a null Var");
    PyObject *s = PyObject_Repr(_o);
    if (!s)
      throw_if_error();
    std::string result = pyStrToUtf8(s);
    Py_DECREF(s);
    return result;
  }

  std::string Var::type_name() const
  {
    if (!*this)
      throw Error("cannot get the type name of a null Var");
    return Py_TYPE(_o)->tp_name;
  }

  Var::Type Var::type() const noexcept
  {
    if (is_none())
      return Type::None;
    if (PyBool_Check(_o)) // must precede the Long check: bool subclasses int
      return Type::Bool;
    if (PyLong_Check(_o))
      return Type::Long;
    if (PyFloat_Check(_o))
      return Type::Float;
    if (PyUnicode_Check(_o))
      return Type::Str;
    if (PyTuple_Check(_o))
      return Type::Tuple;
    if (PyDict_Check(_o))
      return Type::Dict;
    if (PyList_Check(_o))
      return Type::List;
    if (g_isNdarrayHook && g_isNdarrayHook(_o))
      return Type::NumpyNdarray;
    if (PyModule_Check(_o))
      return Type::Module;
    return Type::Other;
  }

  Var::Iterator::Iterator(const Var &iterable)
  {
    PyObject *it = PyObject_GetIter(iterable.get());
    if (!it)
      throw_if_error();
    _iterator = Var::steal(it);
    ++(*this); // prime _current with the first element, or become end()
  }

  Var::Iterator &Var::Iterator::operator++()
  {
    PyObject *next = PyIter_Next(_iterator.get());
    if (!next)
    {
      if (PyErr_Occurred())
        throw_if_error();
      _iterator = Var();
      _current = Var();
    }
    else
    {
      _current = Var::steal(next);
    }
    return *this;
  }

  Var::Iterator Var::begin() const { return Iterator(*this); }
  Var::Iterator Var::end() const noexcept { return Iterator(); }

  List::List(Var v) : Var(std::move(v))
  {
    if (*this && type() != Type::List)
      throw Error("expected a list, got " + type_name());
  }

  List List::create(Py_ssize_t n)
  {
    PyObject *o = PyList_New(n);
    if (!o)
      throw_if_error();
    return List(Var::steal(o));
  }

  void List::append(const Var &value) const
  {
    if (PyList_Append(get(), value.get()) != 0)
      throw_if_error();
  }

  void List::insert(Py_ssize_t index, const Var &value) const
  {
    if (PyList_Insert(get(), index, value.get()) != 0)
      throw_if_error();
  }

  void List::remove(Py_ssize_t index) const
  {
    if (PySequence_DelItem(get(), index) != 0)
      throw_if_error();
  }

  bool List::contains(const Var &value) const
  {
    const int result = PySequence_Contains(get(), value.get());
    if (result < 0)
      throw_if_error();
    return result == 1;
  }

  Dict::Dict(Var v) : Var(std::move(v))
  {
    if (*this && type() != Type::Dict)
      throw Error("expected a dict, got " + type_name());
  }

  Dict Dict::create()
  {
    PyObject *o = PyDict_New();
    if (!o)
      throw_if_error();
    return Dict(Var::steal(o));
  }

  bool Dict::contains(std::string_view key) const
  {
    const Var keyObj = Var::steal(PyUnicode_FromStringAndSize(key.data(), static_cast<Py_ssize_t>(key.size())));
    if (!keyObj)
      throw_if_error();
    const int result = PyDict_Contains(get(), keyObj.get());
    if (result < 0)
      throw_if_error();
    return result == 1;
  }

  void Dict::clear() const
  {
    PyDict_Clear(get());
  }
}
