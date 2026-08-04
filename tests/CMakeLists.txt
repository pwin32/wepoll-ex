if(WEPOLL_EX_PLATFORM_POSIX)
    add_executable(test_wepoll_ex test_api.c)
    target_link_libraries(test_wepoll_ex PRIVATE wepoll_ex::wepoll_ex)

    add_test(NAME wepoll_ex_api COMMAND test_wepoll_ex)
    set_tests_properties(wepoll_ex_api PROPERTIES TIMEOUT 30)

    add_executable(test_wepoll_ex_posix_large_wait
        test_posix_large_wait.c)
    target_link_libraries(test_wepoll_ex_posix_large_wait PRIVATE
        wepoll_ex::wepoll_ex)

    add_test(NAME wepoll_ex_posix_large_wait
        COMMAND test_wepoll_ex_posix_large_wait)
    set_tests_properties(wepoll_ex_posix_large_wait PROPERTIES
        TIMEOUT 60 SKIP_RETURN_CODE 77)

    if(WEPOLL_EX_BUILD_STATIC)
        add_executable(test_wepoll_ex_pool test_pool.c)
        target_include_directories(test_wepoll_ex_pool PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(test_wepoll_ex_pool PRIVATE wepoll_ex_static)

        add_test(NAME wepoll_ex_pool_mpsc COMMAND test_wepoll_ex_pool)
        set_tests_properties(wepoll_ex_pool_mpsc PROPERTIES TIMEOUT 60)
    else()
        message(STATUS "Skipping pool test: it requires the internal static library symbols")
    endif()
else()
    add_executable(test_wepoll_ex_windows test_windows_api.c)
    target_compile_definitions(test_wepoll_ex_windows PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN)

    add_executable(test_wepoll_ex_windows_identity
        test_windows_identity.c)
    target_compile_definitions(test_wepoll_ex_windows_identity PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN
        ${WEPOLL_EX_PLATFORM_DEFINITIONS})

    add_executable(test_wepoll_ex_windows_ipv6
        test_windows_ipv6.c)
    target_compile_definitions(test_wepoll_ex_windows_ipv6 PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN)

    add_executable(test_wepoll_ex_windows_backpressure
        test_windows_backpressure.c)
    target_compile_definitions(test_wepoll_ex_windows_backpressure PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN)

    add_executable(test_wepoll_ex_windows_stress
        test_windows_stress.c)
    target_compile_definitions(test_wepoll_ex_windows_stress PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN)

    add_executable(test_wepoll_ex_windows_edge
        test_windows_edge.c)
    target_compile_definitions(test_wepoll_ex_windows_edge PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN)

    add_executable(test_wepoll_ex_windows_waitable
        test_windows_waitable.c)
    target_compile_definitions(test_wepoll_ex_windows_waitable PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN)

    add_executable(test_wepoll_ex_windows_pipe
        test_windows_pipe.c)
    target_compile_definitions(test_wepoll_ex_windows_pipe PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN)

    add_executable(test_wepoll_ex_windows_socket_events
        test_windows_socket_events.c)
    target_compile_definitions(test_wepoll_ex_windows_socket_events PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN)

    if(WEPOLL_EX_BUILD_STATIC)
        target_link_libraries(test_wepoll_ex_windows PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)
        target_link_libraries(test_wepoll_ex_windows_identity PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)
        target_link_libraries(test_wepoll_ex_windows_ipv6 PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)
        target_link_libraries(test_wepoll_ex_windows_backpressure PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)
        target_link_libraries(test_wepoll_ex_windows_stress PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)
        target_link_libraries(test_wepoll_ex_windows_edge PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)
        target_link_libraries(test_wepoll_ex_windows_waitable PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)
        target_link_libraries(test_wepoll_ex_windows_pipe PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)
        target_link_libraries(test_wepoll_ex_windows_socket_events PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_pool test_pool.c)
        target_include_directories(test_wepoll_ex_pool PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_pool PRIVATE
            ${WEPOLL_EX_PLATFORM_DEFINITIONS})
        target_link_libraries(test_wepoll_ex_pool PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_windows_close_refs
            test_windows_close_refs.c)
        target_include_directories(test_wepoll_ex_windows_close_refs PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_windows_close_refs PRIVATE
            ${WEPOLL_EX_PLATFORM_DEFINITIONS})
        target_link_libraries(test_wepoll_ex_windows_close_refs PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_windows_faults
            test_windows_faults.c)
        target_include_directories(test_wepoll_ex_windows_faults PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_windows_faults PRIVATE
            WEPOLL_EX_ENABLE_FAULT_INJECTION=1
            ${WEPOLL_EX_PLATFORM_DEFINITIONS})

        # Compile a private copy of the implementation with deterministic
        # fault hooks enabled.  Normal libraries (including installed and
        # nginx-embedded builds) compile those hooks to zero-cost no-ops.
        set(_wepoll_ex_fault_test_sources)
        foreach(_source IN LISTS WEPOLL_EX_SOURCES WEPOLL_EX_HEADERS)
            list(APPEND _wepoll_ex_fault_test_sources
                "${PROJECT_SOURCE_DIR}/${_source}")
        endforeach()
        add_library(wepoll_ex_fault_test_static STATIC
            ${_wepoll_ex_fault_test_sources})
        unset(_wepoll_ex_fault_test_sources)
        target_include_directories(wepoll_ex_fault_test_static
            PUBLIC  ${PROJECT_SOURCE_DIR}/include
            PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(wepoll_ex_fault_test_static
            PUBLIC  WEPOLL_EX_STATIC
            PRIVATE WEPOLL_EX_ENABLE_FAULT_INJECTION=1
                    ${WEPOLL_EX_PLATFORM_DEFINITIONS})
        target_link_libraries(wepoll_ex_fault_test_static PRIVATE
            Threads::Threads ws2_32 ntdll)
        if(WEPOLL_EX_WINPTHREAD_LINK_LIBS)
            target_link_libraries(wepoll_ex_fault_test_static INTERFACE
                ${WEPOLL_EX_WINPTHREAD_LINK_LIBS})
        endif()
        target_link_libraries(test_wepoll_ex_windows_faults PRIVATE
            wepoll_ex_fault_test_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_windows_iocp_batch
            test_windows_iocp_batch.c)
        target_include_directories(test_wepoll_ex_windows_iocp_batch PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_windows_iocp_batch PRIVATE
            _WIN32_WINNT=0x0602
            WIN32_LEAN_AND_MEAN
            ${WEPOLL_EX_PLATFORM_DEFINITIONS})
        target_link_libraries(test_wepoll_ex_windows_iocp_batch PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_windows_pwait2
            test_windows_pwait2.c)
        target_include_directories(test_wepoll_ex_windows_pwait2 PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_windows_pwait2 PRIVATE
            _WIN32_WINNT=0x0602
            WIN32_LEAN_AND_MEAN
            ${WEPOLL_EX_PLATFORM_DEFINITIONS})
        target_link_libraries(test_wepoll_ex_windows_pwait2 PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_windows_lifecycle
            test_windows_lifecycle.c)
        target_include_directories(test_wepoll_ex_windows_lifecycle PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_windows_lifecycle PRIVATE
            _WIN32_WINNT=0x0602
            WIN32_LEAN_AND_MEAN
            ${WEPOLL_EX_PLATFORM_DEFINITIONS})
        target_link_libraries(test_wepoll_ex_windows_lifecycle PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_windows_state
            test_windows_state.c)
        target_include_directories(test_wepoll_ex_windows_state PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_windows_state PRIVATE
            _WIN32_WINNT=0x0602
            WIN32_LEAN_AND_MEAN
            ${WEPOLL_EX_PLATFORM_DEFINITIONS})
        target_link_libraries(test_wepoll_ex_windows_state PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_windows_events
            test_windows_events.c)
        target_include_directories(test_wepoll_ex_windows_events PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_windows_events PRIVATE
            _WIN32_WINNT=0x0602
            WIN32_LEAN_AND_MEAN
            ${WEPOLL_EX_PLATFORM_DEFINITIONS})
        target_link_libraries(test_wepoll_ex_windows_events PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_windows_compat
            test_windows_compat.c)
        target_include_directories(test_wepoll_ex_windows_compat PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_windows_compat PRIVATE
            _WIN32_WINNT=0x0602
            WIN32_LEAN_AND_MEAN
            ${WEPOLL_EX_PLATFORM_DEFINITIONS})
        target_link_libraries(test_wepoll_ex_windows_compat PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        foreach(_lifecycle_mode IN ITEMS
                cancel-failure
                del-cancel-failure
                iocp-failure
                drain-timeout
                late-completion-reap
                wake-failure)
            string(REPLACE "-" "_" _lifecycle_name "${_lifecycle_mode}")
            add_test(NAME wepoll_ex_windows_${_lifecycle_name}
                COMMAND test_wepoll_ex_windows_lifecycle
                    ${_lifecycle_mode})
            set_tests_properties(wepoll_ex_windows_${_lifecycle_name}
                PROPERTIES TIMEOUT 15)
        endforeach()

        foreach(_state_mode IN ITEMS
                queued-rearm
                queued-mod
                socket-add-submit-failure
                socket-add-immediate-success
                aux-add-lazy
                failed-mod
                failed-rearm
                pending-metadata-mod
                pending-narrowing-mod
                pending-expansion-mod
                pending-expansion-ready-race
                tcp-current-rdhup
                tcp-current-reset
                transitional-idle
                udp-readless-error
                udp-readless-park
                udp-readless-rollback
                aux-posted-cancel
                waitable-zero-callback
                waitable-zero-ready
                waitable-queued-rearm
                aux-post-close-lease
                pipe-invalid-et
                large-wait
                exclusive-mixed-submit)
            string(REPLACE "-" "_" _state_name "${_state_mode}")
            add_test(NAME wepoll_ex_windows_state_${_state_name}
                COMMAND test_wepoll_ex_windows_state ${_state_mode})
            set_tests_properties(wepoll_ex_windows_state_${_state_name}
                PROPERTIES TIMEOUT 15)
        endforeach()
        foreach(_events_mode IN ITEMS mapping status fin spin)
            add_test(NAME wepoll_ex_windows_events_${_events_mode}
                COMMAND test_wepoll_ex_windows_events ${_events_mode})
            set_tests_properties(wepoll_ex_windows_events_${_events_mode}
                PROPERTIES TIMEOUT 10)
        endforeach()
    else()
        target_link_libraries(test_wepoll_ex_windows PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
        target_link_libraries(test_wepoll_ex_windows_identity PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
        target_link_libraries(test_wepoll_ex_windows_ipv6 PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
        target_link_libraries(test_wepoll_ex_windows_backpressure PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
        target_link_libraries(test_wepoll_ex_windows_stress PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
        target_link_libraries(test_wepoll_ex_windows_edge PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
        target_link_libraries(test_wepoll_ex_windows_waitable PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
        target_link_libraries(test_wepoll_ex_windows_pipe PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
        target_link_libraries(test_wepoll_ex_windows_socket_events PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
    endif()

    add_test(NAME wepoll_ex_windows_api COMMAND test_wepoll_ex_windows)
    set_tests_properties(wepoll_ex_windows_api PROPERTIES TIMEOUT 120)

    add_test(NAME wepoll_ex_windows_ipv6
        COMMAND test_wepoll_ex_windows_ipv6)
    set_tests_properties(wepoll_ex_windows_ipv6
        PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)

    add_test(NAME wepoll_ex_windows_backpressure
        COMMAND test_wepoll_ex_windows_backpressure)
    set_tests_properties(wepoll_ex_windows_backpressure PROPERTIES TIMEOUT 30)

    add_test(NAME wepoll_ex_windows_stress
        COMMAND test_wepoll_ex_windows_stress)
    set_tests_properties(wepoll_ex_windows_stress PROPERTIES TIMEOUT 30)

    set(_wepoll_ex_shared_runtime_tests
        wepoll_ex_windows_api
        wepoll_ex_windows_ipv6
        wepoll_ex_windows_backpressure
        wepoll_ex_windows_stress)

    foreach(_socket_event_mode IN ITEMS
            aliases
            inert-bits
            round-robin
            duplicate-registration
            oob-lt
            oob-et
            oob-oneshot
            oob-mod
            oob-inline-lt
            oob-inline-et
            udp-v4
            udp-v6
            udp-readless-data
            prewait-refresh
            prewait-scale
            udp-error
            udp-error-v6
            udp-error-v6-et
            udp-error-v6-oneshot
            udp-error-v6-zero
            udp-error-v6-err
            udp-error-v6-readless-et
            udp-error-v6-readless-oneshot
            udp-error-v6-out-et
            udp-iocp)
        string(REPLACE "-" "_" _socket_event_name
            "${_socket_event_mode}")
        add_test(NAME wepoll_ex_windows_socket_events_${_socket_event_name}
            COMMAND test_wepoll_ex_windows_socket_events
                ${_socket_event_mode})
        set_tests_properties(
            wepoll_ex_windows_socket_events_${_socket_event_name}
            PROPERTIES TIMEOUT 30)
        if(_socket_event_mode STREQUAL "udp-v6" OR
           _socket_event_mode STREQUAL "udp-error" OR
           _socket_event_mode STREQUAL "udp-error-v6" OR
           _socket_event_mode STREQUAL "udp-error-v6-et" OR
           _socket_event_mode STREQUAL "udp-error-v6-oneshot" OR
           _socket_event_mode STREQUAL "udp-error-v6-zero" OR
           _socket_event_mode STREQUAL "udp-error-v6-err" OR
           _socket_event_mode STREQUAL "udp-error-v6-readless-et" OR
           _socket_event_mode STREQUAL
               "udp-error-v6-readless-oneshot" OR
           _socket_event_mode STREQUAL "udp-error-v6-out-et")
            set_tests_properties(
                wepoll_ex_windows_socket_events_${_socket_event_name}
                PROPERTIES SKIP_RETURN_CODE 77)
        endif()
        list(APPEND _wepoll_ex_shared_runtime_tests
            wepoll_ex_windows_socket_events_${_socket_event_name})
    endforeach()

    foreach(_edge_mode IN ITEMS
            readable
            same-epfd-et
            writable
            exclusive-mod
            exclusive-wake
            exclusive-et
            exclusive-mixed
            ordinary-multi
            ordinary-prearmed
            exclusive-disjoint
            exclusive-scale
            exclusive-invalid
            pwait-sigmask
            wakeup-flag)
        string(REPLACE "-" "_" _edge_name "${_edge_mode}")
        add_test(NAME wepoll_ex_windows_edge_${_edge_name}
            COMMAND test_wepoll_ex_windows_edge ${_edge_mode})
        set_tests_properties(wepoll_ex_windows_edge_${_edge_name}
            PROPERTIES TIMEOUT 20)
        if(_edge_mode STREQUAL "exclusive-scale" OR
           _edge_mode STREQUAL "exclusive-mixed" OR
           _edge_mode STREQUAL "ordinary-multi" OR
           _edge_mode STREQUAL "ordinary-prearmed")
            set_tests_properties(wepoll_ex_windows_edge_${_edge_name}
                PROPERTIES TIMEOUT 60)
        endif()
        list(APPEND _wepoll_ex_shared_runtime_tests
            wepoll_ex_windows_edge_${_edge_name})
    endforeach()

    foreach(_waitable_mode IN ITEMS
            event
            event-et
            auto-reset
            auto-reset-et
            semaphore
            semaphore-et
            zero-semaphore
            zero-pending
            zero-callback
            zero-ready
            queued-rearm
            pending-mod
            ready-mod
            timer-ready-mod
            timer-et
            job
            access
            process-thread
            zero-thread
            thread-et
            exclusive
            mutex
            cleanup
            cancel
            cancel-batch)
        string(REPLACE "-" "_" _waitable_name "${_waitable_mode}")
        add_test(NAME wepoll_ex_windows_waitable_${_waitable_name}
            COMMAND test_wepoll_ex_windows_waitable ${_waitable_mode})
        set_tests_properties(wepoll_ex_windows_waitable_${_waitable_name}
            PROPERTIES TIMEOUT 20)
        list(APPEND _wepoll_ex_shared_runtime_tests
            wepoll_ex_windows_waitable_${_waitable_name})
    endforeach()

    foreach(_pipe_mode IN ITEMS
            readable
            et
            writable
            direction
            oneshot
            named
            hup
            exact-eof
            aliases
            broken-writer
            broken-writer-et
            terminal-oneshot
            terminal-oneshot-rearm
            et-hup
            write-lt
            write-et
            named-overlapped
            named-outbound-fallback
            named-reconnect-et
            named-duplex-et-mixed
            mod
            exclusive
            cancel
            timer
            disk)
        string(REPLACE "-" "_" _pipe_name "${_pipe_mode}")
        add_test(NAME wepoll_ex_windows_pipe_${_pipe_name}
            COMMAND test_wepoll_ex_windows_pipe ${_pipe_mode})
        set_tests_properties(wepoll_ex_windows_pipe_${_pipe_name}
            PROPERTIES TIMEOUT 20)
        list(APPEND _wepoll_ex_shared_runtime_tests
            wepoll_ex_windows_pipe_${_pipe_name})
    endforeach()

    if(WEPOLL_EX_BUILD_STATIC)
        add_test(NAME wepoll_ex_pool_mpsc COMMAND test_wepoll_ex_pool)
        set_tests_properties(wepoll_ex_pool_mpsc PROPERTIES TIMEOUT 60)

        add_test(NAME wepoll_ex_windows_close_refs
            COMMAND test_wepoll_ex_windows_close_refs)
        set_tests_properties(wepoll_ex_windows_close_refs PROPERTIES TIMEOUT 10)

        add_test(NAME wepoll_ex_windows_iocp_batch
            COMMAND test_wepoll_ex_windows_iocp_batch)
        set_tests_properties(wepoll_ex_windows_iocp_batch PROPERTIES TIMEOUT 20)

        foreach(_pwait2_mode IN ITEMS
                conversion
                generation-deadline
                readiness-wins
                close
                fallback)
            string(REPLACE "-" "_" _pwait2_name "${_pwait2_mode}")
            add_test(NAME wepoll_ex_windows_pwait2_${_pwait2_name}
                COMMAND test_wepoll_ex_windows_pwait2 ${_pwait2_mode})
            set_tests_properties(wepoll_ex_windows_pwait2_${_pwait2_name}
                PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
        endforeach()

        foreach(_compat_mode IN ITEMS
                provider
                del-add
                mod-before-wait
                oneshot-rearm-before-wait
                two-epfds
                idle-two-epfds
                concurrent-ctl)
            string(REPLACE "-" "_" _compat_name "${_compat_mode}")
            add_test(NAME wepoll_ex_windows_compat_${_compat_name}
                COMMAND test_wepoll_ex_windows_compat ${_compat_mode})
            set_tests_properties(wepoll_ex_windows_compat_${_compat_name}
                PROPERTIES TIMEOUT 30)
        endforeach()

        foreach(_fault_mode IN ITEMS
                framework
                pool-init
                pool-grow
                afd-open
                afd-submit
                afd-submit-batch
                afd-refresh-submit
                afd-key-fallback
                afd-cancel
                endpoint-identity
                endpoint-policy
                endpoint-closed-token-loss
                provider-base
                iocp-create
                iocp-post
                iocp-post-immediate-socket
                iocp-dequeue
                aux-closed-iocp
                aux-post
                aux-post-immediate
                ready-node-alloc
                waitable-ready-node-alloc
                waitable-zero-disarm
                aux-waitable-disarm
                aux-waitable-disarm-repeat
                aux-consumptive-disarm
                aux-pipe-disarm
                timeout-init
                timeout-arm
                timeout-post)
            string(REPLACE "-" "_" _fault_name "${_fault_mode}")
            add_test(NAME wepoll_ex_windows_fault_${_fault_name}
                COMMAND test_wepoll_ex_windows_faults ${_fault_mode})
            set_tests_properties(wepoll_ex_windows_fault_${_fault_name}
                PROPERTIES TIMEOUT 20)
            if(_fault_mode MATCHES "^timeout-")
                set_tests_properties(wepoll_ex_windows_fault_${_fault_name}
                    PROPERTIES SKIP_RETURN_CODE 77)
            endif()
        endforeach()
    endif()

    foreach(_identity_mode IN ITEMS
            pending-add
            oneshot-rearm
            queued-ready
            transitional-connect
            transitional-mod-before-connect
            transitional-reuse)
        string(REPLACE "-" "_" _identity_name "${_identity_mode}")
        add_test(NAME wepoll_ex_windows_identity_${_identity_name}
            COMMAND test_wepoll_ex_windows_identity ${_identity_mode})
        set_tests_properties(wepoll_ex_windows_identity_${_identity_name}
            PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
        list(APPEND _wepoll_ex_shared_runtime_tests
            wepoll_ex_windows_identity_${_identity_name})
    endforeach()

    if(MINGW)
        add_test(NAME wepoll_ex_windows_no_winpthread
            COMMAND ${CMAKE_COMMAND}
                "-DWEPOLL_EX_OBJDUMP=${CMAKE_OBJDUMP}"
                "-DWEPOLL_EX_BINARY=$<TARGET_FILE:test_wepoll_ex_windows>"
                -P ${PROJECT_SOURCE_DIR}/tests/check_no_winpthread.cmake)
        set_tests_properties(wepoll_ex_windows_no_winpthread
            PROPERTIES TIMEOUT 30)
        if(WEPOLL_EX_BUILD_SHARED)
            add_test(NAME wepoll_ex_shared_no_winpthread
                COMMAND ${CMAKE_COMMAND}
                    "-DWEPOLL_EX_OBJDUMP=${CMAKE_OBJDUMP}"
                    "-DWEPOLL_EX_BINARY=$<TARGET_FILE:wepoll_ex_shared>"
                    -P ${PROJECT_SOURCE_DIR}/tests/check_no_winpthread.cmake)
            set_tests_properties(wepoll_ex_shared_no_winpthread
                PROPERTIES TIMEOUT 30)
        endif()
    endif()

    if(WEPOLL_EX_BUILD_SHARED AND NOT WEPOLL_EX_BUILD_STATIC)
        set_tests_properties(${_wepoll_ex_shared_runtime_tests}
            PROPERTIES
            ENVIRONMENT
                "PATH=$<TARGET_FILE_DIR:wepoll_ex_shared>\;$ENV{PATH}")
    endif()
    unset(_wepoll_ex_shared_runtime_tests)
endif()

if(WEPOLL_EX_BUILD_SHARED)
    if(WEPOLL_EX_PLATFORM_WINDOWS)
        set(_wepoll_ex_export_format PE)
    else()
        set(_wepoll_ex_export_format ELF)
    endif()
    add_test(NAME wepoll_ex_shared_exports
        COMMAND ${CMAKE_COMMAND}
            "-DWEPOLL_EX_BINARY=$<TARGET_FILE:wepoll_ex_shared>"
            "-DWEPOLL_EX_EXPORT_FORMAT=${_wepoll_ex_export_format}"
            "-DWEPOLL_EX_NM=${CMAKE_NM}"
            "-DWEPOLL_EX_OBJDUMP=${CMAKE_OBJDUMP}"
            "-DWEPOLL_EX_EXPECTED_VERSION=${PROJECT_VERSION}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_exports.cmake)
    set_tests_properties(wepoll_ex_shared_exports PROPERTIES TIMEOUT 30)
    unset(_wepoll_ex_export_format)
endif()

set(_package_test_prefix ${CMAKE_CURRENT_BINARY_DIR}/package-consumer-install)
set(_package_test_build ${CMAKE_CURRENT_BINARY_DIR}/package-consumer-build)

add_test(NAME wepoll_ex_package_consumer
    COMMAND ${CMAKE_COMMAND}
        -DWEPOLL_EX_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DWEPOLL_EX_BUILD_DIR=${PROJECT_BINARY_DIR}
        -DWEPOLL_EX_CONSUMER_BUILD_DIR=${_package_test_build}
        -DWEPOLL_EX_INSTALL_PREFIX=${_package_test_prefix}
        -DWEPOLL_EX_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
        -DWEPOLL_EX_INSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}
        -DWEPOLL_EX_GENERATOR=${CMAKE_GENERATOR}
        -DWEPOLL_EX_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DWEPOLL_EX_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
        -DWEPOLL_EX_GENERATOR_INSTANCE=${CMAKE_GENERATOR_INSTANCE}
        -DWEPOLL_EX_C_COMPILER=${CMAKE_C_COMPILER}
        "-DWEPOLL_EX_C_FLAGS=${CMAKE_C_FLAGS}"
        "-DWEPOLL_EX_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
        -DWEPOLL_EX_OBJDUMP=${CMAKE_OBJDUMP}
        -DWEPOLL_EX_CHECK_WINPTHREAD=${MINGW}
        -DWEPOLL_EX_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DWEPOLL_EX_EXPECT_SHARED=${WEPOLL_EX_BUILD_SHARED}
        -DWEPOLL_EX_EXPECT_STATIC=${WEPOLL_EX_BUILD_STATIC}
        -DWEPOLL_EX_EXPECTED_VERSION=${PROJECT_VERSION}
        -DWEPOLL_EX_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DWEPOLL_EX_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/package_consumer/run.cmake)
set_tests_properties(wepoll_ex_package_consumer PROPERTIES TIMEOUT 120)
