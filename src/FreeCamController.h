#pragma once

namespace FreeCam {

    void Install();

    struct Settings {
        float fovStep      = 2.0f;
        float fovMin       = 10.0f;
        float fovMax       = 150.0f;
        float rollSpeed    = 1.5f;
        std::uint32_t rollCCWKey      = 0x10; // Q
        std::uint32_t rollCWKey       = 0x12; // E
        std::uint32_t resetKey        = 0x13; // R
        std::uint32_t freezeTimeKey   = 0;    // keyboard shortcut for freeze time
        std::uint32_t screenshotKey   = 0;    // keyboard shortcut for screenshot
        bool          blockAttacks    = true;  // block LMB/RMB attacks in free cam (default on)
        int           lmbAction       = 0;    // remap LMB → see MouseAction enum
        int           rmbAction       = 0;    // remap RMB → see MouseAction enum
        bool          dialogueCam     = false; // --Claude: allow free-cam movement during dialogue (WASD move, hold Alt to look)
        bool          raceMenuCam     = true;
        std::uint32_t slowKey         = 0x38;  // 0.7.1: hold to move at 1/5 speed (DX scancode, 0x38 = Left Alt, 0 = off)
        bool          disableShift    = false; // 0.7.1: Shift does nothing to the player in free cam (Sprint/Run/ToggleRun + FreeCameraState ProcessButton hooks, 0.7.6)
        bool          disableSpace    = true;  // 0.7.1: Space / Jump does nothing in free cam (JumpHandler hook, 0.7.6; default ON keeps the 0.7.5 jump block)
        bool          disableActivate = true;  // 0.7.2: eat the Activate user event (E / gamepad A) in free cam - no sitting on furniture or using load doors the crosshair hits from the camera's viewpoint
    };

    // Actions that LMB/RMB can be remapped to.
    enum MouseAction : int {
        kNone = 0,        // just block, do nothing
        kScreenshot,      // 1  one-shot
        kFreezeTime,      // 2  one-shot
        kToggleLight,     // 3  one-shot
        kFovIn,           // 4  one-shot
        kFovOut,          // 5  one-shot
        kResetCam,        // 6  one-shot
        kMoveForward,     // 7  continuous (hold) — like W
        kMoveBackward,    // 8  continuous (hold) — like S
        kMoveUp,          // 9  continuous (hold) — world up
        kMoveDown,        // 10 continuous (hold) — world down
        kMouseActionCount
    };

    Settings& GetSettings();

    float GetRollDegrees();
    void  ResetAll();
    bool  IsActive();       // vanilla free camera is on - whoever drives it (input blocks key on this)

    // 0.7.7: free camera is on AND no FreeCamera Framework timeline owns it (SLCC's cinematic camera
    // runs inside the same FreeCameraState). Camera writes (FOV, roll, reset, transforms, HUD hide)
    // key on this, evaluated at call time.
    bool  TFCamDriving();

    // 0.7.7 SLCC hand-over: read the live free-cam transform (FCFW's, just before SLCC lets go) and
    // start the next TFCam-driven free-cam session from it instead of from the third-person camera.
    bool  CaptureFreeCamPose(RE::NiPoint3& a_pos, float& a_pitch, float& a_yaw);
    void  SetEntryPose(const RE::NiPoint3& a_pos, float a_pitch, float a_yaw);
    void  ClearEntryPose();
}
