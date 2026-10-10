# gpu_research demos — standalone CMake project (NOT part of the game build)
#
# Requires (NOT downloaded automatically):
#   - LunarG Vulkan SDK with headers (VULKAN_SDK env) and glslangValidator
#   - CMake >= 3.22, Ninja, MinGW-w64 GCC (same toolchain as the game)
# Configure ONLY after installing the SDK manually:
#   cmake -S gpu_research/demos -B gpu_research/demos/build -G Ninja
#   cmake --build gpu_research/demos/build
# The build directory must stay out of version control (see .gitignore note in 00_plan.md).

cmake_minimum_required(VERSION 3.22)
project(gpu_research_demos CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK is not set. Install the LunarG Vulkan SDK manually first (see gpu_research/sources.md #17).")
endif()
set(VULKAN_SDK_DIR $ENV{VULKAN_SDK})
find_path(VULKAN_HEADERS vulkan/vulkan.h PATHS "${VULKAN_SDK_DIR}/Include" NO_DEFAULT_PATH)
if(NOT VULKAN_HEADERS)
    message(FATAL_ERROR "vulkan/vulkan.h not found under $ENV{VULKAN_SDK}/Include")
endif()
find_program(GLSLANG_VALIDATOR glslangValidator PATHS "${VULKAN_SDK_DIR}/Bin" NO_DEFAULT_PATH)
if(NOT GLSLANG_VALIDATOR)
    message(FATAL_ERROR "glslangValidator not found under $ENV{VULKAN_SDK}/Bin")
endif()

# Compile a GLSL shader to SPIR-V, then embed it as a C array header.
function(add_spv_header target_name shader_path out_header symbol)
    set(spv_file "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.spv")
    add_custom_command(
        OUTPUT "${spv_file}"
        COMMAND "${GLSLANG_VALIDATOR}" -V "${shader_path}" -o "${spv_file}"
        DEPENDS "${shader_path}"
        COMMENT "glslangValidator ${shader_path}")
    # Portable .spv -> .h conversion with CMake script mode (no xxd needed).
    set(header_file "${CMAKE_CURRENT_BINARY_DIR}/${out_header}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/spv_to_header_${target_name}.cmake"
        "file(READ \"${spv_file}\" bytes HEX)\n"
        "string(REGEX REPLACE \"(..)\" \"0x\\\\1,\" spaced \"\${bytes}\")\n"
        "string(REGEX REPLACE \",\$\" \"\" spaced \"\${spaced}\")\n"
        "file(WRITE \"${header_file}\" \"#pragma once\\n#include <cstddef>\\n#include <cstdint>\\n\")\n"
        "file(APPEND \"${header_file}\" \"static const std::uint32_t ${symbol}[] = {\")\n"
        "string(LENGTH \"\${bytes}\" hexlen)\n"
        "math(EXPR words \"\${hexlen} / 8\")\n"
        "set(idx 0)\n"
        "while(idx LESS \${words})\n"
        "  math(EXPR off \"\${idx} * 8\")\n"
        "  string(SUBSTRING \"\${bytes}\" \${off} 8 word)\n"
        "  string(SUBSTRING \"\${word}\" 6 2 b0)\n"
        "  string(SUBSTRING \"\${word}\" 4 2 b1)\n"
        "  string(SUBSTRING \"\${word}\" 2 2 b2)\n"
        "  string(SUBSTRING \"\${word}\" 0 2 b3)\n"
        "  file(APPEND \"${header_file}\" \"0x\${b0}\${b1}\${b2}\${b3},\")\n"
        "  math(EXPR idx \"\${idx} + 1\")\n"
        "endwhile()\n"
        "file(APPEND \"${header_file}\" \"};\\nstatic const std::size_t ${symbol}Words = sizeof(${symbol}) / sizeof(std::uint32_t);\\n\")\n")
    add_custom_command(
        OUTPUT "${header_file}"
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/spv_to_header_${target_name}.cmake"
        DEPENDS "${spv_file}"
        COMMENT "embedding ${spv_file}")
    set(${target_name}_HEADER "${header_file}" PARENT_SCOPE)
endfunction()

function(add_demo name source)
    add_executable(${name} ${source} ${ARGN})
    target_include_directories(${name} PRIVATE "${VULKAN_HEADERS}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    if(WIN32)
        target_link_libraries(${name} PRIVATE vulkan-1)
        target_link_directories(${name} PRIVATE "${VULKAN_SDK_DIR}/Lib")
    endif()
endfunction()

add_spv_header(tri_vert "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.vert" tri_vert.h kVertSpv)
add_spv_header(tri_frag "${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.frag" tri_frag.h kFragSpv)
add_spv_header(tex_vert "${CMAKE_CURRENT_SOURCE_DIR}/shaders/textured.vert" tex_vert.h kTexturedVertSpv)
add_spv_header(tex_frag "${CMAKE_CURRENT_SOURCE_DIR}/shaders/textured.frag" tex_frag.h kTexturedFragSpv)
add_spv_header(bda_comp "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bda_write.comp" bda_comp.h kBdaWriteSpv)

add_demo(01_device 01_device/device_check.cpp)
add_demo(02_triangle 02_triangle/triangle.cpp ${CMAKE_CURRENT_BINARY_DIR}/tri_vert.h ${CMAKE_CURRENT_BINARY_DIR}/tri_frag.h)
add_demo(03_texture 03_texture/texture_upload.cpp ${CMAKE_CURRENT_BINARY_DIR}/tex_vert.h ${CMAKE_CURRENT_BINARY_DIR}/tex_frag.h)
add_demo(04_copies 04_copies/buffer_copies.cpp)
add_demo(05_bda 05_bda/bda_roundtrip.cpp ${CMAKE_CURRENT_BINARY_DIR}/bda_comp.h)
add_demo(06_coherence 06_coherence/coherence.cpp ${CMAKE_CURRENT_BINARY_DIR}/bda_comp.h)
add_demo(07_sync 07_sync/queue_sync.cpp)
add_demo(08_negative 08_negative/validation_dictionary.cpp)
