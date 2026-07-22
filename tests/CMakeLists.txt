if(WEPOLL_EX_PLATFORM_POSIX)
    add_executable(test_wepoll_ex test_api.c)
    target_link_libraries(test_wepoll_ex PRIVATE wepoll_ex::wepoll_ex)

    add_test(NAME wepoll_ex_api COMMAND test_wepoll_ex)
    set_tests_properties(wepoll_ex_api PROPERTIES TIMEOUT 30)

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

    if(WEPOLL_EX_BUILD_STATIC)
        target_link_libraries(test_wepoll_ex_windows PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        add_executable(test_wepoll_ex_windows_lifecycle
            test_windows_lifecycle.c)
        target_include_directories(test_wepoll_ex_windows_lifecycle PRIVATE
            ${PROJECT_SOURCE_DIR}/src)
        target_compile_definitions(test_wepoll_ex_windows_lifecycle PRIVATE
            _WIN32_WINNT=0x0602
            WIN32_LEAN_AND_MEAN)
        target_link_libraries(test_wepoll_ex_windows_lifecycle PRIVATE
            wepoll_ex_static Threads::Threads ws2_32)

        foreach(_lifecycle_mode IN ITEMS
                cancel-failure
                iocp-failure
                drain-timeout
                wake-failure)
            string(REPLACE "-" "_" _lifecycle_name "${_lifecycle_mode}")
            add_test(NAME wepoll_ex_windows_${_lifecycle_name}
                COMMAND test_wepoll_ex_windows_lifecycle
                    ${_lifecycle_mode})
            set_tests_properties(wepoll_ex_windows_${_lifecycle_name}
                PROPERTIES TIMEOUT 15)
        endforeach()
    else()
        target_link_libraries(test_wepoll_ex_windows PRIVATE
            wepoll_ex::wepoll_ex ws2_32)
    endif()

    add_test(NAME wepoll_ex_windows_api COMMAND test_wepoll_ex_windows)
    set_tests_properties(wepoll_ex_windows_api PROPERTIES TIMEOUT 120)

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
        set_tests_properties(wepoll_ex_windows_api PROPERTIES
            ENVIRONMENT
                "PATH=$<TARGET_FILE_DIR:wepoll_ex_shared>\;$ENV{PATH}")
    endif()
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
        -DWEPOLL_EX_GENERATOR=${CMAKE_GENERATOR}
        -DWEPOLL_EX_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DWEPOLL_EX_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
        -DWEPOLL_EX_GENERATOR_INSTANCE=${CMAKE_GENERATOR_INSTANCE}
        -DWEPOLL_EX_C_COMPILER=${CMAKE_C_COMPILER}
        -DWEPOLL_EX_OBJDUMP=${CMAKE_OBJDUMP}
        -DWEPOLL_EX_CHECK_WINPTHREAD=${MINGW}
        -DWEPOLL_EX_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DWEPOLL_EX_EXPECT_SHARED=${WEPOLL_EX_BUILD_SHARED}
        -DWEPOLL_EX_EXPECT_STATIC=${WEPOLL_EX_BUILD_STATIC}
        -DWEPOLL_EX_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DWEPOLL_EX_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/package_consumer/run.cmake)
set_tests_properties(wepoll_ex_package_consumer PROPERTIES TIMEOUT 120)
