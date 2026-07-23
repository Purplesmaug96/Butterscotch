#ifndef _BS_COMMON_H_
#define _BS_COMMON_H_

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

// According to ai 1900 is the magic number
#if (defined(_MSC_VER) && _MSC_VER < 1900)
#include <stdarg.h>
#include <stdio.h>
static inline int snprintfFunction(char* dst, size_t n, const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);

	va_list va2;
	va_start(va2, fmt);
    int formatted_len = _vscprintf(fmt, va2);
	va_end(va2);

    if (dst != NULL && n > 0) {
        int ret = _vsnprintf(dst, n, fmt, va);

        // _vsnprintf returns -1 or n when output is truncated,
        // so we must manually force null-termination on the last character.
        if (ret < 0 || (size_t)ret >= n) {
            dst[n - 1] = '\0';
        }
    }

    va_end(va);

    return formatted_len;
}
#define snprintf(dst, size, fmt, ...) snprintfFunction(dst, size, fmt, __VA_ARGS__)
#endif

#if (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)) || defined(__BIG_ENDIAN__)
#define IS_BIG_ENDIAN
#endif

#if defined(__cplusplus) && __cplusplus >= 201703L
    #define MAYBE_UNUSED [[maybe_unused]]
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
    #define MAYBE_UNUSED [[maybe_unused]]
#elif defined(__GNUC__) || defined(__clang__)
    #define MAYBE_UNUSED __attribute__((unused))
#else
    #define MAYBE_UNUSED
#endif

#if (defined(__GNUC__) && (__GNUC__ >= 3 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 8))) || defined(__clang__) || defined(__TINYC__)
    #define BS_ALIGN(x) __attribute__((aligned(x)))
#elif defined(_MSC_VER)
	#define BS_ALIGN(x) __declspec(align(x))
#else
    #define BS_ALIGN(x)
#endif

#if defined(__GNUC__) || defined(__clang__) || defined(__TINYC__)
    #define NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER) && _MSC_VER >= 1400 // VS2005 or later
    #define NOINLINE __declspec(noinline)
#else
    #define NOINLINE
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

#if defined(__GNUC__) || defined(__clang__) || defined(__TINYC__)
	#define ATTRIBUTE_MALLOC __attribute__((malloc))
#elif defined(_MSC_VER)
	#define ATTRIBUTE_MALLOC __declspec(restrict) __declspec(noalias)
#else
	#define ATTRIBUTE_MALLOC
#endif

#endif /* _BS_COMMON_H_ */
