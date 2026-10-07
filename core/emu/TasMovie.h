#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A tool-assisted run of a game: BizHawk's movie format (.bk2 - what
// TASVideos publishes), the controller presses for every frame from power-on.
// Emulator::LoadRom plays one back (see its movie parameter); BizHawk's
// Virtual Boy core is Mednafen's, like ours, and runs stay in sync with the
// core set the way BizHawk sets it (see Emulator.cpp's RetroEnvironment).
struct TasMovie
{
    std::string gameName; // as BizHawk names the ROM - usually the No-Intro name
    std::string romSha1;  // (lower case hex) the ROM it was made with
    std::string core;     // BizHawk's core ("Virtual Boyee")
    // Per frame: libretro joypad bits (RETRO_DEVICE_ID_JOYPAD_*), as Beetle
    // VB maps them to the Virtual Boy's buttons.
    std::vector<uint32_t> frames;

    // Reads a .bk2 (a zip: Header.txt, Input Log.txt). False with a reason.
    static bool Parse(const std::vector<uint8_t> &bk2, TasMovie &out, std::string &error);

    // SHA-1 of some bytes, lower case hex - to find the movie's ROM.
    static std::string Sha1(const uint8_t *data, size_t size);
};
