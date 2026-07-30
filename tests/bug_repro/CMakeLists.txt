# Phase 0 safety net: standalone diagnostics that reproduce specific bugs
# found in the v1 API against the shipped code, run individually (not via
# ctest, since several intentionally trip ASan/UBSan or fail to compile).
# See /Users/p4010/.claude/plans/this-is-an-utility-elegant-volcano.md Part 1.
#
# bug01 (Var copy-assignment), bug02 (call() double-decref), bug06/bug07
# (NDArray wrap()/dim() pointer and off-by-one bugs), bug09 (toString()
# leak), bug11 (injectVar() leak), and bug13 (getList() does not compile)
# have all been removed: the v1 API surface each one exercised (Var's old
# operator=, the implicit Var->PyObject* conversion, toString/toLong/
# toDouble, injectVar/getVar/getList/Main, and NDArray's old member-mutator
# wrap()/create()/dim() shape) no longer exists as of the Var and NDArray
# rewrites, so their repro code can no longer even compile. That's the
# intended outcome, not a regression -- see tests/tests.cpp for the
# regression coverage that replaced them (Var copy/move refcount
# correctness, call()/operator() argument refcount correctness, str()
# non-leaking, NDArray's 1D wrap() aliasing the caller's buffer correctly,
# and dim1()/dim2() being 0/1-indexed). The v1 evidence remains in git
# history (see the Phase 0 commit).
#
# Usage: cmake --build . --target bug05_wide_to_utf8_overflow && ./tests/bug_repro/bug05_wide_to_utf8_overflow

function(add_bug_repro name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE cppy3::cppy3)
endfunction()

add_bug_repro(bug05_wide_to_utf8_overflow)
