#ifndef _BS_UTILS_H_
#define _BS_UTILS_H_

#include "common.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "math_compat.h"

#include "real_type.h"

#ifdef PLATFORM_PS2
#include <malloc.h>
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)
    #define TYPEOF(x) typeof(x)
#elif defined(_MSC_VER) && defined(__cplusplus) && __cplusplus >= 201103L
    #define TYPEOF(x) std::remove_reference<decltype(x)>::type
#elif defined(__GNUC__) || defined(__clang__) || \
    (defined(__TINYC__) && __TINYC__ >= 913) || \
    (defined(_MSC_VER) && _MSC_VER >= 1940 && !defined(__cplusplus))
    #define TYPEOF(x) __typeof__(x)
#else
    #define TYPEOF(x) long long
#endif

#ifndef PLATFORM_XBOX360_XDK

#define forEach(type, item, array, count) \
    for (TYPEOF(count) item##_i_ = 0; item##_i_ < (count); ++item##_i_) \
    for (type* item = &(array)[item##_i_]; item; item = NULL)

#define forEachIndexed(type, item, index, array, count) \
    for (TYPEOF(count) index = 0; index < (count); ++index) \
    for (type* item = &(array)[index]; item; item = NULL)

#define repeat(n, it) for (TYPEOF(n) it = 0; it < (n); ++it)

#define require(condition) \
    do { \
        if (!(condition)) { \
        fprintf(stderr, "Requirement failed at %s:%d\n", __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

#define requireMessage(condition, message) \
do { \
if (!(condition)) { \
fprintf(stderr, "Requirement failed at %s:%d: %s\n", __FILE__, __LINE__, message); \
abort(); \
} \
} while (0)

static inline void requireMessageFormatted(const char *file, int line, bool condition, const char *fmt, ...) {
    if (condition)
        return;
    va_list args;
    fprintf(stderr, "Requirement failed at %s:%d: ", file, line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    abort();
}

static inline void* requireNotNullFunction(void* ptr, const char* file, int line, const char* name) {
    if (!ptr) {
        fprintf(stderr, "%s:%d: requireNotNull failed: '%s'\n", file, line, name);
        abort();
    }
    return ptr;
}
#define requireNotNull(ptr) requireNotNullFunction((void*)ptr, __FILE__, __LINE__, #ptr)
#define requireNotNullMessage(ptr, msg) requireNotNullFunction((void*)ptr, __FILE__, __LINE__, msg)

// Safe allocation macros - check for nullptr and abort with file/line info
static inline void *safeMallocFunction(size_t size, const char *file, int line) {
    void *ret = malloc(size);
    if (!ret) {
        fprintf(stderr, "FATAL: malloc(%zu) failed at %s:%d\n", size, file, line);
        abort();
    }
    return ret;
}
#define safeMalloc(size) safeMallocFunction(size, __FILE__, __LINE__)

static inline void *safeCallocFunction(size_t count, size_t size, const char *file, int line) {
    void *ret = calloc(count, size);
    if (!ret) {
        fprintf(stderr, "FATAL: calloc(%zu, %zu) failed at %s:%d\n", count, size, file, line);
        abort();
    }
    return ret;
}
#define safeCalloc(count, size) safeCallocFunction(count, size, __FILE__, __LINE__)

static inline void *safeReallocFunction(void *ptr, size_t size, const char *file, int line) {
    void *ret = realloc(ptr, size);
    if (!ret) {
        fprintf(stderr, "FATAL: realloc(%zu) failed at %s:%d\n", size, file, line);
        abort();
    }
    return ret;
}
#define safeRealloc(ptr, size) safeReallocFunction(ptr, size, __FILE__, __LINE__)

#ifdef PLATFORM_PS2

static inline void *safeMemalignFunction(size_t alignment, size_t size, const char *file, int line) {
    void *ret = memalign(alignment, size);
    if (!ret) {
        fprintf(stderr, "FATAL: memalign(%zu, %zu) failed at %s:%d\n", alignment, size, file, line);
        abort();
    }
    return ret;
}
#define safeMemalign(alignment, size) safeMemalignFunction(alignment, size, __FILE__, __LINE__)

#endif

// Reads exactly n bytes or aborts with the "pathForError" that caused the error.
static inline void safeFreadFunction(void *dst, size_t n, FILE *read_file, const char *pathForError, const char *file, int line) {
    if (fread(dst, 1, n, read_file) != n) {
        fprintf(stderr, "FATAL: failed to read %zu bytes from %s at %s:%d\n", n, pathForError, file, line);
        abort();
    }
}
#define safeFread(dst, n, file, pathForError) safeFreadFunction(dst, n, file, pathForError, __FILE__, __LINE__)

static inline char *safeStrdupFunction(const char *str, const char *file, int line) {
    char *ret = strdup(str);
    if (!ret) {
        fprintf(stderr, "FATAL: strdup() failed at %s:%d\n", file, line);
        abort();
    }
    return ret;
}
#define safeStrdup(str) safeStrdupFunction(str, __FILE__, __LINE__)

#else

void diagLog(const char* fmt, ...);
void fdiagLog(FILE* file, const char* fmt, ...);

#include <xtl.h>

static inline double GetFreeMemMB() {
    MEMORYSTATUS memStatus;
    memStatus.dwLength = sizeof(memStatus);

    // Use the 32-bit version for the Xbox 360 XDK
    GlobalMemoryStatus(&memStatus);

    // dwAvailPhys holds the available physical memory in bytes
    unsigned long freeBytes = memStatus.dwAvailPhys;
    double freeMegabytes = (double)freeBytes / (1024.0 * 1024.0);

    // printf("Free Memory: %.2f MB / 512.00 MB\n", freeMegabytes);
	return freeMegabytes;
}

// static inline void safeFree(void* ptr, const char* file, int line) {
// 	diagLog("free called at %s: %d", file, line);
// 	free(ptr);
// 	diagLog("free success!");
// }

// #define free(x) safeFree(x, __FILE__, __LINE__)

#define printf diagLog
#define fprintf fdiagLog

#ifndef __cplusplus
#error Things must be compiled as C++ for xbox 360 xdk to avoid C89
#endif

extern "C" void Butterscotch_xdkExit(int errcode, const char* file, int line);
extern "C" void Butterscotch_xdkAbort(const char* file, int line);

#define exit(errcode) Butterscotch_xdkExit(errcode, __FILE__, __LINE__)
#define abort() Butterscotch_xdkAbort(__FILE__, __LINE__)

// #define exit(errcode) diagLog("exit(%d) called at %s:%d!", errcode, __FILE__, __LINE__);
// #define abort() diagLog("abort() called at %s:%d!\n", __FILE__, __LINE__);

#include <type_traits>

// Renamed the first parameter from 'type' to 'T' to prevent preprocessor collisions
#define forEach(T, item, array, count) \
    for (std::remove_cv<std::remove_reference<decltype(count)>::type>::type item##_i_ = 0; item##_i_ < (count); item##_i_++) \
    for (T* item = &(array)[item##_i_]; item; item = nullptr)

#define forEachIndexed(T, item, index, array, count) \
    for (std::remove_cv<std::remove_reference<decltype(count)>::type>::type index = 0; index < (count); index++) \
    for (T* item = &(array)[index]; item; item = nullptr)

#define repeat(n, it) \
    for (std::remove_cv<std::remove_reference<decltype(n)>::type>::type it = 0; it < (n); ++it)

#define require(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "Requirement failed at %s:%d\n", __FILE__, __LINE__); \
            abort(); \
        } \
    } while (0)

#define requireMessage(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "Requirement failed at %s:%d: %s\n", __FILE__, __LINE__, message); \
            abort(); \
        } \
    } while (0)

static inline void requireMessageFormatted(const char *file, int line, bool condition, const char *fmt, ...) {
    if (condition)
        return;
    va_list args;
    fprintf(stderr, "Requirement failed at %s:%d: ", file, line);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    abort();
}

template <typename T>
inline T _requireNotNullHelper(T ptr, const char* expr, const char* file, int line) {
    if (ptr == nullptr) {
        fprintf(stderr, "%s:%d: requireNotNull failed: '%s'\n", file, line, expr);
        abort();
    }
    return ptr;
}
#define requireNotNull(ptr) _requireNotNullHelper((ptr), #ptr, __FILE__, __LINE__)

template <typename T>
inline T _requireNotNullMessageHelper(T ptr, const char* msg, const char* file, int line) {
    if (ptr == nullptr) {
        fprintf(stderr, "%s:%d: requireNotNull failed: %s\n", file, line, msg);
        abort();
    }
    return ptr;
}
#define requireNotNullMessage(ptr, msg) _requireNotNullMessageHelper((ptr), (msg), __FILE__, __LINE__)

inline void* _safeMallocHelper(size_t size, const char* file, int line) {
    void* ptr = malloc(size);
    if (ptr == nullptr && size > 0) {
        fprintf(stderr, "FATAL: malloc(%zu) failed at %s:%d\n", size, file, line);
        abort();
    }
    return ptr;
}
#define safeMalloc(size) _safeMallocHelper((size), __FILE__, __LINE__)

inline void* _safeCallocHelper(size_t count, size_t size, const char* file, int line) {
    void* ptr = calloc(count, size);
    if (ptr == nullptr && count > 0 && size > 0) {
        fprintf(stderr, "FATAL: calloc(%zu, %zu) failed at %s:%d\n", count, size, file, line);
        abort();
    }
    return ptr;
}
#define safeCalloc(count, size) _safeCallocHelper((count), (size), __FILE__, __LINE__)

inline void* _safeReallocHelper(void* ptr, size_t size, const char* file, int line) {
    void* new_ptr = realloc(ptr, size);
    if (new_ptr == nullptr && size > 0) {
        fprintf(stderr, "FATAL: realloc(%zu) failed at %s:%d\n", size, file, line);
        abort();
    }
    return new_ptr;
}
#define safeRealloc(ptr, size) _safeReallocHelper((ptr), (size), __FILE__, __LINE__)

inline void* _safeMemalignHelper(size_t alignment, size_t size, const char* file, int line) {
    // Note: MSVC uses _aligned_malloc, which flips the argument order relative to memalign!
    void* ptr = _aligned_malloc(size, alignment);
    if (ptr == nullptr && size > 0) {
        fprintf(stderr, "FATAL: memalign(%zu, %zu) failed at %s:%d\n", alignment, size, file, line);
        abort();
    }
    return ptr;
}
#define safeMemalign(alignment, size) _safeMemalignHelper((alignment), (size), __FILE__, __LINE__)

inline char* _safeStrdupHelper(const char* str, const char* file, int line) {
    // MSVC standardizes on _strdup to avoid deprecation warnings
    char* ptr = _strdup(str);
    if (ptr == nullptr && str != nullptr) {
        fprintf(stderr, "FATAL: strdup() failed at %s:%d\n", file, line);
        abort();
    }
    return ptr;
}
#define safeStrdup(str) _safeStrdupHelper((str), __FILE__, __LINE__)

inline void _safeFreadHelper(void* dst, size_t n, FILE* file, const char* pathForError, const char* srcFile, int srcLine) {
    if (fread(dst, 1, n, file) != n) {
        fprintf(stderr, "FATAL: failed to read %zu bytes from %s at %s:%d\n", n, pathForError, srcFile, srcLine);
        abort();
    }
}
#define safeFread(dst, n, file, pathForError) _safeFreadHelper((dst), (size_t)(n), (file), (pathForError), __FILE__, __LINE__)

#endif

#define ZERO_STRUCT(s) memset(&(s), 0, sizeof(s))

// Truncates to 6 decimal places, matching the HTML5 runner's ClampFloat
static inline GMLReal clampFloat(GMLReal f) {
    return ((GMLReal) ((int64_t) (f * 1000000.0))) / 1000000.0;
}

#define BGR_B(c) (((c) >> 16) & 0xFF)
#define BGR_G(c) (((c) >>  8) & 0xFF)
#define BGR_R(c) (((c) >>  0) & 0xFF)
#define BGR_A(c) (((c) >> 24) & 0xFF)

#ifdef PLATFORM_XBOX360_XDK
#define lrintf(x) (int)(x)
#endif

// Mixes 2 colors with a blend factor
static inline int32_t Color_lerp(int32_t color1, int32_t color2, float blending) {
    int32_t r1 = BGR_R(color1), g1 = BGR_G(color1), b1 = BGR_B(color1);
    int32_t r2 = BGR_R(color2), g2 = BGR_G(color2), b2 = BGR_B(color2);
    float inv = 1.0f - blending;
    int32_t r = (int32_t)((float) r2 * blending + (float) r1 * inv) & 0xFF;
    int32_t g = (int32_t)((float) g2 * blending + (float) g1 * inv) & 0xFF;
    int32_t b = (int32_t)((float) b2 * blending + (float) b1 * inv) & 0xFF;
    return r | (g << 8) | (b << 16);
}

#define shcopyFromTo(src, dst)                        \
do {                                        \
(dst) = NULL;                           \
for (int i = 0; i < shlen(src); i++)    \
shput((dst), (src)[i].key, (src)[i].value); \
} while (0)

typedef struct {
    char* key;
    bool value;
} StringBooleanEntry;

#endif /* _BS_UTILS_H_ */
