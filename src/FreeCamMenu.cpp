#include "FreeCamMenu.h"
#include "FreeCamController.h"
#include "FreezeTime.h"
#include "CameraLight.h"
#include "HUDHider.h"

#include <RE/I/INISettingCollection.h>

#include "SKSEMenuFramework.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>

namespace FreeCamMenu {

    static int   s_freeFlyKey    = 0;
    static int   s_rollCCWKey    = 0x10;   // Q
    static int   s_rollCWKey     = 0x12;   // E
    static int   s_resetKey      = 0x13;   // R
    static int   s_freezeTimeKey = 0;
    static int   s_screenshotKey = 0;
    static float s_rollSpeed     = 1.5f;
    static float s_fovStep       = 2.0f;
    static float s_cameraSpeed   = 10.0f;  // game default fFreeCameraTranslationSpeed
    static bool  s_hideHUD       = false;
    static bool  s_blockAttacks  = true;
    static int   s_lmbAction     = 0;
    static int   s_rmbAction     = 0;
    static bool  s_dialogueCam   = false;  // --Claude: free-cam movement during dialogue

    // Camera light settings
    static bool  s_lightScrollBrightness = true;
    static bool  s_lightScrollRadius     = true;
    static float s_lightRadius    = 1000.0f;
    static float s_lightFade      = 1.7f;
    static int   s_lightColorR    = 255;
    static int   s_lightColorG    = 255;
    static int   s_lightColorB    = 255;

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

    static void WriteINIFloat(const char* section, const char* key, float val) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", val);
        WritePrivateProfileStringA(section, key, buf, GetINIPath());
    }

    static void WriteINIInt(const char* section, const char* key, int val) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", val);
        WritePrivateProfileStringA(section, key, buf, GetINIPath());
    }

    static void SaveINI() {
        WriteINIInt("Hotkeys", "iFreeFlyKey", s_freeFlyKey);
        WriteINIInt("Hotkeys", "iResetKey", s_resetKey);
        WriteINIInt("Hotkeys", "iFreezeTimeKey", s_freezeTimeKey);
        WriteINIInt("Hotkeys", "iScreenshotKey", s_screenshotKey);
        WriteINIFloat("Camera", "fSpeed", s_cameraSpeed);
        WriteINIInt("Camera", "bHideHUD", s_hideHUD ? 1 : 0);
        WriteINIInt("Camera", "bBlockAttacks", s_blockAttacks ? 1 : 0);
        WriteINIInt("Camera", "bDialogueCamera", s_dialogueCam ? 1 : 0);
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

        // --Claude: allow the free camera to move during conversations. The game
        // freezes free-cam movement while the Dialogue Menu is up; this drives it
        // manually. WASD to move, PageUp/PageDown to rise/descend, hold Left Alt +
        // mouse to look (release Alt to click dialogue options).
        if (ImGuiMCP::Checkbox("Camera movement in dialogue##dlgcam", &s_dialogueCam)) {
            ApplyToController();
            SaveINI();
        }
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "WASD move, PageUp/PageDown up/down.");
        ImGuiMCP::TextColored({ 0.85f, 0.75f, 0.35f, 1.0f },
            "You must HOLD LEFT ALT to look with the mouse.");
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "(Release Alt to free the mouse for dialogue options.)");

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

    void LoadSettings() {
        const char* ini = GetINIPath();

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
        s_rollSpeed  = readFloat("Roll", "fSpeed", 1.5f);

        s_fovStep = readFloat("FOV", "fStep", 2.0f);
        float fovMin = readFloat("FOV", "fMin", 10.0f);
        float fovMax = readFloat("FOV", "fMax", 150.0f);

        s_cameraSpeed = readFloat("Camera", "fSpeed", 10.0f);
        s_hideHUD     = readInt("Camera", "bHideHUD", 0) != 0;
        s_blockAttacks = readInt("Camera", "bBlockAttacks", 1) != 0;
        s_dialogueCam  = readInt("Camera", "bDialogueCamera", 0) != 0;
        s_lmbAction   = readInt("Camera", "iLMBAction", 0);
        s_rmbAction   = readInt("Camera", "iRMBAction", 0);
        HUDHider::SetEnabled(s_hideHUD);

        // Camera light settings
        s_lightScrollBrightness = readInt("Light", "bScrollBrightness", 1) != 0;
        s_lightScrollRadius     = readInt("Light", "bScrollRadius", 1) != 0;
        s_lightRadius    = readFloat("Light", "fRadius", 1000.0f);
        s_lightFade      = readFloat("Light", "fFade", 1.7f);
        s_lightColorR    = readInt("Light", "iColorR", 255);
        s_lightColorG    = readInt("Light", "iColorG", 255);
        s_lightColorB    = readInt("Light", "iColorB", 255);

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

        SKSE::log::info("FreeCamMenu: loaded — freeFlyKey=0x{:X} resetKey=0x{:X}",
            s_freeFlyKey, s_resetKey);
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
        SKSEMenuFramework::AddInputEvent(OnSMFInput);

        SKSE::log::info("FreeCamMenu: settings section registered");
    }
}
