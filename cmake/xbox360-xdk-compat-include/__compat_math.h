#pragma once

#include <math.h>

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
