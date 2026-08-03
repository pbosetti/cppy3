include(GenerateExportHeader)

add_library(cppy3 utils.cpp error.cpp var.cpp gil.cpp interpreter.cpp)
add_library(cppy3::cppy3 ALIAS cppy3)

set_target_properties(cppy3 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_compile_features(cppy3 PUBLIC cxx_std_20)

target_include_directories(cppy3 PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(cppy3 PUBLIC Python3::Python)

if(WIN32)
    # detect_python_home()'s last-resort registry lookup (RegGetValueW).
    # PUBLIC, not PRIVATE: cppy3 defaults to a static library, so the
    # dependency has to reach whoever links it -- including consumers of the
    # exported cppy3::cppy3 target.
    target_link_libraries(cppy3 PUBLIC advapi32)
endif()

# Generates include/cppy3/cppy3_export.h (build tree) with real
# __declspec(dllexport/dllimport) / visibility("default") annotations
# derived from the target's actual BUILD_SHARED_LIBS setting, replacing the
# permanently-disabled Windows export macro that made shared builds
# impossible (see plan bug #37).
generate_export_header(cppy3
    BASE_NAME CPPY3
    EXPORT_MACRO_NAME CPPY3_API
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/cppy3/cppy3_export.h"
)

# Record the prefix of the Python installation this build links against, so
# detect_python_home() has a last-resort candidate at runtime. On POSIX,
# CPython bakes exactly this into its own binary as the PREFIX macro and
# getpath.py falls back to it; Windows builds have no such compile-time
# constant, which is what made zero-config embedding fragile there (see
# detect_python_home() in interpreter.cpp). base_prefix, not prefix, so that
# configuring from inside a venv still records the real installation.
execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import sys; sys.stdout.write(sys.base_prefix)"
    OUTPUT_VARIABLE CPPY3_BUILD_PYTHON_HOME
    RESULT_VARIABLE _cppy3_base_prefix_result
    ERROR_QUIET
)
if(NOT _cppy3_base_prefix_result EQUAL 0)
    set(CPPY3_BUILD_PYTHON_HOME "")
    message(WARNING "Could not determine sys.base_prefix of ${Python3_EXECUTABLE}; "
                    "zero-config Interpreter construction will not be able to fall back to it")
else()
    # Forward slashes work in every Windows path API and std::filesystem, and
    # keep the generated header free of escaping hazards.
    file(TO_CMAKE_PATH "${CPPY3_BUILD_PYTHON_HOME}" CPPY3_BUILD_PYTHON_HOME)
endif()
message(STATUS "Python home baked in as detect_python_home() fallback: ${CPPY3_BUILD_PYTHON_HOME}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/build_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cppy3/cppy3_build_config.h"
    @ONLY
)

if(Python3_NumPy_FOUND)
    target_sources(cppy3 PRIVATE cppy3_numpy.cpp)
    target_include_directories(cppy3 PUBLIC $<BUILD_INTERFACE:${Python3_NumPy_INCLUDE_DIRS}>)
    target_compile_definitions(cppy3 PUBLIC CPPY3_BUILT_WITH_NUMPY)
    target_link_libraries(cppy3 PUBLIC Python3::NumPy)
endif()

install(TARGETS cppy3
    EXPORT cppy3Targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/cppy3 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cppy3/cppy3_export.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cppy3)
