/*
 * wepoll_ex_version.h — canonical public version definitions
 *
 * Keep the three component definitions as plain decimal integer literals.
 * CMake reads these definitions before project() so package metadata, shared
 * library identity, public headers, and runtime reporting cannot drift.
 */
#ifndef WEPOLL_EX_VERSION_H_
#define WEPOLL_EX_VERSION_H_

#define WEPOLL_EX_VERSION_MAJOR 0
#define WEPOLL_EX_VERSION_MINOR 1
#define WEPOLL_EX_VERSION_PATCH 0

#if WEPOLL_EX_VERSION_MAJOR < 0 || WEPOLL_EX_VERSION_MAJOR > 255
#  error "WEPOLL_EX_VERSION_MAJOR must be in the range 0..255"
#endif
#if WEPOLL_EX_VERSION_MINOR < 0 || WEPOLL_EX_VERSION_MINOR > 255
#  error "WEPOLL_EX_VERSION_MINOR must be in the range 0..255"
#endif
#if WEPOLL_EX_VERSION_PATCH < 0 || WEPOLL_EX_VERSION_PATCH > 255
#  error "WEPOLL_EX_VERSION_PATCH must be in the range 0..255"
#endif

#define WEPOLL_EX_VERSION_NUMBER                                       \
    ((WEPOLL_EX_VERSION_MAJOR << 16) | (WEPOLL_EX_VERSION_MINOR << 8) | \
     WEPOLL_EX_VERSION_PATCH)

#define WEPOLL_EX_VERSION_STRINGIFY_INNER(value) #value
#define WEPOLL_EX_VERSION_STRINGIFY(value) \
    WEPOLL_EX_VERSION_STRINGIFY_INNER(value)
#define WEPOLL_EX_VERSION_STRING                               \
    WEPOLL_EX_VERSION_STRINGIFY(WEPOLL_EX_VERSION_MAJOR) "."   \
    WEPOLL_EX_VERSION_STRINGIFY(WEPOLL_EX_VERSION_MINOR) "."   \
    WEPOLL_EX_VERSION_STRINGIFY(WEPOLL_EX_VERSION_PATCH)

#endif
