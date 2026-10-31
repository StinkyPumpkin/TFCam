#pragma once

#include <cstdint>

// 0.16.0: Conversation camera. While a dialogue is open (the Dialogue Menu, a talking NPC), TFCam's free camera (the vanilla
// FreeCameraState) is turned on by itself and driven from its Update hook, after the idle camera, like a film conversation:
//   NPC speaking   - over the player's shoulder, looking at the NPC (NPC off to one side of the frame)
//   player's turn  - the reverse shot over the NPC's shoulder, or a wide two-shot from the side (reverseKind)
// Every move is smooth: the camera is kept in cylindrical coordinates (angle, radius, height) round the midpoint of the two
// speakers and each is damped (critically damped spring with a speed limit), so a change of shot swings round the pair instead
// of cutting through them or racing across the room. A shot stays at least minShotSec, so the camera never flip-flops. When the
// dialogue ends the camera blends back to where the game's own camera was, then gives the free camera back.
//
// Walls: the camera asks CamCollision for the look point it frames each frame. Main thread (the free camera's Update hook).
namespace ConvoCam {

    // TFCam.ini [Conversation].
    struct Settings {
        bool  enabled        = false;
        bool  thirdPersonOnly = true;   // first person keeps the game's own dialogue view
        float blendIn        = 1.6f;    // seconds to settle on the first shot
        float blendOut       = 1.4f;    // seconds back to the game's camera
        float shotSwitch     = 1.5f;    // seconds a change of shot takes to settle
        float minShotSec     = 2.5f;    // a shot stays at least this long
        float distance       = 115.0f;  // camera behind the foreground speaker (units)
        float shoulder       = 36.0f;   // camera to the side of the foreground speaker's shoulder (units)
        float height         = 10.0f;   // camera above the foreground speaker's eyes (units)
        float lookShift      = 24.0f;   // the framed speaker sits this far off the centre of the frame (units at the speaker)
        float fovNpc         = 46.0f;   // over-the-shoulder lens
        float fovWide        = 56.0f;   // two-shot lens
        bool  alternate      = true;    // reverse shot while it is the player's turn (off: always over the player's shoulder)
        int   reverseKind    = 0;       // 0 = over the NPC's shoulder, 1 = wide two-shot from the side
        bool  swapSide       = false;   // the other shoulder
        float drift          = 1.0f;    // slow handheld sway, 0 = still; 0..3
        bool  noEdgePan      = false;   // the game's mouse edge-pan in dialogue (fPCDialogueLookSpeed) off in every dialogue; always off while the camera films
    };

    Settings& Get();
    void      Clamp();

    void Install();  // kDataLoaded

    // FreeCamController (main thread)
    void Drive(RE::FreeCameraState* a_fcs, bool a_driving);  // Update hook, after AmbientCam::Drive
    bool OwnsMotion();                                       // the conversation camera moves the camera (menu restore etc. stand down)
    void OnGameLoad(const char* a_why);

    // The Activate button (ActivateHandler::ProcessButton, main thread). The game acts on a TAP when the button is RELEASED
    // (HeldStateHandler), so the free camera must not begin on the press: it would be on by the time the release arrives and
    // TFCam would swallow it - the dialogue never opened and the handler's held-button state stayed stuck (2026-10-25 log).
    //   NotePress()     - button went down: remember the NPC under the crosshair if it is one to film (nothing else happens).
    //   PendingRelease() - true when the release that follows should be let through and then start the camera.
    //   BeginPending()  - call right after the game has handled that release.
    void NotePress(RE::ButtonEvent* a_event);
    bool PendingRelease(RE::ButtonEvent* a_event);
    void BeginPending();

    struct MenuStatus {
        char state[96]      = {};
        char shot[48]       = {};
        char speaker[96]    = {};
        char lastEvent[192] = {};
    };
    MenuStatus GetMenuStatus();
}
