#pragma once

// 0.18.0: Action FOV - the field of view reacts to what you do, to sell the hit: drawing a bow pulls it in (tension),
// letting the arrow go snaps it out, a power attack punches in, a big spell or a shout warps it out, and moving fast
// (sprint, Whirlwind Sprint - or the free camera) stretches it wide. An offset on top of whatever FOV is in use (yours,
// another mod's, TFCam's): it goes on the render camera's view, never into the game's FOV value (Camera Studio holds
// that one). Off by default (FreeCam > Action FOV).
namespace FovFx {
    struct Settings {
        bool  enabled     = false;
        float strength    = 1.0f;   // 0..2
        bool  bow         = true;   // draw pulls in, release snaps out
        bool  power       = true;   // power attacks punch in
        bool  spells      = true;   // spells from spellLevel up warp out
        int   spellLevel  = 75;     // minimum skill level: 0 Novice, 25 Apprentice, 50 Adept, 75 Expert, 100 Master
        bool  shouts      = true;   // shouts warp out
        bool  speedPlayer = true;   // your speed stretches it (sprint, Whirlwind Sprint)
        bool  speedCamera = false;  // the free camera's speed stretches it (flying, camera paths)
        // archer camera: while the string is drawn the third-person camera comes in close behind your head, eye high
        bool  aimCam  = true;
        float aimBack = 25.0f;  // units behind the eyes
        float aimSide = 20.0f;  // units to the right (the aim point clear of hair / a helmet)
        float aimUp   = 5.0f;   // units above the eyes
    };

    Settings& Get();
    void      Clamp();
    void      Install();  // kDataLoaded: SKSE action events
    void      Tick();     // every frame, main thread (FixedCam's camera hook)
    void      Reset();    // main thread: our offset off at once (before the free camera reads the FOV)
    // The archer camera, in FixedCam's camera hook after the game has placed the camera (not while fixed / in the free
    // camera / in first person). Main thread.
    void ApplyAimCam();
}
