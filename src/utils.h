#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#include "real_type.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef nullptr
#define nullptr NULL
#endif

#ifdef NXDK
#include <hal/debug.h>
#include <hal/xbox.h>
#include <hal/video.h>
#include <windows.h>

#define XBMemStat() ({ \
    MM_STATISTICS stats; \
    stats.Length = sizeof(MM_STATISTICS); \
    MmQueryStatistics(&stats); \
    uint32_t free_mem = stats.AvailablePages * 4096; \
    debugPrint("Free Memory: %u bytes (%u MB)\n", free_mem, free_mem / 1024 / 1024); \
})

#define printf debugPrint
#define fprintf(stream, ...) \
    ((stream == stderr) ? debugPrint(__VA_ARGS__) : 0)

#define exit(errcode) debugPrint("exit(%d) called at %s:%d! Rebooting in 10s...\n", errcode, __FILE__, __LINE__); XBMemStat(); Sleep(10000); XReboot()
#define abort() debugPrint("abort() called at %s:%d! Rebooting in 10s...\n", __FILE__, __LINE__); XBMemStat(); Sleep(10000); XReboot()

#endif

#define DPInit int __DPNum = 0
#define DPReset __DPNum = 0

#define DPPrint printf("Debug Print %d\n", __DPNum); __DPNum++

#define forEach(type, item, array, count) \
    for (typeof(count) item##_i_ = 0; item##_i_ < (count); item##_i_++) \
    for (type* item = &(array)[item##_i_]; item; item = NULL)

#define forEachIndexed(type, item, index, array, count) \
    for (typeof(count) index = 0; index < (count); index++) \
    for (type* item = &(array)[index]; item; item = NULL)

// The "typeof((typeof(n))0" is used to remove the "const" from the typeof

#define repeat(n, it) for (typeof((typeof(n))0) it = 0; it < (n); it++)

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

#define requireNotNull(ptr) ({ \
typeof(ptr) _val = (ptr); \
if (_val == NULL) { \
fprintf(stderr, "%s:%d: requireNotNull failed: '%s'\n", __FILE__, __LINE__, #ptr); \
abort(); \
} \
_val; \
})

#define requireNotNullMessage(ptr, msg) ({ \
typeof(ptr) _val = (ptr); \
if (_val == NULL) { \
fprintf(stderr, "%s:%d: requireNotNull failed: %s\n", __FILE__, __LINE__, (msg)); \
abort(); \
} \
_val; \
})

// Safe allocation macros - check for nullptr and abort with file/line info
#define safeMalloc(size) ({ \
    void* _ptr = malloc(size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: malloc(%zu) failed at %s:%d (%d mb)\n", (size_t)(size), __FILE__, __LINE__, (size_t)(size) / 1048576); \
        abort(); \
    } \
    _ptr; \
})

#define safeCalloc(count, size) ({ \
    void* _ptr = calloc(count, size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: calloc(%zu, %zu) failed at %s:%d (%d mb)\n", (size_t)(count), __FILE__, __LINE__, (size_t)(size) / 1048576); \
        abort(); \
    } \
    _ptr; \
})

#define safeRealloc(ptr, size) ({ \
    void* _ptr = realloc(ptr, size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: realloc(%zu) failed at %s:%d (%d mb)\n", (size_t)(size), __FILE__, __LINE__, (size_t)(size) / 1048576); \
        abort(); \
    } \
    _ptr; \
})

#define safeMemalign(alignment, size) ({ \
    void* _ptr = memalign(alignment, size); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: memalign(%zu, %zu) failed at %s:%d\n", (size_t)(alignment), (size_t)(size), __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

#define safeStrdup(str) ({ \
    char* _ptr = strdup(str); \
    if (_ptr == nullptr) { \
        fprintf(stderr, "FATAL: strdup() failed at %s:%d\n", __FILE__, __LINE__); \
        abort(); \
    } \
    _ptr; \
})

// Truncates to 6 decimal places, matching the HTML5 runner's ClampFloat
static inline GMLReal clampFloat(GMLReal f) {
    return ((GMLReal) ((int64_t) (f * 1000000.0))) / 1000000.0;
}

#define BGR_B(c) (((c) >> 16) & 0xFF)
#define BGR_G(c) (((c) >>  8) & 0xFF)
#define BGR_R(c) (((c) >>  0) & 0xFF)

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