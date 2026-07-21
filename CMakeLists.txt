cmake_minimum_required(VERSION 3.16)

project(wepoll-ex
    VERSION 1.0.0
    DESCRIPTION "Extended epoll layer for Windows, IOCP+AFD backed, nginx-ready"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Default to Release for non-config builds.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(WEPOLL_EX_BUILD_SHARED   "Build shared library"      ON)
option(WEPOLL_EX_BUILD_STATIC   "Build static library"      ON)
option(WEPOLL_EX_BUILD_TESTS    "Build test suite"          ON)
option(WEPOLL_EX_BUILD_BENCH    "Build microbenchmarks"     OFF)
option(WEPOLL_EX_BUILD_NGINX    "Build nginx module shim"   OFF)
option(WEPOLL_EX_USE_PTHREADS   "Use pthreads (POSIX only)" ON)

# --------------------------------------------------------------------- #
# Platform detection.
# --------------------------------------------------------------------- #

if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(WEPOLL_EX_PLATFORM_WINDOWS ON)
else()
    set(WEPOLL_EX_PLATFORM_POSIX   ON)
endif()

# --------------------------------------------------------------------- #
# Threading library.
# --------------------------------------------------------------------- #

if(WEPOLL_EX_PLATFORM_POSIX AND WEPOLL_EX_USE_PTHREADS)
    find_package(Threads REQUIRED)
    set(WEPOLL_EX_THREAD_LIB Threads::Threads)
endif()

if(WEPOLL_EX_PLATFORM_WINDOWS)
    set(WEPOLL_EX_THREAD_LIB)
endif()

# --------------------------------------------------------------------- #
# Library sources.
# --------------------------------------------------------------------- #

if(WEPOLL_EX_PLATFORM_WINDOWS)
    set(WEPOLL_EX_SOURCES
        src/wepoll_ex_global.c
        src/wepoll_ex_errno.c
        src/wepoll_ex_afd.c
        src/wepoll_ex_pool.c
        src/wepoll_ex_port.c
        src/wepoll_ex_api.c)
else()
    set(WEPOLL_EX_SOURCES
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

add_library(wepoll_ex_compile_defs INTERFACE)
if(WEPOLL_EX_PLATFORM_WINDOWS)
    target_compile_definitions(wepoll_ex_compile_defs INTERFACE
        _WIN32_WINNT=0x0602  # Win8 — required for AFD poll bits
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS)
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
        SOVERSION 1
        C_VISIBILITY_PRESET hidden)
    target_include_directories(wepoll_ex_shared
        PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(wepoll_ex_shared PRIVATE WEPOLL_EX_BUILDING)
    target_link_libraries(wepoll_ex_shared
        PRIVATE wepoll_ex_compile_defs ${WEPOLL_EX_THREAD_LIB})
    if(WEPOLL_EX_PLATFORM_WINDOWS)
        target_link_libraries(wepoll_ex_shared PRIVATE ws2_32 ntdll)
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
                $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(wepoll_ex_static
        PUBLIC  WEPOLL_EX_STATIC
        PRIVATE wepoll_ex_compile_defs)
    target_link_libraries(wepoll_ex_static
        PRIVATE wepoll_ex_compile_defs ${WEPOLL_EX_THREAD_LIB})
    if(WEPOLL_EX_PLATFORM_WINDOWS)
        target_link_libraries(wepoll_ex_static PUBLIC ws2_32 ntdll)
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

if(WEPOLL_EX_BUILD_TESTS)
    enable_testing()
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

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

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

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wepoll_exConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/wepoll_exConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wepoll_ex)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/wepoll_exConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/wepoll_exConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/wepoll_exConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wepoll_ex)
