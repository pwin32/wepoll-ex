set(WEPOLL_EX_TEST_SOURCES
    test_api.c
    test_pool.c)

add_executable(test_wepoll_ex      test_api.c)
add_executable(test_wepoll_ex_pool test_pool.c)

target_link_libraries(test_wepoll_ex      PRIVATE wepoll_ex::wepoll_ex)
target_link_libraries(test_wepoll_ex_pool PRIVATE wepoll_ex::wepoll_ex)

if(WEPOLL_EX_PLATFORM_WINDOWS)
    target_link_libraries(test_wepoll_ex      PRIVATE ws2_32)
    target_link_libraries(test_wepoll_ex_pool PRIVATE ws2_32)
endif()

# The tests use unistd.h / sys/socket.h, so they only build on POSIX
# in their current form.  On Windows we'd need to substitute Winsock
# equivalents — left as a follow-up.
if(NOT WEPOLL_EX_PLATFORM_POSIX)
    set_target_properties(test_wepoll_ex      PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set_target_properties(test_wepoll_ex_pool PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

add_test(NAME wepoll_ex_api       COMMAND test_wepoll_ex)
add_test(NAME wepoll_ex_pool_mpsc COMMAND test_wepoll_ex_pool)
