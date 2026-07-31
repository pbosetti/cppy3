#pragma once

#include <cppy3/pycompat.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#include <cppy3/libdefs.hpp>

namespace cppy3
{
  // A Python exception translated into a C++ exception. UTF-8 std::string
  // throughout (unlike v1's PythonException, which used std::wstring), a
  // bare exception class name via type_name() ("ValueError", not
  // "<class 'ValueError'>"), and the __cause__/__context__ chain via
  // cause() -- none of which v1's PyExceptionData exposed.
  //
  // Not yet wired into exec()/eval()/call() -- those still throw
  // PythonException until the interpreter and calling layers are rewritten
  // (see the v2 plan, Phases 4-5). Usable standalone today via
  // throw_if_error().
  class CPPY3_API Error : public std::runtime_error
  {
  public:
    // type_name empty means this Error did not originate from a Python
    // exception (e.g. a file-not-found reported by cppy3 itself).
    Error(std::string type_name, std::string message, std::string traceback,
          std::unique_ptr<Error> cause = nullptr);
    explicit Error(std::string message);

    Error(const Error &other);
    Error &operator=(const Error &other);
    Error(Error &&) = default;
    Error &operator=(Error &&) = default;
    ~Error() override = default;

    [[nodiscard]] const std::string &type_name() const noexcept { return _type_name; }
    [[nodiscard]] const std::string &message() const noexcept { return _message; }
    [[nodiscard]] const std::string &traceback() const noexcept { return _traceback; }

    // Python-style rendering: a "Traceback (most recent call last):" block
    // (if any), the type/message, and -- recursively -- any chained cause.
    [[nodiscard]] std::string format() const;

    // The exception this one was raised from (Python's __cause__, falling
    // back to __context__ if there is no explicit cause), or nullptr.
    [[nodiscard]] const Error *cause() const noexcept { return _cause.get(); }

  private:
    std::string _type_name;
    std::string _message;
    std::string _traceback;
    std::unique_ptr<Error> _cause;
  };

  // Fetches and clears the current Python exception, if any, and throws it
  // as a cppy3::Error (with its __cause__/__context__ chain). No-op if no
  // exception is set. Requires the GIL to be held.
  CPPY3_API void throw_if_error();
}
