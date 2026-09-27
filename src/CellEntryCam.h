#pragma once

// 0.9.0: cell-entry camera. After a loading screen that puts the player in a new cell (a load door,
// fast travel), the third-person camera zooms to a set distance, then orbits around the player until
// it faces them - by default 180 degrees, ending in front of the player looking back at them. It
// frees the camera that load doors leave wedged inside the player's head, facing forward.
//
// Any mouse movement (or mouse wheel / right stick) cancels it at once and hands the camera back
// where it is; keys and buttons do too unless that option is off. Settings live in TFCam.ini
// [CellEntry] and on the SKSE Menu Framework page FreeCam > Cell Entry.
//
// Driven from a ThirdPersonState::Update vtable hook (plain third person only - horse, bleedout,
// furniture and dragon cameras have their own vtables), written before the original runs so the
// vanilla camera and SmoothCam both build this frame from the new yaw and zoom.
namespace CellEntryCam {

    struct Settings {
        bool  enabled       = true;
        bool  onInteriors   = true;    // arriving in an interior through a loading screen
        bool  onExteriors   = true;    // arriving outdoors (door out, fast travel)
        bool  afterSaveLoad = false;   // also when a save finishes loading
        bool  skipInCombat  = true;
        bool  changeZoom    = true;
        float zoom          = 0.4f;    // the game's own third-person zoom value: -0.2 closest .. 1.0 farthest
        float zoomSpeed     = 1.0f;    // zoom units per second
        float rotation      = 180.0f;  // degrees orbited around the player; 180 = ends in front, facing them
        float rotationSpeed = 60.0f;   // degrees per second
        int   direction     = 0;       // 0 = round the player's left side, 1 = round the right
        float startDelay    = 0.3f;    // seconds of gameplay after the loading screen before it starts
        bool  keysCancel    = true;    // keys / buttons cancel too (mouse movement always does)
    };

    Settings& GetSettings();

    void Install();                                  // kDataLoaded: loading-screen watcher + Update hook
    void OnPreLoadGame();                            // kPreLoadGame / kNewGame: the next loading screen is a load
    void OnInput(RE::InputEvent* const* a_events);   // TFCam's input sink, every event batch
    void TestOnMenuClose();                          // menu button: run once when the menu is closed

    bool        IsRunning();                         // waiting to start or orbiting
    const char* LastEvent();                         // one line for the settings page
}
