#pragma once

// Every translation unit in this project must include Python.h through this
// header instead of directly, to keep the two workarounds below applied
// consistently everywhere.

// PyArg_Parse*/Py_BuildValue's '#' format specifiers (used e.g. by the
// sys.stdout/stderr write() trampoline) raise "SystemError: PY_SSIZE_T_CLEAN
// macro must be defined for '#' formats" at runtime unless this is defined
// before Python.h is first included -- CPython silently accepted the legacy
// `int` size on some builds, which is why this went unnoticed until CI ran
// on a stricter one.
#define PY_SSIZE_T_CLEAN

// MSVC only: if this TU is compiled with the Debug runtime (_DEBUG defined,
// the default for CMAKE_BUILD_TYPE=Debug), Python.h unconditionally switches
// to the CPython debug ABI -- Py_INCREF/Py_DECREF expand to ref-tracking
// calls (_Py_IncRefTotal, etc.) that only exist in a debug build of Python
// itself (python3_d.lib). None of this project's supported ways of getting
// Python on Windows ship that debug build, so undefine _DEBUG around the
// include and restore it after; this is the standard workaround for
// embedding a release CPython from an MSVC Debug configuration.
#if defined(_MSC_VER) && defined(_DEBUG)
#define CPPY3_UNDEF_DEBUG_FOR_PYTHON_H
#undef _DEBUG
#endif

#include <Python.h>

#ifdef CPPY3_UNDEF_DEBUG_FOR_PYTHON_H
#define _DEBUG
#undef CPPY3_UNDEF_DEBUG_FOR_PYTHON_H
#endif
