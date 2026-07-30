include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

add_executable(tests tests.cpp)
target_link_libraries(tests PRIVATE cppy3::cppy3 Catch2::Catch2WithMain)

option(CPPY3_TEST_UNICODE_CONVERTERS "Test for proper working of Unicode conversion utilities" ON)
if(CPPY3_TEST_UNICODE_CONVERTERS)
    target_compile_definitions(tests PRIVATE TEST_UNICODE_CONVERTERS)
else()
    target_compile_definitions(tests PRIVATE TEST_UNICODE_CONVERTERS=0)
endif()

include(Catch)
catch_discover_tests(tests)
