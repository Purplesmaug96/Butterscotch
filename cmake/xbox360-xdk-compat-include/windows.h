#pragma once

#if defined(XENON) || defined(PLATFORM_XBOX360)
#include <xtl.h>
#else
#include_next <windows.h>
#endif
