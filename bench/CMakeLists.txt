set(WEPOLL_EX_BENCH_SOURCES
    bench_latency.c)

add_executable(bench_latency ${WEPOLL_EX_BENCH_SOURCES})
target_link_libraries(bench_latency PRIVATE wepoll_ex::wepoll_ex)

if(NOT WEPOLL_EX_PLATFORM_POSIX)
    set_target_properties(bench_latency PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
