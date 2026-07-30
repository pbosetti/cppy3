/**
 * cppy3 -- embed python3 scripting layer into your c++ app in a few
 * minutes.
 *
 * Minimalistic library for embedding Python 3 in a C++ application.
 * No additional dependencies required. Linux, macOS, Windows supported.
 *
 * - Var: a reference-counted, RAII-managed PyObject* holder with
 *   attribute/item access, iteration, and calling.
 * - Converter<T>: the extension point for converting your own types
 *   to and from Python.
 * - Error: Python exceptions translated to C++ exceptions, including
 *   the __cause__/__context__ chain.
 * - Interpreter/Namespace: interpreter lifecycle (PyConfig-based) and
 *   isolated namespaces to exec()/eval() code in.
 * - GilLock/GilRelease: scoped GIL management.
 *
 * This is the single header most consumers need; each piece above also
 * has its own header (var.hpp, convert.hpp, error.hpp, interpreter.hpp,
 * gil.hpp) if you want to include only what you use.
 */
#pragma once

#include "libdefs.hpp"
#include "utils.hpp"
#include "error.hpp"
#include "var.hpp"
#include "convert.hpp"
#include "gil.hpp"
#include "interpreter.hpp"
