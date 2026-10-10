# AnyPS5 Windows Gaming Launcher — Dear ImGui + Win32 + DirectX 11.
# Pinned dependency: Dear ImGui v1.91.8 vendored in 3rdparty/imgui (MIT).
# Why this stack:
#  - ImGui core/backends are plain C++ and MinGW-w64 clean (no MSVC-only code).
#  - Win32 backend = window/input/DPI infrastructure only; every pixel of the
#    interface is drawn by ImGui (no standard Win32 controls anywhere).
#  - DirectX 11 renderer = inbox OS component (d3d11.dll), MinGW headers/libs
#    available, no extra runtime to install (rejected WebView2: heavy runtime,
#    poor gamepad nav, no overlay story; rejected GLFW: duplicates Win32 infra).
#  - Built-in keyboard + gamepad navigation (io.NavInputs) maps to the Xbox pad.
#  - UTF-8 native: Russian text renders via Segoe UI with Cyrillic glyph ranges.
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/3rdparty/imgui)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_features(imgui PUBLIC cxx_std_17)
set_target_properties(imgui PROPERTIES CXX_EXTENSIONS OFF)

add_executable(anyps5-launcher WIN32
    src/main.cpp
    src/PlatformD3D11.cpp
    src/Theme.cpp
    src/Covers.cpp
    src/Config.cpp
    src/GameLibrary.cpp
    src/LaunchBackend.cpp
    src/InputManager.cpp
    src/GraphicsDiag.cpp
    src/LauncherApp.cpp
    src/Screens.cpp
)
target_include_directories(anyps5-launcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include
    ${CMAKE_SOURCE_DIR}/3rdparty/stb
)
target_link_libraries(anyps5-launcher PRIVATE imgui d3d11 d3dcompiler dwmapi winmm)
target_compile_features(anyps5-launcher PRIVATE cxx_std_20)
set_target_properties(anyps5-launcher PROPERTIES CXX_EXTENSIONS OFF)
if(MINGW)
    target_link_options(anyps5-launcher PRIVATE -static-libgcc -static-libstdc++)
endif()
