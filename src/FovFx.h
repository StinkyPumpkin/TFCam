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
        // archer camera: while the string is drawn the third-person camera comes in close round your head (a side-on
        // shot that shows your face, the drawn bow and where the arrow flies - user's Horizon reference, 10-29). It
        // looks at where the arrow will land, so the middle of the screen is the aim point. Defaults = the user's own.
        bool  aimCam  = true;
        float aimTurn = 22.0f;  // degrees round your head from straight behind (90 = side-on), to your right
        float aimBack = 61.0f;  // units from the eyes
        float aimUp   = -1.0f;  // units above the eyes
        bool  aimDot      = true;   // 10-31: a tiny dot where the arrow will land (the game's crosshair is not needed)
        float aimDotSize  = 1.2f;   // its radius in pixels at 1080p (user 10-31: "much smaller" than 3)
        bool  aimBlur     = false;  // 10-31: the Background blur's edge blur while the string is drawn
        float aimVignette = 0.0f;   // 10-31: darker corners while the string is drawn, 0..1
    };

    float ArcherVignette();  // 0..1 this frame (aimVignette faded with the archer view). Main thread.

    // 10-31: the archer view for the image pass (the dot): true while the string is drawn and the short hold after;
    // a_aimAt = where the arrow will land, a_in = how far in the archer view is (0..1). Main thread.
    bool ArcherAim(RE::NiPoint3& a_aimAt, float& a_in);
    bool HitWhereYouAim();  // that mod is loaded: the arrow goes through the middle of the screen whatever the view

    Settings& Get();
    void      Clamp();
    void      Install();  // kDataLoaded: SKSE action events
    void      Tick();     // every frame, main thread (FixedCam's camera hook)
    void      Reset();    // main thread: our offset off at once (before the free camera reads the FOV)
    // The archer camera, in FixedCam's camera hook after the game has placed the camera (not while fixed / in the free
    // camera / in first person). Main thread.
    void ApplyAimCam();
}
