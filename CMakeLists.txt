cmake_minimum_required(VERSION 3.12)
project(cppy3)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

option(CPPY3_USE_BOOST_CONVERT "use Boost.Locale instead of std::codecvt for string conversion" OFF)
option(CPPY3_BUILD_EXECUTABLES "Build cppy3 examples" OFF)
option(CPPY3_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (recoverable)" OFF)
option(CPPY3_BUILD_BUG_REPRO "Build the v1 bug-reproduction diagnostics used to audit pre-v2 behavior (see plan Part 1 / Phase 0)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/Zc:__cplusplus)
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

# find Boost if necessary
if(USE_BOOST_CONVERT)
    set(Boost_USE_STATIC_LIBS ON)
    set(Boost_USE_MULTITHREADED ON)
    find_package(Boost REQUIRED)
    message(STATUS "Using Boost string convert: Enabled")
    add_definitions(-DCPPY3_USE_BOOST_CONVERT)
endif()

find_package (Python3 3.5 REQUIRED COMPONENTS Interpreter Development NumPy)

message(STATUS "Found Python: ${Python3_FOUND} ${Python3_INTERPRETER_ID} ${Python3_EXECUTABLE}")
message(STATUS "Found Python3_LIBRARIES: ${Python3_LIBRARIES}")
message(STATUS "Found Python3_Development_FOUND: ${Python3_Development_FOUND}")
message(STATUS "Found Python3_NumPy_FOUND: ${Python3_NumPy_FOUND} ${Python3_NumPy_INCLUDE_DIRS}")

if (Python3_Development_FOUND)
    include_directories(${Python3_INCLUDE_DIRS})
else()
    message(FATAL_ERROR "Python dev C headers required (python-dev package)" )
endif()

if(Python3_NumPy_FOUND)
    add_definitions(-DCPPY3_BUILT_WITH_NUMPY)
else()
    message(WARNING "Building cppy3 without numpy.ndarray support because NumPy is not found in system")
endif()

# make lib
add_subdirectory(cppy3)

if(CPPY3_BUILD_EXECUTABLES)
    # make tests
    add_subdirectory(tests)
    # make examples
    add_executable(console examples/console.cpp)
    target_link_libraries(console cppy3)
endif()

if(CPPY3_BUILD_BUG_REPRO)
    add_subdirectory(tests/bug_repro)
endif()
