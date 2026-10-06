#include "input/ButtonMapping.h"

#include "emu/Emulator.h" // VBButtonBit

namespace ButtonMapper
{

uint32_t TranslateToVBBitmask(const uint32_t buttonStates[3], const MappedButtons (&vbButtons)[16])
{
    uint32_t vbBits = 0;
    for (uint32_t vbBit = 0; vbBit < 16; ++vbBit)
    {
        for (const MappedButton &binding : vbButtons[vbBit].Buttons)
        {
            if (!binding.IsSet)
                continue;
            if (binding.InputDevice < 0 || binding.InputDevice > 2)
                continue;
            if (buttonStates[binding.InputDevice] & ButtonMapping[binding.ButtonIndex])
            {
                vbBits |= (1u << vbBit);
                break; // either binding is enough - no need to check the other
            }
        }
    }
    return vbBits;
}

void ApplyDefaultGamepadBindings(MappedButtons (&vbButtons)[16])
{
    auto setDefault = [&](uint32_t vbBit, uint32_t emuButton)
    {
        MappedButton &b = vbButtons[vbBit].Buttons[1];
        if (!b.IsSet)
        {
            b.IsSet = true;
            b.InputDevice = DeviceGamepad;
            b.ButtonIndex = static_cast<int>(emuButton);
        }
    };
    setDefault(VBButtonBit::LeftUp, EmuButton_Up);
    setDefault(VBButtonBit::LeftDown, EmuButton_Down);
    setDefault(VBButtonBit::LeftLeft, EmuButton_Left);
    setDefault(VBButtonBit::LeftRight, EmuButton_Right);
    setDefault(VBButtonBit::RightUp, EmuButton_RightStickUp);
    setDefault(VBButtonBit::RightDown, EmuButton_RightStickDown);
    setDefault(VBButtonBit::RightLeft, EmuButton_RightStickLeft);
    setDefault(VBButtonBit::RightRight, EmuButton_RightStickRight);
    setDefault(VBButtonBit::A, EmuButton_A);
    setDefault(VBButtonBit::B, EmuButton_B);
    setDefault(VBButtonBit::L, EmuButton_LShoulder);
    setDefault(VBButtonBit::R, EmuButton_RShoulder);
    setDefault(VBButtonBit::Start, EmuButton_Enter);
    setDefault(VBButtonBit::Select, EmuButton_Back);
}

} // namespace ButtonMapper
