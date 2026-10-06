# Test harness: the app's core + coloring code as a shared library that
# Python drives through ctypes (vbp.py) - run a game headless, read the tile
# tracker's tags, render both eyes exactly as the app paints them, import
# paintings into packs. Not part of the app build.
#
#   cmake -S tools/harness -B build/harness -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/harness -j
#   VBP_LIB=build/harness/libvbp.so python3 tools/harness/worlds.py ROM.vb
cmake_minimum_required(VERSION 3.22)
project(vbgo_harness LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)

get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(VB_CORE_DIR ${REPO}/third_party/beetle-vb-libretro)

# Exactly the repo's own patch step (the tile tracker's hooks in vip.c).
include(${REPO}/cmake/PatchBeetleVip.cmake)
vbgo_generate_patched_vip(${VB_CORE_DIR} ${CMAKE_BINARY_DIR}/generated/beetle-vb)

add_library(vb_core_pic STATIC
    ${VB_CORE_DIR}/mednafen/hw_cpu/v810/v810_cpu.cpp
    ${VB_CORE_DIR}/mednafen/mempatcher.cpp
    ${VB_CORE_DIR}/libretro.cpp
    ${VB_CORE_DIR}/mednafen/vb/vsu.c
    ${VB_CORE_DIR}/mednafen/vb/input.c
    ${VB_CORE_DIR}/mednafen/vb/timer.c
    ${VB_CORE_DIR}/mednafen/hw_cpu/v810/fpu-new/softfloat.c
    ${VB_CORE_DIR}/mednafen/sound/Blip_Buffer.c
    ${VB_CORE_DIR}/mednafen/state.c
    ${VB_CORE_DIR}/mednafen/settings.c
    ${VB_CORE_DIR}/libretro-common/compat/compat_strl.c
    ${VB_CORE_DIR}/libretro-common/compat/compat_snprintf.c
    ${REPO}/core/emu/vbgo_tiletrack.c
    ${VBGO_PATCHED_VIP_SOURCE})
set_target_properties(vb_core_pic PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(vb_core_pic PUBLIC ${VB_CORE_DIR} ${VB_CORE_DIR}/mednafen
    ${VB_CORE_DIR}/mednafen/include ${VB_CORE_DIR}/libretro-common/include)
target_include_directories(vb_core_pic PRIVATE ${VB_CORE_DIR}/mednafen/vb ${REPO}/core/emu)
target_compile_definitions(vb_core_pic PRIVATE WANT_32BPP __LIBRETRO__ STDC_HEADERS
    __STDC_LIMIT_MACROS MEDNAFEN_VERSION="0.9.31" MEDNAFEN_VERSION_NUMERIC=931 INLINE=inline)
target_compile_options(vb_core_pic PRIVATE -w -O2)

add_library(vbp SHARED vbp.cpp
    ${REPO}/core/emu/ShadeColorizer.cpp ${REPO}/core/emu/TileColorPack.cpp
    ${REPO}/core/emu/ColorPackRenderer.cpp ${REPO}/core/emu/DepthColors.cpp ${REPO}/core/emu/UncoloredCollector.cpp)
target_include_directories(vbp PRIVATE ${REPO}/core ${REPO}/core/emu)
target_compile_options(vbp PRIVATE -O2 -include cstdint)
# (x86: POPCNT like every x86-64 desktop CPU since 2008 - the app gets it from MSVC's __popcnt64 / ARM64's cnt)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(vbp PRIVATE -mpopcnt)
endif()
target_link_libraries(vbp PRIVATE vb_core_pic m)
