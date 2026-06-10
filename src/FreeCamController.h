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
    bool  IsActive();
}
