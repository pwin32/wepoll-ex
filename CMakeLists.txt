cmake_minimum_required(VERSION 3.16)

project(wepoll-ex
    VERSION 0.1.0
    DESCRIPTION "Experimental extended epoll compatibility layer for Windows"
    LANGUAGES C)

# PROJECT_IS_TOP_LEVEL was added in CMake 3.21.  Keep the declared 3.16
# minimum while providing the same subproject distinction to older CMake
# releases.
if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL ON)
    else()
        set(PROJECT_IS_TOP_LEVEL OFF)
    endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckSymbolExists)
include(CTest)

# Default a standalone, single-config build to Release.  Do not alter a
# parent project's build type when wepoll-ex is consumed with add_subdirectory.
if(PROJECT_IS_TOP_LEVEL AND
   NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build")
endif()

option(WEPOLL_EX_BUILD_SHARED   "Build shared library"      ON)
option(WEPOLL_EX_BUILD_STATIC   "Build static library"      ON)
option(WEPOLL_EX_BUILD_TESTS    "Build test suite"
       ${PROJECT_IS_TOP_LEVEL})
option(WEPOLL_EX_BUILD_BENCH    "Build microbenchmarks"     OFF)
option(WEPOLL_EX_BUILD_NGINX    "Compile-check nginx adapter" OFF)
option(WEPOLL_EX_FORCE_EPOLL_PWAIT2_FALLBACK
       "Force Linux epoll_pwait2_ex through the millisecond fallback" OFF)
set(WEPOLL_EX_SOCKET_LIFETIME_MODE "best-effort" CACHE STRING
    "Windows socket lifetime policy: best-effort, strict, or synchronized")
set_property(CACHE WEPOLL_EX_SOCKET_LIFETIME_MODE PROPERTY STRINGS
    best-effort strict synchronized)

set(WEPOLL_EX_NGINX_SOURCE_DIR "" CACHE PATH
    "Configured nginx source tree for the optional adapter compile check")
set(WEPOLL_EX_NGINX_BUILD_DIR "" CACHE PATH
    "nginx generated-header directory (for example, <tree>/objs-wepoll)")

if(DEFINED WEPOLL_EX_USE_PTHREADS AND NOT WEPOLL_EX_USE_PTHREADS)
    message(FATAL_ERROR
        "WEPOLL_EX_USE_PTHREADS=OFF is unsupported: the current implementation requires pthread-compatible threads")
endif()

if(NOT WEPOLL_EX_BUILD_SHARED AND NOT WEPOLL_EX_BUILD_STATIC)
    message(FATAL_ERROR
        "At least one of WEPOLL_EX_BUILD_SHARED or WEPOLL_EX_BUILD_STATIC must be enabled")
endif()

# --------------------------------------------------------------------- #
# Platform detection.
# --------------------------------------------------------------------- #

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(WEPOLL_EX_PLATFORM_WINDOWS ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(WEPOLL_EX_PLATFORM_POSIX   ON)
else()
    message(FATAL_ERROR
        "wepoll-ex supports only Windows and Linux; the POSIX shim requires Linux epoll and eventfd")
endif()

# --------------------------------------------------------------------- #
# Threading library.  Both the POSIX shim and the current Windows
# implementation use the pthread-compatible API.
# --------------------------------------------------------------------- #

find_package(Threads REQUIRED)
if(NOT CMAKE_USE_PTHREADS_INIT)
    message(FATAL_ERROR
        "The current implementation requires a pthread-compatible Threads package")
endif()

# MinGW's POSIX thread model adds -lpthread to every final link command.  That
# normally selects libwinpthread-1.dll, which is not present on a clean Windows
# installation.  Keep the runtime self-contained by selecting the static
# archive explicitly.  The link items are deliberately expressed as
# toolchain-relative linker flags instead of an absolute archive path so
# installed static targets remain usable with another MinGW installation.
set(WEPOLL_EX_WINPTHREAD_LINK_LIBS)
if(MINGW)
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" -print-file-name=libwinpthread.a
        OUTPUT_VARIABLE _wepoll_ex_winpthread_archive
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _wepoll_ex_winpthread_result)
    if(NOT _wepoll_ex_winpthread_result EQUAL 0 OR
       NOT EXISTS "${_wepoll_ex_winpthread_archive}")
        message(FATAL_ERROR
            "MinGW static pthread runtime not found; expected libwinpthread.a")
    endif()
    set(WEPOLL_EX_WINPTHREAD_LINK_LIBS
        "-Wl,-Bstatic" "-lwinpthread" "-Wl,-Bdynamic")
endif()

# --------------------------------------------------------------------- #
# Library sources.
# --------------------------------------------------------------------- #

if(WEPOLL_EX_PLATFORM_WINDOWS)
    set(WEPOLL_EX_SOURCES
        src/wepoll_ex_global.c
        src/wepoll_ex_errno.c
        src/wepoll_ex_fault.c
        src/wepoll_ex_afd.c
        src/wepoll_ex_pool.c
        src/wepoll_ex_port.c
        src/wepoll_ex_api.c)
else()
    set(WEPOLL_EX_SOURCES
        src/wepoll_ex_fault.c
        src/wepoll_ex_posix.c
        src/wepoll_ex_pool.c)
endif()

set(WEPOLL_EX_HEADERS
    include/wepoll_ex.h
    include/wepoll_ex_export.h
    src/wepoll_ex_internal.h)

# --------------------------------------------------------------------- #
# Compile definitions and flags.
# --------------------------------------------------------------------- #

set(WEPOLL_EX_PLATFORM_DEFINITIONS)
if(WEPOLL_EX_PLATFORM_WINDOWS)
    if(WEPOLL_EX_FORCE_EPOLL_PWAIT2_FALLBACK)
        message(FATAL_ERROR
            "WEPOLL_EX_FORCE_EPOLL_PWAIT2_FALLBACK is supported only on Linux")
    endif()
    list(APPEND WEPOLL_EX_PLATFORM_DEFINITIONS
        _WIN32_WINNT=0x0602  # Win8 — required for AFD poll bits
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS)
    if(WEPOLL_EX_SOCKET_LIFETIME_MODE STREQUAL "strict")
        list(APPEND WEPOLL_EX_PLATFORM_DEFINITIONS
            WEPOLL_EX_STRICT_SOCKET_IDENTITY=1)
    elseif(WEPOLL_EX_SOCKET_LIFETIME_MODE STREQUAL "synchronized")
        list(APPEND WEPOLL_EX_PLATFORM_DEFINITIONS
            WEPOLL_EX_ASSUME_SYNCHRONIZED_SOCKET_LIFETIME=1)
    elseif(NOT WEPOLL_EX_SOCKET_LIFETIME_MODE STREQUAL "best-effort")
        message(FATAL_ERROR
            "WEPOLL_EX_SOCKET_LIFETIME_MODE must be best-effort, strict, or synchronized")
    endif()
    message(STATUS
        "wepoll-ex Windows socket lifetime policy: ${WEPOLL_EX_SOCKET_LIFETIME_MODE}")
else()
    if(WEPOLL_EX_FORCE_EPOLL_PWAIT2_FALLBACK)
        message(STATUS
            "wepoll-ex Linux epoll_pwait2 mode: forced millisecond fallback")
    else()
        set(_wepoll_ex_saved_required_definitions
            "${CMAKE_REQUIRED_DEFINITIONS}")
        list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
        check_symbol_exists(epoll_pwait2 "sys/epoll.h"
            WEPOLL_EX_HAVE_EPOLL_PWAIT2)
        set(CMAKE_REQUIRED_DEFINITIONS
            "${_wepoll_ex_saved_required_definitions}")
        unset(_wepoll_ex_saved_required_definitions)
        if(WEPOLL_EX_HAVE_EPOLL_PWAIT2)
            list(APPEND WEPOLL_EX_PLATFORM_DEFINITIONS
                WEPOLL_EX_HAVE_EPOLL_PWAIT2=1)
            message(STATUS
                "wepoll-ex Linux epoll_pwait2 mode: native with runtime fallback")
        else()
            message(STATUS
                "wepoll-ex Linux epoll_pwait2 mode: millisecond fallback")
        endif()
    endif()
endif()

# --------------------------------------------------------------------- #
# Optional nginx adapter compile check.
# --------------------------------------------------------------------- #

if(WEPOLL_EX_BUILD_NGINX)
    if(NOT WEPOLL_EX_PLATFORM_WINDOWS)
        message(FATAL_ERROR
            "WEPOLL_EX_BUILD_NGINX requires a Windows/MinGW configure")
    endif()
    if(NOT IS_DIRECTORY "${WEPOLL_EX_NGINX_SOURCE_DIR}")
        message(FATAL_ERROR
            "Set WEPOLL_EX_NGINX_SOURCE_DIR to the configured nginx source tree")
    endif()
    if(NOT IS_DIRECTORY "${WEPOLL_EX_NGINX_BUILD_DIR}")
        message(FATAL_ERROR
            "Set WEPOLL_EX_NGINX_BUILD_DIR to nginx's generated-header directory")
    endif()
    foreach(_nginx_header
            ngx_auto_config.h
            ngx_auto_headers.h)
        if(NOT EXISTS "${WEPOLL_EX_NGINX_BUILD_DIR}/${_nginx_header}")
            message(FATAL_ERROR
                "Missing ${_nginx_header} in WEPOLL_EX_NGINX_BUILD_DIR")
        endif()
    endforeach()

    add_library(wepoll_ex_nginx_adapter OBJECT
        nginx/ngx_wepoll_module.c
        nginx/ngx_wepoll_module.h)
    target_include_directories(wepoll_ex_nginx_adapter PRIVATE
        ${WEPOLL_EX_NGINX_SOURCE_DIR}/src/core
        ${WEPOLL_EX_NGINX_SOURCE_DIR}/src/event
        ${WEPOLL_EX_NGINX_SOURCE_DIR}/src/event/modules
        ${WEPOLL_EX_NGINX_SOURCE_DIR}/src/event/quic
        ${WEPOLL_EX_NGINX_SOURCE_DIR}/src/os/win32
        ${WEPOLL_EX_NGINX_BUILD_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/nginx)
    target_compile_definitions(wepoll_ex_nginx_adapter PRIVATE
        WEPOLL_EX_STATIC)
endif()

# --------------------------------------------------------------------- #
# Shared library.
# --------------------------------------------------------------------- #

if(WEPOLL_EX_BUILD_SHARED)
    add_library(wepoll_ex_shared SHARED ${WEPOLL_EX_SOURCES} ${WEPOLL_EX_HEADERS})
    set_target_properties(wepoll_ex_shared PROPERTIES
        OUTPUT_NAME wepoll_ex
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        # The 0.x preview does not promise ABI compatibility between
        # releases.  Keep the ELF SONAME exact until an ABI is frozen.
        SOVERSION ${PROJECT_VERSION}
        C_VISIBILITY_PRESET hidden)
    target_include_directories(wepoll_ex_shared
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(wepoll_ex_shared PRIVATE
        WEPOLL_EX_BUILDING
        ${WEPOLL_EX_PLATFORM_DEFINITIONS})
    target_link_libraries(wepoll_ex_shared PRIVATE Threads::Threads)
    if(WEPOLL_EX_PLATFORM_WINDOWS)
        target_link_libraries(wepoll_ex_shared PRIVATE
            ws2_32 ntdll ${WEPOLL_EX_WINPTHREAD_LINK_LIBS})
    endif()
endif()

# --------------------------------------------------------------------- #
# Static library.
# --------------------------------------------------------------------- #

if(WEPOLL_EX_BUILD_STATIC)
    add_library(wepoll_ex_static STATIC ${WEPOLL_EX_SOURCES} ${WEPOLL_EX_HEADERS})
    set_target_properties(wepoll_ex_static PROPERTIES
        OUTPUT_NAME wepoll_ex_static
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(wepoll_ex_static
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(wepoll_ex_static
        PUBLIC  WEPOLL_EX_STATIC
        PRIVATE ${WEPOLL_EX_PLATFORM_DEFINITIONS})
    target_link_libraries(wepoll_ex_static PRIVATE Threads::Threads)
    if(WEPOLL_EX_PLATFORM_WINDOWS)
        target_link_libraries(wepoll_ex_static PRIVATE ws2_32 ntdll)
    endif()
    if(WEPOLL_EX_WINPTHREAD_LINK_LIBS)
        # A static archive has no link step of its own.  Propagate the
        # explicit archive selection to every final executable that consumes
        # the exported static target.
        target_link_libraries(wepoll_ex_static INTERFACE
            ${WEPOLL_EX_WINPTHREAD_LINK_LIBS})
    endif()
endif()

# Alias for consumers: wepoll_ex::wepoll_ex
if(WEPOLL_EX_BUILD_SHARED)
    add_library(wepoll_ex::wepoll_ex ALIAS wepoll_ex_shared)
elseif(WEPOLL_EX_BUILD_STATIC)
    add_library(wepoll_ex::wepoll_ex ALIAS wepoll_ex_static)
endif()

# --------------------------------------------------------------------- #
# Tests.
# --------------------------------------------------------------------- #

if(BUILD_TESTING AND WEPOLL_EX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# --------------------------------------------------------------------- #
# Benchmarks.
# --------------------------------------------------------------------- #

if(WEPOLL_EX_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# --------------------------------------------------------------------- #
# Install.
# --------------------------------------------------------------------- #

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES
    LICENSE
    NOTICE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/wepoll-ex)

if(WEPOLL_EX_BUILD_SHARED)
    install(TARGETS wepoll_ex_shared
        EXPORT wepoll_exTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(WEPOLL_EX_BUILD_STATIC)
    install(TARGETS wepoll_ex_static
        EXPORT wepoll_exTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(EXPORT wepoll_exTargets
    FILE wepoll_exTargets.cmake
    NAMESPACE wepoll_ex::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wepoll_ex)

if(WEPOLL_EX_BUILD_STATIC)
    set(WEPOLL_EX_PACKAGE_NEEDS_THREADS 1)
else()
    set(WEPOLL_EX_PACKAGE_NEEDS_THREADS 0)
endif()

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wepoll_exConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/wepoll_exConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wepoll_ex)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/wepoll_exConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ExactVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/wepoll_exConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/wepoll_exConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wepoll_ex)
