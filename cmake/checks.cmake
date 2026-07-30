include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckCCompilerFlag)
include(CheckCSourceCompiles)

if(MSVC)
    check_c_source_compiles("
        int main(void) {
            int a = 0; ++a; int b = a; return b;
        }
    " HAVE_MIXED_DECLARATIONS)
    if(NOT HAVE_MIXED_DECLARATIONS)
        add_compile_options("/Tp")
    endif()
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
    check_c_compiler_flag("-fno-builtin" HAVE_FNO_BUILTIN)
    if(HAVE_FNO_BUILTIN)
        add_compile_options("-fno-builtin")
    endif()
endif()

# stdbool.h check
check_include_file("stdbool.h" HAVE_STDBOOL_H)
if(NOT HAVE_STDBOOL_H)
    list(APPEND COMPAT_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/compat/stdbool")
endif()

# stdint.h & sys/types.h check
check_include_file("stdint.h" HAVE_STDINT_H)
if(NOT HAVE_STDINT_H)
    list(APPEND COMPAT_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/compat/stdint")
    if(NOT MSVC)
        check_include_file("sys/types.h" HAVE_SYS_TYPES_H)
        if(HAVE_SYS_TYPES_H)
            add_compile_definitions(HAVE_SYS_TYPES_H=1)
        endif()
    endif()
endif()

# strings.h check
check_include_file("strings.h" HAVE_STRINGS_H)
if(NOT HAVE_STRINGS_H)
    add_compile_definitions(NO_STRINGS_H=1)
endif()

# getopt_long check
check_include_file("getopt.h" HAVE_GETOPT_H)
if(HAVE_GETOPT_H)
    check_symbol_exists(getopt_long "getopt.h" HAVE_GETOPT_LONG)
endif()
if(NOT HAVE_GETOPT_LONG)
    list(APPEND COMPAT_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/compat/getopt")
endif()

# __func__ symbol check
check_c_source_compiles("
    #include <stdio.h>
    int main(void) { puts(__func__); return 0; }
" HAVE_FUNC_SYMBOL)
if(NOT HAVE_FUNC_SYMBOL)
    add_compile_definitions(__func__="unknown")
endif()

find_library(MATH_LIB m)
if(MATH_LIB)
    set(CMAKE_REQUIRED_LIBRARIES ${MATH_LIB})
endif()

# Math functions checks (defines NO_<FUNC> if missing)
set(MATH_FUNCS fmin fmax round log2 lround sqrtf fabsf fmodf sinf cosf floorf roundf)
foreach(func ${MATH_FUNCS})
    string(TOUPPER ${func} UPPER_FUNC)
    check_symbol_exists(${func} "math.h" HAVE_${UPPER_FUNC})
    if(NOT HAVE_${UPPER_FUNC})
        add_compile_definitions(NO_${UPPER_FUNC}=1)
    endif()
endforeach()

unset(CMAKE_REQUIRED_LIBRARIES)

# Macro/Math checks (isinf, isnan)
check_c_source_compiles("
    #include <math.h>
    int main(void) { return isinf(0.0); }
" HAVE_ISINF)
if(NOT HAVE_ISINF)
    add_compile_definitions(NO_ISINF=1)
endif()

check_c_source_compiles("
    #include <math.h>
    int main(void) { return isnan(0.0); }
" HAVE_ISNAN)
if(NOT HAVE_ISNAN)
    add_compile_definitions(NO_ISNAN=1)
endif()

# strtok_r check
check_c_source_compiles("
    #include <string.h>
    int main(void) { char *s; strtok_r(0, \"\", &s); return 0; }
" HAVE_STRTOK_R)
if(NOT HAVE_STRTOK_R)
    add_compile_definitions(NO_STRTOK_R=1)
endif()

# strcasecmp check
if(HAVE_STRINGS_H)
    set(STRINGS_HEADER "strings.h")
else()
    set(STRINGS_HEADER "string.h")
endif()

check_c_source_compiles("
    #include <${STRINGS_HEADER}>
    int main(void) { return strcasecmp(\"\", \"\"); }
" HAVE_STRCASECMP)
if(NOT HAVE_STRCASECMP)
    add_compile_definitions(NO_STRCASECMP=1)
endif()

# snprintf check
check_symbol_exists(snprintf "stdio.h" HAVE_SNPRINTF)
if(NOT HAVE_SNPRINTF)
    add_compile_definitions(NO_SNPRINTF=1)
    list(APPEND COMPAT_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/compat/stdio")
    list(APPEND COMPAT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compat/stdio/printf.c")
endif()
