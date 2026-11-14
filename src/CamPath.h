#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 0.18.0: Camera Path - film anything. Fly TFCam's free camera to a spot and add it as a camera (1, 2, 3 ... no
// limit). Every camera says how it looks and how the camera travels on to the next one; Play runs them in order.
//
// The pivot is what a camera is about: none until you give it one (10-29: no pivot for new cameras), then you (or
// your SexLab scene) or any object / NPC - picked by pointing the free camera at it, or in the console. Every camera
// has its own. "Follow" makes the cameras move and turn with their pivots (a walking NPC), otherwise the cameras stay
// where they were put; "around the pivot" circles where the pivot is now - one middle for the whole move (the
// leaving camera's pivot, else the next one's), so the distance blends smoothly. No pivot at either end: a curve.
//
// At each camera:   Look at the pivot / the way it was set / where it is going (the look blends between cameras),
//                   wait there N seconds, background blur on/off (+ strength).
// On to the next:   Straight line / smooth Curve through the cameras / Around the pivot (distance, height and angle
//                   round the pivot blend - a big circle at one camera and a small one at the next is a spiral) / Cut
//                   (jump there at once), extra full turns and which way round (orbits only, 10-31), seconds, slow
//                   start + stop, bend left / right and up / down (straight line and curve), zoom trick ("Dolly":
//                   the FOV changes so the pivot keeps its size; while waiting the camera can push in / pull back).
// Play from any camera, play backwards, rewind (backwards at 2x to camera 1), and a camera added while the path plays
// goes into the path at that moment.
// Effects: the pivot's point (head, eyes, chest, hips, bum, groin, hands, feet of an actor), camera shake (a bumpy
// cart), crash zoom (the zoom to the next camera in a fast burst at the end), slow motion on part of a move (the world
// slows too if asked; bullet time = only the world slows), elastic arrival (overshoot and spring back), slingshot (pull
// back, then shoot off). Clean screen hides all game UI for recording; the window steps aside while the path plays.
// Motion smoothness takes the edge off sudden turns and speed changes. Record manual path: fly by hand and the
// flight becomes cameras.
//
// Paths live in Data\SKSE\Plugins\TFCam\Paths\<name>.tsv; the one being edited is "Current" (kept over restarts).
//
// Main thread, except the menu / window entry points (any thread: under a lock, engine work queued as SKSE tasks).
namespace CamPath {

    enum Look : int { kLookPivot = 0, kLookSet = 1, kLookAhead = 2 };
    enum Move : int { kMoveStraight = 0, kMoveCurve = 1, kMoveAround = 2, kMoveCut = 3 };
    enum Way : int { kWayShortest = 0, kWayLeft = 1, kWayRight = 2 };
    enum Bone : int {
        kBoneMiddle = 0,
        kBoneHead,
        kBoneEyes,
        kBoneChest,
        kBoneHips,
        kBoneBum,
        kBoneGroin,
        kBoneHands,
        kBoneFeet,
        kBoneCount
    };
    const char* BoneName(int a_bone);

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
        // this camera's pivot: -1 = none, 0 = you / your scene, 1 = a reference, 2 = (11-14 round 13) a position in your SexLab
        // scene (pivotRef = its index in the scene's positions: whoever plays it this time)
        int          pivotKind = -1;
        RE::FormID   pivotRef  = 0;
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
        bool  zoomTrick = false;  // "Dolly" in the editor: the zoom keeps the pivot's size while the camera moves
        float dolly     = 0.0f;   // -1..1 (zoom trick on): while waiting here the camera pulls back (-) / pushes in (+)
        float bendSide  = 0.0f;  // -1..1: bends left / right (1 = the middle sits half the distance off the line)
        float bendUp    = 0.0f;  // -1..1: bends down / up
        int   bone      = kBoneMiddle;  // the pivot's point (actors)
        float shake     = 0.0f;   // 0..1: camera shake here, blending to the next camera's on the way
        bool  crashZoom = false;  // the zoom (field of view) to the next camera comes in a fast burst at the end
        bool  slowMo     = false;  // slow motion on part of the way on
        float slowAt     = 0.66f;  // where: 0..1 of the way
        float slowSpeed  = 0.2f;   // how slow: 0.05..1 of normal speed
        float slowLength = 0.3f;   // how much of the way: 0.05..1
        bool  slowWorld  = false;  // the world slows down with it
        bool  slowCamera = true;   // false = bullet time: only the world slows, the camera keeps its pace
        float elastic    = 0.0f;   // 0..1: the camera flies past the next camera and springs back (bounciness)
        float sling      = 0.0f;   // 0..1: before leaving, the camera pulls back the other way, then shoots off
        // speed curve of the move (10-29): off = automatic (slow start / stop as set); on = this curve - time across,
        // progress up, a CSS-style cubic bezier from (0,0) to (1,1) through two handles (x 0..1, y 0..1)
        bool  curveOn = false;
        float curveX1 = 0.42f;
        float curveY1 = 0.0f;
        float curveX2 = 0.58f;
        float curveY2 = 1.0f;
        // 11-13 (user: "the saved position was in another cell and it tried to go there - the screen went black"): where it
        // was made - "I:<plugin>|<local id>" (an interior cell) or "W:<plugin>|<local id>" (a worldspace); empty = an older
        // camera (unknown)
        std::string space;
    };

    struct Settings {
        int   openKey    = 0;  // DX scan codes, 0 = unset
        int   addKey     = 0;
        int   playKey    = 0;
        int   recordKey  = 0;
        float smoothness = 0.3f;  // 0..1
        float aimHeight  = 0.0f;  // -60..60 units above the pivot's middle
        bool  showPath   = true;  // the path drawn in the world while the free camera is on (CamPathView)
        bool  prismaEditor = true;  // the window key opens the PrismaUI editor when PrismaUI is installed (else the SMF window)
        // 11-14 round 13: a SexLab stage's saved scene path plays by itself when that stage starts (looping until it changes)
        bool  sceneAutoPlay = true;
    };

    Settings& Get();
    void      Clamp();
    void      ToggleEditor();  // any thread: the PrismaUI editor when there is one and it is chosen, else the SMF window

    // free camera Update hook (after the other drives)
    void Drive(RE::FreeCameraState* a_fcs, bool a_driving);
    bool OwnsMotion();  // playing (paused included)
    bool Playing();
    void Stop(std::string_view a_why);
    // 11-14 round 14: the editor was closed by the user - a playing or paused path ends (not a SexLab scene path). Main thread
    void StopForEditorClose(std::string_view a_how);
    bool OnHotkey(std::uint32_t a_code);  // keyboard key-down, no menu open; true = a camera path key
    void OnFreeCamEnd();
    void OnGameLoad();

    // CamBlur: while a path plays the path decides the blur itself (through CamBlur::External).
    bool FocusPoint(RE::NiPoint3& a_out, const char*& a_what, bool& a_on);

    // ---- the window / menu (any thread) -------------------------------------------------------------------------
    struct Snapshot {
        std::vector<Cam> cams;
        std::string      name;
        bool             follow       = false;
        bool             loop         = false;
        std::vector<std::string> camPivotNames;  // per camera
        bool             playing      = false;
        bool             paused       = false;
        bool             rewinding    = false;  // going backwards (rate < 0), not paused
        float            rate         = 1.0f;   // 1 forwards, -1 playing backwards, -2 rewinding
        bool             cleanScreen  = false;
        bool             recording    = false;
        bool             freeCam      = false;  // TFCam drives a free camera (cameras can be added)
        bool             frozen       = false;
        int              atCam        = -1;     // the camera the path is at / leaving
        float            progress     = 0.0f;   // 0..1 of the whole path
        float            total        = 0.0f;   // seconds of the whole path (one lap), as the cameras are now
        std::string      lastEvent;
        std::vector<std::string> savedPaths;
        // 11-14 round 13: the player's SexLab scene (scene paths)
        bool             sceneIn      = false;  // a stage of the player's scene is known
        std::string      sceneLabel;            // its name and stage
        bool             sceneSaved   = false;  // this stage has a saved scene path
        bool             scenePlaying = false;  // the playing path is a scene path
    };
    Snapshot GetSnapshot();
    // Seconds the move on from a camera takes, slow motion (it adds time), slingshot and elastic arrival included.
    float MoveSeconds(const Cam& a_c);
    // Edits a camera's options (not its position) under the lock; false if a_index is gone.
    bool EditCam(int a_index, const Cam& a_options);
    void SetFollow(bool a_on);
    void SetLoop(bool a_on);

    // queued to the main thread
    void RequestAddCam();          // at the free camera, after the last one (while a path plays: into it, at that moment)
    void RequestSetCamHere(int a_index);
    void RequestGoToCam(int a_index);
    void RequestDeleteCam(int a_index);
    void RequestClear();
    void RequestPlay(int a_state);  // 1 play (from the start, or go on after a pause), 0 stop, -1 play / pause
    void RequestPlayFrom(int a_index);
    void RequestRewind();           // run the path backwards to camera 1 (from the end when it is not playing)
    void RequestPlayBackwards();    // the same at normal speed
    void RequestPause();
    // The path slider: the camera goes to a_seconds into the path and waits there, paused (Play goes on from it). The
    // path starts paused when it is not playing, or again when it was edited since Play (the slider shows the edit).
    void RequestSeek(float a_seconds);
    // Paused during camera a_cam's move: that move slows down from this point (how long / how slow are edited after).
    void RequestSlowHere(int a_cam);
    // Camera a_cam's pivot.
    void RequestPickPivot(int a_cam);     // what the free camera points at
    void RequestPivotConsole(int a_cam);  // the console's selected reference
    void RequestPivotMe(int a_cam);
    void RequestPivotNone(int a_cam);
    // Clean screen: all game UI hidden (the console's tm - SMF HUD elements follow it); off when the free camera ends.
    void SetCleanScreen(bool a_on);
    bool CleanScreen();
    void RequestFreezeWorld();
    void RequestRecord(int a_state);  // 1 start, 0 stop, -1 toggle
    void RequestSaveAs(std::string a_name);
    void RequestLoad(std::string a_name);

    // 11-14 round 13 (user: "sexlab can save paths as bone relative so the same scene played elsewhere can play the saved path
    // linked to that animation" - normal paths stay world spots): SexLab scene paths. One per stage of an animation and your
    // position in it, in Data\SKSE\Plugins\TFCam\ScenePaths, the cameras kept against the scene's own spot (your character's
    // placement in it, as the Scene Camera's stage shots) and the partners as scene positions, so it plays the same wherever
    // that stage plays. It loops until the stage changes (the last camera's move settings take it back to camera 1).
    void RefreshScene();           // main thread: the scene the editor shows (the editor's tick and Drive call it)
    void RequestSceneSave();       // the editor's path becomes this stage's scene path
    void RequestSceneOpen();       // this stage's scene path into the editor (around where the scene is now)
    void RequestSceneDelete();
    void RequestScenePlay();       // play this stage's scene path now (looping)
}
