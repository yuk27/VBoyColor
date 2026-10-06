#pragma once
#include "menu/MenuPage.h"

#include <memory>

class MenuList;
struct AppSettings;
class Platform;

// Adjust Screen: screen placement and view mode - position (Yaw/Pitch/Roll/
// Distance/Scale), Follow Head, 3D Screen, your room around it (passthrough,
// where supported), IPD offset, and Reset Values.
// These only matter on the OpenXR path (they feed OpenXrApp's screen quad
// pose); pc2d's flat debug window has no 3D screen to move.
class MoveScreenPage : public MenuPage
{
public:
    MenuPage *settingsPage = nullptr;

    void Init(UiRenderer &ui, const UiMenuResources &resources) override;
    std::string Title() const override { return "Adjust screen"; }
    std::string Subtitle() const override { return "Settings"; }

private:
    static constexpr float kYawPitchStep = 1.0f;  // degrees
    static constexpr float kYawPitchMin = -25.0f; // degrees
    static constexpr float kYawPitchMax = 25.0f;  // degrees
    static constexpr float kRollStep = 1.0f;      // degrees
    static constexpr float kDistanceStep = 0.1f;  // meters
    static constexpr float kScaleStep = 0.1f;
    static constexpr float kIpdStep = 1.0f / 256.0f; // matches FrontendGo's IPD_STEP_SIZE
    static constexpr float kIpdMin = -0.5f;
    static constexpr float kIpdMax = 0.5f;

    static constexpr float kDefaultYaw = 0.0f;
    static constexpr float kDefaultPitch = 0.0f;
    static constexpr float kDefaultRoll = 0.0f;
    static constexpr float kDefaultDistance = 2.2f;
    static constexpr float kDefaultScale = 1.0f;

    void ChangeYaw(float delta);
    void ChangePitch(float delta);
    void ChangeRoll(float delta);
    void ChangeDistance(float delta);
    void ChangeScale(float delta);
    // Select's press action on Yaw/Pitch/Roll/Distance/Scale - resets just
    // that one field to its default instead of advancing it (unlike the
    // other rows, where press acts like Right - see Init).
    void ResetToDefault(float AppSettings::*field, float defaultValue);
    void ResetView();
    // direction +1/-1 cycles FollowHeadMode forward/backward, wrapping
    // Off<->Smooth<->Instant.
    void CycleFollowHeadMode(int direction);
    void ToggleThreeDeeMode();
    void TogglePassthrough();
    void ChangeIpd(int delta);
    void RefreshLabels();

    std::shared_ptr<MenuList::Entry> m_yawEntry;
    std::shared_ptr<MenuList::Entry> m_pitchEntry;
    std::shared_ptr<MenuList::Entry> m_rollEntry;
    std::shared_ptr<MenuList::Entry> m_distanceEntry;
    std::shared_ptr<MenuList::Entry> m_scaleEntry;
    std::shared_ptr<MenuList::Entry> m_followHeadEntry;
    std::shared_ptr<MenuList::Entry> m_threeDeeEntry;
    std::shared_ptr<MenuList::Entry> m_passthroughEntry; // only where passthrough is supported
    std::shared_ptr<MenuList::Entry> m_ipdEntry;
    AppSettings *m_settings = nullptr;
    Platform *m_platform = nullptr;
};
