#pragma once

#include "TFCamAPI.h"

#include <cstdint>
#include <string>
#include <string_view>

// 0.10.0: Scene Camera. Drives TFCam's free camera (the vanilla FreeCameraState) from its Update hook, right after
// the vanilla Update, while TFCam drives it (TFCamDriving(): no FCFW timeline / SLCC owns it):
//   Free  - plain TFCam free-fly. A saved shot blends the camera there (auto at a stage change / pose, or on request);
//           afterwards you fly on from there.
//   Orbit - circles the player's SexLab scene actors (else the posed actor, else the player), always looking at the
//           smoothed centre of their torsos. WASD / mouse do nothing; the mouse wheel changes the radius.
//   Eye   - looks out of a scene partner's eyes (player SexLab scene only). The mouse looks around (+-85 degrees),
//           the partner's head is hidden and the near clip pulled in while it lasts.
//   Preset - 0.15.0: fixed views round the player's SexLab scene - the stage's saved shot first (when there is one),
//           then 10 built-in angles (front, the sides, back, top, low) relative to the player's heading. The mouse
//           wheel steps through them. "Set cam pos" leaves for plain free-fly; the middle mouse button then saves the
//           camera as the stage's shot (instead of a screenshot) and preset view comes back on it.
// 0.15.0 orbit: the mouse wheel is the orbit FOV, Shift + wheel the radius, and the mouse's up / down tilts the orbit
// (camera up to a straight top view, down to looking up from below - never under the scene's floor).
// Saved shots (SceneShots.h) are kept per scene stage + the player's slot, and per pose (SLUI tells TFCam which
// pose plays through ITFCam2::SetActivePose).
// 0.13.0: with autoEnterShots a stage (or an NPC pose) that has a saved shot turns TFCam free-fly on by itself, once per
// committed key, waiting while a pausing / focus menu is open; a free cam entered that way for an NPC pose is left
// again when the pose ends.
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
        float orbitHeight    = 20.0f;   // -20..75 degrees above the target (the "tilt" in API v3)
        float orbitSpeed     = 10.0f;   // -90..90 degrees per second, sign = direction
        float orbitFOV       = 70.0f;   // 20..120
        float orbitAim       = 0.0f;    // 0.13.0: -60..60 units, the orbit circles / looks at the target this far up
        float orbitSway      = 8.0f;    // 0.13.0: 0..15 degrees of elevation breathing (was a fixed 8)
        int   orbitTarget    = 0;       // 0.13.0: 0 = auto (scene actors > posed actor > player), 1 = the player only,
                                        // 0.17.0: 2 = the console's selected reference (else auto)
        float orbitRoll      = 0.0f;    // 0.17.0: -90..90 degrees of camera roll while orbiting (a Dutch angle)
        float orbitRollSpeed = 0.0f;    // 0.17.0: -90..90 degrees per second: the roll keeps turning (0 = fixed)
        float eyeFOV         = 90.0f;   // 30..130
        float eyeNearClip    = 3.0f;    // 1..10, fNearDistance while in eye view
        float eyeForward     = 1.0f;    // -5..10, camera this far in front of the eye point
        bool  eyeHideHead    = true;    // hide the partner's head (FaceGen node) in eye view
        bool  autoStageShots = true;    // blend to a stage's saved shot at scene start / stage change (Free mode)
        bool  autoPoseShots  = true;    // blend to a pose's saved shot when SLUI starts the pose (Free mode)
        bool  autoEnterShots = true;    // 0.13.0: a stage / NPC pose with a saved shot turns the free camera on itself
        float shotBlend      = 0.6f;    // 0..3 seconds
    };

    Config& GetConfig();

    // 0.17.0 (main thread): the camera path (CamPath.h) and the background blur (CamBlur.h).
    bool SubjectPoint(RE::NiPoint3& a_out, std::string* a_what);  // your scene's actors, else the posed actor, else you
    void StandDown(std::string_view a_why);                         // orbit / eye / preset / a shot blend off, FOV back
    // The point to keep sharp while orbit / preset / eye view moves the camera. a_on = one of them runs.
    bool BlurSubject(RE::NiPoint3& a_out, const char*& a_what, bool& a_on);
    void    ClampConfig();      // after loading the ini / a menu change
    void    OnConfigChanged();  // SKSE Menu Framework page changed a value (clamps; the drive reads it every frame)

    void Install();       // kDataLoaded
    void ScheduleTick();  // any thread: run the main-thread tick (scene reads, pending requests, status) next frame

    // FreeCamController
    void Drive(RE::FreeCameraState* a_fcs, float a_prePitch, float a_preYaw, bool a_driving);  // Update hook, after vanilla
    bool OwnsMotion();                    // orbit / eye view / a blend moves the camera (menu restore + drive stand down)
    bool OnWheel(int a_dir);              // -1 wheel up, +1 wheel down; true = used (orbit / eye FOV, preset step)
    bool OnShiftWheel(int a_dir);         // 0.15.0: Shift + wheel; true = used (orbit radius)
    void OnMouseMove(float a_dy);         // 0.15.0: raw mouse Y (TFCam's input sink, a free camera on): orbit tilt
    bool OnMiddleClick();                 // 0.15.0: middle mouse, no menu open; true = "set cam pos" saved / used it
    bool OnHotkey(std::uint32_t a_code);  // keyboard key-down, no menu open; true = a Scene Camera key, consume it
    void OnFreeCamEnd();                  // main thread, before the vanilla End's FOV write-back
    void NoteOwnLeave();                  // right before TFCam itself leaves its free-fly: that End is not the user's

    // SceneTracker (main thread)
    void OnPlayerSceneStart();
    void OnSceneChanged(bool a_keyChanged);  // stage key committed (true) or only the actors changed (false)
    void OnPlayerSceneEnd();

    // 0.11.0 (AmbientCam, main thread): the furniture the player sits on has a saved shot / go to it. The second one
    // enters TFCam free-fly first when needed (like the "Go to saved shot" key).
    bool HasFurnitureShot();
    bool RecallFurnitureShot(const char* a_source);

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

    // TFCamAPI v3 (any thread)
    void ApiSetSettings3(const TFCAM_API::SceneCamSettings3& a_settings);
    bool ApiGetStatus3(TFCAM_API::SceneCamStatus3* a_out);
    bool ApiDeleteSceneShots();

    // TFCamAPI v4 (any thread)
    bool ApiSetPresetView(std::int32_t a_state);
    bool ApiStepPreset(std::int32_t a_step);
    bool ApiSetShotPlacing(std::int32_t a_state);
    bool ApiGetPreset4(TFCAM_API::SceneCamPreset4* a_out);

    // SKSE Menu Framework page (any thread)
    struct MenuStatus {
        TFCAM_API::SceneCamStatus status{};
        int                       sceneShots = 0;  // saved shots for the current scene (all stages / slots)
        char                      lastEvent[192] = {};
    };
    MenuStatus GetMenuStatus();
    void       RequestDeleteSceneShots();
}
