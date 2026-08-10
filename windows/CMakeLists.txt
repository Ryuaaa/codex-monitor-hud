cmake_minimum_required(VERSION 3.21)

project(CodexMonitorHUDWindows VERSION 0.3.0 LANGUAGES CXX)

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

    add_executable(CodexMonitorModuleStateTests
        src/module_state.cpp
        tests/module_state_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorModuleStateTests)
    add_test(NAME windows_module_state COMMAND CodexMonitorModuleStateTests)

    add_executable(CodexMonitorSamplingScheduleTests
        src/sampling_schedule.cpp
        tests/sampling_schedule_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorSamplingScheduleTests)
    add_test(NAME windows_sampling_schedule COMMAND CodexMonitorSamplingScheduleTests)

    add_executable(CodexMonitorCodexRefreshScheduleTests
        src/codex/codex_refresh_schedule.cpp
        tests/codex_refresh_schedule_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexRefreshScheduleTests)
    add_test(NAME windows_codex_refresh_schedule
        COMMAND CodexMonitorCodexRefreshScheduleTests)
endif()

if(WIN32)
    if(BUILD_TESTING)
        add_executable(CodexMonitorFakeCodexAppServer
            tests/fake_codex_app_server.cpp
        )
        configure_codex_monitor_target(CodexMonitorFakeCodexAppServer)

        add_executable(CodexMonitorCodexProcessTests
            src/codex/codex_executable.cpp
            src/codex/codex_process.cpp
            tests/codex_process_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorCodexProcessTests)
        target_compile_definitions(CodexMonitorCodexProcessTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        add_test(NAME windows_codex_process
            COMMAND CodexMonitorCodexProcessTests
                    $<TARGET_FILE:CodexMonitorFakeCodexAppServer>)

        add_executable(CodexMonitorCodexJsonTests
            src/codex/codex_json_win32.cpp
            tests/codex_json_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorCodexJsonTests)
        target_compile_definitions(CodexMonitorCodexJsonTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorCodexJsonTests PRIVATE windowsapp)
        add_test(NAME windows_codex_json COMMAND CodexMonitorCodexJsonTests)
    endif()

    add_executable(CodexMonitorHUD WIN32
        src/main.cpp
        src/module_state.cpp
        src/performance_worker.cpp
        src/sampling_schedule.cpp
        src/settings_store_win32.cpp
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
    target_link_libraries(CodexMonitorHUD PRIVATE user32 gdi32 psapi shell32 ole32)
else()
    message(STATUS "Non-Windows host: configuring portable snapshot and state tests only.")
endif()
