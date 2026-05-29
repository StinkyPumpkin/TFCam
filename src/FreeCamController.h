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
    };

    Settings& GetSettings();

    float GetRollDegrees();
    void  ResetAll();
    bool  IsActive();
}
