add_executable(console console.cpp)
target_link_libraries(console PRIVATE cppy3::cppy3)

add_executable(venv venv.cpp)
target_link_libraries(venv PRIVATE cppy3::cppy3)

add_executable(callback callback.cpp)
target_link_libraries(callback PRIVATE cppy3::cppy3)
