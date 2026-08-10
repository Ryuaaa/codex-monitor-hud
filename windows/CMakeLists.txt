cmake_minimum_required(VERSION 3.21)

project(CodexMonitorHUDWindows VERSION 0.3.0 LANGUAGES CXX)

include(CTest)

if(WIN32)
    enable_language(RC)
endif()

function(configure_codex_monitor_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:/W4>
            $<$<COMPILE_LANGUAGE:CXX>:/permissive->
            $<$<COMPILE_LANGUAGE:CXX>:/utf-8>
            $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
        )
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wall>
            $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
            $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
        )
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

    add_executable(CodexMonitorCodexUsageMathTests
        src/codex/codex_usage_math.cpp
        tests/codex_usage_math_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexUsageMathTests)
    add_test(NAME windows_codex_usage_math COMMAND CodexMonitorCodexUsageMathTests)
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
            _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorCodexJsonTests PRIVATE windowsapp)
        add_test(NAME windows_codex_json COMMAND CodexMonitorCodexJsonTests)

        add_executable(CodexMonitorCodexAppServerClientTests
            src/codex/codex_app_server_client.cpp
            src/codex/codex_executable.cpp
            src/codex/codex_json_win32.cpp
            src/codex/codex_process.cpp
            tests/codex_app_server_client_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorCodexAppServerClientTests)
        target_compile_definitions(CodexMonitorCodexAppServerClientTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorCodexAppServerClientTests PRIVATE windowsapp)
        add_test(NAME windows_codex_app_server_client
            COMMAND CodexMonitorCodexAppServerClientTests
                    $<TARGET_FILE:CodexMonitorFakeCodexAppServer>)

        add_executable(CodexMonitorCodexWorkerTests
            src/codex/codex_app_server_client.cpp
            src/codex/codex_executable.cpp
            src/codex/codex_json_win32.cpp
            src/codex/codex_process.cpp
            src/codex/codex_refresh_schedule.cpp
            src/codex/codex_worker.cpp
            tests/codex_worker_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorCodexWorkerTests)
        target_compile_definitions(CodexMonitorCodexWorkerTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorCodexWorkerTests PRIVATE user32 windowsapp)
        add_test(NAME windows_codex_worker
            COMMAND CodexMonitorCodexWorkerTests
                    $<TARGET_FILE:CodexMonitorFakeCodexAppServer>)
        add_test(NAME windows_codex_worker_success
            COMMAND CodexMonitorCodexWorkerTests
                    $<TARGET_FILE:CodexMonitorFakeCodexAppServer> success)
        add_test(NAME windows_codex_worker_pause
            COMMAND CodexMonitorCodexWorkerTests
                    $<TARGET_FILE:CodexMonitorFakeCodexAppServer> pause)
        add_test(NAME windows_codex_worker_backoff
            COMMAND CodexMonitorCodexWorkerTests
                    $<TARGET_FILE:CodexMonitorFakeCodexAppServer> backoff)
        set_tests_properties(
            windows_codex_worker
            windows_codex_worker_success
            windows_codex_worker_pause
            windows_codex_worker_backoff
            PROPERTIES TIMEOUT 30)
    endif()

    add_executable(CodexMonitorHUD WIN32
        resources/CodexMonitorHUD.rc
        src/codex/codex_app_server_client.cpp
        src/codex/codex_executable.cpp
        src/codex/codex_json_win32.cpp
        src/codex/codex_process.cpp
        src/codex/codex_refresh_schedule.cpp
        src/codex/codex_usage_math.cpp
        src/codex/codex_worker.cpp
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
        _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )
    target_link_libraries(CodexMonitorHUD PRIVATE
        user32 gdi32 psapi shell32 ole32 windowsapp)
else()
    message(STATUS "Non-Windows host: configuring portable snapshot and state tests only.")
endif()
