# Copies the repo's color packs (SRC/*.vbcp) next to a desktop executable
# (DST, its colorpacks/ folder) and writes DST/index.txt: the ROM each pack
# was made for ("<CRC-32 hex> <ROM size> <pack file>" a line, from the pack's
# 16-byte "VBGOROM1" footer), so a game finds its built-in pack whatever its
# ROM file is called (Emulator::FindPackForRom) - the same index the Android
# build writes (android/app/build.gradle). Run at build time:
#   cmake -D SRC=<repo>/colorpacks -D DST=<exe dir>/colorpacks -P BundleColorPacks.cmake

file(GLOB packs "${SRC}/*.vbcp")
list(SORT packs)
file(MAKE_DIRECTORY "${DST}")
set(index "")
foreach(pack IN LISTS packs)
    get_filename_component(name "${pack}" NAME)
    file(COPY "${pack}" DESTINATION "${DST}")
    file(SIZE "${pack}" size)
    if(size LESS 28)
        continue()
    endif()
    math(EXPR at "${size} - 16")
    file(READ "${pack}" tail OFFSET ${at} LIMIT 16 HEX)
    string(SUBSTRING "${tail}" 0 16 magic)
    if(NOT magic STREQUAL "5642474f524f4d31") # "VBGOROM1"
        message(STATUS "  ${name}: made before packs recorded their ROM - found only by the ROM's file name")
        continue()
    endif()
    # Little-endian CRC-32 and size.
    foreach(i RANGE 0 3)
        math(EXPR c "16 + ${i} * 2")
        math(EXPR s "24 + ${i} * 2")
        string(SUBSTRING "${tail}" ${c} 2 crc${i})
        string(SUBSTRING "${tail}" ${s} 2 size${i})
    endforeach()
    set(crc "${crc3}${crc2}${crc1}${crc0}")
    math(EXPR romSize "0x${size3}${size2}${size1}${size0}" OUTPUT_FORMAT DECIMAL)
    string(APPEND index "${crc} ${romSize} ${name}\n")
endforeach()
file(WRITE "${DST}/index.txt" "${index}")
