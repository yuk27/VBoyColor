#pragma once

#include <cstdint>

// How the library's thumbnails find each game's title screen (see
// Emulator::GiveThumbnailInput): the game runs from power-on with Start held
// for 4 frames every 120 frames - through the "IMPORTANT: read the
// instruction booklet" screen, the Virtual Boy logo, the automatic pause
// option and the alignment screen - up to frame lastPress, then on with no
// input; the thumbnail is the left picture at frame `frame`. Only numbers:
// nothing of any game is in here. Worked out for each game in the harness
// (tools/harness) from the No-Intro set; a ROM not listed takes kDefault,
// which lands on the title of most games too.
struct ThumbnailRecipe
{
    uint32_t crc; // the ROM's CRC-32
    uint16_t lastPress;
    uint16_t frame;
};

inline constexpr ThumbnailRecipe kDefaultThumbnailRecipe = {0, 600, 1100};

inline constexpr ThumbnailRecipe kThumbnailRecipes[] = {
    {0xbb71b522, 600, 1170},  // 3-D Tetris (USA)
    {0xe81a3703, 600, 1020},  // Bound High (Japan) (En) (Proto)
    {0xc9710a36, 480, 720},   // Galactic Pinball (Japan, USA)
    {0x2199af41, 600, 1020},  // Golf (USA)
    {0x83cb6a00, 600, 650},   // Innsmouth no Yakata (Japan)
    {0xa44de03c, 600, 770},   // Jack Bros. (USA)
    {0xcab61e8b, 600, 660},   // Jack Bros. no Meiro de Hiihoo! (Japan)
    {0xa47de78c, 600, 800},   // Mario Clash (Japan, USA)
    {0x7ce7460d, 600, 1060},  // Mario's Tennis (Japan, USA)
    {0xdf4d56b4, 600, 640},   // Nester's Funky Bowling (USA)
    {0xf3cd40dd, 600, 1780},  // Niko-chan Battle (Japan) (Proto)
    {0x19bb2dfb, 600, 930},   // Panic Bomber (USA)
    {0x7e85c45d, 600, 1150},  // Red Alarm (Japan) - its wireframe intro
    {0xaa10a7b4, 600, 1150},  // Red Alarm (USA)
    {0x44788197, 1200, 1270}, // SD Gundam - Dimension War (Japan)
    {0xfa44402d, 600, 1670},  // Space Invaders - Virtual Collection (Japan)
    {0x44c2b723, 480, 730},   // Space Pinball (Japan) (En) (Proto) - stage select
    {0x60895693, 600, 700},   // Space Squash (Japan)
    {0x6ba07915, 600, 720},   // T&E Virtual Golf (Japan)
    {0x36103000, 600, 1790},  // Teleroboxer (Japan, USA)
    {0x40498f5e, 480, 640},   // Tobidase! Panibon (Japan)
    {0x3ccb67ae, 600, 1150},  // V-Tetris (Japan)
    {0x9e9b8b92, 480, 540},   // Vertical Force (Japan)
    {0x4c32ba5e, 600, 640},   // Vertical Force (USA)
    {0x20688279, 600, 650},   // Virtual Bowling (Japan)
    {0x133e9372, 600, 720},   // Virtual Boy Wario Land (Japan, USA)
    {0x526cc969, 480, 660},   // Virtual Fishing (Japan)
    {0x8989fe0a, 600, 1060},  // Virtual Lab (Japan)
    {0x736b40d6, 600, 1100},  // Virtual League Baseball (USA)
    {0x9ba8bb5e, 480, 960},   // Virtual Pro Yakyuu '95 (Japan)
    {0x82a95e51, 600, 640},   // Waterworld (USA)
};

inline const ThumbnailRecipe &ThumbnailRecipeFor(uint32_t crc)
{
    for (const ThumbnailRecipe &recipe : kThumbnailRecipes)
        if (recipe.crc == crc)
            return recipe;
    return kDefaultThumbnailRecipe;
}
