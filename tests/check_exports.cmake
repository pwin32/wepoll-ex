foreach(_required_var IN ITEMS
        WEPOLL_EX_BINARY
        WEPOLL_EX_EXPORT_FORMAT)
    if(NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
        message(FATAL_ERROR "${_required_var} is required")
    endif()
endforeach()
if(NOT EXISTS "${WEPOLL_EX_BINARY}")
    message(FATAL_ERROR "Shared library does not exist: ${WEPOLL_EX_BINARY}")
endif()

if(WEPOLL_EX_EXPORT_FORMAT STREQUAL "ELF")
    foreach(_required_var IN ITEMS
            WEPOLL_EX_NM
            WEPOLL_EX_OBJDUMP
            WEPOLL_EX_EXPECTED_VERSION)
        if(NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
            message(FATAL_ERROR "${_required_var} is required for ELF")
        endif()
    endforeach()
    set(_expected_exports
        epoll_create_ex
        epoll_ctl_batch
        epoll_ctl_ctx
        epoll_drain
        epoll_fd_count
        epoll_pwait2_ex
        epoll_rearm
        epoll_rearm_classes
        epoll_wait_ex
        wepoll_close
        wepoll_ex_get_capabilities
        wepoll_ex_get_global_stats
        wepoll_ex_get_socket_lifetime_policy
        wepoll_ex_get_stats
        wepoll_ex_version
        wepoll_ex_version_string
        wepoll_ex_wake)

    execute_process(
        COMMAND "${WEPOLL_EX_NM}"
            --dynamic --extern-only --defined-only --format=posix
            "${WEPOLL_EX_BINARY}"
        RESULT_VARIABLE _nm_result
        OUTPUT_VARIABLE _nm_output
        ERROR_VARIABLE _nm_error)
    if(NOT _nm_result EQUAL 0)
        message(FATAL_ERROR
            "nm failed for ${WEPOLL_EX_BINARY}:\n${_nm_error}")
    endif()
    string(REPLACE "\r\n" "\n" _nm_output "${_nm_output}")
    string(REPLACE "\r" "\n" _nm_output "${_nm_output}")
    string(REGEX MATCHALL "[^\n]+" _nm_lines "${_nm_output}")
    foreach(_line IN LISTS _nm_lines)
        string(STRIP "${_line}" _line)
        if(NOT _line MATCHES "^([^ \t]+)[ \t]+[A-Za-z][ \t]+")
            message(FATAL_ERROR "Unexpected nm output line: ${_line}")
        endif()
        list(APPEND _actual_exports "${CMAKE_MATCH_1}")
    endforeach()

    execute_process(
        COMMAND "${WEPOLL_EX_OBJDUMP}" -p "${WEPOLL_EX_BINARY}"
        RESULT_VARIABLE _objdump_result
        OUTPUT_VARIABLE _objdump_output
        ERROR_VARIABLE _objdump_error)
    if(NOT _objdump_result EQUAL 0)
        message(FATAL_ERROR
            "objdump failed for ${WEPOLL_EX_BINARY}:\n${_objdump_error}")
    endif()
    if(NOT _objdump_output MATCHES "SONAME[ \t]+([^ \r\n]+)")
        message(FATAL_ERROR
            "ELF shared library has no readable SONAME: ${WEPOLL_EX_BINARY}")
    endif()
    set(_actual_soname "${CMAKE_MATCH_1}")
    set(_expected_soname "libwepoll_ex.so.${WEPOLL_EX_EXPECTED_VERSION}")
    if(NOT _actual_soname STREQUAL _expected_soname)
        message(FATAL_ERROR
            "Unexpected SONAME: expected ${_expected_soname}, got ${_actual_soname}")
    endif()
elseif(WEPOLL_EX_EXPORT_FORMAT STREQUAL "PE")
    if(NOT DEFINED WEPOLL_EX_OBJDUMP OR
       "${WEPOLL_EX_OBJDUMP}" STREQUAL "")
        message(FATAL_ERROR "WEPOLL_EX_OBJDUMP is required for PE")
    endif()
    set(_expected_exports
        epoll_create
        epoll_create1
        epoll_create_ex
        epoll_ctl
        epoll_ctl_batch
        epoll_ctl_ctx
        epoll_drain
        epoll_fd_count
        epoll_pwait
        epoll_pwait2
        epoll_pwait2_ex
        epoll_rearm
        epoll_rearm_classes
        epoll_wait
        epoll_wait_ex
        wepoll_close
        wepoll_ex_get_capabilities
        wepoll_ex_get_global_stats
        wepoll_ex_get_socket_lifetime_policy
        wepoll_ex_get_stats
        wepoll_ex_version
        wepoll_ex_version_string
        wepoll_ex_wake)

    execute_process(
        COMMAND "${WEPOLL_EX_OBJDUMP}" -p "${WEPOLL_EX_BINARY}"
        RESULT_VARIABLE _objdump_result
        OUTPUT_VARIABLE _objdump_output
        ERROR_VARIABLE _objdump_error)
    if(NOT _objdump_result EQUAL 0)
        message(FATAL_ERROR
            "objdump failed for ${WEPOLL_EX_BINARY}:\n${_objdump_error}")
    endif()
    string(REPLACE "\r\n" "\n" _objdump_output "${_objdump_output}")
    string(REPLACE "\r" "\n" _objdump_output "${_objdump_output}")

    if(NOT _objdump_output MATCHES
       "Export Address Table[ \t]+([0-9A-Fa-f]+)")
        message(FATAL_ERROR "PE export address count was not found")
    endif()
    set(_address_count_hex "${CMAKE_MATCH_1}")
    if(NOT _objdump_output MATCHES
       "\\[Name Pointer/Ordinal\\] Table[ \t]+([0-9A-Fa-f]+)")
        message(FATAL_ERROR "PE named export count was not found")
    endif()
    set(_named_count_hex "${CMAKE_MATCH_1}")
    math(EXPR _address_count "0x${_address_count_hex}")
    math(EXPR _named_count "0x${_named_count_hex}")

    string(REGEX MATCHALL "[^\n]+" _objdump_lines "${_objdump_output}")
    set(_in_name_table FALSE)
    set(_saw_export FALSE)
    foreach(_line IN LISTS _objdump_lines)
        if(_line MATCHES
           "^[ \t]*\\[Ordinal/Name Pointer\\] Table")
            set(_in_name_table TRUE)
            continue()
        endif()
        if(_in_name_table)
            if(_line MATCHES
               "^[ \t]*\\[[^]]+\\].*[ \t]([A-Za-z_][A-Za-z0-9_]*)$")
                list(APPEND _actual_exports "${CMAKE_MATCH_1}")
                set(_saw_export TRUE)
            elseif(_saw_export)
                break()
            endif()
        endif()
    endforeach()

    list(LENGTH _expected_exports _expected_count)
    if(NOT _address_count EQUAL _named_count OR
       NOT _named_count EQUAL _expected_count)
        message(FATAL_ERROR
            "PE export counts differ: address=${_address_count}, "
            "named=${_named_count}, expected=${_expected_count}")
    endif()
else()
    message(FATAL_ERROR
        "Unknown WEPOLL_EX_EXPORT_FORMAT: ${WEPOLL_EX_EXPORT_FORMAT}")
endif()

list(SORT _expected_exports)
list(SORT _actual_exports)
if(NOT _actual_exports STREQUAL _expected_exports)
    string(REPLACE ";" "\n  " _expected_text "${_expected_exports}")
    string(REPLACE ";" "\n  " _actual_text "${_actual_exports}")
    message(FATAL_ERROR
        "Shared export surface changed.\n"
        "Expected:\n  ${_expected_text}\n"
        "Actual:\n  ${_actual_text}")
endif()
