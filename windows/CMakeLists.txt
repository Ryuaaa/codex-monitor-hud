cmake_minimum_required(VERSION 3.21)

project(CodexMonitorHUDWindows VERSION 0.2.0 LANGUAGES CXX)

include(CTest)

function(configure_codex_monitor_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

if(BUILD_TESTING)
    add_executable(CodexMonitorSnapshotMathTests
        tests/snapshot_math_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorSnapshotMathTests)
    add_test(NAME windows_snapshot_math COMMAND CodexMonitorSnapshotMathTests)
endif()

if(WIN32)
    add_executable(CodexMonitorHUD WIN32
        src/main.cpp
        src/windows_sampler.cpp
    )
    configure_codex_monitor_target(CodexMonitorHUD)
    target_compile_definitions(CodexMonitorHUD PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        PSAPI_VERSION=1
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )
    target_link_libraries(CodexMonitorHUD PRIVATE user32 gdi32 psapi)
else()
    message(STATUS "Non-Windows host: configuring portable snapshot tests only.")
endif()
