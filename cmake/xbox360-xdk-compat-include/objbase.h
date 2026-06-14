#pragma once

#if defined(XENON) || defined(PLATFORM_XBOX360)
#include <xobjbase.h>
#else
#include_next <objbase.h>
#endif
