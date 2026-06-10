# 1. CRUCIAL FIX: Bypass CMake's executable linking test for bare-metal systems
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Target Operating System and Processor
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

# Force the cross-compiler
set(CMAKE_C_COMPILER xenon-gcc)
set(CMAKE_CXX_COMPILER xenon-g++)
set(CMAKE_ASM_COMPILER xenon-gcc)

# Specify the target environment root path
set(CMAKE_FIND_ROOT_PATH $ENV{DEVKITXENON})

# Adjust the find program/library behavior
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Required compiler and linker flags for libxenon
set(XENON_FLAGS "-m32 -fno-pic -mpowerpc64 -mhard-float -mcpu=cell -mno-altivec")
set(CMAKE_C_FLAGS "${XENON_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${XENON_FLAGS} -fno-exceptions -fno-rtti" CACHE STRING "" FORCE)

# Explicitly force the assembler to use the same 32-bit flag as the C compiler
set(CMAKE_ASM_FLAGS "-m32 -mpowerpc64" CACHE STRING "" FORCE)

# 2. CRUCIAL FIX: Explicitly append the 32-bit toolchain library directories (-L)
set(CMAKE_EXE_LINKER_FLAGS "-m32 -Wl,-T,${CMAKE_FIND_ROOT_PATH}/app.lds -nostartfiles -L${CMAKE_FIND_ROOT_PATH}/usr/lib -L${CMAKE_FIND_ROOT_PATH}/xenon/lib/32" CACHE STRING "" FORCE)
