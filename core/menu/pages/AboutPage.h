#pragma once
#include "menu/MenuPage.h"

// About VBoy Color: what it is, its license and emulation core, that it
// isn't affiliated with Nintendo or Meta, and thanks. Nothing to select -
// the focus stays in the sidebar.
class AboutPage : public MenuPage
{
public:
    void Init(UiRenderer &ui, const UiMenuResources &resources) override;
    std::string Title() const override { return "About"; }
    std::string Subtitle() const override { return kVersion; }
    bool HasFocusable() const override { return false; }

private:
    std::string kVersion;
};
