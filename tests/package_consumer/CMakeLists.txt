cmake_minimum_required(VERSION 3.16)

project(wepoll_ex_package_consumer LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(_wepoll_ex_components)
if(WEPOLL_EX_EXPECT_SHARED)
    list(APPEND _wepoll_ex_components shared)
endif()
if(WEPOLL_EX_EXPECT_STATIC)
    list(APPEND _wepoll_ex_components static)
endif()

find_package(wepoll_ex 1.0.0 EXACT CONFIG REQUIRED
    COMPONENTS ${_wepoll_ex_components})

if(NOT TARGET wepoll_ex::wepoll_ex)
    message(FATAL_ERROR "Installed package is missing wepoll_ex::wepoll_ex")
endif()
if(WEPOLL_EX_EXPECT_SHARED AND NOT TARGET wepoll_ex::wepoll_ex_shared)
    message(FATAL_ERROR "Installed package is missing its shared target")
elseif(NOT WEPOLL_EX_EXPECT_SHARED AND TARGET wepoll_ex::wepoll_ex_shared)
    message(FATAL_ERROR "Installed package unexpectedly exports a shared target")
endif()
if(WEPOLL_EX_EXPECT_STATIC AND NOT TARGET wepoll_ex::wepoll_ex_static)
    message(FATAL_ERROR "Installed package is missing its static target")
elseif(NOT WEPOLL_EX_EXPECT_STATIC AND TARGET wepoll_ex::wepoll_ex_static)
    message(FATAL_ERROR "Installed package unexpectedly exports a static target")
endif()

add_executable(package_consumer main.c)
target_link_libraries(package_consumer PRIVATE wepoll_ex::wepoll_ex)

if(TARGET wepoll_ex::wepoll_ex_shared)
    add_executable(package_consumer_shared main.c)
    target_link_libraries(package_consumer_shared PRIVATE
        wepoll_ex::wepoll_ex_shared)
endif()

if(TARGET wepoll_ex::wepoll_ex_static)
    add_executable(package_consumer_static main.c)
    target_link_libraries(package_consumer_static PRIVATE
        wepoll_ex::wepoll_ex_static)
endif()

set(_consumer_targets package_consumer)
if(TARGET package_consumer_shared)
    list(APPEND _consumer_targets package_consumer_shared)
endif()
if(TARGET package_consumer_static)
    list(APPEND _consumer_targets package_consumer_static)
endif()

set(_consumer_manifest "set(WEPOLL_EX_CONSUMER_EXECUTABLES\n")
foreach(_consumer_target IN LISTS _consumer_targets)
    string(APPEND _consumer_manifest
        "    \"$<TARGET_FILE:${_consumer_target}>\"\n")
endforeach()
string(APPEND _consumer_manifest ")\n")

file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/package-consumer-targets-$<CONFIG>.cmake"
    CONTENT "${_consumer_manifest}")
