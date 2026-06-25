#pragma once

#include <stdbool.h>
#ifndef nullptr
#define nullptr NULL
#endif

#include <stdint.h>

/* on some platforms, stdint.h exists but is incomplete */
#ifndef UINT32_MAX
#define UINT32_MAX 0xFFFFFFFFU
#endif
#ifndef INT32_MAX
#define INT32_MAX 0x7FFFFFFF
#endif
#ifndef INT32_MIN
#define INT32_MIN (-INT32_MAX - 1)
#endif

#if (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)) || defined(__BIG_ENDIAN__)
#define IS_BIG_ENDIAN
#endif

#if defined(__has_c_attribute)
    #if __has_c_attribute(maybe_unused)
        #define MAYBE_UNUSED [[maybe_unused]]
    #endif
#endif

#ifndef MAYBE_UNUSED
    #if defined(__GNUC__) || defined(__clang__)
        #define MAYBE_UNUSED __attribute__((unused))
	#elif defined(_MSC_VER)
		// Just gonna disable it globally.
		#define MAYBE_UNUSED /*__pragma(warning(suppress: 4100 4101 4505 4189))*/
		// C4100:
		//   unreferenced formal parameter
		// C4104:
		//   unreferenced local variable
		// C4505:
		//   unreferenced local function has been removed
		// C4189:
		//   local variable is initialized but not referenced
    #else
        #define MAYBE_UNUSED
    #endif
#endif

#if (defined(__GNUC__) && (__GNUC__ >= 3 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 8))) || defined(__TINYC__)
    #define BS_ALIGN(x) __attribute__((aligned(x)));
#else
    #define BS_ALIGN(x)
#endif

#if defined(__GNUC__) || defined(__TINYC__)
    #define NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER) && _MSC_VER >= 1400 // VS2005 or later
    #define NOINLINE __declspec(noinline)
#else
    #define NOINLINE
#endif

#ifdef PLATFORM_XBOX360_XDK
	#include <__compat_math.h>
	#define snprintf _snprintf
	#undef ALIGN
	#undef NOINLINE
	#undef ALWAYSINLINE
	#define ALIGN(x) __declspec(align(x))
	#define NOINLINE __declspec(noinline)
	#define ALWAYSINLINE __forceinline
#endif

#if defined(__GNUC__) || defined(__clang__)
    #if defined(__x86_64__) || defined(__i386__)
        #define YIELD() __asm__ volatile("rep; nop" : : : "memory")
    #elif defined(__aarch64__) || (defined(__arm__) && defined(__ARM_ARCH) && (__ARM_ARCH >= 7))
        #define YIELD() __asm__ volatile("yield" : : : "memory")
    #elif defined(__riscv)
        #define YIELD() __asm__ volatile("pause" : : : "memory")
    #else
        #define YIELD() ((void)0)
    #endif
#elif defined(_MSC_VER)
	#if defined(_M_X64) || defined(_M_IX86)
		#include <intrin.h>
        #define YIELD() _mm_pause()
    #elif defined(_M_ARM64) || defined(_M_ARM)
		#include <intrin.h>
        #define YIELD() __yield()
    #else
        #define YIELD() ((void)0)
    #endif
#else
    #define YIELD() ((void)0)
#endif
