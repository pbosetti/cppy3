# Phase 0 safety net: standalone diagnostics that reproduce specific bugs
# found in the v1 API against the shipped code, run individually (not via
# ctest, since several intentionally trip ASan/UBSan or fail to compile).
# See /Users/p4010/.claude/plans/this-is-an-utility-elegant-volcano.md Part 1.
#
# Usage: cmake --build . --target bug01_var_copy_assign && ./tests/bug_repro/bug01_var_copy_assign

function(add_bug_repro name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE cppy3::cppy3)
endfunction()

add_bug_repro(bug01_var_copy_assign)
add_bug_repro(bug02_call_args_double_decref)
add_bug_repro(bug05_wide_to_utf8_overflow)
add_bug_repro(bug06_ndarray_wrap1d)
add_bug_repro(bug07_ndarray_dim_off_by_one)
add_bug_repro(bug09_tostring_leak)
add_bug_repro(bug11_injectvar_leak)

# EXCLUDE_FROM_ALL: this target is expected to FAIL TO COMPILE (see the file
# for why) and must never be built by a plain `cmake --build .`.
add_executable(bug13_getlist_does_not_compile EXCLUDE_FROM_ALL bug13_getlist_does_not_compile.cpp)
target_link_libraries(bug13_getlist_does_not_compile PRIVATE cppy3::cppy3)
