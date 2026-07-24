if(WEPOLL_EX_PLATFORM_POSIX)
    add_executable(bench_latency bench_latency.c)
    target_link_libraries(bench_latency PRIVATE wepoll_ex::wepoll_ex)

    add_executable(bench_wait_scaling bench_wait_scaling.c)
    target_link_libraries(bench_wait_scaling PRIVATE wepoll_ex::wepoll_ex)
elseif(WEPOLL_EX_PLATFORM_WINDOWS)
    add_executable(bench_windows bench_windows.c)
    target_compile_definitions(bench_windows PRIVATE
        _WIN32_WINNT=0x0602
        WIN32_LEAN_AND_MEAN)
    if(WEPOLL_EX_BUILD_STATIC)
        target_link_libraries(bench_windows PRIVATE wepoll_ex_static ws2_32)
    else()
        target_link_libraries(bench_windows PRIVATE wepoll_ex_shared ws2_32)
    endif()
endif()
