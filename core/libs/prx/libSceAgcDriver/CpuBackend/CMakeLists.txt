add_library(agcCpuBackend STATIC
    src/Simd.cpp
    src/SimdSse.cpp
    src/SimdAvx2.cpp
    src/Decode.cpp
    src/Resources.cpp
    src/Exec.cpp
    src/Schedule.cpp
    src/Backend.cpp
    src/Bench.cpp
)
target_include_directories(agcCpuBackend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(agcCpuBackend PUBLIC cxx_std_20)
set_target_properties(agcCpuBackend PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/SimdSse.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(src/SimdAvx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
elseif(MSVC)
    set_source_files_properties(src/SimdAvx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
endif()
if(MINGW)
    target_compile_options(agcCpuBackend PRIVATE -fasynchronous-unwind-tables)
endif()
if(BUILD_TESTING)
    add_test_executable(cpu_backend_unit_tests tests/Unit.cpp)
    target_include_directories(cpu_backend_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(cpu_backend_unit_tests PRIVATE agcCpuBackend)
    set_target_properties(cpu_backend_unit_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(cpu_backend_unit_tests)
    add_test(NAME cpu_backend_unit COMMAND cpu_backend_unit_tests)
    add_test_executable(cpu_backend_bench_tests tests/Bench.cpp)
    target_include_directories(cpu_backend_bench_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(cpu_backend_bench_tests PRIVATE agcCpuBackend)
    set_target_properties(cpu_backend_bench_tests PROPERTIES CXX_EXTENSIONS OFF)
    configure_windows_unwind(cpu_backend_bench_tests)
    add_test(NAME cpu_backend_bench COMMAND cpu_backend_bench_tests)
    if(MINGW)
        target_compile_options(cpu_backend_unit_tests PRIVATE -fasynchronous-unwind-tables)
        target_compile_options(cpu_backend_bench_tests PRIVATE -fasynchronous-unwind-tables)
    endif()
endif()
