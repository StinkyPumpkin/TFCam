#pragma once

#include <cstdint>
#include <string>

// 0.11.0: Idle (vanity) camera + Furniture camera. Both drive TFCam's free camera (the vanilla FreeCameraState) from
// its Update hook, after the Scene Camera, and give the camera back with a short blend.
//
// Idle camera: after N seconds without input (and nothing going on: no menu, combat, SexLab scene, cell-entry move or
// free camera), the camera cycles through routines round the player until any input:
//   Dolly zoom - the camera dollies in/out while the FOV zooms the opposite way, so the subject keeps its size and the
//                background stretches (distance = framing / (2 tan(fov / 2))). 0.11.1: from low near the ground,
//                tilted up at the face, circling.
//   Spiral     - 0.11.1: from the floor up to the face in two loops, settling on the bum (from behind), the genitals,
//                the breasts and the face (from the front).
// After a set duration it hands over to po3's Sandbox When Idle: that mod hooks the engine's auto-vanity start, whose
// idle countdown is PlayerCamera idleTimer; TFCam sets it so the sandbox starts the moment the camera is given back.
//
// Furniture camera: a camera shot saved per furniture (base form), in the player's frame - sit, turn on free-fly, fly to
// the spot, press the Scene Camera "Save camera shot" key. Sitting on that furniture later blends the camera there.
// The shots live in SceneShots.tsv under the key "furn:<plugin>|<local id>".
//
// Main thread, except where noted.
namespace AmbientCam {

    struct Settings {
        // ---- idle camera ----
        bool  vanityEnabled   = true;
        float vanityStart     = 25.0f;   // seconds without input before it starts
        float vanityDuration  = 75.0f;   // seconds it runs; 0 = until input
        bool  handOffSandbox  = true;    // when the duration ends, let Sandbox When Idle start
        bool  dolly           = true;
        bool  spiral          = true;
        float crossfade       = 1.5f;    // seconds between routines
        float returnTime      = 0.7f;    // seconds back to the normal camera
        float interiorScale   = 0.6f;    // radii / distances in interior cells
        // dolly zoom
        float dollyFraming    = 110.0f;  // game units visible (vertically) at the subject
        float dollyNarrowFOV  = 28.0f;
        float dollyWideFOV    = 85.0f;
        float dollyPeriod     = 9.0f;    // seconds for one in-and-out
        int   dollyCycles     = 2;       // in-and-outs before the next routine
        float dollyCamHeight  = 25.0f;   // camera height above the ground; it looks up at the face
        float dollySpin       = 10.0f;   // degrees per second round the player (negative = other way)
        // spiral
        float spiralFar       = 170.0f;
        float spiralClose     = 55.0f;
        float spiralFOV       = 65.0f;
        float spiralAreaSec   = 3.2f;    // seconds per focus area (the move there and the settle)
        float spiralSpeed     = 40.0f;   // only the sign is used since 0.11.1: + / - = the two directions round the body
        // ---- furniture camera ----
        bool  furnEnabled     = true;
        float furnDelay       = 1.2f;    // seconds after sitting down before the camera moves
        bool  furnMoveCancels = true;    // movement / jump / activate keys give the camera back
    };

    Settings& Get();
    void      Clamp();

    void Install();  // kDataLoaded

    // FreeCamController (main thread)
    void Drive(RE::FreeCameraState* a_fcs, bool a_driving);  // Update hook, after SceneCam::Drive
    bool OwnsMotion();                                       // a routine / blend moves the camera
    void OnInput(RE::InputEvent* const* a_events);           // input sink, every batch
    void OnFreeCamEnd();                                     // before the vanilla End
    void OnGameLoad(const char* a_why);
    void OnPlayerSceneStart();

    void TestVanityOnMenuClose();  // menu button: start the idle camera once the menu is closed

    // SceneCam: the furniture the player occupies, as a shot key. False when not on furniture.
    bool CurrentFurniture(std::string& a_key, std::string& a_label);

    struct MenuStatus {
        char state[96]     = {};
        char routine[48]   = {};
        char furniture[160] = {};
        bool furnHasShot   = false;
        bool swiLoaded     = false;
        char timer[96]     = {};
        char lastEvent[192] = {};
    };
    MenuStatus GetMenuStatus();
}
