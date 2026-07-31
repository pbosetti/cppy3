cmake_minimum_required(VERSION 3.18)
project(cppy3 VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

option(CPPY3_BUILD_TESTS "Build cppy3 test suite" OFF)
option(CPPY3_BUILD_EXAMPLES "Build cppy3 examples" OFF)
option(CPPY3_BUILD_EXECUTABLES "Deprecated: sets both CPPY3_BUILD_TESTS and CPPY3_BUILD_EXAMPLES" OFF)
option(CPPY3_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (recoverable)" OFF)
option(CPPY3_BUILD_BUG_REPRO "Build the v1 bug-reproduction diagnostics used to audit pre-v2 behavior (see plan Part 1 / Phase 0)" OFF)

if(CPPY3_BUILD_EXECUTABLES)
    set(CPPY3_BUILD_TESTS ON)
    set(CPPY3_BUILD_EXAMPLES ON)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    # Without /utf-8, MSVC decodes non-ASCII narrow/wide string literals
    # using the build machine's active code page instead of the source
    # file's actual UTF-8 encoding, corrupting any literal Unicode text
    # (e.g. tests.cpp's Cyrillic/CJK round-trip tests) before it ever
    # reaches this project's own (correct) UTF-8 <-> wide-string codec.
    add_compile_options(/Zc:__cplusplus /utf-8)
endif()

if(CPPY3_SANITIZE)
    if(MSVC)
        message(WARNING "CPPY3_SANITIZE is not supported with MSVC yet; ignoring")
    else()
        message(STATUS "Building with ASan+UBSan (recoverable)")
        add_compile_options(-fsanitize=address,undefined -fsanitize-recover=address,undefined -fno-omit-frame-pointer -g -O1)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()

include(GNUInstallDirs)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development.Embed)
find_package(Python3 3.10 COMPONENTS NumPy)

message(STATUS "Found Python: ${Python3_EXECUTABLE} (${Python3_VERSION})")
if(Python3_NumPy_FOUND)
    message(STATUS "Found NumPy: ${Python3_NumPy_INCLUDE_DIRS}")
else()
    message(WARNING "Building cppy3 without numpy.ndarray support because NumPy is not found in system")
endif()

add_subdirectory(src)

if(CPPY3_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)

    # tools/migrate_v1_to_v2.py's own test suite (stdlib unittest, no
    # pytest/third-party dependency) -- verifies the migration script
    # actually produces what MIGRATION.md documents.
    add_test(
        NAME migrate_v1_to_v2_script
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_migrate.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

if(CPPY3_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CPPY3_BUILD_BUG_REPRO)
    add_subdirectory(tests/bug_repro)
endif()

include(CMakePackageConfigHelpers)

install(EXPORT cppy3Targets
    FILE cppy3Targets.cmake
    NAMESPACE cppy3::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppy3
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cppy3Config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cppy3Config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppy3
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cppy3ConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cppy3Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cppy3ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppy3
)
