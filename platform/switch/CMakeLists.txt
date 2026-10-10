cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{DEVKITPRO})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/Switch.cmake" CACHE FILEPATH "Switch toolchain")
    else()
        set(CMAKE_TOOLCHAIN_FILE "/opt/devkitpro/cmake/Switch.cmake" CACHE FILEPATH "Switch toolchain")
    endif()
endif()

project(daytona_switch LANGUAGES C CXX)

get_filename_component(DAYTONA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
    -D__SWITCH__
    -DM2_VITA_RENDER_OPT=1
    -O3
    -DNDEBUG
    -Wall -Wextra
    -fno-fast-math
    -ffp-contract=off
    -ffunction-sections
    -fdata-sections
)

set(M2_ROMSET "daytona" CACHE STRING "Game to build: daytona or vf2")

if(M2_ROMSET STREQUAL "vf2")
    set(DEFAULT_GEN_ROOT "${DAYTONA_ROOT}/build-vf2/gen")
    set(GAME_NAME "Virtua Fighter 2")
    set(APP_VERSION "1.1.0")
    set(APP_TARGET "vf2_switch")
    set(ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/icon_vf2.jpg")
    # Local builds may bundle the user's ROM set and NVRAM here; release builds
    # pass -DVF2_ROMFS_DIR=none (the app then reads sdmc:/switch/vf2/vf2.zip).
    set(VF2_ROMFS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/romfs_vf2" CACHE PATH "RomFS folder bundled into the NRO")
    set(ROMFS_DIR "${VF2_ROMFS_DIR}")
    add_compile_definitions(M2_ROMSET_VF2=1)
else()
    set(DEFAULT_GEN_ROOT "${DAYTONA_ROOT}/build-daytona/gen")
    set(GAME_NAME "Daytona USA")
    set(APP_VERSION "1.0.0")
    set(APP_TARGET "daytona_switch")
    set(ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
    set(ROMFS_DIR "")
endif()

set(DAYTONA_GEN_ROOT "${DEFAULT_GEN_ROOT}" CACHE PATH "Host-generated i960, TGP and 68000 C++ root")

set(SOFTFLOAT_DIR "${DAYTONA_ROOT}/extern/softfloat")
set(YMFM_DIR "${DAYTONA_ROOT}/extern/ymfm/src")

include("${DAYTONA_ROOT}/cmake/softfloat_sources.cmake")
add_library(switch_softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(switch_softfloat PUBLIC "${SOFTFLOAT_DIR}/source/include"
    PRIVATE "${DAYTONA_ROOT}/cmake/softfloat-portable" "${SOFTFLOAT_DIR}/source/8086-SSE")
target_compile_definitions(switch_softfloat PUBLIC SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1 THREAD_LOCAL=
    PRIVATE SOFTFLOAT_ROUND_ODD INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
target_compile_options(switch_softfloat PRIVATE -w)

add_library(switch_i960 STATIC
    "${DAYTONA_ROOT}/src/i960/isa.cpp"
    "${DAYTONA_ROOT}/src/i960/decode.cpp"
    "${DAYTONA_ROOT}/src/i960/reach.cpp"
    "${DAYTONA_ROOT}/src/i960/fp.cpp"
)
target_include_directories(switch_i960 PUBLIC "${DAYTONA_ROOT}/src")
target_link_libraries(switch_i960 PUBLIC switch_softfloat)

add_library(switch_trace STATIC "${DAYTONA_ROOT}/src/trace/trace.cpp")
target_include_directories(switch_trace PUBLIC "${DAYTONA_ROOT}/src")

add_library(switch_ymfm STATIC
    "${YMFM_DIR}/ymfm_opn.cpp"
    "${YMFM_DIR}/ymfm_adpcm.cpp"
    "${YMFM_DIR}/ymfm_ssg.cpp"
)
target_include_directories(switch_ymfm SYSTEM PUBLIC "${YMFM_DIR}")
target_compile_options(switch_ymfm PRIVATE -w)

set(RUNTIME_FILES
    cpu tgp m2_tgp_board geo raster video m2_board game_loop zip rom_import
    archive m2_replay_bus lockstep snd_cpu multipcm scsp sound_board
    native_sample_mixer native_sound_sequencer native_sound_engine comm_board
    enhance
)
set(RUNTIME_SOURCES)
foreach(name IN LISTS RUNTIME_FILES)
    list(APPEND RUNTIME_SOURCES "${DAYTONA_ROOT}/src/runtime/${name}.cpp")
endforeach()

add_library(switch_runtime STATIC ${RUNTIME_SOURCES})
target_include_directories(switch_runtime PUBLIC "${DAYTONA_ROOT}/src")
target_link_libraries(switch_runtime PUBLIC switch_trace switch_i960 switch_ymfm)
target_compile_definitions(switch_runtime PUBLIC M2_ROMSET="${M2_ROMSET}")
if(M2_ROMSET STREQUAL "vf2")
    target_compile_definitions(switch_runtime PUBLIC M2_ROMSET_VF2=1)
endif()

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${DAYTONA_GEN_ROOT}/${M2_ROMSET}/*.cpp")
list(APPEND GEN_SOURCES "${DAYTONA_GEN_ROOT}/${M2_ROMSET}_tgp/tgp_gen.cpp" "${DAYTONA_GEN_ROOT}/${M2_ROMSET}_snd/snd_gen.cpp")

add_executable(${APP_TARGET} main.cpp ${GEN_SOURCES})
target_include_directories(${APP_TARGET} PRIVATE
    "${DAYTONA_ROOT}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    /opt/devkitpro/portlibs/switch/include
    /opt/devkitpro/portlibs/switch/include/SDL2
    /opt/devkitpro/libnx/include
)
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")

target_link_libraries(${APP_TARGET} PRIVATE
    switch_runtime
    -lSDL2
    -lEGL
    -lGLESv2
    -lglapi
    -ldrm_nouveau
    -lz
    -lnx
    -lm
)

target_link_options(${APP_TARGET} PRIVATE -Wl,--gc-sections)

set(SWITCH_NACP "${CMAKE_CURRENT_BINARY_DIR}/${APP_TARGET}.nacp")

nx_generate_nacp(OUTPUT "${SWITCH_NACP}"
    NAME "${GAME_NAME}"
    AUTHOR "Thorhax"
    VERSION "${APP_VERSION}"
)

if(EXISTS "${ROMFS_DIR}")
    nx_create_nro(${APP_TARGET}
        NACP "${SWITCH_NACP}"
        ICON "${ICON_FILE}"
        ROMFS "${ROMFS_DIR}"
    )
else()
    nx_create_nro(${APP_TARGET}
        NACP "${SWITCH_NACP}"
        ICON "${ICON_FILE}"
    )
endif()
