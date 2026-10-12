#pragma once

#include <string>

// 0.12.0: walls for TFCam's automatic cameras - the idle camera (spiral / dolly / blend back), the Scene Camera orbit
// and the blends to a saved shot (scene stage, pose, furniture). The driver that moved the camera this frame names the
// point it looks at (Request); after every driver ran, Apply casts rays from that point to the camera and pulls the
// camera in front of anything solid in between, then eases it back out once the way is clear. Characters are passed
// through. Manual free-fly and the eye view are never touched.
//
// Main thread (the free camera's Update hook).
namespace CamCollision {

    // TFCam.ini [Collision].
    struct Settings {
        bool  enabled   = true;
        float margin    = 16.0f;  // units kept between the camera and a wall; 4..60
        float returnSec = 0.8f;   // seconds to ease back out (about 95% of the way) once clear; 0.1..3
        int   layer     = 36;     // collision layer of the probe rays (36 = camera sphere, 41 = line of sight); ini only
    };

    Settings& Get();
    void      Clamp();

    void Request(const RE::NiPoint3& a_pivot, const char* a_who);  // a driver wrote the camera this frame
    void Apply(RE::FreeCameraState* a_fcs, bool a_driving);       // Update hook, after every driver

    std::string MenuStatus();  // any thread
}
