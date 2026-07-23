foreach(_required_var IN ITEMS
        WEPOLL_EX_SOURCE_DIR
        WEPOLL_EX_BUILD_DIR
        WEPOLL_EX_CONSUMER_BUILD_DIR
        WEPOLL_EX_INSTALL_PREFIX
        WEPOLL_EX_INSTALL_LIBDIR
        WEPOLL_EX_INSTALL_DATADIR)
    if(NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
        message(FATAL_ERROR "${_required_var} is required")
    endif()
endforeach()

if(NOT DEFINED WEPOLL_EX_CONFIG OR "${WEPOLL_EX_CONFIG}" STREQUAL "")
    if(DEFINED WEPOLL_EX_BUILD_TYPE AND
       NOT "${WEPOLL_EX_BUILD_TYPE}" STREQUAL "")
        set(WEPOLL_EX_CONFIG "${WEPOLL_EX_BUILD_TYPE}")
    else()
        set(WEPOLL_EX_CONFIG Release)
    endif()
endif()

file(REMOVE_RECURSE
    "${WEPOLL_EX_CONSUMER_BUILD_DIR}"
    "${WEPOLL_EX_INSTALL_PREFIX}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${WEPOLL_EX_BUILD_DIR}"
            --prefix "${WEPOLL_EX_INSTALL_PREFIX}"
            --config "${WEPOLL_EX_CONFIG}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "Package install failed:\n${_install_output}\n${_install_error}")
endif()

foreach(_notice_file IN ITEMS LICENSE NOTICE)
    set(_notice_path
        "${WEPOLL_EX_INSTALL_PREFIX}/${WEPOLL_EX_INSTALL_DATADIR}/licenses/wepoll-ex/${_notice_file}")
    if(NOT EXISTS "${_notice_path}")
        message(FATAL_ERROR
            "Installed package is missing ${_notice_file}: ${_notice_path}")
    endif()
endforeach()

set(_configure_command
    "${CMAKE_COMMAND}"
    -S "${WEPOLL_EX_SOURCE_DIR}/tests/package_consumer"
    -B "${WEPOLL_EX_CONSUMER_BUILD_DIR}"
    "-Dwepoll_ex_DIR=${WEPOLL_EX_INSTALL_PREFIX}/${WEPOLL_EX_INSTALL_LIBDIR}/cmake/wepoll_ex"
    "-DWEPOLL_EX_EXPECT_SHARED=${WEPOLL_EX_EXPECT_SHARED}"
    "-DWEPOLL_EX_EXPECT_STATIC=${WEPOLL_EX_EXPECT_STATIC}")
if(DEFINED WEPOLL_EX_GENERATOR AND NOT "${WEPOLL_EX_GENERATOR}" STREQUAL "")
    list(APPEND _configure_command -G "${WEPOLL_EX_GENERATOR}")
endif()
if(DEFINED WEPOLL_EX_GENERATOR_PLATFORM AND
   NOT "${WEPOLL_EX_GENERATOR_PLATFORM}" STREQUAL "")
    list(APPEND _configure_command -A "${WEPOLL_EX_GENERATOR_PLATFORM}")
endif()
if(DEFINED WEPOLL_EX_GENERATOR_TOOLSET AND
   NOT "${WEPOLL_EX_GENERATOR_TOOLSET}" STREQUAL "")
    list(APPEND _configure_command -T "${WEPOLL_EX_GENERATOR_TOOLSET}")
endif()
if(DEFINED WEPOLL_EX_GENERATOR_INSTANCE AND
   NOT "${WEPOLL_EX_GENERATOR_INSTANCE}" STREQUAL "")
    list(APPEND _configure_command
        "-DCMAKE_GENERATOR_INSTANCE=${WEPOLL_EX_GENERATOR_INSTANCE}")
endif()
if(DEFINED WEPOLL_EX_C_COMPILER AND
   NOT "${WEPOLL_EX_C_COMPILER}" STREQUAL "")
    list(APPEND _configure_command
        "-DCMAKE_C_COMPILER=${WEPOLL_EX_C_COMPILER}")
endif()
if(DEFINED WEPOLL_EX_TOOLCHAIN_FILE AND
   NOT "${WEPOLL_EX_TOOLCHAIN_FILE}" STREQUAL "")
    list(APPEND _configure_command
        "-DCMAKE_TOOLCHAIN_FILE=${WEPOLL_EX_TOOLCHAIN_FILE}")
endif()
if(DEFINED WEPOLL_EX_BUILD_TYPE AND NOT "${WEPOLL_EX_BUILD_TYPE}" STREQUAL "")
    list(APPEND _configure_command
        "-DCMAKE_BUILD_TYPE=${WEPOLL_EX_BUILD_TYPE}")
endif()

execute_process(
    COMMAND ${_configure_command}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Package consumer configure failed:\n${_configure_output}\n${_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${WEPOLL_EX_CONSUMER_BUILD_DIR}"
            --config "${WEPOLL_EX_CONFIG}"
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Package consumer build failed:\n${_build_output}\n${_build_error}")
endif()

set(_consumer_manifest
    "${WEPOLL_EX_CONSUMER_BUILD_DIR}/package-consumer-targets-${WEPOLL_EX_CONFIG}.cmake")
if(NOT EXISTS "${_consumer_manifest}")
    message(FATAL_ERROR
        "Package consumer target manifest was not generated: ${_consumer_manifest}")
endif()
include("${_consumer_manifest}")
if(NOT WEPOLL_EX_CONSUMER_EXECUTABLES)
    message(FATAL_ERROR "Package consumer target manifest is empty")
endif()

if(WEPOLL_EX_CHECK_WINPTHREAD)
    if(NOT DEFINED WEPOLL_EX_OBJDUMP OR
       "${WEPOLL_EX_OBJDUMP}" STREQUAL "")
        message(FATAL_ERROR
            "WEPOLL_EX_OBJDUMP is required for the MinGW dependency check")
    endif()
    foreach(WEPOLL_EX_BINARY IN LISTS WEPOLL_EX_CONSUMER_EXECUTABLES)
        include("${WEPOLL_EX_SOURCE_DIR}/tests/check_no_winpthread.cmake")
    endforeach()
endif()

if(WIN32)
    set(_runtime_path
        "PATH=${WEPOLL_EX_INSTALL_PREFIX}/bin;$ENV{PATH}")
elseif(APPLE)
    set(_runtime_path
        "DYLD_LIBRARY_PATH=${WEPOLL_EX_INSTALL_PREFIX}/${WEPOLL_EX_INSTALL_LIBDIR}:$ENV{DYLD_LIBRARY_PATH}")
else()
    set(_runtime_path
        "LD_LIBRARY_PATH=${WEPOLL_EX_INSTALL_PREFIX}/${WEPOLL_EX_INSTALL_LIBDIR}:$ENV{LD_LIBRARY_PATH}")
endif()

foreach(_consumer_executable IN LISTS WEPOLL_EX_CONSUMER_EXECUTABLES)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "${_runtime_path}"
                "${_consumer_executable}"
        RESULT_VARIABLE _run_result
        OUTPUT_VARIABLE _run_output
        ERROR_VARIABLE _run_error)
    if(NOT _run_result EQUAL 0)
        message(FATAL_ERROR
            "${_consumer_executable} failed:\n${_run_output}\n${_run_error}")
    endif()
endforeach()
