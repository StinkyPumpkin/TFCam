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
        float approachSec = 0.3f; // 0.15.1: seconds to ease in towards a wall (about 95% of the way); 0 = at once; 0..2
        bool  passLarge   = false; // 0.15.4: furniture and other big props (bounding sphere from ignoreSmall up to largeItem) are not walls
        bool  passWalls   = false; // 0.15.4: the walls of buildings and rooms are not walls (floors, ceilings and terrain still are)
        float largeItem   = 200.0f; // 0.15.4: bounding sphere radius that separates a large item from architecture; ini only; 60..600
        float ignoreSmall = 50.0f; // 0.15.3: objects whose bounding sphere is smaller than this (sconces, things on a table) are not walls; 0 = all count; 0..200
        int   layer     = 36;     // collision layer of the probe rays (36 = camera sphere, 41 = line of sight); ini only
    };

    Settings& Get();
    void      Clamp();

    // a driver wrote the camera this frame. a_ground (11-17): also keep the camera above the real ground under it (a probe
    // straight down), not just out of walls between the look point and the camera - for the scene orbit / preset on uneven ground.
    // a_realTime (11-22): the driver moves on real time under a menu that pauses the game (the conversation camera's trade
    // shot), so the push off the wall eases on real time too instead of freezing where it was when the menu opened.
    void Request(const RE::NiPoint3& a_pivot, const char* a_who, bool a_ground = false, bool a_realTime = false);
    void Apply(RE::FreeCameraState* a_fcs, bool a_driving);       // Update hook, after every driver
    void SnapNext();   // 11-22: a cut - the next Apply sets the push off the wall at once instead of easing from the old spot's

    // 10-29: the same walls for a camera that is not the free camera (the archer camera): where a camera wanted at a_cam,
    // seen from a_pivot, may be - in front of anything solid in between, characters and small props passed. a_cam itself
    // when the way is clear or walls are off. Main thread.
    RE::NiPoint3 ClearSpot(const RE::NiPoint3& a_pivot, const RE::NiPoint3& a_cam, const char* a_who = "archer camera");

    std::string MenuStatus();  // any thread
}
