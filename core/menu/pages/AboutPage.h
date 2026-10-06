#pragma once
#include "menu/MenuPage.h"

// About VBoy Color: what it is, its license, who it builds on, and that it
// isn't affiliated with Nintendo or Meta. Reached from Settings.
class AboutPage : public MenuPage
{
public:
    MenuPage *settingsPage = nullptr;

    void Init(UiRenderer &ui, const UiMenuResources &resources) override;
};
