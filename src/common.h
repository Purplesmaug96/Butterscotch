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
#ifndef INFINITY
#define INFINITY (1.0f / 0.0f)
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
    #else
        #define MAYBE_UNUSED
    #endif
#endif

#ifdef PLATFORM_XBOX360_XDK
	#include <__compat_math.h>
	#define snprintf _snprintf
	#define ALIGNED(x) __declspec(align(x))
	#define NOINLINE __declspec(noinline)
	#define ALWAYSINLINE __forceinline
#endif

#ifndef ALIGNED
	#define ALIGNED(x) __attribute__((aligned(x)))
#endif

#ifndef NOINLINE
	#define NOINLINE __attribute__((noinline))
#endif

#ifndef ALWAYSINLINE
	#define ALWAYSINLINE __attribute__((always_inline))
#endif

#if defined(__GNUC__) || defined(__clang__)
    #if defined(__x86_64__) || defined(__i386__) || defined(__riscv)
        #define YIELD() __asm__ volatile("rep; nop" : : : "memory")
    #elif defined(__aarch64__) || (defined(__arm__) && defined(__ARM_ARCH) && (__ARM_ARCH >= 7))
        #define YIELD() __asm__ volatile("yield" : : : "memory")
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
