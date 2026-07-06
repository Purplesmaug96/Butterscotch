#pragma once

#include <math.h>

#include <windows.h>

#ifndef round
#define round(x) ((x) >= 0.0 ? floor((x) + 0.5) : ceil((x) - 0.5))
#endif

#ifndef roundf
#define roundf(x) ((x) >= 0.0f ? floorf((x) + 0.5f) : ceilf((x) - 0.5f))
#endif

#ifndef isnan
#define isnan(x) _isnan(static_cast<double>(x))
#endif

#ifndef isinf
#define isinf(x) (!_finite((double)(x)) && !_isnan((double)(x)))
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef fmax
#define fmax(x, y) (x > y ? x : y)
#endif

#ifndef fmin
#define fmin(x, y) (x < y ? x : y)
#endif

#ifndef log2
#define log2(x) (log(x) / 0.69314718055994530941723212145818)
#endif

#ifndef lround
#define lround(x) ((long)floor((x) + 0.5))
#endif

#ifndef lroundf
#define lroundf(x) ((long)floorf((x) + 0.5f))
#endif

#ifndef INFINITY
#include <float.h>
#define INFINITY (FLT_MAX + FLT_MAX)
#endif
