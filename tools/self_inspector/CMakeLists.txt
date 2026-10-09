cmake_minimum_required(VERSION 3.20)
project(SelfInspector CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(self_inspector
    src/main.cpp
    src/SelfInspector.cpp
)
target_include_directories(self_inspector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(self_inspector_tests
    tests/test_self_inspector.cpp
    src/SelfInspector.cpp
)
target_include_directories(self_inspector_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

enable_testing()
add_test(NAME self_inspector_tests COMMAND self_inspector_tests)
set_tests_properties(self_inspector_tests PROPERTIES TIMEOUT 120)
