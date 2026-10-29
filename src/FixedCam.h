#pragma once

#include <string_view>

// 0.18.0: Fixed camera - the camera stays where you put it while you play on. Compose the shot in TFCam's free
// camera, press "Fix camera here": the free camera ends and the game's own third-person camera takes over again, so
// every control works as normal (walking, attacks, menus) - only what that camera SHOWS is replaced: the camera root
// gets the fixed view right after the game has placed it (PlayerCamera::Update's camera-root update call). "Turn to
// watch me" keeps the position and turns the view to follow you.
// The same technique as Cinematic Mode's freezecam (vinymayan, github.com/vinymayan/Cinematic-Mode). The camera
// state's own GetRotation / GetTranslation do NOT work for this: the third-person camera ignores them (tested 10-29).
namespace FixedCam {
    void Install();  // kDataLoaded (SE / AE only)
    bool IsOn();
    bool Watching();
    void SetWatch(bool a_on);              // any thread
    void RequestFixHere();                 // any thread: keep the free camera's view, leave the free camera
    void RequestRelease(std::string_view a_why);  // any thread
    void OnFreeCamBegin();                 // main thread: the free camera takes over (and starts from the fixed view)
    void OnGameLoad();
}
