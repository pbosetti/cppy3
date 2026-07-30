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

if(Python3_NumPy_FOUND)
    target_sources(cppy3 PRIVATE cppy3_numpy.cpp)
    target_include_directories(cppy3 PUBLIC ${Python3_NumPy_INCLUDE_DIRS})
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
