#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 0.18.0: Camera Path - film anything. Fly TFCam's free camera to a spot and add it as a camera (1, 2, 3 ... no
// limit). Every camera says how it looks and how the camera travels on to the next one; Play runs them in order.
//
// The pivot is what the path is about: you (or your SexLab scene) by default, or any object / NPC - picked by
// pointing the free camera at it, or in the console. "Follow the pivot" makes the whole path move and turn with it
// (a walking NPC), otherwise the cameras stay where they were put.
//
// At each camera:   Look at the pivot / the way it was set / where it is going (the look blends between cameras),
//                   wait there N seconds, background blur on/off (+ strength).
// On to the next:   Straight line / smooth Curve through the cameras / Around the pivot (distance, height and angle
//                   round the pivot blend - a big circle at one camera and a small one at the next is a spiral), extra
//                   full turns (round the pivot, or turning in place when both cameras keep their own view), which
//                   way round, seconds, slow start + stop, zoom trick (dolly zoom: the FOV changes so the pivot keeps
//                   its size).
// Motion smoothness takes the edge off sudden turns and speed changes. Record manual path: fly by hand and the
// flight becomes cameras.
//
// Paths live in Data\SKSE\Plugins\TFCam\Paths\<name>.tsv; the one being edited is "Current" (kept over restarts).
//
// Main thread, except the menu / window entry points (any thread: under a lock, engine work queued as SKSE tasks).
namespace CamPath {

    enum Look : int { kLookPivot = 0, kLookSet = 1, kLookAhead = 2 };
    enum Move : int { kMoveStraight = 0, kMoveCurve = 1, kMoveAround = 2 };
    enum Way : int { kWayShortest = 0, kWayLeft = 1, kWayRight = 2 };

    struct Cam {
        // where the camera was put (world) and the pivot at that moment
        RE::NiPoint3 pos{};
        float        yaw   = 0.0f;
        float        pitch = 0.0f;
        float        roll  = 0.0f;
        float        fov   = 70.0f;
        RE::NiPoint3 pivotPos{};
        float        pivotHeading = 0.0f;
        bool         havePivot    = false;
        // at this camera
        int   look         = kLookPivot;
        float wait         = 0.0f;  // seconds
        bool  blur         = false;
        float blurStrength = 0.6f;
        // on the way to the next camera
        int   move      = kMoveCurve;
        float time      = 3.0f;  // seconds
        bool  smooth    = true;  // slow start and stop
        int   turns     = 0;     // extra full turns
        int   way       = kWayShortest;
        bool  zoomTrick = false;
    };

    struct Settings {
        int   openKey    = 0;  // DX scan codes, 0 = unset
        int   addKey     = 0;
        int   playKey    = 0;
        int   recordKey  = 0;
        float smoothness = 0.3f;  // 0..1
        float aimHeight  = 0.0f;  // -60..60 units above the pivot's middle
    };

    Settings& Get();
    void      Clamp();

    // free camera Update hook (after the other drives)
    void Drive(RE::FreeCameraState* a_fcs, bool a_driving);
    bool OwnsMotion();  // playing (paused included)
    bool Playing();
    void Stop(std::string_view a_why);
    bool OnHotkey(std::uint32_t a_code);  // keyboard key-down, no menu open; true = a camera path key
    void OnFreeCamEnd();
    void OnGameLoad();

    // CamBlur: while a path plays the path decides the blur itself (through CamBlur::External).
    bool FocusPoint(RE::NiPoint3& a_out, const char*& a_what, bool& a_on);

    // ---- the window / menu (any thread) -------------------------------------------------------------------------
    struct Snapshot {
        std::vector<Cam> cams;
        std::string      name;
        std::string      pivotName;
        bool             pivotIsMe    = true;
        bool             follow       = false;
        bool             loop         = false;
        bool             playing      = false;
        bool             paused       = false;
        bool             recording    = false;
        bool             freeCam      = false;  // TFCam drives a free camera (cameras can be added)
        bool             frozen       = false;
        int              atCam        = -1;     // the camera the path is at / leaving
        float            progress     = 0.0f;   // 0..1 of the whole path
        std::string      lastEvent;
        std::vector<std::string> savedPaths;
    };
    Snapshot GetSnapshot();
    // Edits a camera's options (not its position) under the lock; false if a_index is gone.
    bool EditCam(int a_index, const Cam& a_options);
    void SetFollow(bool a_on);
    void SetLoop(bool a_on);

    // queued to the main thread
    void RequestAddCam();          // at the free camera, after the last one
    void RequestSetCamHere(int a_index);
    void RequestGoToCam(int a_index);
    void RequestDeleteCam(int a_index);
    void RequestClear();
    void RequestPlay(int a_state);  // 1 play (from the start, or go on after a pause), 0 stop, -1 play / pause
    void RequestPause();
    void RequestPickPivot();        // what the free camera points at
    void RequestPivotConsole();     // the console's selected reference
    void RequestPivotMe();
    void RequestFreezeWorld();
    void RequestRecord(int a_state);  // 1 start, 0 stop, -1 toggle
    void RequestSaveAs(std::string a_name);
    void RequestLoad(std::string a_name);
}
