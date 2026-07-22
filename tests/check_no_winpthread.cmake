if(NOT DEFINED WEPOLL_EX_OBJDUMP OR NOT DEFINED WEPOLL_EX_BINARY)
    message(FATAL_ERROR
        "WEPOLL_EX_OBJDUMP and WEPOLL_EX_BINARY are required")
endif()
if(NOT EXISTS "${WEPOLL_EX_BINARY}")
    message(FATAL_ERROR "Binary does not exist: ${WEPOLL_EX_BINARY}")
endif()

execute_process(
    COMMAND "${WEPOLL_EX_OBJDUMP}" -p "${WEPOLL_EX_BINARY}"
    RESULT_VARIABLE _objdump_result
    OUTPUT_VARIABLE _objdump_output
    ERROR_VARIABLE _objdump_error)
if(NOT _objdump_result EQUAL 0)
    message(FATAL_ERROR
        "objdump failed for ${WEPOLL_EX_BINARY}:\n${_objdump_error}")
endif()

string(TOLOWER "${_objdump_output}" _objdump_lower)
if(_objdump_lower MATCHES "libwinpthread(-1)?\\.dll")
    message(FATAL_ERROR
        "${WEPOLL_EX_BINARY} still imports libwinpthread-1.dll")
endif()
