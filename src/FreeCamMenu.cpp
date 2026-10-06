#include "FreeCamMenu.h"
#include "FreeCamController.h"
#include "FreezeTime.h"
#include "CameraLight.h"
#include "HUDHider.h"
#include "CellEntryCam.h"
#include "SceneCam.h"
#include "SceneShots.h"

#include <RE/I/INISettingCollection.h>

#include "SKSEMenuFramework.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>

namespace FreeCamMenu {

    // 0.7.9: TFCam.ini is no longer shipped (user rule 2026-09-26: a settings file in the download resets the user's
    // settings on every update). These code defaults now carry the values the shipped TFCam.ini used to give a fresh
    // install (fStep 5, roll 1.6, speed 14.5, Hide HUD on, light 215/233/255), so new installs behave as before.
    static int   s_freeFlyKey    = 0;
    static int   s_rollCCWKey    = 0x10;   // Q
    static int   s_rollCWKey     = 0x12;   // E
    static int   s_resetKey      = 0x13;   // R
    static int   s_freezeTimeKey = 0;
    static int   s_screenshotKey = 0;
    static float s_rollSpeed     = 1.6f;
    static float s_fovStep       = 5.0f;
    static float s_cameraSpeed   = 14.5f;  // game default fFreeCameraTranslationSpeed is 10
    static bool  s_hideHUD       = true;
    static bool  s_blockAttacks  = true;
    static int   s_lmbAction     = 0;
    static int   s_rmbAction     = 0;
    static bool  s_dialogueCam   = false;  // --Claude: free-cam movement during dialogue
    static int   s_slowKey       = 0x38;   // 0.7.1: hold-to-slow key (Left Alt)
    static bool  s_disableShift  = false;  // 0.7.1
    static bool  s_disableSpace  = true;   // 0.7.1; default ON since 0.7.6 (keeps 0.7.3-0.7.5's jump block)
    static bool  s_disableActivate = true; // 0.7.2
    static bool  s_raceMenuCam   = false;  // SHELVED 2026-08-29: RaceMenu drive not working; UI removed, forced off

    // Camera light settings
    static bool  s_lightScrollBrightness = true;
    static bool  s_lightScrollRadius     = true;
    static float s_lightRadius    = 1000.0f;
    static float s_lightFade      = 1.7f;
    static int   s_lightColorR    = 215;
    static int   s_lightColorG    = 233;
    static int   s_lightColorB    = 255;
    static float s_fovMin         = 10.0f;
    static float s_fovMax         = 150.0f;

    int   GetFreeFlyKey()   { return s_freeFlyKey; }
    float GetCameraSpeed()  { return s_cameraSpeed; }

    // --- Key capture state (thread-safe) ---
    static std::atomic<bool> s_capturing{false};
    static std::atomic<int>  s_capturedKey{-1};
    static int*              s_bindTarget = nullptr;

    bool IsCapturingKey() { return s_capturing.load(); }

    bool IsOverlayOpen() { return SKSEMenuFramework::IsAnyBlockingWindowOpened(); }

    void StartKeyCapture() {
        s_capturedKey.store(-1);
        s_capturing.store(true);
    }

    void OnKeyCaptured(int dxScanCode) {
        s_capturedKey.store(dxScanCode);
        s_capturing.store(false);
    }

    void CancelCapture() {
        s_capturing.store(false);
        s_capturedKey.store(-1);
    }

    static const char* GetINIPath() {
        static char path[MAX_PATH] = {};
        if (!path[0]) {
            GetFullPathNameA("Data\\SKSE\\Plugins\\TFCam.ini", MAX_PATH, path, nullptr);
        }
        return path;
    }

    static const char* GetKeyName(int code) {
        switch (code) {
            case 0:    return "Unset";
            case 0x01: return "Esc";
            case 0x02: return "1"; case 0x03: return "2"; case 0x04: return "3";
            case 0x05: return "4"; case 0x06: return "5"; case 0x07: return "6";
            case 0x08: return "7"; case 0x09: return "8"; case 0x0A: return "9";
            case 0x0B: return "0";
            case 0x0C: return "-"; case 0x0D: return "=";
            case 0x0E: return "Backspace"; case 0x0F: return "Tab";
            case 0x10: return "Q"; case 0x11: return "W"; case 0x12: return "E";
            case 0x13: return "R"; case 0x14: return "T"; case 0x15: return "Y";
            case 0x16: return "U"; case 0x17: return "I"; case 0x18: return "O";
            case 0x19: return "P"; case 0x1A: return "["; case 0x1B: return "]";
            case 0x1C: return "Enter"; case 0x1D: return "LCtrl";
            case 0x1E: return "A"; case 0x1F: return "S"; case 0x20: return "D";
            case 0x21: return "F"; case 0x22: return "G"; case 0x23: return "H";
            case 0x24: return "J"; case 0x25: return "K"; case 0x26: return "L";
            case 0x27: return ";"; case 0x28: return "'"; case 0x29: return "`";
            case 0x2A: return "LShift"; case 0x2B: return "\\";
            case 0x2C: return "Z"; case 0x2D: return "X"; case 0x2E: return "C";
            case 0x2F: return "V"; case 0x30: return "B"; case 0x31: return "N";
            case 0x32: return "M"; case 0x33: return ","; case 0x34: return ".";
            case 0x35: return "/"; case 0x36: return "RShift";
            case 0x37: return "Num*"; case 0x38: return "LAlt"; case 0x39: return "Space";
            case 0x3A: return "CapsLock";
            case 0x3B: return "F1"; case 0x3C: return "F2"; case 0x3D: return "F3";
            case 0x3E: return "F4"; case 0x3F: return "F5"; case 0x40: return "F6";
            case 0x41: return "F7"; case 0x42: return "F8"; case 0x43: return "F9";
            case 0x44: return "F10"; case 0x57: return "F11"; case 0x58: return "F12";
            case 0x45: return "NumLock"; case 0x46: return "ScrollLock";
            case 0x47: return "Num7"; case 0x48: return "Num8"; case 0x49: return "Num9";
            case 0x4A: return "Num-"; case 0x4B: return "Num4"; case 0x4C: return "Num5";
            case 0x4D: return "Num6"; case 0x4E: return "Num+";
            case 0x4F: return "Num1"; case 0x50: return "Num2"; case 0x51: return "Num3";
            case 0x52: return "Num0"; case 0x53: return "Num.";
            case 0x9C: return "NumEnter"; case 0x9D: return "RCtrl"; case 0xB5: return "Num/";
            case 0xB8: return "RAlt";
            case 0xC7: return "Home"; case 0xC8: return "Up"; case 0xC9: return "PgUp";
            case 0xCB: return "Left"; case 0xCD: return "Right";
            case 0xCF: return "End"; case 0xD0: return "Down"; case 0xD1: return "PgDn";
            case 0xD2: return "Insert"; case 0xD3: return "Delete";
            default:   return nullptr;
        }
    }

    // 0.7.9: every write is checked. The live TFCam.ini (mods\TFCam 0.7.2) was last written 2026-09-22 although the
    // user changed checkboxes on 09-24 and 09-26, and even a same-value WritePrivateProfileString bumps the file time
    // - so either those saves failed or never ran. SaveINI now says which, in TFCam.log.
    static bool  s_saveWriteOk  = true;
    static DWORD s_saveWriteErr = 0;

    static void WriteINIString(const char* section, const char* key, const char* val) {
        if (!WritePrivateProfileStringA(section, key, val, GetINIPath()) && s_saveWriteOk) {
            s_saveWriteOk  = false;
            s_saveWriteErr = GetLastError();
        }
    }

    static void WriteINIFloat(const char* section, const char* key, float val) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", val);
        WriteINIString(section, key, buf);
    }

    static void WriteINIInt(const char* section, const char* key, int val) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", val);
        WriteINIString(section, key, buf);
    }

    static void SaveINIValues();
    static void WriteSceneCamValues();

    // Called on every settings change (every frame while a slider is dragged), so the log is rate-limited.
    static void SaveINI() {
        s_saveWriteOk  = true;
        s_saveWriteErr = 0;
        SaveINIValues();

        // Read one value back through the same path (MO2's virtual file system included).
        const int  back     = static_cast<int>(GetPrivateProfileIntA("Camera", "bDisableSpace", -1, GetINIPath()));
        const bool verified = back == (s_disableSpace ? 1 : 0);

        using clock = std::chrono::steady_clock;
        static clock::time_point s_lastOkLog{};
        static clock::time_point s_lastFailLog{};
        const auto now = clock::now();
        if (!s_saveWriteOk || !verified) {
            if (now - s_lastFailLog > std::chrono::seconds(3)) {
                s_lastFailLog = now;
                SKSE::log::error("FreeCamMenu: saving settings to {} FAILED (writes ok={}, Windows error {}, read back "
                                 "bDisableSpace={} expected {}) - this change only lasts until the game closes",
                    GetINIPath(), s_saveWriteOk, s_saveWriteErr, back, s_disableSpace ? 1 : 0);
            }
        } else if (now - s_lastOkLog > std::chrono::seconds(3)) {
            s_lastOkLog = now;
            SKSE::log::info("FreeCamMenu: settings saved to {} (read back OK) - Disable Space={} Disable Shift={} "
                            "Disable Activate={} Block attacks={} Hide HUD={} speed={:.1f}",
                GetINIPath(), s_disableSpace, s_disableShift, s_disableActivate, s_blockAttacks, s_hideHUD, s_cameraSpeed);
        }
    }

    static void SaveINIValues() {
        WriteINIInt("Hotkeys", "iFreeFlyKey", s_freeFlyKey);
        WriteINIInt("Hotkeys", "iResetKey", s_resetKey);
        WriteINIInt("Hotkeys", "iFreezeTimeKey", s_freezeTimeKey);
        WriteINIInt("Hotkeys", "iScreenshotKey", s_screenshotKey);
        WriteINIFloat("Camera", "fSpeed", s_cameraSpeed);
        WriteINIInt("Camera", "bHideHUD", s_hideHUD ? 1 : 0);
        WriteINIInt("Camera", "bBlockAttacks", s_blockAttacks ? 1 : 0);
        WriteINIInt("Camera", "bDialogueCamera", s_dialogueCam ? 1 : 0);
        WriteINIInt("Hotkeys", "iSlowKey", s_slowKey);
        WriteINIInt("Camera", "bDisableShift", s_disableShift ? 1 : 0);
        WriteINIInt("Camera", "bDisableSpace", s_disableSpace ? 1 : 0);
        WriteINIInt("Camera", "bDisableActivate", s_disableActivate ? 1 : 0);
        WriteINIInt("Camera", "bRaceMenuCamera", s_raceMenuCam ? 1 : 0);
        WriteINIInt("Camera", "iLMBAction", s_lmbAction);
        WriteINIInt("Camera", "iRMBAction", s_rmbAction);
        WriteINIInt("Roll", "iKeyCCW", s_rollCCWKey);
        WriteINIInt("Roll", "iKeyCW", s_rollCWKey);
        WriteINIFloat("Roll", "fSpeed", s_rollSpeed);
        WriteINIFloat("FOV", "fStep", s_fovStep);
        WriteINIInt("Light", "bScrollBrightness", s_lightScrollBrightness ? 1 : 0);
        WriteINIInt("Light", "bScrollRadius", s_lightScrollRadius ? 1 : 0);
        WriteINIFloat("Light", "fRadius", s_lightRadius);
        WriteINIFloat("Light", "fFade", s_lightFade);
        WriteINIInt("Light", "iColorR", s_lightColorR);
        WriteINIInt("Light", "iColorG", s_lightColorG);
        WriteINIInt("Light", "iColorB", s_lightColorB);

        const auto& ce = CellEntryCam::GetSettings();
        WriteINIInt("CellEntry", "bEnabled", ce.enabled ? 1 : 0);
        WriteINIInt("CellEntry", "bInteriors", ce.onInteriors ? 1 : 0);
        WriteINIInt("CellEntry", "bExteriors", ce.onExteriors ? 1 : 0);
        WriteINIInt("CellEntry", "bAfterSaveLoad", ce.afterSaveLoad ? 1 : 0);
        WriteINIInt("CellEntry", "bSkipInCombat", ce.skipInCombat ? 1 : 0);
        WriteINIInt("CellEntry", "bChangeZoom", ce.changeZoom ? 1 : 0);
        WriteINIFloat("CellEntry", "fZoom", ce.zoom);
        WriteINIFloat("CellEntry", "fZoomSpeed", ce.zoomSpeed);
        WriteINIFloat("CellEntry", "fRotation", ce.rotation);
        WriteINIFloat("CellEntry", "fRotationSpeed", ce.rotationSpeed);
        WriteINIInt("CellEntry", "iDirection", ce.direction);
        WriteINIFloat("CellEntry", "fStartDelay", ce.startDelay);
        WriteINIInt("CellEntry", "bKeysCancel", ce.keysCancel ? 1 : 0);

        WriteSceneCamValues();
    }

    // 0.10.0 Scene Camera, TFCam.ini [SceneCam].
    static void WriteSceneCamValues() {
        const auto& sc = SceneCam::GetConfig();
        WriteINIInt("SceneCam", "iSceneOrbitKey", sc.orbitKey);
        WriteINIInt("SceneCam", "iSceneEyeKey", sc.eyeKey);
        WriteINIInt("SceneCam", "iSceneEyeNextKey", sc.eyeNextKey);
        WriteINIInt("SceneCam", "iSceneShotSaveKey", sc.shotSaveKey);
        WriteINIInt("SceneCam", "iSceneShotRecallKey", sc.shotRecallKey);
        WriteINIInt("SceneCam", "iSceneShotDeleteKey", sc.shotDeleteKey);
        WriteINIFloat("SceneCam", "fOrbitRadius", sc.orbitRadius);
        WriteINIFloat("SceneCam", "fOrbitHeight", sc.orbitHeight);
        WriteINIFloat("SceneCam", "fOrbitSpeed", sc.orbitSpeed);
        WriteINIFloat("SceneCam", "fOrbitFOV", sc.orbitFOV);
        WriteINIFloat("SceneCam", "fEyeFOV", sc.eyeFOV);
        WriteINIFloat("SceneCam", "fEyeNearClip", sc.eyeNearClip);
        WriteINIFloat("SceneCam", "fEyeForward", sc.eyeForward);
        WriteINIInt("SceneCam", "bEyeHideHead", sc.eyeHideHead ? 1 : 0);
        WriteINIInt("SceneCam", "bAutoStageShots", sc.autoStageShots ? 1 : 0);
        WriteINIInt("SceneCam", "bAutoPoseShots", sc.autoPoseShots ? 1 : 0);
        WriteINIFloat("SceneCam", "fShotBlend", sc.shotBlend);
    }

    void SaveSceneCamSettings() {
        s_saveWriteOk  = true;
        s_saveWriteErr = 0;
        WriteSceneCamValues();
        const auto& sc = SceneCam::GetConfig();
        if (s_saveWriteOk) {
            SKSE::log::info("FreeCamMenu: Scene Camera settings saved - radius {:.0f}, height {:.0f}, speed {:.0f}, orbit FOV "
                            "{:.0f}, eye FOV {:.0f}", sc.orbitRadius, sc.orbitHeight, sc.orbitSpeed, sc.orbitFOV, sc.eyeFOV);
        } else {
            SKSE::log::error("FreeCamMenu: saving the Scene Camera settings to {} FAILED (Windows error {}) - they only last "
                             "until the game closes", GetINIPath(), s_saveWriteErr);
        }
    }

    static void ApplyToController() {
        auto& settings = FreeCam::GetSettings();
        settings.rollCCWKey     = static_cast<std::uint32_t>(s_rollCCWKey);
        settings.rollCWKey      = static_cast<std::uint32_t>(s_rollCWKey);
        settings.resetKey       = static_cast<std::uint32_t>(s_resetKey);
        settings.freezeTimeKey  = static_cast<std::uint32_t>(s_freezeTimeKey);
        settings.screenshotKey  = static_cast<std::uint32_t>(s_screenshotKey);
        settings.rollSpeed      = s_rollSpeed;
        settings.fovStep        = s_fovStep;
        settings.blockAttacks   = s_blockAttacks;
        settings.lmbAction      = s_lmbAction;
        settings.rmbAction      = s_rmbAction;
        settings.dialogueCam    = s_dialogueCam;
        settings.slowKey       = static_cast<std::uint32_t>(s_slowKey);
        settings.disableActivate = s_disableActivate;
        settings.disableShift  = s_disableShift;
        settings.disableSpace  = s_disableSpace;
        settings.raceMenuCam    = s_raceMenuCam;
    }

    // --- Press-to-bind key widget (returns true if key changed) ---
    static bool KeyBindField(const char* id, const char* label, int* keyCode) {
        bool changed = false;
        bool isThisBinding = (s_bindTarget == keyCode);

        if (isThisBinding && !s_capturing.load()) {
            int captured = s_capturedKey.exchange(-1);
            if (captured >= 0) {
                *keyCode = captured;
                s_bindTarget = nullptr;
                isThisBinding = false;
                changed = true;
            }
        }

        bool waiting = isThisBinding && s_capturing.load();

        ImGuiMCP::Text("%s:", label);
        ImGuiMCP::SameLine();

        char btnLabel[64];
        if (waiting) {
            snprintf(btnLabel, sizeof(btnLabel), "Press a key...##%s", id);
        } else {
            const char* name = GetKeyName(*keyCode);
            if (name) {
                snprintf(btnLabel, sizeof(btnLabel), "%s##%s", name, id);
            } else if (*keyCode > 0) {
                snprintf(btnLabel, sizeof(btnLabel), "0x%02X##%s", *keyCode, id);
            } else {
                snprintf(btnLabel, sizeof(btnLabel), "Unset##%s", id);
            }
        }

        if (ImGuiMCP::Button(btnLabel)) {
            if (waiting) {
                CancelCapture();
                s_bindTarget = nullptr;
            } else {
                if (s_bindTarget) CancelCapture();
                s_bindTarget = keyCode;
                StartKeyCapture();
            }
        }

        ImGuiMCP::SameLine();
        char unmapLabel[32];
        snprintf(unmapLabel, sizeof(unmapLabel), "Unmap##%s", id);
        if (ImGuiMCP::SmallButton(unmapLabel)) {
            *keyCode = 0;
            if (s_bindTarget == keyCode) {
                CancelCapture();
                s_bindTarget = nullptr;
            }
            changed = true;
        }

        return changed;
    }

    static void __stdcall RenderSettings() {
        ImGuiMCP::SeparatorText("Free Fly Camera");

        if (KeyBindField("freeFly", "Toggle Free Camera", &s_freeFlyKey)) {
            SaveINI();
        }

        if (KeyBindField("slowKey", "Slow movement (hold)", &s_slowKey)) {
            ApplyToController();
            SaveINI();
        }
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Hold to fly at 1/5 speed. Default Left Alt; unset = off.");

        if (ImGuiMCP::Checkbox("Disable Shift in free cam##noshift", &s_disableShift)) {
            ApplyToController();
            SaveINI();
        }
        if (ImGuiMCP::Checkbox("Disable Space / Jump in free cam##nospace", &s_disableSpace)) {
            ApplyToController();
            SaveINI();
        }
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Ticked, while flying: Shift no longer drives sprint / run / run toggle (a Shift+key run "
            "toggle too), and Jump is blocked on any key or gamepad. Mods that read a key directly "
            "may still see it. Unticked: the key works as it does without TFCam. Either way, in a "
            "SexLab scene Space still reaches SexLab (Advance), and a mod that closes the free camera "
            "on Jump (Poser Hotkeys Plus) cannot close one it did not open.");
        if (ImGuiMCP::Checkbox("Disable Activate in free cam##noactivate", &s_disableActivate)) {
            ApplyToController();
            SaveINI();
        }
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Stops E / gamepad A from sitting you on furniture or using a door the camera is pointing at.");

        ImGuiMCP::Separator();

        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::SliderFloat("Camera Speed##speed", &s_cameraSpeed, 0.5f, 50.0f, "%.1f")) {
            auto* ini = RE::INISettingCollection::GetSingleton();
            if (ini) {
                auto* setting = ini->GetSetting("fFreeCameraTranslationSpeed:Camera");
                if (setting) setting->data.f = s_cameraSpeed;
            }
            SaveINI();
        }

        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::SliderFloat("FOV Step (mouse wheel)##fov", &s_fovStep, 0.5f, 10.0f, "%.1f")) {
            ApplyToController();
            SaveINI();
        }

        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::SliderFloat("Roll Speed##roll", &s_rollSpeed, 0.1f, 5.0f, "%.1f")) {
            ApplyToController();
            SaveINI();
        }

        if (ImGuiMCP::Checkbox("Hide HUD in Free Cam##hideHud", &s_hideHUD)) {
            HUDHider::SetEnabled(s_hideHUD);
            SaveINI();
        }

        // Surgical attack block via AttackBlockHandler hook (no ControlMap — that
        // crashed in free cam; this leaves mouse camera-vertical untouched).
        if (ImGuiMCP::Checkbox("Block attacks##blockatk", &s_blockAttacks)) {
            ApplyToController();
            SaveINI();
        }
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Blocks weapon/spell attacks and enables L/R click remap below.");

        // LMB/RMB remap — only available when attacks are blocked (otherwise
        // both the remap action and the attack would fire simultaneously).
        if (s_blockAttacks) {
            static const char* actionNames[] = {
                "None (block only)", "Screenshot", "Freeze Time", "Toggle Light",
                "FOV In", "FOV Out", "Reset FOV/Roll",
                "Move Forward (W)", "Move Backward (S)", "Move Up", "Move Down"
            };
            ImGuiMCP::SetNextItemWidth(180.0f);
            if (ImGuiMCP::Combo("Left click##lmbAct", &s_lmbAction, actionNames, 11, 11)) {
                ApplyToController();
                SaveINI();
            }
            ImGuiMCP::SetNextItemWidth(180.0f);
            if (ImGuiMCP::Combo("Right click##rmbAct", &s_rmbAction, actionNames, 11, 11)) {
                ApplyToController();
                SaveINI();
            }
        }

        // SHELVED 2026-09-17: dialogue camera checkbox removed (manual drive under the Dialogue
        // Menu does not work properly yet; forced off in LoadINI). Code paths kept for a retry.
        // SHELVED 2026-08-29: RaceMenu camera checkbox removed (feature not working; forced off).

        if (KeyBindField("rollCCW", "Roll Left", &s_rollCCWKey)) {
            ApplyToController();
            SaveINI();
        }
        if (KeyBindField("rollCW", "Roll Right", &s_rollCWKey)) {
            ApplyToController();
            SaveINI();
        }
        if (KeyBindField("resetK", "Reset FOV / Roll", &s_resetKey)) {
            ApplyToController();
            SaveINI();
        }
        if (KeyBindField("freezeK", "Freeze Time (keyboard)", &s_freezeTimeKey)) {
            ApplyToController();
            SaveINI();
        }
        if (KeyBindField("sshotK", "Screenshot (keyboard)", &s_screenshotKey)) {
            ApplyToController();
            SaveINI();
        }

        ImGuiMCP::SeparatorText("Camera Light");

        if (CameraLight::IsActive()) {
            ImGuiMCP::TextColored({ 1.0f, 0.9f, 0.4f, 1.0f }, "Light: ON");
            ImGuiMCP::SameLine();
            char infoTxt[64];
            snprintf(infoTxt, sizeof(infoTxt), "(Brightness: %.1f  Radius: %.0f)",
                CameraLight::GetIntensity(), CameraLight::GetRadius());
            ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, "%s", infoTxt);
        } else {
            ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "Light: OFF");
        }

        // --- Light properties ---
        ImGuiMCP::SetNextItemWidth(200.0f);
        if (ImGuiMCP::SliderFloat("Radius##camLight", &s_lightRadius, 100.0f, 5000.0f, "%.0f")) {
            CameraLight::SetRadius(s_lightRadius);
            SaveINI();
        }

        ImGuiMCP::SetNextItemWidth(200.0f);
        if (ImGuiMCP::SliderFloat("Fade##camLight", &s_lightFade, 0.1f, 10.0f, "%.1f")) {
            CameraLight::SetFade(s_lightFade);
            SaveINI();
        }

        ImGuiMCP::SetNextItemWidth(200.0f);
        if (ImGuiMCP::SliderInt("Red##camLight", &s_lightColorR, 0, 255, "%d")) {
            CameraLight::SetColor(s_lightColorR / 255.0f, s_lightColorG / 255.0f, s_lightColorB / 255.0f);
            SaveINI();
        }
        ImGuiMCP::SetNextItemWidth(200.0f);
        if (ImGuiMCP::SliderInt("Green##camLight", &s_lightColorG, 0, 255, "%d")) {
            CameraLight::SetColor(s_lightColorR / 255.0f, s_lightColorG / 255.0f, s_lightColorB / 255.0f);
            SaveINI();
        }
        ImGuiMCP::SetNextItemWidth(200.0f);
        if (ImGuiMCP::SliderInt("Blue##camLight", &s_lightColorB, 0, 255, "%d")) {
            CameraLight::SetColor(s_lightColorR / 255.0f, s_lightColorG / 255.0f, s_lightColorB / 255.0f);
            SaveINI();
        }

        ImGuiMCP::TextColored(
            { s_lightColorR / 255.0f, s_lightColorG / 255.0f, s_lightColorB / 255.0f, 1.0f },
            "Color Preview ████████");

        // --- Presets ---
        struct LightPreset {
            const char* name;
            float radius; float fade; int r, g, b;
        };
        static const LightPreset presets[] = {
            { "Default",    1000.0f, 1.7f, 255, 255, 255 },
            { "Wide",       2500.0f, 1.5f, 255, 255, 255 },
            { "MageLight",  1000.0f, 1.5f, 200, 200, 255 },
            { "Torch",       500.0f, 1.5f, 255, 170, 100 },
            { "FaceLight",   300.0f, 2.5f, 255, 255, 255 },
            { "Candlelight", 400.0f, 1.3f, 255, 200, 120 },
            { "Moonlight",  2000.0f, 1.0f, 160, 180, 220 },
        };

        for (const auto& p : presets) {
            if (ImGuiMCP::SmallButton(p.name)) {
                s_lightRadius = p.radius;
                s_lightFade   = p.fade;
                s_lightColorR = p.r;
                s_lightColorG = p.g;
                s_lightColorB = p.b;
                CameraLight::SetRadius(s_lightRadius);
                CameraLight::SetFade(s_lightFade);
                CameraLight::SetColor(s_lightColorR / 255.0f, s_lightColorG / 255.0f, s_lightColorB / 255.0f);
                SaveINI();
            }
            ImGuiMCP::SameLine();
        }
        ImGuiMCP::NewLine();

        // --- Scroll behavior ---
        if (ImGuiMCP::Checkbox("Increase brightness with scroll##lightBright", &s_lightScrollBrightness)) {
            CameraLight::SetScrollBrightness(s_lightScrollBrightness);
            SaveINI();
        }
        if (ImGuiMCP::Checkbox("Increase radius with scroll##lightRadius", &s_lightScrollRadius)) {
            CameraLight::SetScrollRadius(s_lightScrollRadius);
            SaveINI();
        }

        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Shift+Scroll Up: Light on/brighter | Shift+Scroll Down: dimmer/off");

        ImGuiMCP::Separator();
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Alt: Slow camera | Shift+MMB / Key: Freeze | MMB / Key: Screenshot");

        if (FreezeTime::IsFrozen()) {
            ImGuiMCP::TextColored({ 0.3f, 0.8f, 1.0f, 1.0f }, "Time: FROZEN");
        }
    }

    // 0.9.0: FreeCam > Cell Entry page.
    static void __stdcall RenderCellEntry() {
        auto& ce = CellEntryCam::GetSettings();
        bool changed = false;

        ImGuiMCP::SeparatorText("Cell Entry Camera");
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "After a loading screen into a new cell, the third-person camera zooms to a set distance, then orbits "
            "round the player until it faces them. Any mouse movement stops it at once and gives the camera back.");

        changed |= ImGuiMCP::Checkbox("Enabled##ceOn", &ce.enabled);
        changed |= ImGuiMCP::Checkbox("Entering interiors##ceInt", &ce.onInteriors);
        changed |= ImGuiMCP::Checkbox("Arriving outdoors (doors, fast travel)##ceExt", &ce.onExteriors);
        changed |= ImGuiMCP::Checkbox("After loading a save##ceSave", &ce.afterSaveLoad);
        changed |= ImGuiMCP::Checkbox("Skip when in combat##ceCombat", &ce.skipInCombat);

        ImGuiMCP::SeparatorText("Zoom");
        changed |= ImGuiMCP::Checkbox("Change zoom first##ceZoomOn", &ce.changeZoom);
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Zoom##ceZoom", &ce.zoom, -0.2f, 1.0f, "%.2f");
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "The game's own zoom value: -0.2 = closest, 1.0 = farthest (the mouse-wheel range).");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Zoom speed (per second)##ceZoomSpd", &ce.zoomSpeed, 0.1f, 5.0f, "%.1f");

        ImGuiMCP::SeparatorText("Rotation");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Rotation (degrees)##ceRot", &ce.rotation, 0.0f, 360.0f, "%.0f");
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "180 = the camera ends in front of the player, facing them.");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Rotation speed (degrees/s)##ceRotSpd", &ce.rotationSpeed, 10.0f, 360.0f, "%.0f");
        static const char* dirNames[] = { "Round the player's left side", "Round the player's right side" };
        ImGuiMCP::SetNextItemWidth(220.0f);
        changed |= ImGuiMCP::Combo("Direction##ceDir", &ce.direction, dirNames, 2, 2);

        ImGuiMCP::SeparatorText("Timing and cancel");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Start delay (s)##ceDelay", &ce.startDelay, 0.0f, 3.0f, "%.1f");
        changed |= ImGuiMCP::Checkbox("Keys and buttons cancel too##ceKeys", &ce.keysCancel);
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Mouse movement, the mouse wheel and the right stick always cancel.");

        if (changed) SaveINI();

        ImGuiMCP::Separator();
        if (ImGuiMCP::Button("Test (runs when this menu closes)##ceTest")) {
            CellEntryCam::TestOnMenuClose();
        }
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "Last: %s", CellEntryCam::LastEvent());
    }

    // 0.10.0: FreeCam > Scene Camera page.
    static void __stdcall RenderSceneCam() {
        auto& sc      = SceneCam::GetConfig();
        bool  changed = false;

        ImGuiMCP::SeparatorText("Scene Camera");
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Orbit round your SexLab scene's actors (or a posed actor, or you), look through a partner's eyes, and save a "
            "camera shot per scene stage or per pose. TFCam's free camera does the work: these turn it on when needed.");

        // Status (a snapshot the game updates every frame while something is going on).
        const auto  ms = SceneCam::GetMenuStatus();
        const auto& st = ms.status;
        static const char* modeNames[] = { "Off (TFCam is not driving a free camera)", "Free-fly", "Orbit", "Eye view" };
        ImGuiMCP::Text("Mode: %s", modeNames[st.mode <= 3 ? st.mode : 0]);
        if (st.inPlayerScene) {
            ImGuiMCP::Text("In your SexLab scene: yes, %u partner(s)", st.partnerCount);
            if (st.stageKnown) {
                ImGuiMCP::Text("Scene %s   stage %s   your slot %d", st.sceneId, st.stageId, st.playerSlot);
                ImGuiMCP::Text("Shot for this stage: %s   (%d saved for this scene)", st.hasStageShot ? "saved" : "none",
                    ms.sceneShots);
            } else {
                ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "Scene and stage: not read yet");
            }
        } else {
            ImGuiMCP::Text("In your SexLab scene: no");
        }
        if (st.poseActive) {
            ImGuiMCP::Text("Pose: %s   shot %s", st.poseKey, st.hasPoseShot ? "saved" : "none");
        }
        if (ms.lastEvent[0]) {
            ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "Last: %s", ms.lastEvent);
        }
        ImGuiMCP::BeginDisabled(st.sceneId[0] == '\0');
        if (ImGuiMCP::Button("Delete all shots for this scene##scDelScene")) {
            SceneCam::RequestDeleteSceneShots();
        }
        ImGuiMCP::EndDisabled();

        ImGuiMCP::SeparatorText("Hotkeys");
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "All unset until you bind them. Orbit and the shot keys work anywhere; eye view only in your SexLab scene.");
        changed |= KeyBindField("scOrbit", "Orbit on / off", &sc.orbitKey);
        changed |= KeyBindField("scEye", "Eye view on / off", &sc.eyeKey);
        changed |= KeyBindField("scEyeNext", "Eye view: next partner", &sc.eyeNextKey);
        changed |= KeyBindField("scSave", "Save camera shot", &sc.shotSaveKey);
        changed |= KeyBindField("scRecall", "Go to saved shot", &sc.shotRecallKey);
        changed |= KeyBindField("scDelete", "Delete saved shot", &sc.shotDeleteKey);

        ImGuiMCP::SeparatorText("Orbit");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Radius##scRadius", &sc.orbitRadius, 30.0f, 500.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Height (degrees)##scHeight", &sc.orbitHeight, -20.0f, 75.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Speed (degrees/s)##scSpeed", &sc.orbitSpeed, -90.0f, 90.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("FOV##scOrbitFov", &sc.orbitFOV, 20.0f, 120.0f, "%.0f");
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "A negative speed orbits the other way. Mouse wheel while orbiting: radius. WASD and the mouse do nothing.");

        ImGuiMCP::SeparatorText("Eye view");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("FOV##scEyeFov", &sc.eyeFOV, 30.0f, 130.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Near clip##scNear", &sc.eyeNearClip, 1.0f, 10.0f, "%.1f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Camera in front of the eyes##scFwd", &sc.eyeForward, -5.0f, 10.0f, "%.1f");
        changed |= ImGuiMCP::Checkbox("Hide the partner's head##scHideHead", &sc.eyeHideHead);
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "The mouse looks around (up to 85 degrees each way). Mouse wheel: eye-view FOV.");

        ImGuiMCP::SeparatorText("Saved shots");
        changed |= ImGuiMCP::Checkbox("Go to a stage's shot at scene start / stage change##scAutoStage", &sc.autoStageShots);
        changed |= ImGuiMCP::Checkbox("Go to a pose's shot when the pose starts##scAutoPose", &sc.autoPoseShots);
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Blend time (s)##scBlend", &sc.shotBlend, 0.0f, 3.0f, "%.1f");
        static const std::string s_shotFile = SceneShots::FilePath();
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Automatic moves only happen in free-fly (not in orbit or eye view) and never turn the free camera on. "
            "Shots are kept per stage and your position slot. File: %s", s_shotFile.c_str());

        if (changed) {
            SceneCam::OnConfigChanged();
            SaveINI();
        }
    }

    // 0.7.9: first run with no TFCam.ini - write one from the code defaults (MO2 puts a new file in overwrite\).
    // Afterwards only the in-game page writes it; no update or deploy ever ships or copies it.
    static void CreateDefaultINI(const char* a_path) {
        std::ofstream f(a_path, std::ios::out | std::ios::trunc);
        if (!f) {
            SKSE::log::error("FreeCamMenu: could not create {} - defaults apply, the first setting change retries", a_path);
            return;
        }
        auto fl = [](float v) { return std::format("{:.4f}", v); };
        f << "; TFCam settings. TFCam created this file on first launch; it is not part of the download, so updating\n"
             "; TFCam never resets it. Delete it to return to the defaults. The in-game page (SKSE Menu Framework >\n"
             "; FreeCam > Settings) saves every change here immediately.\n\n";
        f << "[FOV]\n; Degrees per mouse wheel tick\nfStep=" << fl(s_fovStep) << "\n"
          << "; FOV range limits\nfMin=" << fl(s_fovMin) << "\nfMax=" << fl(s_fovMax) << "\n\n";
        f << "[Roll]\n; Radians per second while key held\nfSpeed=" << fl(s_rollSpeed) << "\n"
          << "; Q = roll counter-clockwise (0x10 = 16)\niKeyCCW=" << s_rollCCWKey << "\n"
          << "; E = roll clockwise (0x12 = 18)\niKeyCW=" << s_rollCWKey << "\n\n";
        f << "[Hotkeys]\n; DX scancode for free fly toggle (0 = unmapped)\niFreeFlyKey=" << s_freeFlyKey << "\n"
          << "; R = reset FOV + roll (0x13 = 19)\niResetKey=" << s_resetKey << "\n"
          << "; Keyboard shortcuts for freeze time / screenshot (0 = unmapped: Shift+Middle Click / Middle Click)\n"
          << "iFreezeTimeKey=" << s_freezeTimeKey << "\niScreenshotKey=" << s_screenshotKey << "\n"
          << "; Hold to fly at 1/5 speed. DX scancode, 0x38 = Left Alt (56). 0 = off\niSlowKey=" << s_slowKey << "\n\n";
        f << "[Camera]\nfSpeed=" << fl(s_cameraSpeed) << "\nbHideHUD=" << (s_hideHUD ? 1 : 0)
          << "\nbBlockAttacks=" << (s_blockAttacks ? 1 : 0) << "\niLMBAction=" << s_lmbAction
          << "\niRMBAction=" << s_rmbAction << "\n"
          << "; 1 = in free cam Shift no longer drives sprint / run / run toggle (incl. a Shift+key run toggle). "
             "0 = works as without TFCam\nbDisableShift=" << (s_disableShift ? 1 : 0) << "\n"
          << "; 1 = Space / Jump does nothing in free cam (no jumping under the camera, any key or gamepad). "
             "0 = works as without TFCam\nbDisableSpace=" << (s_disableSpace ? 1 : 0) << "\n"
          << "; 1 = eat the Activate key (E / gamepad A) while in free cam: no sitting on furniture or using a door "
             "the camera points at\nbDisableActivate=" << (s_disableActivate ? 1 : 0) << "\n\n";
        f << "[Light]\nbScrollBrightness=" << (s_lightScrollBrightness ? 1 : 0)
          << "\nbScrollRadius=" << (s_lightScrollRadius ? 1 : 0) << "\nfRadius=" << fl(s_lightRadius)
          << "\nfFade=" << fl(s_lightFade) << "\niColorR=" << s_lightColorR << "\niColorG=" << s_lightColorG
          << "\niColorB=" << s_lightColorB << "\n\n";
        const auto& ce = CellEntryCam::GetSettings();
        f << "[CellEntry]\n; After a loading screen into a new cell: zoom to fZoom, then orbit fRotation degrees round the player.\n"
             "; Any mouse movement cancels it at once (bKeysCancel=1: keys and buttons too).\n"
          << "bEnabled=" << (ce.enabled ? 1 : 0) << "\nbInteriors=" << (ce.onInteriors ? 1 : 0)
          << "\nbExteriors=" << (ce.onExteriors ? 1 : 0) << "\nbAfterSaveLoad=" << (ce.afterSaveLoad ? 1 : 0)
          << "\nbSkipInCombat=" << (ce.skipInCombat ? 1 : 0) << "\nbChangeZoom=" << (ce.changeZoom ? 1 : 0)
          << "\n; game zoom value: -0.2 = closest, 1.0 = farthest\nfZoom=" << fl(ce.zoom)
          << "\n; zoom units per second\nfZoomSpeed=" << fl(ce.zoomSpeed)
          << "\n; degrees; 180 = the camera ends in front of the player, facing them\nfRotation=" << fl(ce.rotation)
          << "\n; degrees per second\nfRotationSpeed=" << fl(ce.rotationSpeed)
          << "\n; 0 = round the player's left side, 1 = round the right\niDirection=" << ce.direction
          << "\n; seconds after the loading screen\nfStartDelay=" << fl(ce.startDelay)
          << "\nbKeysCancel=" << (ce.keysCancel ? 1 : 0) << "\n\n";
        const auto& sc = SceneCam::GetConfig();
        f << "[SceneCam]\n; 0.10.0 Scene Camera (SKSE Menu Framework > FreeCam > Scene Camera). DX scan codes, 0 = unset.\n"
          << "; Orbit on/off works anywhere; eye view and next partner only in your SexLab scene.\n"
          << "iSceneOrbitKey=" << sc.orbitKey << "\niSceneEyeKey=" << sc.eyeKey << "\niSceneEyeNextKey=" << sc.eyeNextKey
          << "\n; Saved shots: per scene stage (in your scene) or per pose (a pose SLUI started)\n"
          << "iSceneShotSaveKey=" << sc.shotSaveKey << "\niSceneShotRecallKey=" << sc.shotRecallKey
          << "\niSceneShotDeleteKey=" << sc.shotDeleteKey
          << "\n; Orbit: radius 30..500 units, height -20..75 degrees, speed -90..90 degrees/s (sign = direction), FOV 20..120\n"
          << "fOrbitRadius=" << fl(sc.orbitRadius) << "\nfOrbitHeight=" << fl(sc.orbitHeight)
          << "\nfOrbitSpeed=" << fl(sc.orbitSpeed) << "\nfOrbitFOV=" << fl(sc.orbitFOV)
          << "\n; Eye view: FOV 30..130, near clip 1..10, camera in front of the eyes -5..10, 1 = hide the partner's head\n"
          << "fEyeFOV=" << fl(sc.eyeFOV) << "\nfEyeNearClip=" << fl(sc.eyeNearClip) << "\nfEyeForward=" << fl(sc.eyeForward)
          << "\nbEyeHideHead=" << (sc.eyeHideHead ? 1 : 0)
          << "\n; 1 = blend to a saved shot at scene start / stage change, and when a pose starts (free-fly only)\n"
          << "bAutoStageShots=" << (sc.autoStageShots ? 1 : 0) << "\nbAutoPoseShots=" << (sc.autoPoseShots ? 1 : 0)
          << "\n; seconds, 0..3\nfShotBlend=" << fl(sc.shotBlend) << "\n";
        f.flush();
        if (!f) {
            SKSE::log::error("FreeCamMenu: writing the new {} failed part-way", a_path);
            return;
        }
        SKSE::log::info("FreeCamMenu: first run - created {} from the code defaults", a_path);
    }

    void LoadSettings() {
        const char* ini = GetINIPath();
        // Through MO2's virtual file system too (the same open GetPrivateProfileString does).
        const bool exists = std::ifstream(ini).good();

        char buf[64], defBuf[64];
        auto readFloat = [&](const char* section, const char* key, float def) -> float {
            snprintf(defBuf, sizeof(defBuf), "%.4f", def);
            GetPrivateProfileStringA(section, key, defBuf, buf, sizeof(buf), ini);
            return static_cast<float>(atof(buf));
        };
        auto readInt = [&](const char* section, const char* key, int def) -> int {
            return GetPrivateProfileIntA(section, key, def, ini);
        };

        s_freeFlyKey    = readInt("Hotkeys", "iFreeFlyKey", 0);
        s_resetKey      = readInt("Hotkeys", "iResetKey", 0x13);
        s_freezeTimeKey = readInt("Hotkeys", "iFreezeTimeKey", 0);
        s_screenshotKey = readInt("Hotkeys", "iScreenshotKey", 0);

        s_rollCCWKey = readInt("Roll", "iKeyCCW", 0x10);
        s_rollCWKey  = readInt("Roll", "iKeyCW",  0x12);
        s_rollSpeed  = readFloat("Roll", "fSpeed", 1.6f);

        s_fovStep = readFloat("FOV", "fStep", 5.0f);
        s_fovMin  = readFloat("FOV", "fMin", 10.0f);
        s_fovMax  = readFloat("FOV", "fMax", 150.0f);
        float fovMin = s_fovMin;
        float fovMax = s_fovMax;

        s_cameraSpeed = readFloat("Camera", "fSpeed", 14.5f);
        s_hideHUD     = readInt("Camera", "bHideHUD", 1) != 0;
        s_blockAttacks = readInt("Camera", "bBlockAttacks", 1) != 0;
        s_dialogueCam  = false; // SHELVED 2026-09-17: forced off regardless of ini (drive not working properly yet)
        s_slowKey      = readInt("Hotkeys", "iSlowKey", 0x38);
        s_disableShift = readInt("Camera", "bDisableShift", 0) != 0;
        s_disableSpace = readInt("Camera", "bDisableSpace", 1) != 0;  // 0.7.6: default ON, see s_disableSpace
        s_disableActivate = readInt("Camera", "bDisableActivate", 1) != 0;
        s_raceMenuCam  = false; // SHELVED 2026-08-29: forced off regardless of ini
        s_lmbAction   = readInt("Camera", "iLMBAction", 0);
        s_rmbAction   = readInt("Camera", "iRMBAction", 0);
        HUDHider::SetEnabled(s_hideHUD);

        // Camera light settings
        s_lightScrollBrightness = readInt("Light", "bScrollBrightness", 1) != 0;
        s_lightScrollRadius     = readInt("Light", "bScrollRadius", 1) != 0;
        s_lightRadius    = readFloat("Light", "fRadius", 1000.0f);
        s_lightFade      = readFloat("Light", "fFade", 1.7f);
        s_lightColorR    = readInt("Light", "iColorR", 215);
        s_lightColorG    = readInt("Light", "iColorG", 233);
        s_lightColorB    = readInt("Light", "iColorB", 255);

        // 0.9.0 cell-entry camera
        {
            auto& ce = CellEntryCam::GetSettings();
            const CellEntryCam::Settings def{};
            ce.enabled       = readInt("CellEntry", "bEnabled", def.enabled ? 1 : 0) != 0;
            ce.onInteriors   = readInt("CellEntry", "bInteriors", def.onInteriors ? 1 : 0) != 0;
            ce.onExteriors   = readInt("CellEntry", "bExteriors", def.onExteriors ? 1 : 0) != 0;
            ce.afterSaveLoad = readInt("CellEntry", "bAfterSaveLoad", def.afterSaveLoad ? 1 : 0) != 0;
            ce.skipInCombat  = readInt("CellEntry", "bSkipInCombat", def.skipInCombat ? 1 : 0) != 0;
            ce.changeZoom    = readInt("CellEntry", "bChangeZoom", def.changeZoom ? 1 : 0) != 0;
            ce.zoom          = readFloat("CellEntry", "fZoom", def.zoom);
            ce.zoomSpeed     = readFloat("CellEntry", "fZoomSpeed", def.zoomSpeed);
            ce.rotation      = readFloat("CellEntry", "fRotation", def.rotation);
            ce.rotationSpeed = readFloat("CellEntry", "fRotationSpeed", def.rotationSpeed);
            ce.direction     = readInt("CellEntry", "iDirection", def.direction) == 1 ? 1 : 0;
            ce.startDelay    = readFloat("CellEntry", "fStartDelay", def.startDelay);
            ce.keysCancel    = readInt("CellEntry", "bKeysCancel", def.keysCancel ? 1 : 0) != 0;
        }

        // 0.10.0 Scene Camera. An existing TFCam.ini without [SceneCam] gets the code defaults (the file is not rewritten).
        {
            auto&                  sc = SceneCam::GetConfig();
            const SceneCam::Config def{};
            sc.orbitKey       = readInt("SceneCam", "iSceneOrbitKey", def.orbitKey);
            sc.eyeKey         = readInt("SceneCam", "iSceneEyeKey", def.eyeKey);
            sc.eyeNextKey     = readInt("SceneCam", "iSceneEyeNextKey", def.eyeNextKey);
            sc.shotSaveKey    = readInt("SceneCam", "iSceneShotSaveKey", def.shotSaveKey);
            sc.shotRecallKey  = readInt("SceneCam", "iSceneShotRecallKey", def.shotRecallKey);
            sc.shotDeleteKey  = readInt("SceneCam", "iSceneShotDeleteKey", def.shotDeleteKey);
            sc.orbitRadius    = readFloat("SceneCam", "fOrbitRadius", def.orbitRadius);
            sc.orbitHeight    = readFloat("SceneCam", "fOrbitHeight", def.orbitHeight);
            sc.orbitSpeed     = readFloat("SceneCam", "fOrbitSpeed", def.orbitSpeed);
            sc.orbitFOV       = readFloat("SceneCam", "fOrbitFOV", def.orbitFOV);
            sc.eyeFOV         = readFloat("SceneCam", "fEyeFOV", def.eyeFOV);
            sc.eyeNearClip    = readFloat("SceneCam", "fEyeNearClip", def.eyeNearClip);
            sc.eyeForward     = readFloat("SceneCam", "fEyeForward", def.eyeForward);
            sc.eyeHideHead    = readInt("SceneCam", "bEyeHideHead", def.eyeHideHead ? 1 : 0) != 0;
            sc.autoStageShots = readInt("SceneCam", "bAutoStageShots", def.autoStageShots ? 1 : 0) != 0;
            sc.autoPoseShots  = readInt("SceneCam", "bAutoPoseShots", def.autoPoseShots ? 1 : 0) != 0;
            sc.shotBlend      = readFloat("SceneCam", "fShotBlend", def.shotBlend);
            SceneCam::ClampConfig();
        }

        CameraLight::SetScrollBrightness(s_lightScrollBrightness);
        CameraLight::SetScrollRadius(s_lightScrollRadius);
        CameraLight::SetRadius(s_lightRadius);
        CameraLight::SetFade(s_lightFade);
        CameraLight::SetColor(s_lightColorR / 255.0f, s_lightColorG / 255.0f, s_lightColorB / 255.0f);

        auto& settings = FreeCam::GetSettings();
        settings.fovStep    = s_fovStep;
        settings.fovMin     = fovMin;
        settings.fovMax     = fovMax;
        settings.rollSpeed  = s_rollSpeed;
        settings.rollCCWKey    = static_cast<std::uint32_t>(s_rollCCWKey);
        settings.rollCWKey     = static_cast<std::uint32_t>(s_rollCWKey);
        settings.resetKey      = static_cast<std::uint32_t>(s_resetKey);
        settings.freezeTimeKey = static_cast<std::uint32_t>(s_freezeTimeKey);
        settings.screenshotKey = static_cast<std::uint32_t>(s_screenshotKey);
        settings.blockAttacks  = s_blockAttacks;
        settings.lmbAction     = s_lmbAction;
        settings.rmbAction     = s_rmbAction;
        settings.dialogueCam   = s_dialogueCam;
        settings.slowKey       = static_cast<std::uint32_t>(s_slowKey);
        settings.disableActivate = s_disableActivate;
        settings.disableShift  = s_disableShift;
        settings.disableSpace  = s_disableSpace;
        settings.raceMenuCam   = s_raceMenuCam;

        if (!exists) {
            CreateDefaultINI(ini);
        }
        SKSE::log::info("FreeCamMenu: settings {} {} - freeFlyKey=0x{:X} resetKey=0x{:X} Disable Space={} Disable Shift={} "
                        "Disable Activate={} Block attacks={} Hide HUD={} speed={:.1f}",
            exists ? "loaded from" : "(defaults) for new file", ini, s_freeFlyKey, s_resetKey, s_disableSpace,
            s_disableShift, s_disableActivate, s_blockAttacks, s_hideHUD, s_cameraSpeed);
    }

    static bool __stdcall OnSMFInput(RE::InputEvent* a_event) {
        if (!s_capturing.load()) return false;

        for (auto* evt = a_event; evt; evt = evt->next) {
            if (evt->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
            auto* btn = evt->AsButtonEvent();
            if (!btn || !btn->IsDown()) continue;
            if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;

            int code = static_cast<int>(btn->GetIDCode());
            if (code == 0x01) {
                CancelCapture();
            } else {
                OnKeyCaptured(code);
            }
            return true;
        }
        return false;
    }

    void ApplyGameSettings() {
        auto* ini = RE::INISettingCollection::GetSingleton();
        if (ini) {
            auto* setting = ini->GetSetting("fFreeCameraTranslationSpeed:Camera");
            if (setting) {
                setting->data.f = s_cameraSpeed;
                SKSE::log::info("FreeCamMenu: camera speed set to {:.1f}", s_cameraSpeed);
            } else {
                SKSE::log::warn("FreeCamMenu: fFreeCameraTranslationSpeed:Camera not found");
            }
        }
    }

    void Register() {
        if (!SKSEMenuFramework::IsInstalled()) {
            SKSE::log::error("FreeCamMenu: SKSE Menu Framework not found");
            return;
        }

        SKSEMenuFramework::SetSection("FreeCam");
        SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
        SKSEMenuFramework::AddSectionItem("Cell Entry", RenderCellEntry);  // 0.9.0
        SKSEMenuFramework::AddSectionItem("Scene Camera", RenderSceneCam);  // 0.10.0
        SKSEMenuFramework::AddInputEvent(OnSMFInput);

        SKSE::log::info("FreeCamMenu: settings section registered");
    }
}
