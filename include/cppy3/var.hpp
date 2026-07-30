#pragma once

#include <Python.h>

#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#include <cppy3/error.hpp>
#include <cppy3/libdefs.hpp>

namespace cppy3
{
  // Keyword arguments for Var::call_kw(). Forward-declared; fully defined
  // in convert.hpp (it stores Vars by name, so it needs the complete Var
  // type, and convert.hpp is where to_var()/Converter<T> already live).
  class Kwargs;

  // Lets Var::type() report Type::NumpyNdarray without var.cpp itself
  // depending on numpy headers: cppy3_numpy.cpp registers this hook (via
  // importNumpy()) only when NumPy support is actually compiled in and
  // initialized. Superseds v1's approach, which checked
  // `#ifdef NPY_NDARRAYOBJECT_H` in var's own translation unit -- a macro
  // that TU never defined, so the branch was permanently dead regardless of
  // whether NumPy support was built (bug #14).
  using IsNdarrayHook = bool (*)(PyObject *);
  LIB_API void register_is_ndarray_hook(IsNdarrayHook hook) noexcept;

  // A reference-counted, RAII-managed PyObject* holder -- the core value
  // type of cppy3. Every Python object that crosses into C++ is a Var.
  //
  // Requires the GIL to be held for construction/destruction/copying, same
  // as any other CPython C API call; cppy3 does not acquire it implicitly
  // per-object (see GilLock/Interpreter for coarser-grained GIL management).
  //
  // Fixes v1's Var bugs: a correct copy-assignment operator (v1 declared a
  // copy ctor and dtor but no operator=, so the compiler-generated one
  // corrupted refcounts -- plan bug #1); no implicit conversion to
  // PyObject* (v1's `operator PyObject*()` is what let call() silently
  // double-decref its arguments by handing a borrowed pointer to a
  // reference-stealing API -- bug #2); a NULL-safe type() (bug #4); and
  // throwing Error on invalid access instead of assert(), which vanishes
  // under NDEBUG (bug #19).
  class LIB_API Var
  {
  public:
    enum class Type
    {
      None,
      Bool,
      Long,
      Float,
      Str,
      List,
      Dict,
      Tuple,
      Module,
      NumpyNdarray, // reserved; populated starting with the NumPy rework
      Other,
    };

    Var() noexcept = default;
    Var(const Var &other) noexcept;
    Var &operator=(const Var &other) noexcept;
    Var(Var &&other) noexcept;
    Var &operator=(Var &&other) noexcept;
    ~Var();

    // Takes a new reference to `object` (increfs). Use for objects reached
    // via a "borrowed reference" API, e.g. PyDict_GetItem/PyList_GetItem,
    // or any PyObject* you do not already own a reference to.
    [[nodiscard]] static Var borrow(PyObject *object) noexcept;

    // Takes ownership of `object` without incref'ing. Use for objects
    // returned by a "new reference" API, e.g. PyLong_FromLong/PyObject_Str.
    [[nodiscard]] static Var steal(PyObject *object) noexcept;

    // Raw pointer access. Deliberately no implicit conversion to PyObject*:
    // that is exactly what let v1's call() pass a borrowed pointer to a
    // reference-stealing C API without the caller noticing (bug #2). Call
    // get() explicitly at every C API boundary.
    [[nodiscard]] PyObject *get() const noexcept { return _o; }

    // Releases ownership without decref'ing; the caller becomes responsible
    // for the reference. *this becomes empty.
    [[nodiscard]] PyObject *release() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return _o != nullptr; }
    [[nodiscard]] bool is_none() const noexcept { return _o == nullptr || _o == Py_None; }

    // Attribute access (obj.name). Throws Error if absent or *this is empty.
    [[nodiscard]] Var attr(std::string_view name) const;
    [[nodiscard]] bool has_attr(std::string_view name) const noexcept;
    // const: like a pointer/shared_ptr, Var's constness governs the handle
    // (_o) itself, not the mutability of the Python object it refers to.
    void set_attr(std::string_view name, const Var &value) const;

    // Mapping access (obj[key]) for dict-likes and any object implementing
    // the mapping protocol with string keys. Throws Error if *this is not
    // a mapping or the key is absent.
    [[nodiscard]] Var operator[](std::string_view key) const;
    void set_item(std::string_view key, const Var &value) const;

    // Sequence access (obj[index]) for list/tuple/any sequence. Negative
    // indices follow Python semantics (count from the end). Throws Error
    // if *this is not a sequence or the index is out of range.
    [[nodiscard]] Var operator[](Py_ssize_t index) const;
    void set_item(Py_ssize_t index, const Var &value) const;

    // Size of a sequence, mapping, or anything else PyObject_Size() accepts.
    [[nodiscard]] Py_ssize_t size() const;

    [[nodiscard]] bool callable() const noexcept;

    // Calls *this with positional arguments, each converted via
    // to_var()/Converter<T>. Throws Error if *this is not callable or the
    // call raises. Declared here but defined in convert.hpp, same reason
    // as to<T>()/try_to<T>() above.
    template <typename... A>
    [[nodiscard]] Var operator()(A &&...args) const;

    // Calls *this with keyword arguments plus optional positional ones,
    // e.g. obj.call_kw({{"x", to_var(1)}}, positional_arg).
    template <typename... A>
    [[nodiscard]] Var call_kw(const Kwargs &kwargs, A &&...args) const;

    // Looks up and calls a bound method: obj.method("name", args...) is
    // shorthand for obj.attr("name")(args...).
    template <typename... A>
    [[nodiscard]] Var method(std::string_view name, A &&...args) const;

    // Python str()/repr(), and the bare C type name (e.g. "int",
    // "NoneType"), all as UTF-8. Unlike v1's toString()/typeName(), these
    // never leak the PyObject_Str()/Repr() result (bug #9) and never
    // dereference a NULL _o (throw Error instead -- bug #4).
    [[nodiscard]] std::string str() const;
    [[nodiscard]] std::string repr() const;
    [[nodiscard]] std::string type_name() const;

    // A coarse classification of the underlying object, for quick
    // dispatch/debugging. NULL-safe: returns Type::None for an empty Var
    // instead of dereferencing it (v1's type() crashed on a NULL _o --
    // bug #4).
    [[nodiscard]] Type type() const noexcept;

    // Value conversion via Converter<T> (see convert.hpp, which must be
    // included for these to be usable -- cppy3.hpp does so). to<T>() throws
    // Error on a type mismatch; try_to<T>() reports it as std::nullopt
    // instead. Declared here but defined in convert.hpp: Converter<T> is
    // not yet visible at this point, and convert.hpp itself needs the full
    // Var definition, so the two headers cannot include each other.
    template <typename T>
    [[nodiscard]] T to() const;
    template <typename T>
    [[nodiscard]] std::optional<T> try_to() const;

    // Iterates via the Python iterator protocol (PyObject_GetIter /
    // PyIter_Next), so this works for any iterable -- list, dict (yields
    // keys, matching Python), tuple, generator, custom __iter__ -- not just
    // list/dict as in v1.
    //
    // Forward-declared and defined below, out of line: it holds Var members,
    // which would otherwise be incomplete-type members of Var while Var's
    // own body is still being parsed.
    class Iterator;

    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end() const noexcept;

  private:
    PyObject *_o = nullptr;
  };

  class Var::Iterator
  {
  public:
    using iterator_category = std::input_iterator_tag;
    using value_type = Var;
    using difference_type = std::ptrdiff_t;
    using pointer = const Var *;
    using reference = const Var &;

    Iterator() noexcept = default;
    explicit Iterator(const Var &iterable);

    [[nodiscard]] const Var &operator*() const noexcept { return _current; }
    Iterator &operator++();
    [[nodiscard]] bool operator==(const Iterator &other) const noexcept
    {
      return _iterator.get() == other._iterator.get();
    }
    [[nodiscard]] bool operator!=(const Iterator &other) const noexcept { return !(*this == other); }

  private:
    Var _iterator; // the Python iterator object itself; empty at/past the end
    Var _current;
  };

  // A validating view over a Python list. Constructing from a non-list,
  // non-empty Var throws Error (v1 used assert(), silently skipped in
  // release builds -- bug #19).
  class LIB_API List : public Var
  {
  public:
    List() = default;
    explicit List(Var v);
    [[nodiscard]] static List create(Py_ssize_t n = 0);

    void append(const Var &value) const;
    void insert(Py_ssize_t index, const Var &value) const;
    void remove(Py_ssize_t index) const;
    [[nodiscard]] bool contains(const Var &value) const;
  };

  // A validating view over a Python dict.
  class LIB_API Dict : public Var
  {
  public:
    Dict() = default;
    explicit Dict(Var v);
    [[nodiscard]] static Dict create();

    [[nodiscard]] bool contains(std::string_view key) const;
    void clear() const;
  };
}
