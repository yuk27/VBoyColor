#include "menu/pages/MoveScreenPage.h"
#include "io/Platform.h"
#include "io/Settings.h"
#include "menu/MenuPage.h"
#include "menu/pages/AppMenuLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
std::string FormatDeg(const char *prefix, float value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%.0f\xC2\xB0", prefix, value);
    return buf;
}
std::string FormatValue(const char *prefix, float value, const char *suffix = "")
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s%.2f%s", prefix, value, suffix);
    return buf;
}
std::string FormatFloat(const char *prefix, float value, int precision = 3, const char *suffix = "")
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%.*f%s", prefix, precision, value, suffix);
    return buf;
}
} // namespace

void MoveScreenPage::Init(UiRenderer &ui, const UiMenuResources &resources)
{
    m_settings = resources.settings;
    m_platform = resources.platform;

    auto list = MakeList(ui, resources);

    // Select resets just that row's value to its default (unlike the other
    // rows below, where Select acts like Right).
    list->AddHeader("Position");
    m_yawEntry = list->AddEntry("Yaw", [this](MenuItem *) { ResetToDefault(&AppSettings::screenYaw, kDefaultYaw); },
        [this](MenuItem *) { ChangeYaw(-kYawPitchStep); }, [this](MenuItem *) { ChangeYaw(kYawPitchStep); }, UiIconId::LeftRight);
    m_pitchEntry = list->AddEntry("Pitch", [this](MenuItem *) { ResetToDefault(&AppSettings::screenPitch, kDefaultPitch); },
        [this](MenuItem *) { ChangePitch(-kYawPitchStep); }, [this](MenuItem *) { ChangePitch(kYawPitchStep); }, UiIconId::UpDown);
    m_rollEntry = list->AddEntry("Roll", [this](MenuItem *) { ResetToDefault(&AppSettings::screenRoll, kDefaultRoll); },
        [this](MenuItem *) { ChangeRoll(-kRollStep); }, [this](MenuItem *) { ChangeRoll(kRollStep); }, UiIconId::Reset);
    m_distanceEntry = list->AddEntry("Distance", [this](MenuItem *) { ResetToDefault(&AppSettings::screenDistance, kDefaultDistance); },
        [this](MenuItem *) { ChangeDistance(-kDistanceStep); }, [this](MenuItem *) { ChangeDistance(kDistanceStep); }, UiIconId::Distance);
    m_scaleEntry = list->AddEntry("Scale", [this](MenuItem *) { ResetToDefault(&AppSettings::screenScale, kDefaultScale); },
        [this](MenuItem *) { ChangeScale(-kScaleStep); }, [this](MenuItem *) { ChangeScale(kScaleStep); }, UiIconId::Scale);

    // Off/Smooth/Instant - Left/Right cycle backward/forward, Select acts
    // like Right (matches Yaw/Pitch/Roll/Distance/Scale above).
    list->AddHeader("View");
    m_followHeadEntry = list->AddEntry("Follow head", [this](MenuItem *) { CycleFollowHeadMode(1); },
        [this](MenuItem *) { CycleFollowHeadMode(-1); }, [this](MenuItem *) { CycleFollowHeadMode(1); }, UiIconId::FollowHead);
    m_threeDeeEntry = list->AddEntry("3D screen", [this](MenuItem *) { ToggleThreeDeeMode(); }, nullptr, nullptr, UiIconId::ThreeD);
    m_threeDeeEntry->toggle = [this]() { return m_settings && m_settings->useThreeDeeMode; };
    // The room around the screen (passthrough) or black - only where the
    // headset can show it.
    if (resources.passthroughSupported)
    {
        m_passthroughEntry = list->AddEntry("Show your room around it", [this](MenuItem *) { TogglePassthrough(); });
        m_passthroughEntry->toggle = [this]() { return m_settings && m_settings->passthrough; };
        m_passthroughEntry->reserveIconSpace = true;
    }
    // IPD is the one exception to "Select acts like Right" - press already
    // has a distinct, meaningful action (reset to 0), so it stays that way
    // rather than doubling up with Right's step.
    m_ipdEntry = list->AddEntry("IPD offset", [this](MenuItem *) { ChangeIpd(0); /* press resets - see ChangeIpd */ },
        [this](MenuItem *) { ChangeIpd(-1); }, [this](MenuItem *) { ChangeIpd(1); }, UiIconId::Ipd);

    list->AddSpacer(kGroupGap);
    list->AddEntry("Reset values", [this](MenuItem *) { ResetView(); }, nullptr, nullptr, UiIconId::ResetView)->centered = true;

    m_menu.MenuItems.push_back(list);

    m_menu.BackPress = [this]() { if (settingsPage) Navigate(settingsPage, -1); };
    m_menu.Init();

    RefreshLabels();
}

void MoveScreenPage::ChangeYaw(float delta)
{
    if (!m_settings) return;
    m_settings->screenYaw = std::clamp(m_settings->screenYaw + delta, kYawPitchMin, kYawPitchMax);
    RefreshLabels();
}

void MoveScreenPage::ChangePitch(float delta)
{
    if (!m_settings) return;
    m_settings->screenPitch = std::clamp(m_settings->screenPitch + delta, kYawPitchMin, kYawPitchMax);
    RefreshLabels();
}

void MoveScreenPage::ChangeRoll(float delta)
{
    if (!m_settings) return;
    // Wraps instead of clamping - a full rotation around the view axis is
    // always a valid, sensible orientation (unlike Yaw/Pitch's limited
    // range), so overshooting past 360 just continues from 0.
    m_settings->screenRoll = std::fmod(m_settings->screenRoll + delta, 360.0f);
    if (m_settings->screenRoll < 0.0f) m_settings->screenRoll += 360.0f;
    RefreshLabels();
}

void MoveScreenPage::ChangeDistance(float delta)
{
    if (!m_settings) return;
    m_settings->screenDistance += delta;
    if (m_settings->screenDistance < 0.5f) m_settings->screenDistance = 0.5f;
    RefreshLabels();
}

void MoveScreenPage::ChangeScale(float delta)
{
    if (!m_settings) return;
    m_settings->screenScale = std::clamp(m_settings->screenScale + delta, 0.2f, 2.0f);
    RefreshLabels();
}

void MoveScreenPage::ResetToDefault(float AppSettings::*field, float defaultValue)
{
    if (!m_settings) return;
    m_settings->*field = defaultValue;
    RefreshLabels();
}

void MoveScreenPage::ResetView()
{
    if (!m_settings) return;
    m_settings->screenYaw = kDefaultYaw;
    m_settings->screenPitch = kDefaultPitch;
    m_settings->screenRoll = kDefaultRoll;
    m_settings->screenDistance = kDefaultDistance;
    m_settings->screenScale = kDefaultScale;
    m_settings->followHeadMode = FollowHeadMode::Off;
    m_settings->useThreeDeeMode = true;
    m_settings->ipdOffset = 0.0f;
    RefreshLabels();
}

void MoveScreenPage::CycleFollowHeadMode(int direction)
{
    if (!m_settings) return;
    constexpr int kModeCount = 3;
    const int mode = (static_cast<int>(m_settings->followHeadMode) + direction + kModeCount) % kModeCount;
    m_settings->followHeadMode = static_cast<FollowHeadMode>(mode);
    RefreshLabels();
}

void MoveScreenPage::ToggleThreeDeeMode()
{
    if (!m_settings) return;
    m_settings->useThreeDeeMode = !m_settings->useThreeDeeMode;
    RefreshLabels();
}

void MoveScreenPage::TogglePassthrough()
{
    if (!m_settings) return;
    m_settings->passthrough = !m_settings->passthrough;
    RefreshLabels();
}

void MoveScreenPage::ChangeIpd(int delta)
{
    if (!m_settings) return;
    if (delta == 0)
    {
        m_settings->ipdOffset = 0.0f; // press resets to 0, matches FrontendGo's OnClickIPD
    }
    else
    {
        m_settings->ipdOffset += delta * kIpdStep;
        if (m_settings->ipdOffset < kIpdMin) m_settings->ipdOffset = kIpdMin;
        if (m_settings->ipdOffset > kIpdMax) m_settings->ipdOffset = kIpdMax;
    }
    RefreshLabels();
}

void MoveScreenPage::RefreshLabels()
{
    if (!m_settings)
        return;

    m_yawEntry->SetValue(FormatDeg("", m_settings->screenYaw));
    m_pitchEntry->SetValue(FormatDeg("", m_settings->screenPitch));
    m_rollEntry->SetValue(FormatDeg("", m_settings->screenRoll));
    m_distanceEntry->SetValue(FormatValue("", m_settings->screenDistance, " m"));
    m_scaleEntry->SetValue(FormatValue("", m_settings->screenScale, "x"));

    const char *followHeadLabel = m_settings->followHeadMode == FollowHeadMode::Off      ? "Off"
                                  : m_settings->followHeadMode == FollowHeadMode::Smooth ? "Smooth"
                                                                                          : "Instant";
    m_followHeadEntry->SetValue(followHeadLabel);
    m_threeDeeEntry->icon = m_settings->useThreeDeeMode ? UiIconId::ThreeD : UiIconId::TwoD;
    m_ipdEntry->SetValue(FormatFloat("", m_settings->ipdOffset));

    m_settings->Save(*m_platform); // always-on autosave - no explicit save action anywhere in the menu anymore
}
