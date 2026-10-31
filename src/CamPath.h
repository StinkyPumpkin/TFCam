#pragma once

#include <cstdint>
#include <string_view>

// 0.17.0: camera path. Fly TFCam's free camera to a spot and add it as a point (A, B, C ...); Play moves the camera
// through the points:
//   curved   - one smooth curve through all the points (centripetal Catmull-Rom) at an even speed, eased at the ends
//   straight - straight lines from point to point, easing in and out at each one (a dolly / slider move)
//   cut      - (snap) jumps from point to point, each held for an equal share of the time
// Look: at the focus (default: you, or your SexLab scene's actors; or the reference picked in the console - a moving NPC
// is followed live), or the way each point looked when it was added (unfocused: the camera keeps its own aim, so a
// straight path pans / dollies into the scene). Roll and FOV come from the points either way.
// "Path moves with the focus": the points are kept relative to the focus (offset and heading at the time each was
// added), so the same path follows a walking NPC or works again for the same pose elsewhere.
// The points live in Data\SKSE\Plugins\TFCam\CameraPath.tsv (kept over restarts).
//
// Main thread, except the Request* menu entry points (any thread, queued as SKSE tasks).
namespace CamPath {

    struct Settings {
        bool  curved      = true;   // false = straight lines
        bool  snap        = false;  // cut from point to point
        float duration    = 12.0f;  // seconds for the whole path, 1..300
        int   loop        = 0;      // 0 = once, 1 = loop, 2 = back and forth
        int   look        = 0;      // 0 = at the focus, 1 = the way each point looked
        int   focus       = 0;      // 0 = you (your SexLab scene's actors in one), 1 = the console's selected reference
        float focusHeight = 0.0f;   // -60..60 units up / down from the focus point
        bool  followFocus = false;  // the path moves (and turns) with the focus
        int   addKey      = 0;      // DX scan codes, 0 = unset
        int   playKey     = 0;
    };

    Settings& Get();
    void      Clamp();

    void Drive(RE::FreeCameraState* a_fcs, bool a_driving);  // free camera Update hook, after the other drives
    bool OwnsMotion();                                       // a path plays (its blend to the first point included)
    bool Playing();
    void Stop(std::string_view a_why);
    bool OnHotkey(std::uint32_t a_code);  // keyboard key-down, no menu open; true = a camera path key, consume it
    void OnFreeCamEnd();
    void OnGameLoad();

    // CamBlur: the point to keep sharp while a path plays. a_on = a path plays at all; false with a_on = it looks the
    // way its points looked (no subject: screen-centre autofocus).
    bool FocusPoint(RE::NiPoint3& a_out, const char*& a_what, bool& a_on);

    // SKSE Menu Framework page (any thread)
    void RequestAddPoint();
    void RequestRemoveLast();
    void RequestClear();
    void RequestPlay(int a_state);  // 1 = play, 0 = stop, -1 = toggle

    struct MenuStatus {
        int   points        = 0;
        bool  playing       = false;
        bool  freeCam       = false;  // TFCam drives a free camera (points can be added)
        float progress      = 0.0f;   // 0..1 of the current pass
        float length        = 0.0f;   // game units along the path
        char  focus[96]     = {};
        char  lastEvent[160] = {};
    };
    MenuStatus GetMenuStatus();
}
