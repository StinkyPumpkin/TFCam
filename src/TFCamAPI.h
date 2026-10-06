#pragma once

#include <cstdint>

// --Claude: TFCam cross-plugin API (E:\dev\FreeCamClaude, TFCam.dll).
//
// v1: lets a photo tool (PEM / Whore Horde PhotoStudio) take over TFCam's screenshot key while
//     the player flies the camera for a photo.
// v2: Scene Camera control for SLUI (SexLab UI): orbit, partner eye view, saved shots per
//     SexLab scene stage and per pose. ITFCam2 extends ITFCam1, so a v2 pointer is also a v1.
//
//   auto* dll = GetModuleHandleW(L"TFCam.dll");
//   auto  fn  = dll ? reinterpret_cast<TFCAM_API::RequestFn>(GetProcAddress(dll, "RequestTFCamAPI")) : nullptr;
//   auto* api = fn ? static_cast<TFCAM_API::ITFCam2*>(fn(TFCAM_API::kVersion2)) : nullptr;
//
// Older TFCam builds have no export or return nullptr for an unknown version. Plain types only.
// This header is copied verbatim into consumers (SLUI include/TFCamAPI.h): change both together.
namespace TFCAM_API
{
    inline constexpr std::uint32_t kVersion1 = 1;
    inline constexpr std::uint32_t kVersion2 = 2;

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

    using RequestFn = void* (*)(std::uint32_t a_version);
}
