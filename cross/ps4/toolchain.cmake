SET(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

SET(CMAKE_C_COMPILER_WORKS TRUE)
SET(CMAKE_C_COMPILER_FORCED TRUE)
SET(CMAKE_CXX_COMPILER_WORKS TRUE)
SET(CMAKE_CXX_COMPILER_FORCED TRUE)

set(SCE_SDK_ROOTDIR $ENV{SCE_ORBIS_SDK_DIR})
set(CONFIGURATION $ENV{configuration} CACHE STRING "Configuration type: Debug/Release")

if(NOT EXISTS ${SCE_SDK_ROOTDIR})
message(FATAL_ERROR "${SCE_SDK_ROOTDIR} does not exist! You should either set an environment variable SCE_SDK_ROOTDIR")
endif()

# specify the cross compiler
SET(CMAKE_C_COMPILER ${SCE_SDK_ROOTDIR}/host_tools/bin/orbis-clang.exe CACHE PATH "c compiler")
SET(CMAKE_CXX_COMPILER ${SCE_SDK_ROOTDIR}/host_tools/bin/orbis-clang++.exe CACHE PATH "c++ compiler")

#there may be a way to make cmake deduce these TODO deduce the rest of the tools
set(CMAKE_AS ${SCE_SDK_ROOTDIR}/host_tools/bin/orbis-as.exe CACHE PATH "archive")
set(CMAKE_AR ${SCE_SDK_ROOTDIR}/host_tools/bin/orbis-ar.exe CACHE PATH "archive")
set(CMAKE_LINKER ${SCE_SDK_ROOTDIR}/host_tools/bin/orbis-ld.exe CACHE PATH "linker")
set(CMAKE_NM ${SCE_SDK_ROOTDIR}/host_tools/bin/orbis-nm.exe CACHE PATH "nm")
set(CMAKE_OBJCOPY ${SCE_SDK_ROOTDIR}/host_tools/bin/orbis-objcopy.exe CACHE PATH "objcopy")
set(CMAKE_OBJDUMP ${SCE_SDK_ROOTDIR}/host_tools/bin/orbis-objdump.exe CACHE PATH "objdump")

# allow programs like swig to be found -- but can be deceiving for
# system tool dependencies.
#SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM ONLY)
# only search for libraries and includes in the ndk toolchain
#SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
#SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)


SET(CMAKE_CXX_FLAGS "-v -std=c++11 -Wunknown-pragmas -Woption-implicitly-enabled -momit-leaf-frame-pointer -Os -Wall -mstackrealign -fobjc-runtime=gnustep -fno-caret-diagnostics -fdiagnostics-show-option -fcxx-exceptions -fexceptions -pthread -x c++")
SET(CMAKE_C_FLAGS "-v -ansi -Wunknown-pragmas -Woption-implicitly-enabled -momit-leaf-frame-pointer -Os -Wall -mstackrealign -fobjc-runtime=gnustep -fno-caret-diagnostics -fdiagnostics-show-option -pthread")

## Debug/Release flags
if(CONFIGURATION STREQUAL "Debug")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDEBUG=1 -g -ggdb -D_DEBUG")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DDEBUG=1 -g -ggdb -D_DEBUG")
else()
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Os -DNDEBUG=1")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Os -DNDEBUG=1")
endif()

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" CACHE STRING "c++ flags")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}" CACHE STRING "c flags")

##
#SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
#SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
#SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
