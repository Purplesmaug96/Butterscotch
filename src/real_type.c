#include <stdint.h>
#include "real_type.h"

GMLReal GMLReal_bankersRound(GMLReal v) {
    if (isnan(v) || isinf(v)) return v;
    GMLReal f = GMLReal_floor(v);
    GMLReal frac = v - f;
    if (0.5 > frac) return f;
    if (frac > 0.5) return f + 1.0;
    // Exactly halfway: round to the even neighbor.
    int64_t fi = (int64_t) f;
    return (fi & 1) == 0 ? f : f + 1.0;
}