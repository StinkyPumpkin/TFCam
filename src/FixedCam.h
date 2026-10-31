#pragma once

// 0.18.0: the camera-root hook - PlayerCamera::Update's call that updates the camera root (the site Cinematic Mode's
// freezecam uses, vinymayan, github.com/vinymayan/Cinematic-Mode), every frame and every camera state, after the game has
// placed the camera. Action FOV's frame tick and the archer camera ride it.
namespace FixedCam {
    void Install();  // kDataLoaded (SE / AE only)
}
