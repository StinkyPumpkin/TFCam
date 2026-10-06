#pragma once

#include "TFCamAPI.h"

#include <cstdint>

// 0.10.0: Scene Camera. Drives TFCam's free camera (the vanilla FreeCameraState) from its Update hook, right after
// the vanilla Update, while TFCam drives it (TFCamDriving(): no FCFW timeline / SLCC owns it):
//   Free  - plain TFCam free-fly. A saved shot blends the camera there (auto at a stage change / pose, or on request);
//           afterwards you fly on from there.
//   Orbit - circles the player's SexLab scene actors (else the posed actor, else the player), always looking at the
//           smoothed centre of their torsos. WASD / mouse do nothing; the mouse wheel changes the radius.
//   Eye   - looks out of a scene partner's eyes (player SexLab scene only). The mouse looks around (+-85 degrees),
//           the partner's head is hidden and the near clip pulled in while it lasts.
// Saved shots (SceneShots.h) are kept per scene stage + the player's slot, and per pose (SLUI tells TFCam which
// pose plays through ITFCam2::SetActivePose).
//
// Everything here runs on the main thread; the Api* entry points copy their arguments into an SKSE task.
namespace SceneCam {

    // TFCam.ini [SceneCam]. Keys are DX scan codes, 0 = unset.
    struct Config {
        int   orbitKey       = 0;
        int   eyeKey         = 0;
        int   eyeNextKey     = 0;
        int   shotSaveKey    = 0;
        int   shotRecallKey  = 0;
        int   shotDeleteKey  = 0;
        float orbitRadius    = 120.0f;  // 30..500 game units
        float orbitHeight    = 20.0f;   // -20..75 degrees above the target
        float orbitSpeed     = 10.0f;   // -90..90 degrees per second, sign = direction
        float orbitFOV       = 70.0f;   // 20..120
        float eyeFOV         = 90.0f;   // 30..130
        float eyeNearClip    = 3.0f;    // 1..10, fNearDistance while in eye view
        float eyeForward     = 1.0f;    // -5..10, camera this far in front of the eye point
        bool  eyeHideHead    = true;    // hide the partner's head (FaceGen node) in eye view
        bool  autoStageShots = true;    // blend to a stage's saved shot at scene start / stage change (Free mode)
        bool  autoPoseShots  = true;    // blend to a pose's saved shot when SLUI starts the pose (Free mode)
        float shotBlend      = 0.6f;    // 0..3 seconds
    };

    Config& GetConfig();
    void    ClampConfig();      // after loading the ini / a menu change
    void    OnConfigChanged();  // SKSE Menu Framework page changed a value (clamps; the drive reads it every frame)

    void Install();       // kDataLoaded
    void ScheduleTick();  // any thread: run the main-thread tick (scene reads, pending requests, status) next frame

    // FreeCamController
    void Drive(RE::FreeCameraState* a_fcs, float a_prePitch, float a_preYaw, bool a_driving);  // Update hook, after vanilla
    bool OwnsMotion();                    // orbit / eye view / a blend moves the camera (menu restore + drive stand down)
    bool OnWheel(int a_dir);              // -1 wheel up, +1 wheel down; true = used (orbit radius / eye FOV)
    bool OnHotkey(std::uint32_t a_code);  // keyboard key-down, no menu open; true = a Scene Camera key, consume it
    void OnFreeCamEnd();                  // main thread, before the vanilla End's FOV write-back

    // SceneTracker (main thread)
    void OnPlayerSceneStart();
    void OnSceneChanged(bool a_keyChanged);  // stage key committed (true) or only the actors changed (false)
    void OnPlayerSceneEnd();

    void OnGameLoad(const char* a_why);  // kPreLoadGame / kPostLoadGame / kNewGame

    // TFCamAPI v2 (any thread)
    bool ApiSetOrbit(std::int32_t a_state);
    bool ApiSetEyeView(std::int32_t a_state);
    bool ApiNextEyeTarget();
    void ApiSetSettings(const TFCAM_API::SceneCamSettings& a_settings);
    bool ApiSaveShot();
    bool ApiRecallShot();
    bool ApiDeleteShot();
    void ApiSetActivePose(std::uint32_t a_actor, const char* a_animEvent);
    void ApiGetStatus(TFCAM_API::SceneCamStatus* a_out);

    // SKSE Menu Framework page (any thread)
    struct MenuStatus {
        TFCAM_API::SceneCamStatus status{};
        int                       sceneShots = 0;  // saved shots for the current scene (all stages / slots)
        char                      lastEvent[192] = {};
    };
    MenuStatus GetMenuStatus();
    void       RequestDeleteSceneShots();
}
