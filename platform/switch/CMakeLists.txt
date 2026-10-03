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

set(DAYTONA_GEN_ROOT "${DAYTONA_ROOT}/build-daytona/gen" CACHE PATH "Host-generated i960, TGP and 68000 C++ root")

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
    archive m2_replay_bus lockstep snd_cpu multipcm sound_board
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
target_compile_definitions(switch_runtime PUBLIC M2_ROMSET="daytona")

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${DAYTONA_GEN_ROOT}/daytona/*.cpp")
list(APPEND GEN_SOURCES "${DAYTONA_GEN_ROOT}/daytona_tgp/tgp_gen.cpp" "${DAYTONA_GEN_ROOT}/daytona_snd/snd_gen.cpp")

add_executable(daytona_switch main.cpp ${GEN_SOURCES})
target_include_directories(daytona_switch PRIVATE
    "${DAYTONA_ROOT}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    /opt/devkitpro/portlibs/switch/include
    /opt/devkitpro/portlibs/switch/include/SDL2
    /opt/devkitpro/libnx/include
)
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")

target_link_libraries(daytona_switch PRIVATE
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

target_link_options(daytona_switch PRIVATE -Wl,--gc-sections)

set(DAYTONA_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
set(DAYTONA_NACP "${CMAKE_CURRENT_BINARY_DIR}/daytona_switch.nacp")

nx_generate_nacp(OUTPUT "${DAYTONA_NACP}"
    NAME "Daytona USA"
    AUTHOR "Thorhax"
    VERSION "1.0.0"
)

nx_create_nro(daytona_switch
    NACP "${DAYTONA_NACP}"
    ICON "${DAYTONA_ICON}"
)
