/**
 * cppy3 -- embed python3 scripting layer into your c++ app in a few
 * minutes.
 *
 * Adapters for numpy.ndarray.
 */

#pragma once

// undef will surpress python warnings
#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif
#ifdef _XOPEN_SOURCE
#undef _XOPEN_SOURCE
#endif

#include <cppy3/pycompat.hpp>

// deal with crazy numpy 1.7.x api init procedure
#define PY_ARRAY_UNIQUE_SYMBOL PyArray_API__CPPY3_APP_TOKEN
#if !defined(INCLUDED_FROM_CPPY3_NUMPY_CPP)
#define NO_IMPORT_ARRAY
#endif
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <numpy/arrayobject.h>

#include <algorithm>
#include <initializer_list>
#include <span>

#include <cppy3/error.hpp>
#include <cppy3/var.hpp>

namespace cppy3
{
  // Must be called (once) before any NDArray use.
  void importNumpy();

  // Maps a C++ element type to its numpy dtype. Specialize for your own
  // element types the same way you'd specialize Converter<T> for
  // Var::to<T>(); replaces v1's toNumpyDType(double)/toNumpyDType(int)
  // overloads, which covered exactly two types and offered no way to add
  // more without editing cppy3 itself.
  template <typename T>
  struct NumpyDType; // specialize per element type

  template <>
  struct NumpyDType<double>
  {
    static constexpr NPY_TYPES value = NPY_DOUBLE;
  };
  template <>
  struct NumpyDType<float>
  {
    static constexpr NPY_TYPES value = NPY_FLOAT;
  };
  template <>
  struct NumpyDType<int>
  {
    static constexpr NPY_TYPES value = NPY_INT;
  };
  template <>
  struct NumpyDType<long>
  {
    static constexpr NPY_TYPES value = NPY_LONG;
  };

  namespace detail
  {
    // PyArray_SetBaseObject() steals the reference it's given.
    inline void set_ndarray_base(PyArrayObject *arr, const Var &owner)
    {
      if (!owner)
        return;
      PyObject *base = owner.get();
      Py_INCREF(base);
      if (PyArray_SetBaseObject(arr, base) != 0)
        throw_if_error();
    }
  }

  // A validating view over a numpy.ndarray with a known, fixed element type
  // T, i.e. T must match the array's actual dtype -- this class does not
  // convert between dtypes. Inherits Var's already-correct rule of five
  // (copy = share the same underlying array, exactly like a Var) instead of
  // v1's NDArray managing its own separate, ad hoc PyArrayObject* refcount
  // (with no copy/move control at all, so it was implicitly, silently
  // copyable while owning a raw pointer -- bug #8).
  //
  // Named T, not Type: Var has a nested Type enum, and inside a class
  // template deriving from Var, an unqualified name that collides with an
  // inherited member is resolved to that inherited member, not the
  // enclosing template parameter -- naming this parameter Type would have
  // silently shadowed itself with cppy3::Var::Type throughout this class.
  template <typename T>
  class NDArray : public Var
  {
  public:
    NDArray() = default;

    explicit NDArray(Var v) : Var(std::move(v))
    {
      if (*this && !PyArray_Check(get()))
        throw Error("expected a numpy.ndarray, got " + type_name());
    }

    // Creates a new, numpy-owned, uninitialized (or zero-filled) array of
    // the given shape, e.g. NDArray<double>::create({3, 5}). A single
    // initializer_list-of-extents parameter -- rather than separate
    // create(int)/create(size_t,size_t) overloads as in v1 -- has no
    // possible ambiguity with a 2-argument wrap()/copy() call (v1's
    // NDArray(int, int) constructor was ambiguous with create(int, bool),
    // and silently resolved to the wrong, 1D one -- bug #8b).
    [[nodiscard]] static NDArray create(std::initializer_list<Py_ssize_t> shape, bool fill_zeros = false)
    {
      std::vector<npy_intp> dims(shape.begin(), shape.end());
      PyObject *arr = fill_zeros
                          ? PyArray_ZEROS(static_cast<int>(dims.size()), dims.data(), NumpyDType<T>::value, 0)
                          : PyArray_SimpleNew(static_cast<int>(dims.size()), dims.data(), NumpyDType<T>::value);
      if (!arr)
        throw_if_error();
      return NDArray(Var::steal(arr));
    }

    // Deep-copies data into a new, numpy-owned array.
    [[nodiscard]] static NDArray copy(std::span<const T> data)
    {
      NDArray result = create({static_cast<Py_ssize_t>(data.size())});
      std::copy(data.begin(), data.end(), result.data());
      return result;
    }

    [[nodiscard]] static NDArray copy(const T *data, Py_ssize_t rows, Py_ssize_t cols)
    {
      NDArray result = create({rows, cols});
      std::copy(data, data + rows * cols, result.data());
      return result;
    }

    // Wraps external memory without copying -- the returned array does not
    // own `data`. If `owner` is given, it's set as the array's base object
    // (PyArray_SetBaseObject), so numpy keeps its own reference to it for as
    // long as the array (or any view sliced from it) is alive; pass
    // whatever Python-visible object actually owns `data`'s storage if
    // that lifetime isn't otherwise guaranteed to outlive the array. With
    // no owner, the caller remains responsible for keeping `data` alive,
    // same as v1 (undocumented there). v1's 1D overload passed
    // `(void*)&data` -- the address of the *local pointer parameter*, not
    // the caller's buffer -- to PyArray_SimpleNewFromData (bug #6); the 2D
    // overload was correct. Both go through one shape-span-taking
    // implementation here.
    [[nodiscard]] static NDArray wrap(std::span<T> data, Var owner = {})
    {
      npy_intp dims[1] = {static_cast<npy_intp>(data.size())};
      PyObject *arr = PyArray_SimpleNewFromData(1, dims, NumpyDType<T>::value, data.data());
      if (!arr)
        throw_if_error();
      detail::set_ndarray_base(reinterpret_cast<PyArrayObject *>(arr), owner);
      return NDArray(Var::steal(arr));
    }

    [[nodiscard]] static NDArray wrap(T *data, Py_ssize_t rows, Py_ssize_t cols, Var owner = {})
    {
      npy_intp dims[2] = {rows, cols};
      PyObject *arr = PyArray_SimpleNewFromData(2, dims, NumpyDType<T>::value, data);
      if (!arr)
        throw_if_error();
      detail::set_ndarray_base(reinterpret_cast<PyArrayObject *>(arr), owner);
      return NDArray(Var::steal(arr));
    }

    [[nodiscard]] T &operator()(Py_ssize_t i) const
    {
      if (ndim() != 1)
        throw Error("NDArray::operator()(i) requires a 1D array");
      if (i < 0 || i >= dim(0))
        throw Error("NDArray index out of range");
      return *static_cast<T *>(PyArray_GETPTR1(arr(), i));
    }

    [[nodiscard]] T &operator()(Py_ssize_t i, Py_ssize_t j) const
    {
      if (ndim() != 2)
        throw Error("NDArray::operator()(i, j) requires a 2D array");
      if (i < 0 || i >= dim(0) || j < 0 || j >= dim(1))
        throw Error("NDArray index out of range");
      return *static_cast<T *>(PyArray_GETPTR2(arr(), i, j));
    }

    [[nodiscard]] int ndim() const
    {
      if (!*this)
        throw Error("cannot get ndim() of an empty NDArray");
      return PyArray_NDIM(arr());
    }

    // 0-indexed, unlike v1's dim(), which took a 0-indexed argument
    // internally but was called exclusively via dim1()/dim2() forwarding
    // to dim(1)/dim(2) -- both off by one (bug #7): dim1() returned the
    // *second* extent, and dim2() read past the array's own dimension
    // count, tripping an assert (or reading OOB under NDEBUG).
    [[nodiscard]] Py_ssize_t dim(int n) const
    {
      if (!*this)
        throw Error("cannot get dim() of an empty NDArray");
      if (n < 0 || n >= PyArray_NDIM(arr()))
        throw Error("NDArray dimension index out of range");
      return PyArray_DIM(arr(), n);
    }

    [[nodiscard]] Py_ssize_t dim1() const { return dim(0); }
    [[nodiscard]] Py_ssize_t dim2() const { return dim(1); }

    [[nodiscard]] T *data() const
    {
      if (!*this)
        throw Error("cannot get data() of an empty NDArray");
      return static_cast<T *>(PyArray_DATA(arr()));
    }

  private:
    PyArrayObject *arr() const { return reinterpret_cast<PyArrayObject *>(get()); }
  };
}
