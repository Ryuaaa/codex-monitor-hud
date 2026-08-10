cmake_minimum_required(VERSION 3.21)

project(CodexMonitorHUDWindows VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Windows HUD target must be configured on Windows or with a Windows cross-toolchain.")
endif()

add_executable(CodexMonitorHUD WIN32
    src/main.cpp
)

target_compile_features(CodexMonitorHUD PRIVATE cxx_std_17)
target_compile_definitions(CodexMonitorHUD PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(CodexMonitorHUD PRIVATE /W4 /permissive- /utf-8 /EHsc)
    set_property(TARGET CodexMonitorHUD PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(CodexMonitorHUD PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(CodexMonitorHUD PRIVATE user32 gdi32)
