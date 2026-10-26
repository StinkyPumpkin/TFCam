#pragma once

#include <cstdint>

// --Claude: TFCam cross-plugin API (E:\dev\FreeCamClaude, TFCam.dll).
//
// v1: lets a photo tool (PEM / Whore Horde PhotoStudio) take over TFCam's screenshot key while
//     the player flies the camera for a photo.
// v2: Scene Camera control for SLUI (SexLab UI): orbit, partner eye view, saved shots per
//     SexLab scene stage and per pose. ITFCam2 extends ITFCam1, so a v2 pointer is also a v1.
// v3 (TFCam 0.13.0): orbit aim / sway / target, automatic free camera for saved shots, a status
//     with the free-camera state and the scene's shot count, delete every shot of the scene.
//     ITFCam3 extends ITFCam2 (single inheritance: the v1 / v2 parts of the vtable are unchanged).
//     The v2 structs stay as they were; v3 has its own, each with a size field.
// v4 (TFCam 0.15.0): preset views round the player's SexLab scene (10 built-in angles, the stage's
//     saved shot first) and "set cam pos" (fly the camera, the middle mouse button saves the
//     stage's shot). ITFCam4 extends ITFCam3; the status structs are unchanged (mode 4 = preset).
// v5 (TFCam 0.17.0): background blur (ENB's depth of field kept sharp on a subject) and an extra camera roll for
//     another mod's camera - PEM / Whore Horde PhotoStudio, an inventory camera. ITFCam5 extends ITFCam4.
//
//   auto* dll = GetModuleHandleW(L"TFCam.dll");
//   auto  fn  = dll ? reinterpret_cast<TFCAM_API::RequestFn>(GetProcAddress(dll, "RequestTFCamAPI")) : nullptr;
//   auto* api = fn ? static_cast<TFCAM_API::ITFCam4*>(fn(TFCAM_API::kVersion4)) : nullptr;
//   // nullptr from an older TFCam: ask for kVersion3 / kVersion2 and use ITFCam3 / ITFCam2.
//
// Older TFCam builds have no export or return nullptr for an unknown version. Plain types only.
// This header is copied verbatim into consumers (SLUI include/TFCamAPI.h): change both together.
namespace TFCAM_API
{
    inline constexpr std::uint32_t kVersion1 = 1;
    inline constexpr std::uint32_t kVersion2 = 2;
    inline constexpr std::uint32_t kVersion3 = 3;
    inline constexpr std::uint32_t kVersion4 = 4;
    inline constexpr std::uint32_t kVersion5 = 5;

    using SnapHandler = void (*)();

    struct ITFCam1
    {
        // While set, TFCam's screenshot key / button calls a_handler (game thread) instead of
        // pressing PrintScreen. nullptr restores the normal screenshot.
        virtual void SetSnapHandler(SnapHandler a_handler) noexcept = 0;
    };

    // What the Scene Camera is doing to the free camera right now.
    enum SceneCamMode : std::uint8_t
    {
        kModeNone = 0,   // TFCam is not driving a free camera
        kModeFree = 1,   // normal TFCam free-fly (saved shots jump here and leave you flying)
        kModeOrbit = 2,  // auto orbit round the scene actors / posed actor / player
        kModeEye = 3,    // partner eye view (player SexLab scene only)
        kModePreset = 4, // v4: preset views round the player's SexLab scene (reported by v4 TFCam only)
    };

    // Persisted in TFCam.ini [SceneCam]. Values outside the ranges are clamped by TFCam.
    struct SceneCamSettings
    {
        float orbitRadius;  // game units from the target, 30..500 (default 120)
        float orbitHeight;  // elevation above the target in degrees, -20..75 (default 20)
        float orbitSpeed;   // degrees per second, -90..90, sign = direction (default 10)
        float orbitFOV;     // 20..120 (default 70)
        float eyeFOV;       // 30..130 (default 90)
    };

    // Snapshot for UIs. Caller sets size = sizeof(SceneCamStatus) before calling GetStatus.
    struct SceneCamStatus
    {
        std::uint32_t size;
        std::uint8_t mode;              // SceneCamMode
        bool inPlayerScene;             // the player is in a running SexLab scene
        bool stageKnown;                // sceneId / stageId resolved for that scene
        bool hasStageShot;              // a saved shot exists for the current scene + stage + player slot
        bool poseActive;                // SLUI reported a pose (SetActivePose) that is still current
        bool hasPoseShot;               // a saved shot exists for that pose
        bool eyeAvailable;              // the scene has at least one partner with a head to look from
        std::uint8_t reserved0;
        std::uint32_t eyeTarget;        // FormID of the actor whose eyes we look through, 0 = none
        std::uint32_t partnerCount;     // non-player actors in the player's scene
        std::int32_t playerSlot;        // player's position index in the scene, -1 unknown
        char sceneId[32];               // SLSB scene id (lower case), "" unknown
        char stageId[32];               // SLSB stage id (lower case), "" unknown
        char poseKey[96];               // "pose:<animevent lower case>", "" none
        SceneCamSettings settings;
    };

    // All ITFCam2 calls may come from any thread. TFCam copies the arguments and does every engine
    // touch on the main thread. A false return means the request was refused (the reason is logged
    // to TFCam.log and shown as a HUD notification when the user caused it).
    struct ITFCam2 : ITFCam1
    {
        // a_state: 0 = off, 1 = on, -1 = toggle. Turning one mode on turns the other off.
        // Enters TFCam free-fly first when the free camera is not active.
        virtual bool SetOrbit(std::int32_t a_state) noexcept = 0;
        virtual bool SetEyeView(std::int32_t a_state) noexcept = 0;  // player scene only
        virtual bool NextEyeTarget() noexcept = 0;                    // next partner, wraps

        // Applies live and saves to TFCam.ini (debounced).
        virtual void SetSettings(const SceneCamSettings& a_settings) noexcept = 0;

        // Context: in a player scene -> the current scene + stage shot; otherwise the active pose shot.
        virtual bool SaveShot() noexcept = 0;    // stores the current free camera (pos, yaw, pitch, roll, FOV)
        virtual bool RecallShot() noexcept = 0;  // blends the camera there (enters free-fly if needed)
        virtual bool DeleteShot() noexcept = 0;

        // SLUI tells TFCam which pose an actor is playing. a_actor = FormID; a_animEvent = the
        // pose's animation event (any case). a_actor == 0 or a_animEvent null/"" clears the pose.
        virtual void SetActivePose(std::uint32_t a_actor, const char* a_animEvent) noexcept = 0;

        // Thread-safe snapshot. Leaves *a_out untouched if a_out->size is smaller than expected.
        virtual void GetStatus(SceneCamStatus* a_out) noexcept = 0;
    };

    // ---- v3 ------------------------------------------------------------------------------------
    // Persisted in TFCam.ini [SceneCam]. Values outside the ranges are clamped by TFCam.
    struct SceneCamSettings3
    {
        std::uint32_t size;           // = sizeof(SceneCamSettings3); SetSettings3 ignores any other size
        float orbitRadius;            // game units from the target, 30..500 (default 120)
        float orbitTilt;              // elevation above the target in degrees, -20..75 (default 20; v2 orbitHeight)
        float orbitSpeed;             // degrees per second, -90..90, sign = direction (default 10)
        float orbitFOV;               // 20..120 (default 70)
        float eyeFOV;                 // 30..130 (default 90)
        float orbitAim;               // look-at height offset in game units, -60..60 (default 0): the orbit
                                      // circles and looks at the target this far up / down, keeping its tilt
        float orbitSway;              // elevation breathing amplitude in degrees, 0..15 (default 8; 0 = none)
        std::int32_t orbitTarget;     // 0 = auto (scene actors > posed actor > player), 1 = the player only
        std::int32_t autoEnterShots;  // 1 = a saved stage shot (or an NPC pose's shot) turns TFCam's free
                                      // camera on by itself (default 1)
    };

    // Snapshot for UIs. Caller sets size = sizeof(SceneCamStatus3) before calling GetStatus3;
    // TFCam fills settings.size itself.
    struct SceneCamStatus3
    {
        std::uint32_t size;
        std::int32_t mode;            // SceneCamMode (0 none, 1 free, 2 orbit, 3 eye)
        std::int32_t freeCamActive;   // 1 = TFCam drives a free camera right now (no FCFW timeline / SLCC owns it)
        std::int32_t inPlayerScene;   // the player is in a running SexLab scene
        std::int32_t stageKnown;      // sceneId / stageId resolved for that scene
        std::int32_t hasStageShot;    // a saved shot exists for the current scene + stage + player slot
        std::int32_t poseActive;      // SLUI reported a pose (SetActivePose) that is still current
        std::int32_t hasPoseShot;     // a saved shot exists for that pose
        std::int32_t eyeAvailable;    // the scene has at least one partner with a head to look from
        std::uint32_t eyeTarget;      // FormID of the actor whose eyes we look through, 0 = none
        std::int32_t partnerCount;    // non-player actors in the player's scene
        std::int32_t playerSlot;      // player's position index in the scene, -1 unknown
        std::int32_t sceneShotCount;  // saved stage shots of the current scene (all stages / slots)
        char sceneId[32];             // SLSB scene id (lower case), "" unknown
        char stageId[32];             // SLSB stage id (lower case), "" unknown
        char poseKey[96];             // "pose:<animevent lower case>", "" none
        SceneCamSettings3 settings;
    };

    // Same threading and return rules as ITFCam2.
    struct ITFCam3 : ITFCam2
    {
        // Applies live and saves to TFCam.ini (debounced).
        virtual void SetSettings3(const SceneCamSettings3& a_settings) noexcept = 0;
        // Thread-safe snapshot. false (and *a_out untouched) if a_out is null or a_out->size != sizeof(SceneCamStatus3).
        virtual bool GetStatus3(SceneCamStatus3* a_out) noexcept = 0;
        // Deletes every saved stage shot of the player's current scene (all stages and slots).
        virtual bool DeleteSceneShots() noexcept = 0;
    };

    // ---- v4 ------------------------------------------------------------------------------------
    // Preset view state for UIs. Caller sets size = sizeof(SceneCamPreset4) before calling GetPreset4.
    struct SceneCamPreset4
    {
        std::uint32_t size;
        std::int32_t view;      // the view shown: 0 = the stage's saved shot, 1..10 = built-in views, -1 = preset view off
        std::int32_t viewCount; // views to step through: 11 with a saved shot for this stage, else 10
        std::int32_t hasShot;   // the current stage has a saved shot (view 0)
        std::int32_t placing;   // 1 = "set cam pos" is on: free-fly, the middle mouse button saves the stage's shot
        char name[32];          // the view's name ("Saved shot", "Front", "Top", ...), "" when off
    };

    // Same threading and return rules as ITFCam2.
    struct ITFCam4 : ITFCam3
    {
        // a_state: 0 = off, 1 = on, -1 = toggle. Player SexLab scene only; enters free-fly first. It
        // starts on the stage's saved shot when there is one, else on the front view; a new stage
        // goes to its saved shot (or from a saved shot back to the front view).
        virtual bool SetPresetView(std::int32_t a_state) noexcept = 0;
        // a_step +1 / -1: the next / previous view (wraps). Preset view only. The mouse wheel does
        // the same while preset view is on and no menu is open.
        virtual bool StepPreset(std::int32_t a_step) noexcept = 0;
        // a_state: 1 = "set cam pos" (leaves orbit / eye / preset view for plain free-fly; the
        // middle mouse button then saves the camera as this stage's shot instead of a screenshot,
        // and preset view comes on showing it), 0 = cancel, -1 = toggle. Player SexLab scene only.
        virtual bool SetShotPlacing(std::int32_t a_state) noexcept = 0;
        // Thread-safe snapshot. false (and *a_out untouched) if a_out is null or a_out->size != sizeof(SceneCamPreset4).
        virtual bool GetPreset4(SceneCamPreset4* a_out) noexcept = 0;
    };

    // ---- v5 ------------------------------------------------------------------------------------
    // Game thread.
    struct ITFCam5 : ITFCam4
    {
        // Call every frame while your camera shows a subject: TFCam keeps the point (x, y, z, game units) sharp and
        // blurs what is behind it with ENB's depth of field. a_strength 0..1 (below 0 = TFCam's own Background blur
        // strength). a_on = false - or no call for half a second - gives ENB its own values back. Works for any camera
        // (the free camera or a menu camera); a TFCam camera with its own blur on is overridden while you call.
        virtual void SetBlurSubject(bool a_on, float a_x, float a_y, float a_z, float a_strength) noexcept = 0;
        // true when the blur can work: ENB with its depth of field shader (Marty McFly's ADOF) is there.
        virtual bool BlurAvailable() noexcept = 0;
        // Extra roll in radians for a camera you drive through the vanilla free camera (FreeCameraState), on top of
        // TFCam's own roll. Set 0 when your camera ends (TFCam also clears it when the free camera ends).
        virtual void SetCameraRoll(float a_radians) noexcept = 0;
    };

    using RequestFn = void* (*)(std::uint32_t a_version);
}
