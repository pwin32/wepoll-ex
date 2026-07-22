if(NOT WEPOLL_EX_PLATFORM_POSIX)
    return()
endif()

add_executable(bench_latency bench_latency.c)
target_link_libraries(bench_latency PRIVATE wepoll_ex::wepoll_ex)
