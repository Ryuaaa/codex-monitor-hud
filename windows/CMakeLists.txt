cmake_minimum_required(VERSION 3.21)

project(CodexMonitorHUDWindows VERSION 0.3.0 LANGUAGES CXX)

include(CTest)

set(CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256 "" CACHE STRING
    "SHA-256 fingerprint of the trusted Windows release signing certificate")
if(NOT CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256 STREQUAL "")
    string(LENGTH "${CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256}"
        CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256_LENGTH)
    if(NOT CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256_LENGTH EQUAL 64 OR
       NOT CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256 MATCHES
           "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR
            "CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256 must be exactly 64 hexadecimal characters")
    endif()
endif()

if(WIN32)
    enable_language(RC)
endif()

function(configure_codex_monitor_target target)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/resources
    )
    target_compile_definitions(${target} PRIVATE
        CODEX_MONITOR_WINDOWS_VERSION="${PROJECT_VERSION}"
        CODEX_MONITOR_WINDOWS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        CODEX_MONITOR_WINDOWS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        CODEX_MONITOR_WINDOWS_VERSION_PATCH=${PROJECT_VERSION_PATCH}
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
    add_executable(CodexMonitorPerformanceDiagnosisTests
        src/performance_diagnosis.cpp
        tests/performance_diagnosis_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorPerformanceDiagnosisTests)
    add_test(NAME windows_performance_diagnosis
        COMMAND CodexMonitorPerformanceDiagnosisTests)

    add_executable(CodexMonitorServiceStatusModelTests
        src/service_status_model.cpp
        tests/service_status_model_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorServiceStatusModelTests)
    add_test(NAME windows_service_status_model
        COMMAND CodexMonitorServiceStatusModelTests)

    add_executable(CodexMonitorSnapshotMathTests
        tests/snapshot_math_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorSnapshotMathTests)
    add_test(NAME windows_snapshot_math COMMAND CodexMonitorSnapshotMathTests)

    add_executable(CodexMonitorUILayoutMathTests
        src/ui_layout_math.cpp
        tests/ui_layout_math_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorUILayoutMathTests)
    add_test(NAME windows_ui_layout_math COMMAND CodexMonitorUILayoutMathTests)

    add_executable(CodexMonitorSystemIoRateTests
        src/system_io_rate.cpp
        tests/system_io_rate_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorSystemIoRateTests)
    add_test(NAME windows_system_io_rate COMMAND CodexMonitorSystemIoRateTests)

    add_executable(CodexMonitorSystemIoDisplayTests
        src/system_io_display.cpp
        tests/system_io_display_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorSystemIoDisplayTests)
    add_test(NAME windows_system_io_display
        COMMAND CodexMonitorSystemIoDisplayTests)

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

    add_executable(CodexMonitorCodexCostModelTests
        src/codex/codex_cost_model.cpp
        tests/codex_cost_model_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexCostModelTests)
    add_test(NAME windows_codex_cost_model COMMAND CodexMonitorCodexCostModelTests)

    add_executable(CodexMonitorCodexCostSummaryTests
        src/codex/codex_cost_model.cpp
        src/codex/codex_cost_summary.cpp
        tests/codex_cost_summary_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexCostSummaryTests)
    add_test(NAME windows_codex_cost_summary
        COMMAND CodexMonitorCodexCostSummaryTests)

    add_executable(CodexMonitorCodexCostHybridTests
        src/codex/codex_cost_model.cpp
        src/codex/codex_cost_summary.cpp
        src/codex/codex_cost_hybrid.cpp
        src/codex/codex_usage_math.cpp
        tests/codex_cost_hybrid_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexCostHybridTests)
    add_test(NAME windows_codex_cost_hybrid
        COMMAND CodexMonitorCodexCostHybridTests)

    add_executable(CodexMonitorCodexCostEventParserTests
        src/codex/codex_cost_model.cpp
        src/codex/codex_cost_event_parser.cpp
        tests/codex_cost_event_parser_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexCostEventParserTests)
    add_test(NAME windows_codex_cost_event_parser
        COMMAND CodexMonitorCodexCostEventParserTests)

    add_executable(CodexMonitorCodexCostFileScanTests
        src/codex/codex_cost_file_scan.cpp
        tests/codex_cost_file_scan_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexCostFileScanTests)
    add_test(NAME windows_codex_cost_file_scan
        COMMAND CodexMonitorCodexCostFileScanTests)

    add_executable(CodexMonitorCodexActivityScanTests
        src/codex/codex_activity_scan.cpp
        src/codex/codex_cost_file_scan.cpp
        tests/codex_activity_scan_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexActivityScanTests)
    add_test(NAME windows_codex_activity_scan
        COMMAND CodexMonitorCodexActivityScanTests)

    add_executable(CodexMonitorCodexCostHistoryStateTests
        src/codex/codex_cost_model.cpp
        src/codex/codex_cost_event_parser.cpp
        src/codex/codex_cost_history_state.cpp
        tests/codex_cost_history_state_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexCostHistoryStateTests)
    add_test(NAME windows_codex_cost_history_state
        COMMAND CodexMonitorCodexCostHistoryStateTests)

    add_executable(CodexMonitorCodexCostHistoryStoreTests
        src/codex/codex_cost_event_parser.cpp
        src/codex/codex_cost_history_state.cpp
        src/codex/codex_cost_history_store.cpp
        src/codex/codex_cost_model.cpp
        tests/codex_cost_history_store_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorCodexCostHistoryStoreTests)
    add_test(NAME windows_codex_cost_history_store
        COMMAND CodexMonitorCodexCostHistoryStoreTests)

    add_executable(CodexMonitorQuotaForecastTests
        src/codex/quota_forecast.cpp
        tests/quota_forecast_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorQuotaForecastTests)
    add_test(NAME windows_quota_forecast COMMAND CodexMonitorQuotaForecastTests)

    add_executable(CodexMonitorQuotaHistoryStoreTests
        src/codex/quota_history_store.cpp
        tests/quota_history_store_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorQuotaHistoryStoreTests)
    add_test(NAME windows_quota_history_store
        COMMAND CodexMonitorQuotaHistoryStoreTests)

    add_executable(CodexMonitorWeeklyQuotaAlertTests
        src/codex/weekly_quota_alert.cpp
        tests/weekly_quota_alert_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorWeeklyQuotaAlertTests)
    add_test(NAME windows_weekly_quota_alert
        COMMAND CodexMonitorWeeklyQuotaAlertTests)

    add_executable(CodexMonitorWeeklyQuotaAlertStateStoreTests
        src/codex/weekly_quota_alert.cpp
        src/codex/weekly_quota_alert_state_store.cpp
        tests/weekly_quota_alert_state_store_test.cpp
    )
    configure_codex_monitor_target(
        CodexMonitorWeeklyQuotaAlertStateStoreTests)
    add_test(NAME windows_weekly_quota_alert_state_store
        COMMAND CodexMonitorWeeklyQuotaAlertStateStoreTests)

    add_executable(CodexMonitorGitHubReleaseSelectorTests
        src/update/github_release_selector.cpp
        tests/github_release_selector_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorGitHubReleaseSelectorTests)
    add_test(NAME windows_github_release_selector
        COMMAND CodexMonitorGitHubReleaseSelectorTests)

    add_executable(CodexMonitorUpdateStateStoreTests
        src/update/update_state_store.cpp
        tests/update_state_store_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorUpdateStateStoreTests)
    add_test(NAME windows_update_state_store
        COMMAND CodexMonitorUpdateStateStoreTests)

    add_executable(CodexMonitorUpdateInstallerVerifierTests
        src/update/update_installer_verifier_win32.cpp
        tests/update_installer_verifier_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorUpdateInstallerVerifierTests)
    if(WIN32)
        target_compile_definitions(
            CodexMonitorUpdateInstallerVerifierTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(
            CodexMonitorUpdateInstallerVerifierTests PRIVATE bcrypt)
    endif()
    add_test(NAME windows_update_installer_verifier
        COMMAND CodexMonitorUpdateInstallerVerifierTests)

    add_executable(CodexMonitorUpdateAssetDownloadTests
        src/update/update_asset_download_win32.cpp
        tests/update_asset_download_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorUpdateAssetDownloadTests)
    if(WIN32)
        target_compile_definitions(
            CodexMonitorUpdateAssetDownloadTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(
            CodexMonitorUpdateAssetDownloadTests PRIVATE winhttp)
    endif()
    add_test(NAME windows_update_asset_download
        COMMAND CodexMonitorUpdateAssetDownloadTests)

    add_executable(CodexMonitorUpdateMsiIdentityTests
        src/update/update_msi_identity_win32.cpp
        tests/update_msi_identity_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorUpdateMsiIdentityTests)
    if(WIN32)
        target_compile_definitions(
            CodexMonitorUpdateMsiIdentityTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorUpdateMsiIdentityTests PRIVATE
            wintrust crypt32 msi)
    endif()
    add_test(NAME windows_update_msi_identity
        COMMAND CodexMonitorUpdateMsiIdentityTests)

    add_executable(CodexMonitorUpdateApplyTransactionTests
        src/update/update_apply_transaction_win32.cpp
        src/update/update_installer_verifier_win32.cpp
        src/update/update_msi_identity_win32.cpp
        tests/update_apply_transaction_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorUpdateApplyTransactionTests)
    target_compile_definitions(
        CodexMonitorUpdateApplyTransactionTests PRIVATE
        CODEX_MONITOR_UPDATE_APPLY_TRANSACTION_TESTING=1
    )
    if(WIN32)
        target_compile_definitions(
            CodexMonitorUpdateApplyTransactionTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorUpdateApplyTransactionTests PRIVATE
            bcrypt wintrust crypt32 msi)
    endif()
    add_test(NAME windows_update_apply_transaction
        COMMAND CodexMonitorUpdateApplyTransactionTests)

    add_executable(CodexMonitorUpdateHelperTests
        src/update/update_apply_transaction_win32.cpp
        src/update/update_helper_win32.cpp
        src/update/update_helper_launcher_win32.cpp
        src/update/update_installed_hud_win32.cpp
        src/update/update_installer_verifier_win32.cpp
        src/update/update_msi_identity_win32.cpp
        tests/update_helper_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorUpdateHelperTests)
    target_compile_definitions(
        CodexMonitorUpdateHelperTests PRIVATE
        CODEX_MONITOR_UPDATE_HELPER_TESTING=1
    )
    if(WIN32)
        target_compile_definitions(
            CodexMonitorUpdateHelperTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorUpdateHelperTests PRIVATE
            advapi32 bcrypt crypt32 msi shell32 user32 version wintrust)
    endif()
    add_test(NAME windows_update_helper
        COMMAND CodexMonitorUpdateHelperTests)
    set_tests_properties(windows_update_helper PROPERTIES TIMEOUT 20)

    add_executable(CodexMonitorUpdateDirectoryCleanupTests
        src/update/update_directory_cleanup_win32.cpp
        tests/update_directory_cleanup_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorUpdateDirectoryCleanupTests)
    if(WIN32)
        target_compile_definitions(
            CodexMonitorUpdateDirectoryCleanupTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
    endif()
    add_test(NAME windows_update_directory_cleanup
        COMMAND CodexMonitorUpdateDirectoryCleanupTests)

    add_executable(CodexMonitorUpdateInstallTests
        src/update/github_release_selector.cpp
        src/update/update_apply_transaction_win32.cpp
        src/update/update_asset_download_win32.cpp
        src/update/update_directory_cleanup_win32.cpp
        src/update/update_helper_launcher_win32.cpp
        src/update/update_helper_win32.cpp
        src/update/update_install_win32.cpp
        src/update/update_installed_hud_win32.cpp
        src/update/update_installer_verifier_win32.cpp
        src/update/update_msi_identity_win32.cpp
        tests/update_install_test.cpp
    )
    configure_codex_monitor_target(CodexMonitorUpdateInstallTests)
    if(WIN32)
        target_compile_definitions(CodexMonitorUpdateInstallTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorUpdateInstallTests PRIVATE
            advapi32 bcrypt crypt32 msi shell32 user32 version winhttp wintrust)
    endif()
    add_test(NAME windows_update_install
        COMMAND CodexMonitorUpdateInstallTests)
    set_tests_properties(windows_update_install PROPERTIES TIMEOUT 20)
endif()

if(WIN32)
    if(BUILD_TESTING)
        add_executable(CodexMonitorSystemIoSamplerTests
            src/system_io_rate.cpp
            src/system_io_sampler_win32.cpp
            tests/system_io_sampler_win32_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorSystemIoSamplerTests)
        target_compile_definitions(CodexMonitorSystemIoSamplerTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorSystemIoSamplerTests PRIVATE
            iphlpapi pdh)
        add_test(NAME windows_system_io_sampler
            COMMAND CodexMonitorSystemIoSamplerTests)

        add_executable(CodexMonitorCodexActivityWorkerTests
            src/codex/codex_activity_scan.cpp
            src/codex/codex_activity_worker.cpp
            src/codex/codex_cost_file_scan.cpp
            tests/codex_activity_worker_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorCodexActivityWorkerTests)
        target_compile_definitions(CodexMonitorCodexActivityWorkerTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorCodexActivityWorkerTests PRIVATE
            user32)
        add_test(NAME windows_codex_activity_worker
            COMMAND CodexMonitorCodexActivityWorkerTests)
        set_tests_properties(windows_codex_activity_worker
            PROPERTIES TIMEOUT 20)

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

        add_executable(CodexMonitorServiceStatusJsonTests
            src/service_status_json_win32.cpp
            tests/service_status_json_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorServiceStatusJsonTests)
        target_compile_definitions(CodexMonitorServiceStatusJsonTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorServiceStatusJsonTests PRIVATE windowsapp)
        add_test(NAME windows_service_status_json
            COMMAND CodexMonitorServiceStatusJsonTests)

        add_executable(CodexMonitorGitHubReleaseJsonTests
            src/update/github_release_json_win32.cpp
            tests/github_release_json_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorGitHubReleaseJsonTests)
        target_compile_definitions(CodexMonitorGitHubReleaseJsonTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorGitHubReleaseJsonTests PRIVATE windowsapp)
        add_test(NAME windows_github_release_json
            COMMAND CodexMonitorGitHubReleaseJsonTests)

        add_executable(CodexMonitorUpdateCheckTests
            src/update/github_release_json_win32.cpp
            src/update/github_release_selector.cpp
            src/update/update_check_win32.cpp
            src/update/update_fetch_win32.cpp
            tests/update_check_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorUpdateCheckTests)
        target_compile_definitions(CodexMonitorUpdateCheckTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorUpdateCheckTests PRIVATE
            winhttp windowsapp)
        add_test(NAME windows_update_check
            COMMAND CodexMonitorUpdateCheckTests)

        add_executable(CodexMonitorUpdateWorkerTests
            src/update/github_release_json_win32.cpp
            src/update/github_release_selector.cpp
            src/update/update_check_win32.cpp
            src/update/update_fetch_win32.cpp
            src/update/update_state_store.cpp
            src/update/update_worker.cpp
            tests/update_worker_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorUpdateWorkerTests)
        target_compile_definitions(CodexMonitorUpdateWorkerTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorUpdateWorkerTests PRIVATE
            user32 winhttp windowsapp)
        add_test(NAME windows_update_worker
            COMMAND CodexMonitorUpdateWorkerTests)
        set_tests_properties(windows_update_worker PROPERTIES TIMEOUT 20)

        add_executable(CodexMonitorUpdateInstallWorkerTests
            src/update/update_install_worker.cpp
            tests/update_install_worker_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorUpdateInstallWorkerTests)
        target_compile_definitions(CodexMonitorUpdateInstallWorkerTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorUpdateInstallWorkerTests PRIVATE
            user32)
        add_test(NAME windows_update_install_worker
            COMMAND CodexMonitorUpdateInstallWorkerTests)
        set_tests_properties(windows_update_install_worker PROPERTIES TIMEOUT 20)

        add_executable(CodexMonitorServiceStatusWorkerTests
            src/service_status_worker.cpp
            tests/service_status_worker_test.cpp
        )
        configure_codex_monitor_target(CodexMonitorServiceStatusWorkerTests)
        target_compile_definitions(CodexMonitorServiceStatusWorkerTests PRIVATE
            UNICODE
            _UNICODE
            NOMINMAX
            _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
            WINVER=0x0A00
            _WIN32_WINNT=0x0A00
        )
        target_link_libraries(CodexMonitorServiceStatusWorkerTests PRIVATE
            user32 windowsapp)
        add_test(NAME windows_service_status_worker
            COMMAND CodexMonitorServiceStatusWorkerTests)
        set_tests_properties(windows_service_status_worker
            PROPERTIES TIMEOUT 20)

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
            src/codex/codex_cost_event_parser.cpp
            src/codex/codex_cost_file_scan.cpp
            src/codex/codex_cost_history_state.cpp
            src/codex/codex_cost_history_store.cpp
            src/codex/codex_cost_model.cpp
            src/codex/codex_cost_summary.cpp
            src/codex/codex_executable.cpp
            src/codex/codex_json_win32.cpp
            src/codex/codex_process.cpp
            src/codex/quota_forecast.cpp
            src/codex/quota_history_store.cpp
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
        if(MSVC)
            target_compile_options(CodexMonitorCodexWorkerTests PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:/Zi>
                $<$<COMPILE_LANGUAGE:CXX>:/Zo>)
            target_link_options(CodexMonitorCodexWorkerTests PRIVATE
                /DEBUG:FULL
                /INCREMENTAL:NO
                /OPT:REF
                /OPT:ICF)
        endif()
        target_link_libraries(CodexMonitorCodexWorkerTests PRIVATE
            user32 windowsapp dbghelp)
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
        add_test(NAME windows_codex_worker_quota
            COMMAND CodexMonitorCodexWorkerTests
                    $<TARGET_FILE:CodexMonitorFakeCodexAppServer> quota)
        add_test(NAME windows_codex_worker_cost
            COMMAND CodexMonitorCodexWorkerTests
                    $<TARGET_FILE:CodexMonitorFakeCodexAppServer> cost)
        set_tests_properties(
            windows_codex_worker
            windows_codex_worker_success
            windows_codex_worker_pause
            windows_codex_worker_backoff
            windows_codex_worker_quota
            windows_codex_worker_cost
            PROPERTIES TIMEOUT 30)
    endif()

    add_executable(CodexMonitorHUD WIN32
        resources/CodexMonitorHUD.rc
        src/codex/codex_activity_scan.cpp
        src/codex/codex_activity_worker.cpp
        src/codex/codex_app_server_client.cpp
        src/codex/codex_cost_event_parser.cpp
        src/codex/codex_cost_file_scan.cpp
        src/codex/codex_cost_history_state.cpp
        src/codex/codex_cost_history_store.cpp
        src/codex/codex_cost_hybrid.cpp
        src/codex/codex_cost_model.cpp
        src/codex/codex_cost_summary.cpp
        src/codex/codex_executable.cpp
        src/codex/codex_json_win32.cpp
        src/codex/codex_process.cpp
        src/codex/quota_forecast.cpp
        src/codex/quota_history_store.cpp
        src/codex/codex_refresh_schedule.cpp
        src/codex/codex_usage_math.cpp
        src/codex/codex_worker.cpp
        src/main.cpp
        src/module_state.cpp
        src/performance_diagnosis.cpp
        src/performance_worker.cpp
        src/sampling_schedule.cpp
        src/service_status_fetch_win32.cpp
        src/service_status_json_win32.cpp
        src/service_status_model.cpp
        src/service_status_worker.cpp
        src/settings_store_win32.cpp
        src/system_io_display.cpp
        src/system_io_rate.cpp
        src/system_io_sampler_win32.cpp
        src/ui_layout_math.cpp
        src/update/github_release_json_win32.cpp
        src/update/github_release_selector.cpp
        src/update/update_check_win32.cpp
        src/update/update_asset_download_win32.cpp
        src/update/update_apply_transaction_win32.cpp
        src/update/update_directory_cleanup_win32.cpp
        src/update/update_fetch_win32.cpp
        src/update/update_helper_win32.cpp
        src/update/update_helper_launcher_win32.cpp
        src/update/update_install_win32.cpp
        src/update/update_install_worker.cpp
        src/update/update_installed_hud_win32.cpp
        src/update/update_installer_verifier_win32.cpp
        src/update/update_msi_identity_win32.cpp
        src/update/update_state_store.cpp
        src/update/update_worker.cpp
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
    if(NOT CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256 STREQUAL "")
        target_compile_definitions(CodexMonitorHUD PRIVATE
            "CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256=\"${CODEX_MONITOR_WINDOWS_PUBLISHER_SHA256}\"")
    endif()
    target_link_libraries(CodexMonitorHUD PRIVATE
        user32 gdi32 psapi shell32 ole32 winhttp windowsapp bcrypt iphlpapi pdh
        wintrust crypt32 msi advapi32 version)
else()
    message(STATUS "Non-Windows host: configuring portable snapshot and state tests only.")
endif()
