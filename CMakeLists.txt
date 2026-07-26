# UADE Playback Plugin
#
# This plugin uses UADE (Unix Amiga Delitracker Emulator) to decode Amiga music formats.
# Based on UADE by Heikki Orsila and Michael Doering - https://gitlab.com/mvtiaine/uade
#
# Build structure:
#   1. Use pre-generated CPU emulation code (generated/ directory)
#   2. Build libuade (frontend common modules)
#   3. Build uadecore (68000 emulator + Amiga hardware)
#   4. Build uade_playback plugin (links libuade + uadecore)

cmake_minimum_required(VERSION 3.16)
project(playback_uade C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

set(UADE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/uade")
set(UADE_SRC "${UADE_ROOT}/src")
set(UADE_FRONTENDS "${UADE_SRC}/frontends")
set(UADE_COMMON "${UADE_FRONTENDS}/common")
set(UADE_INCLUDE "${UADE_FRONTENDS}/include")
set(UADE_COMPAT "${UADE_ROOT}/compat")
set(BENCODE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/bencode")

if(NOT EXISTS "${UADE_SRC}/uade.c")
    message(WARNING "UADE source not found at ${UADE_SRC}. UADE plugin will not be built.")
    return()
endif()

# Platform-specific compile definitions for UADE
# These replace what autoconf would normally generate
set(UADE_COMPILE_DEFS
    HAVE_STRING_H=1
    HAVE_SYS_STAT_H=1
    HAVE_STRDUP=1
    SIZEOF_SHORT=2
    SIZEOF_INT=4
    SIZEOF_LONG_LONG=8
)

if(WIN32)
    list(APPEND UADE_COMPILE_DEFS
        SIZEOF_LONG=4
        SIZEOF___INT64=8
    )
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        # MSVC's UCRT prefixes these POSIX-compatible entry points. Without
        # explicit aliases, legacy C accepts them as implicitly declared
        # int-returning functions, which also truncates pointer returns.
        list(APPEND UADE_COMPILE_DEFS
            access=_access
            close=_close
            fdopen=_fdopen
            fileno=_fileno
            fstat=_fstat
            lseek=_lseek
            open=_open
            read=_read
            strdup=_strdup
            unlink=_unlink
            write=_write
        )
    endif()
else()
    list(APPEND UADE_COMPILE_DEFS
        HAVE_STRINGS_H=1
        HAVE_SYS_TYPES_H=1
        HAVE_UNISTD_H=1
        HAVE_FCNTL_H=1
        HAVE_DIRENT_H=1
        SIZEOF_LONG=8
    )
endif()

# Pre-generated 68000 CPU emulation files
# Generated from uade/src/table68k using build68k and gencpu tools.
# To regenerate: run build68k < table68k > cpudefs.c, then gencpu to produce the rest.
set(UADE_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")

# =============================================================================
# Step 2: Build bencode library (required for UADE)
# =============================================================================

add_library(bencode STATIC
    ${BENCODE_ROOT}/bencode.c
)
target_include_directories(bencode PUBLIC
    ${BENCODE_ROOT}/include
)
set_target_properties(bencode PROPERTIES POSITION_INDEPENDENT_CODE ON)

function(uade_enable_msvc_c target_name)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        # UADE uses C11 designated initializers. __STDC__ is also required by
        # its inherited UAE compatibility headers, but MSVC only defines it
        # when explicitly requested.
        target_compile_options(${target_name} PRIVATE /std:c11 /Zc:__STDC__)
    endif()
endfunction()

function(uade_reject_implicit_msvc_functions target_name)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        # Keep third-party warnings quiet, but never permit an undeclared
        # function: its assumed int return type can corrupt 64-bit pointers.
        target_compile_options(${target_name} PRIVATE /we4013)
    endif()
endfunction()

uade_enable_msvc_c(bencode)

# =============================================================================
# Step 3: Build libuade (frontend common modules)
# =============================================================================

set(LIBUADE_SOURCES
    ${UADE_COMMON}/unixatomic.c
    ${UADE_COMMON}/uadeipc.c
    ${UADE_COMMON}/amifilemagic.c
    ${UADE_COMMON}/eagleplayer.c
    ${UADE_COMMON}/unixwalkdir.c
    ${UADE_COMMON}/effects.c
    ${UADE_COMMON}/uadecontrol.c
    ${UADE_COMMON}/uadeconf.c
    ${UADE_COMMON}/uadestate.c
    ${UADE_COMMON}/uadeutils.c
    ${UADE_COMMON}/md5.c
    ${UADE_COMMON}/ossupport.c
    ${UADE_COMMON}/rmc.c
    ${UADE_COMMON}/songdb.c
    ${UADE_COMMON}/songinfo.c
    ${UADE_COMMON}/vparray.c
    ${UADE_COMMON}/support.c
    ${UADE_COMMON}/fifo.c
)

# strlcpy/strlcat are native on macOS/BSD, only use compat on Linux/Windows
if(NOT APPLE)
    list(APPEND LIBUADE_SOURCES ${UADE_COMPAT}/strlrep.c)
endif()

# Windows-specific compat files
if(WIN32)
    list(APPEND LIBUADE_SOURCES ${UADE_COMPAT}/mingw-socketpair.c)
endif()

add_library(libuade STATIC ${LIBUADE_SOURCES})
target_include_directories(libuade PUBLIC
    ${UADE_INCLUDE}
    ${UADE_COMMON}
    ${UADE_SRC}
    ${UADE_SRC}/include
    ${UADE_COMPAT}
    ${BENCODE_ROOT}/include
)
target_compile_definitions(libuade PRIVATE ${UADE_COMPILE_DEFS})
uade_enable_msvc_c(libuade)
suppress_external_warnings(libuade)
uade_reject_implicit_msvc_functions(libuade)
# Clang 16+ makes implicit-function-declaration a hard error that -w doesn't suppress
if(NOT MSVC)
    target_compile_options(libuade PRIVATE -Wno-error=implicit-function-declaration)
endif()
set_target_properties(libuade PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(libuade PRIVATE bencode)

# =============================================================================
# Step 4: Build uadecore (68000 emulator + Amiga hardware)
# =============================================================================

# CPU emulation parts (compiled with different PART_N defines)
set(CPUEMU_PARTS 1 2 3 4 5 6 7 8)

foreach(PART ${CPUEMU_PARTS})
    add_library(cpuemu${PART} OBJECT ${UADE_GENERATED_DIR}/cpuemu.c)
    target_compile_definitions(cpuemu${PART} PRIVATE PART_${PART} ${UADE_COMPILE_DEFS})
    uade_enable_msvc_c(cpuemu${PART})
    target_include_directories(cpuemu${PART} PRIVATE
        ${UADE_SRC}
        ${UADE_SRC}/include
        ${UADE_INCLUDE}
        ${UADE_GENERATED_DIR}
    )
    suppress_external_warnings(cpuemu${PART})
    uade_reject_implicit_msvc_functions(cpuemu${PART})
    if(NOT MSVC)
        target_compile_options(cpuemu${PART} PRIVATE -Wno-error=implicit-function-declaration)
    endif()
    set_target_properties(cpuemu${PART} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

set(UADECORE_SOURCES
    ${UADE_SRC}/newcpu.c
    ${UADE_SRC}/memory.c
    ${UADE_SRC}/custom.c
    ${UADE_SRC}/cia.c
    ${UADE_SRC}/audio.c
    ${UADE_SRC}/compiler.c
    ${UADE_SRC}/missing.c
    ${UADE_SRC}/sd-sound-generic.c
    ${UADE_SRC}/md-support.c
    ${UADE_SRC}/cfgfile.c
    ${UADE_SRC}/fpp.c
    ${UADE_SRC}/debug.c
    ${UADE_SRC}/readcpu.c
    ${UADE_SRC}/uade.c
    ${UADE_SRC}/uademain.c
    ${UADE_SRC}/sinctable.c
    ${UADE_SRC}/text_scope.c
    ${UADE_SRC}/state_detection.c
    ${UADE_SRC}/uade_logging.c
    ${UADE_SRC}/write_audio.c
    ${UADE_GENERATED_DIR}/cpudefs.c
    ${UADE_GENERATED_DIR}/cpustbl.c
)

add_library(uadecore STATIC
    ${UADECORE_SOURCES}
    $<TARGET_OBJECTS:cpuemu1>
    $<TARGET_OBJECTS:cpuemu2>
    $<TARGET_OBJECTS:cpuemu3>
    $<TARGET_OBJECTS:cpuemu4>
    $<TARGET_OBJECTS:cpuemu5>
    $<TARGET_OBJECTS:cpuemu6>
    $<TARGET_OBJECTS:cpuemu7>
    $<TARGET_OBJECTS:cpuemu8>
)

target_include_directories(uadecore PUBLIC
    ${UADE_SRC}
    ${UADE_SRC}/include
    ${UADE_INCLUDE}
    ${UADE_COMMON}
    ${UADE_COMPAT}
    ${UADE_GENERATED_DIR}
)
target_compile_definitions(uadecore PRIVATE
    DISABLE_ZAKALWE=1
    UADE_EMBEDDED_CORE=1
    ${UADE_COMPILE_DEFS}
    # Return control to the embedded core thread instead of terminating the host process.
    exit=uade_thread_exit
)
uade_enable_msvc_c(uadecore)
suppress_external_warnings(uadecore)
uade_reject_implicit_msvc_functions(uadecore)
if(NOT MSVC)
    target_compile_options(uadecore PRIVATE -Wno-error=implicit-function-declaration)
endif()
set_target_properties(uadecore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# =============================================================================
# Step 5: Build uade_playback plugin
# =============================================================================

add_library(uade_playback SHARED
    uade_plugin.cpp
)

target_include_directories(uade_playback PRIVATE
    ${UADE_INCLUDE}
    ${UADE_COMMON}
    ${UADE_SRC}
    ${UADE_SRC}/include
)

if(NOT MSVC)
    target_compile_options(uade_playback PRIVATE
        -Wno-unused-parameter
    )
endif()

# Link all the static libraries
target_link_libraries(uade_playback PRIVATE
    libuade
    uadecore
    bencode
)

# std::thread uses the platform's native threading implementation.
find_package(Threads REQUIRED)
target_link_libraries(uade_playback PRIVATE Threads::Threads)
if(UNIX)
    target_link_libraries(uade_playback PRIVATE m)
endif()

# Link winsock on Windows
if(WIN32)
    target_link_libraries(uade_playback PRIVATE ws2_32)
endif()

set_target_properties(uade_playback PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
)

# =============================================================================
# Step 6: Install UADE data files (players, eagleplayer.conf)
# =============================================================================

set(UADE_DATA_DIR "${RETROVERT_PLUGIN_OUTPUT_DIR}/uade_data")

# Install UADE data files
add_custom_command(TARGET uade_playback POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${UADE_DATA_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${UADE_ROOT}/eagleplayer.conf"
        "${UADE_DATA_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/uaerc"
        "${UADE_DATA_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/score"
        "${UADE_DATA_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${UADE_ROOT}/players"
        "${UADE_DATA_DIR}/players"
    COMMENT "Installing UADE data files to ${UADE_DATA_DIR}"
)

rv_set_plugin_properties(uade_playback)

option(UADE_BUILD_SMOKE_TEST "Build the standalone UADE playback smoke test" OFF)
if(UADE_BUILD_SMOKE_TEST)
    enable_testing()
    add_executable(uade_playback_smoke tests/uade_playback_smoke.cpp)
    target_include_directories(uade_playback_smoke PRIVATE ${RETROVERT_INCLUDE_DIR})
    set_target_properties(uade_playback_smoke PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY "${RETROVERT_PLUGIN_OUTPUT_DIR}"
    )
    if(UNIX)
        target_link_libraries(uade_playback_smoke PRIVATE ${CMAKE_DL_LIBS})
    endif()
    add_test(
        NAME uade_playback_smoke
        COMMAND uade_playback_smoke "$<TARGET_FILE:uade_playback>"
    )
    set_tests_properties(uade_playback_smoke PROPERTIES
        WORKING_DIRECTORY "${RETROVERT_PLUGIN_OUTPUT_DIR}"
    )
endif()
