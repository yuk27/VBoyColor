#!/usr/bin/env python3
"""Show or record the ROM a color pack (.vbcp) was made for.

A pack records its ROM (CRC-32 and size) in a 16-byte footer ("VBGOROM1", CRC,
size), so the app finds it whatever the ROM's file is called (see
Emulator::FindPackForRom). Packs imported by a current build have it already;
this records it in older ones.

usage:
  vbcp_rom.py PACK.vbcp [...]          show which ROM each pack records
  vbcp_rom.py PACK.vbcp ROM.vb         record ROM.vb in PACK.vbcp (in place)
"""
import struct
import sys
import zlib

MAGIC = b"VBGOROM1"


def rom_of(pack):
    if len(pack) >= 28 and pack[-16:-8] == MAGIC:
        return struct.unpack("<II", pack[-8:])
    return None


def main(args):
    if len(args) == 2 and args[1].lower().endswith(".vb"):
        pack_path, rom_path = args
        pack = open(pack_path, "rb").read()
        if not pack.startswith(b"VBGOCP0"):
            sys.exit("%s isn't a color pack" % pack_path)
        rom = open(rom_path, "rb").read()
        crc, size = zlib.crc32(rom) & 0xFFFFFFFF, len(rom)
        if rom_of(pack):
            pack = pack[:-16]
        open(pack_path, "wb").write(pack + MAGIC + struct.pack("<II", crc, size))
        print("%s: made for %s (CRC %08x, %d bytes)" % (pack_path, rom_path, crc, size))
        return
    if not args:
        sys.exit(__doc__)
    for path in args:
        r = rom_of(open(path, "rb").read())
        print("%s: %s" % (path, "CRC %08x, %d bytes" % r if r else "no ROM recorded (found by the ROM's file name only)"))


if __name__ == "__main__":
    main(sys.argv[1:])
