/*
 * wepoll_ex_export.h — symbol export macros
 *
 * On Windows we use __declspec(dllexport/dllimport) so the same header
 * works whether the consumer is compiling against the static lib or
 * the DLL.  On other platforms we degrade to GCC/Clang visibility.
 */
#ifndef WEPOLL_EX_EXPORT_H_
#define WEPOLL_EX_EXPORT_H_

#if defined(_WIN32)
#  if defined(WEPOLL_EX_BUILDING)
#    define WEPOLL_EX_API __declspec(dllexport)
#  elif defined(WEPOLL_EX_STATIC)
#    define WEPOLL_EX_API
#  else
#    define WEPOLL_EX_API __declspec(dllimport)
#  endif
#else
#  if defined(WEPOLL_EX_BUILDING)
#    define WEPOLL_EX_API __attribute__((visibility("default")))
#  else
#    define WEPOLL_EX_API
#  endif
#endif

#endif
