#include "FreeCamMenu.h"
#include "FovFx.h"
#include "FreeCamController.h"
#include "FreezeTime.h"
#include "CameraLight.h"
#include "HUDHider.h"
#include "CellEntryCam.h"
#include "AimDot.h"
#include "CamBlur.h"
#include "CamPath.h"
#include "SceneCam.h"
#include "AmbientCam.h"
#include "ConvoCam.h"
#include "CamCollision.h"
#include "SceneShots.h"

#include <RE/I/INISettingCollection.h>

#include "SKSEMenuFramework.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
    static int   s_fastKey       = 0;      // 0.14.0: hold-to-go-fast key (0 = off)
    static float s_fastMult      = 4.0f;   // 0.14.0
    static bool  s_smoothFov     = true;   // 0.14.0: eased FOV changes
    static float s_fovSmoothTime = 0.12f;  // 0.14.0
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
    static void WriteAmbientValues();
    static void WriteCamToolsValues();
    static void WriteCollisionValues();
    static void WriteConvoValues();

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
        WriteINIInt("Hotkeys", "iFastKey", s_fastKey);
        WriteINIFloat("Camera", "fFastMultiplier", s_fastMult);
        WriteINIInt("FOV", "bSmooth", s_smoothFov ? 1 : 0);
        WriteINIFloat("FOV", "fSmoothTime", s_fovSmoothTime);
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
        WriteAmbientValues();
        WriteCollisionValues();
        WriteConvoValues();
        WriteCamToolsValues();
    }

    // 0.17.0 background blur + camera path, TFCam.ini [Blur] / [CameraPath].
    static void WriteCamToolsValues() {
        const auto& b = CamBlur::Get();
        WriteINIInt("Blur", "bEnabled", b.enabled ? 1 : 0);
        WriteINIFloat("Blur", "fStrength", b.strength);
        WriteINIInt("Blur", "bInFreeFly", b.inFreeFly ? 1 : 0);
        WriteINIInt("Blur", "iStyle", b.style);  // 0.18.0: 0 edges (TFCam's own), 1 ENB depth of field
        WriteINIFloat("Blur", "fSharpMiddle", b.sharp);
        const auto& f = FovFx::Get();  // 0.18.0
        WriteINIInt("ActionFOV", "bEnabled", f.enabled ? 1 : 0);
        WriteINIFloat("ActionFOV", "fStrength", f.strength);
        WriteINIInt("ActionFOV", "bBow", f.bow ? 1 : 0);
        WriteINIInt("ActionFOV", "bPowerAttacks", f.power ? 1 : 0);
        WriteINIInt("ActionFOV", "bSpells", f.spells ? 1 : 0);
        WriteINIInt("ActionFOV", "iSpellLevel", f.spellLevel);
        WriteINIInt("ActionFOV", "bShouts", f.shouts ? 1 : 0);
        WriteINIInt("ActionFOV", "bSpeed", f.speedPlayer ? 1 : 0);
        WriteINIInt("ActionFOV", "bCameraSpeed", f.speedCamera ? 1 : 0);
        WriteINIInt("ActionFOV", "bArcherCamera", f.aimCam ? 1 : 0);
        WriteINIFloat("ActionFOV", "fArcherTurn", f.aimTurn);
        WriteINIFloat("ActionFOV", "fArcherDistance", f.aimBack);
        WriteINIFloat("ActionFOV", "fArcherUp", f.aimUp);
        WriteINIInt("ActionFOV", "bArcherDot", f.aimDot ? 1 : 0);
        WriteINIFloat("ActionFOV", "fArcherDotSize", f.aimDotSize);
        WriteINIInt("ActionFOV", "bArcherBlur", f.aimBlur ? 1 : 0);
        WriteINIFloat("ActionFOV", "fArcherVignette", f.aimVignette);
        WriteINIInt("AimDot", "bHitTheDot", AimDot::Get().enabled ? 1 : 0);
        const auto& c = CamPath::Get();
        WriteINIInt("CameraPath", "bCurved", c.curved ? 1 : 0);
        WriteINIInt("CameraPath", "bCut", c.snap ? 1 : 0);
        WriteINIFloat("CameraPath", "fDuration", c.duration);
        WriteINIInt("CameraPath", "iLoop", c.loop);
        WriteINIInt("CameraPath", "iLook", c.look);
        WriteINIInt("CameraPath", "iFocus", c.focus);
        WriteINIFloat("CameraPath", "fFocusHeight", c.focusHeight);
        WriteINIInt("CameraPath", "bFollowFocus", c.followFocus ? 1 : 0);
        WriteINIInt("CameraPath", "iAddPointKey", c.addKey);
        WriteINIInt("CameraPath", "iPlayKey", c.playKey);
    }

    // 0.16.0 conversation camera, TFCam.ini [Conversation].
    static void WriteConvoValues() {
        const auto& c = ConvoCam::Get();
        WriteINIInt("Conversation", "bEnabled", c.enabled ? 1 : 0);
        WriteINIInt("Conversation", "bThirdPersonOnly", c.thirdPersonOnly ? 1 : 0);
        WriteINIFloat("Conversation", "fBlendIn", c.blendIn);
        WriteINIFloat("Conversation", "fBlendOut", c.blendOut);
        WriteINIFloat("Conversation", "fShotSwitch", c.shotSwitch);
        WriteINIFloat("Conversation", "fMinShotSeconds", c.minShotSec);
        WriteINIFloat("Conversation", "fDistance", c.distance);
        WriteINIFloat("Conversation", "fShoulder", c.shoulder);
        WriteINIFloat("Conversation", "fHeight", c.height);
        WriteINIFloat("Conversation", "fLookShift", c.lookShift);
        WriteINIFloat("Conversation", "fFovNpc", c.fovNpc);
        WriteINIFloat("Conversation", "fFovWide", c.fovWide);
        WriteINIInt("Conversation", "bAlternate", c.alternate ? 1 : 0);
        WriteINIInt("Conversation", "iReverseKind", c.reverseKind);
        WriteINIInt("Conversation", "bSwapSide", c.swapSide ? 1 : 0);
        WriteINIFloat("Conversation", "fDrift", c.drift);
        WriteINIInt("Conversation", "bNoEdgePan", c.noEdgePan ? 1 : 0);
    }

    // 0.12.0 walls for the automatic cameras, TFCam.ini [Collision].
    static void WriteCollisionValues() {
        const auto& c = CamCollision::Get();
        WriteINIInt("Collision", "bEnabled", c.enabled ? 1 : 0);
        WriteINIFloat("Collision", "fMargin", c.margin);
        WriteINIFloat("Collision", "fReturnSeconds", c.returnSec);
        WriteINIFloat("Collision", "fApproachSeconds", c.approachSec);
        WriteINIFloat("Collision", "fIgnoreSmallerThan", c.ignoreSmall);
        WriteINIInt("Collision", "bPassLargeItems", c.passLarge ? 1 : 0);
        WriteINIInt("Collision", "bPassWalls", c.passWalls ? 1 : 0);
        WriteINIFloat("Collision", "fLargeItemRadius", c.largeItem);
        WriteINIInt("Collision", "iLayer", c.layer);
    }

    // 0.11.0 Idle + Furniture camera, TFCam.ini [Idle] / [Furniture].
    static void WriteAmbientValues() {
        const auto& a = AmbientCam::Get();
        WriteINIInt("Idle", "bEnabled", a.vanityEnabled ? 1 : 0);
        WriteINIFloat("Idle", "fStartDelay", a.vanityStart);
        WriteINIFloat("Idle", "fDuration", a.vanityDuration);
        WriteINIInt("Idle", "bHandOffSandbox", a.handOffSandbox ? 1 : 0);
        WriteINIInt("Idle", "bDolly", a.dolly ? 1 : 0);
        WriteINIInt("Idle", "bSpiral", a.spiral ? 1 : 0);
        WriteINIFloat("Idle", "fCrossfade", a.crossfade);
        WriteINIFloat("Idle", "fReturnTime", a.returnTime);
        WriteINIFloat("Idle", "fInteriorScale", a.interiorScale);
        WriteINIFloat("Idle", "fDollyFraming", a.dollyFraming);
        WriteINIFloat("Idle", "fDollyMinDistance", a.dollyMinDist);
        WriteINIFloat("Idle", "fDollyNarrowFOV", a.dollyNarrowFOV);
        WriteINIFloat("Idle", "fDollyWideFOV", a.dollyWideFOV);
        WriteINIFloat("Idle", "fDollyPeriod", a.dollyPeriod);
        WriteINIInt("Idle", "iDollyCycles", a.dollyCycles);
        WriteINIFloat("Idle", "fDollyCameraHeight", a.dollyCamHeight);
        WriteINIFloat("Idle", "fDollySpin", a.dollySpin);
        WriteINIFloat("Idle", "fSpiralFar", a.spiralFar);
        WriteINIFloat("Idle", "fSpiralClose", a.spiralClose);
        WriteINIFloat("Idle", "fSpiralFOV", a.spiralFOV);
        WriteINIFloat("Idle", "fSpiralAreaSeconds", a.spiralAreaSec);
        WriteINIFloat("Idle", "fSpiralSpeed", a.spiralSpeed);
        WriteINIInt("Furniture", "bEnabled", a.furnEnabled ? 1 : 0);
        WriteINIFloat("Furniture", "fDelay", a.furnDelay);
        WriteINIInt("Furniture", "bMoveCancels", a.furnMoveCancels ? 1 : 0);
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
        WriteINIFloat("SceneCam", "fOrbitAim", sc.orbitAim);
        WriteINIFloat("SceneCam", "fOrbitSway", sc.orbitSway);
        WriteINIInt("SceneCam", "iOrbitTarget", sc.orbitTarget);
        WriteINIFloat("SceneCam", "fOrbitRoll", sc.orbitRoll);
        WriteINIFloat("SceneCam", "fOrbitRollSpeed", sc.orbitRollSpeed);
        WriteINIFloat("SceneCam", "fEyeFOV", sc.eyeFOV);
        WriteINIFloat("SceneCam", "fEyeNearClip", sc.eyeNearClip);
        WriteINIFloat("SceneCam", "fEyeForward", sc.eyeForward);
        WriteINIInt("SceneCam", "bEyeHideHead", sc.eyeHideHead ? 1 : 0);
        WriteINIInt("SceneCam", "bAutoStageShots", sc.autoStageShots ? 1 : 0);
        WriteINIInt("SceneCam", "bAutoPoseShots", sc.autoPoseShots ? 1 : 0);
        WriteINIInt("SceneCam", "bAutoEnterShots", sc.autoEnterShots ? 1 : 0);
        WriteINIFloat("SceneCam", "fShotBlend", sc.shotBlend);
    }

    void SaveSceneCamSettings() {
        s_saveWriteOk  = true;
        s_saveWriteErr = 0;
        WriteSceneCamValues();
        const auto& sc = SceneCam::GetConfig();
        if (s_saveWriteOk) {
            SKSE::log::info("FreeCamMenu: Scene Camera settings saved - radius {:.0f}, tilt {:.0f}, speed {:.0f}, orbit FOV "
                            "{:.0f}, eye FOV {:.0f}, aim {:+.0f}, sway {:.0f}, orbit target {}, automatic free camera {}",
                sc.orbitRadius, sc.orbitHeight, sc.orbitSpeed, sc.orbitFOV, sc.eyeFOV, sc.orbitAim, sc.orbitSway,
                sc.orbitTarget, sc.autoEnterShots ? 1 : 0);
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
        settings.fastKey       = static_cast<std::uint32_t>(s_fastKey);
        settings.fastMultiplier = s_fastMult;
        settings.smoothFov     = s_smoothFov;
        settings.fovSmoothTime = s_fovSmoothTime;
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

        if (KeyBindField("fastKey", "Fast movement (hold)", &s_fastKey)) {
            ApplyToController();
            SaveINI();
        }
        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::SliderFloat("Fast speed (times)##fastMult", &s_fastMult, 1.5f, 10.0f, "%.1f")) {
            ApplyToController();
            SaveINI();
        }
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Hold to fly faster. Unset by default. Ctrl + mouse wheel changes the base speed while flying.");

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

        if (ImGuiMCP::Checkbox("Smooth FOV changes##smoothFov", &s_smoothFov)) {
            ApplyToController();
            SaveINI();
        }
        if (s_smoothFov) {
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(150.0f);
            if (ImGuiMCP::SliderFloat("Smoothing time (s)##fovSmoothTime", &s_fovSmoothTime, 0.02f, 0.6f, "%.2f")) {
                ApplyToController();
                SaveINI();
            }
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

        // 0.17.0 background blur (every TFCam camera, kept on this page so it is found)
        {
            auto&      bl       = CamBlur::Get();
            const auto bs       = CamBlur::GetStatus();
            bool       bChanged = false;
            const bool enbStyle = bl.style == CamBlur::kStyleEnb;
            ImGuiMCP::SeparatorText("Background blur");
            // 0.18.0: two styles; the edge blur (TFCam's own image pass) is the default - the ENB depth of field blurred
            // everything past the subject and could not go strong enough.
            static const char* const kBlurStyles[] = { "Blurry round the edge, sharp in the middle (TFCam's own)",
                "ENB's depth of field (sharp at the subject's distance)" };
            ImGuiMCP::SetNextItemWidth(360.0f);
            bChanged |= ImGuiMCP::Combo("How##blStyle", &bl.style, kBlurStyles, 2, -1);
            ImGuiMCP::BeginDisabled(enbStyle && !bs.linked);
            bChanged |= ImGuiMCP::Checkbox("Blur the background##blOn", &bl.enabled);
            ImGuiMCP::SetNextItemWidth(200.0f);
            bChanged |= ImGuiMCP::SliderFloat("Strength##blStrength", &bl.strength, 0.0f, 1.0f, "%.2f");
            if (!enbStyle) {
                ImGuiMCP::SetNextItemWidth(200.0f);
                bChanged |= ImGuiMCP::SliderFloat("Sharp middle (how far out)##blSharp", &bl.sharp, 0.05f, 0.9f, "%.2f");
            }
            bChanged |= ImGuiMCP::Checkbox("Also in plain free-fly (sharp: the middle of the screen)##blFree", &bl.inFreeFly);
            ImGuiMCP::EndDisabled();
            if (enbStyle && bs.error[0]) {
                ImGuiMCP::TextColored({ 0.9f, 0.45f, 0.35f, 1.0f }, "%s", bs.error);
            } else if (bs.active) {
                if (enbStyle && bs.distance > 0.0f) {
                    ImGuiMCP::Text("Now: sharp at %s, %.0f units away", bs.what, bs.distance);
                } else {
                    ImGuiMCP::Text("Now: %s", bs.what);
                }
            }
            if (enbStyle) {
                ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
                    "Keeps the subject sharp and blurs what is behind it while orbit, preset view, eye view, a camera path, the "
                    "idle camera or the conversation camera moves the camera. It uses ENB's depth of field, and ENB gets its own "
                    "values back the moment that camera stops (nothing is saved into your ENB preset). Needs ENB.");
            } else {
                ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
                    "While orbit, preset view, eye view, a camera path, the idle camera or the conversation camera moves the "
                    "camera: the picture blurs more and more toward the screen's edge and stays sharp in the middle, on the "
                    "subject. TFCam's own image pass, before the HUD - no ENB needed. Strength 1 is a wash at the edge.");
            }
            if (bChanged) {
                CamBlur::Clamp();
                SaveINI();
            }
        }

        // Status (a snapshot the game updates every frame while something is going on).
        const auto  ms = SceneCam::GetMenuStatus();
        const auto& st = ms.status;
        static const char* modeNames[] = { "Off (TFCam is not driving a free camera)", "Free-fly", "Orbit", "Eye view",
            "Preset view" };
        ImGuiMCP::Text("Mode: %s", modeNames[st.mode <= 4 ? st.mode : 0]);
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
        changed |= ImGuiMCP::SliderFloat("Tilt (degrees above)##scHeight", &sc.orbitHeight, -20.0f, 75.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Speed (degrees/s)##scSpeed", &sc.orbitSpeed, -90.0f, 90.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("FOV##scOrbitFov", &sc.orbitFOV, 20.0f, 120.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Aim height (units)##scAim", &sc.orbitAim, -60.0f, 60.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Sway (degrees)##scSway", &sc.orbitSway, 0.0f, 15.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Roll (degrees)##scRoll", &sc.orbitRoll, -90.0f, 90.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Roll speed (degrees/s)##scRollSpeed", &sc.orbitRollSpeed, -90.0f, 90.0f, "%.0f");
        {
            static const char* targets[] = { "Auto (scene actors, else the posed actor, else you)", "You only",
                "The reference picked in the console (else auto)" };
            int t = std::clamp(sc.orbitTarget, 0, 2);
            ImGuiMCP::SetNextItemWidth(320.0f);
            if (ImGuiMCP::Combo("Orbit round##scTarget", &t, targets, 3)) {
                sc.orbitTarget = t;
                changed        = true;
            }
        }
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "A negative speed orbits the other way. Aim height moves the point the orbit circles and looks at up or down "
            "(the tilt stays the angle the camera looks down at). Sway: how far the tilt slowly rises and falls (0 = "
            "none). Roll tilts the picture (a Dutch angle); Roll speed keeps it turning while it orbits (0 = it stays at "
            "the roll). While orbiting: mouse wheel = FOV, Shift + wheel = radius, mouse up / down tilts the orbit (up to a "
            "top view, down to looking up from below, never under the floor). WASD does nothing. Console target: open the "
            "console, click an NPC or object, close the console - the orbit circles it (a walking NPC is followed).");

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
        changed |= ImGuiMCP::Checkbox("Turn the free camera on for a saved shot##scAutoEnter", &sc.autoEnterShots);
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Blend time (s)##scBlend", &sc.shotBlend, 0.0f, 3.0f, "%.1f");
        static const std::string s_shotFile = SceneShots::FilePath();
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f },
            "Automatic moves only happen in free-fly (not in orbit or eye view). With 'Turn the free camera on' a stage "
            "with a saved shot (and an NPC's pose with one) turns TFCam free-fly on by itself, once nothing is open "
            "(SLUI, console, dialogue, a paused menu); leave the free camera during a scene and it stays off for the "
            "rest of that scene. A free camera turned on for an NPC's pose goes off again when the pose ends. Your own "
            "poses never turn it on. Shots are kept per stage and your position slot. File: %s", s_shotFile.c_str());

        if (changed) {
            SceneCam::OnConfigChanged();
            SaveINI();
        }
    }

    // 0.17.0: FreeCam > Camera Path page.
    static void __stdcall RenderCamPath() {
        auto&                  cp      = CamPath::Get();
        bool                   changed = false;
        const ImGuiMCP::ImVec4 grey{ 0.5f, 0.5f, 0.5f, 1.0f };
        const auto             ms = CamPath::GetMenuStatus();

        ImGuiMCP::SeparatorText("Camera path");
        ImGuiMCP::TextColored(grey,
            "Fly the free camera to a spot and add it as a point (A, B, C ...), then the next spot, and so on. Play moves "
            "the camera through the points - during a SexLab scene, a pose, or anywhere else.");
        ImGuiMCP::Text("Points: %d%s", ms.points, ms.playing ? "   - playing" : "");
        if (ms.playing) {
            ImGuiMCP::Text("Progress: %.0f%%   (%.0f units along the path)", ms.progress * 100.0f, ms.length);
            ImGuiMCP::Text("Focus: %s", ms.focus);
        }
        if (ms.lastEvent[0]) ImGuiMCP::TextColored(grey, "Last: %s", ms.lastEvent);
        ImGuiMCP::BeginDisabled(!ms.freeCam);
        if (ImGuiMCP::Button("Add point (where the camera is)##cpAdd")) CamPath::RequestAddPoint();
        ImGuiMCP::EndDisabled();
        ImGuiMCP::SameLine();
        ImGuiMCP::BeginDisabled(ms.points == 0);
        if (ImGuiMCP::Button("Remove last##cpDel")) CamPath::RequestRemoveLast();
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Clear##cpClear")) CamPath::RequestClear();
        ImGuiMCP::EndDisabled();
        ImGuiMCP::SameLine();
        ImGuiMCP::BeginDisabled(ms.points < 2 && !ms.playing);
        if (ImGuiMCP::Button(ms.playing ? "Stop##cpPlay" : "Play##cpPlay")) CamPath::RequestPlay(ms.playing ? 0 : 1);
        ImGuiMCP::EndDisabled();
        if (!ms.freeCam) ImGuiMCP::TextColored(grey, "Turn the free camera on to add points.");

        ImGuiMCP::SeparatorText("Movement");
        {
            static const char* shapes[] = { "Curved through the points", "Straight lines (eases in and out at each point)" };
            int k = cp.curved ? 0 : 1;
            ImGuiMCP::SetNextItemWidth(320.0f);
            if (ImGuiMCP::Combo("Path##cpShape", &k, shapes, 2)) {
                cp.curved = k == 0;
                changed   = true;
            }
            static const char* moves[] = { "Glide", "Cut from point to point" };
            k = cp.snap ? 1 : 0;
            ImGuiMCP::SetNextItemWidth(320.0f);
            if (ImGuiMCP::Combo("Move##cpMove", &k, moves, 2)) {
                cp.snap = k == 1;
                changed = true;
            }
            ImGuiMCP::SetNextItemWidth(200.0f);
            changed |= ImGuiMCP::SliderFloat("Time for the whole path (s)##cpTime", &cp.duration, 1.0f, 120.0f, "%.0f");
            static const char* loops[] = { "Once", "Loop", "Back and forth" };
            k = std::clamp(cp.loop, 0, 2);
            ImGuiMCP::SetNextItemWidth(200.0f);
            if (ImGuiMCP::Combo("Repeat##cpLoop", &k, loops, 3)) {
                cp.loop = k;
                changed = true;
            }
        }

        ImGuiMCP::SeparatorText("Look");
        {
            static const char* looks[] = { "At the focus", "The way each point looked (no focus: straight pans and dollies)" };
            int k = std::clamp(cp.look, 0, 1);
            ImGuiMCP::SetNextItemWidth(420.0f);
            if (ImGuiMCP::Combo("Camera looks##cpLook", &k, looks, 2)) {
                cp.look = k;
                changed = true;
            }
            static const char* focus[] = { "You (your SexLab scene's actors when in one)",
                "The reference picked in the console (else you)" };
            k = std::clamp(cp.focus, 0, 1);
            ImGuiMCP::SetNextItemWidth(420.0f);
            if (ImGuiMCP::Combo("Focus##cpFocus", &k, focus, 2)) {
                cp.focus = k;
                changed  = true;
            }
            ImGuiMCP::SetNextItemWidth(200.0f);
            changed |= ImGuiMCP::SliderFloat("Aim height (units)##cpAim", &cp.focusHeight, -60.0f, 60.0f, "%.0f");
            changed |= ImGuiMCP::Checkbox("The path moves with the focus##cpFollow", &cp.followFocus);
        }
        ImGuiMCP::TextColored(grey,
            "Roll (Q / E) and FOV (mouse wheel) are kept with each point. Console focus: open the console, click an NPC or "
            "object, close it - a walking NPC is followed. 'The path moves with the focus' keeps the points relative to the "
            "focus, so the path follows a moving NPC and works again for the same pose somewhere else. Background blur "
            "(Scene Camera page) keeps the focus sharp while a path plays.");

        ImGuiMCP::SeparatorText("Hotkeys");
        ImGuiMCP::TextColored(grey, "Unset until you bind them.");
        changed |= KeyBindField("cpAddKey", "Add point", &cp.addKey);
        changed |= KeyBindField("cpPlayKey", "Play / stop", &cp.playKey);

        if (changed) {
            CamPath::Clamp();
            SaveINI();
        }
    }

    // 0.18.0: FreeCam > Action FOV page.
    static void __stdcall RenderActionFov() {
        auto&                  f       = FovFx::Get();
        bool                   changed = false;
        const ImGuiMCP::ImVec4 grey{ 0.5f, 0.5f, 0.5f, 1.0f };

        ImGuiMCP::SeparatorText("Action FOV");
        ImGuiMCP::TextWrapped("The view reacts to what you do: drawing a bow pulls it in, letting the arrow go snaps it out, "
                              "a power attack punches in, big spells and shouts warp it out, and moving fast stretches it wide.");
        changed |= ImGuiMCP::Checkbox("On##afOn", &f.enabled);
        ImGuiMCP::SetNextItemWidth(220.0f);
        changed |= ImGuiMCP::SliderFloat("How strong##afStrength", &f.strength, 0.0f, 2.0f, "%.1f", 0);
        changed |= ImGuiMCP::Checkbox("Bow: drawing pulls in, letting go snaps out##afBow", &f.bow);
        changed |= ImGuiMCP::Checkbox("Archer camera: close round your head while the string is drawn##afAim", &f.aimCam);
        if (f.aimCam && ImGuiMCP::TreeNode("Archer camera position##afAimTree")) {
            ImGuiMCP::SetNextItemWidth(220.0f);
            changed |= ImGuiMCP::SliderFloat("Round to your face (0 behind, 90 side-on)##afAimTurn", &f.aimTurn, -110.0f, 110.0f, "%.0f", 0);
            ImGuiMCP::SetNextItemWidth(220.0f);
            changed |= ImGuiMCP::SliderFloat("Distance from your head##afAimBack", &f.aimBack, 10.0f, 400.0f, "%.0f", 0);
            ImGuiMCP::SetNextItemWidth(220.0f);
            changed |= ImGuiMCP::SliderFloat("Up / down##afAimUp", &f.aimUp, -80.0f, 80.0f, "%.0f", 0);
            ImGuiMCP::TextColored(grey, "The middle of the screen is where the arrow will land: a line from the arrow tip "
                                        "along your aim. Minus turns go round your left side.");
            changed |= ImGuiMCP::Checkbox("A tiny dot where the arrow will land##afAimDot", &f.aimDot);
            if (f.aimDot) {
                ImGuiMCP::SetNextItemWidth(220.0f);
                changed |= ImGuiMCP::SliderFloat("Dot size (pixels at 1080p)##afAimDotSize", &f.aimDotSize, 0.5f, 8.0f, "%.1f", 0);
            }
            {
                // TFCam's own "hit where you aim" - one switch, no menu
                auto& ad       = AimDot::Get();
                bool  adChange = ImGuiMCP::Checkbox("Arrows and spells go where the dot is (TFCam's own aim)##afHitDot", &ad.enabled);
                ImGuiMCP::TextColored(grey, "Aim: %s", AimDot::Status());
                changed |= adChange;
            }
            changed |= ImGuiMCP::Checkbox("Blur round the edge of the screen while the string is drawn##afAimBlur", &f.aimBlur);
            ImGuiMCP::SetNextItemWidth(220.0f);
            changed |= ImGuiMCP::SliderFloat("Vignette (darker corners) while the string is drawn##afAimVig", &f.aimVignette, 0.0f, 1.0f, "%.2f", 0);
            if (f.aimBlur) {
                ImGuiMCP::TextColored(grey, "Strength and the sharp middle come from Background blur on the Scene page "
                                            "(the \"blurry round the edge\" style).");
            }
            ImGuiMCP::TreePop();
        }
        changed |= ImGuiMCP::Checkbox("Power attacks punch in##afPower", &f.power);
        changed |= ImGuiMCP::Checkbox("Big spells warp out##afSpells", &f.spells);
        if (f.spells) {
            static const char* const kLevels[] = { "Novice", "Apprentice", "Adept", "Expert", "Master" };
            int idx = std::clamp(f.spellLevel / 25, 0, 4);
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(160.0f);
            if (ImGuiMCP::Combo("and up##afLevel", &idx, kLevels, 5, -1)) {
                f.spellLevel = idx * 25;
                changed      = true;
            }
        }
        changed |= ImGuiMCP::Checkbox("Shouts warp out##afShouts", &f.shouts);
        changed |= ImGuiMCP::Checkbox("Moving fast stretches it (sprint, Whirlwind Sprint, a fast horse)##afSpeed", &f.speedPlayer);
        changed |= ImGuiMCP::Checkbox("The free camera's speed stretches it (flying, camera paths)##afCamSpeed", &f.speedCamera);
        ImGuiMCP::TextColored(grey, "Works on top of any FOV you or other mods set. Off by default.");
        if (changed) {
            FovFx::Clamp();
            SaveINI();
        }
    }

    // 0.11.0: FreeCam > Idle & Furniture page.
    static void __stdcall RenderAmbient() {
        auto& a       = AmbientCam::Get();
        bool  changed = false;
        const ImGuiMCP::ImVec4 grey{ 0.5f, 0.5f, 0.5f, 1.0f };

        const auto ms = AmbientCam::GetMenuStatus();
        ImGuiMCP::SeparatorText("Idle camera");
        ImGuiMCP::TextColored(grey,
            "After a while without input the camera circles you: a dolly zoom (the camera moves in while the lens zooms out, "
            "so you keep your size and the world stretches) and a spiral that opens on your face, drops to your feet and climbs back up to your face, swelling in and out "
            "on each area. Any key, click or mouse move gives the camera back.");
        ImGuiMCP::Text("Now: %s", ms.state[0] ? ms.state : "-");
        if (ms.routine[0]) ImGuiMCP::Text("Routine: %s", ms.routine);

        changed |= ImGuiMCP::Checkbox("Idle camera on##amOn", &a.vanityEnabled);
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Starts after (s without input)##amStart", &a.vanityStart, 5.0f, 300.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Runs for (s, 0 = until input)##amDur", &a.vanityDuration, 0.0f, 600.0f, "%.0f");
        changed |= ImGuiMCP::Checkbox("Then let Sandbox When Idle start##amSwi", &a.handOffSandbox);
        if (!ms.swiLoaded) {
            ImGuiMCP::TextColored(grey, "po3's Sandbox When Idle is not installed - nothing to hand over to.");
        } else {
            ImGuiMCP::TextColored(grey,
                "When the time is up TFCam gives the camera back and sets the engine's idle timer so Sandbox When Idle starts "
                "straight away. Engine idle timer: %s", ms.timer);
        }
        ImGuiMCP::TextColored(grey,
            "Keep 'starts after' + 'runs for' under the game's auto-vanity delay (fAutoVanityModeDelay, 120 s by default).");

        ImGuiMCP::SeparatorText("Routines (played in order, then again)");
        changed |= ImGuiMCP::Checkbox("Dolly zoom##amDolly", &a.dolly);
        ImGuiMCP::SameLine();
        changed |= ImGuiMCP::Checkbox("Spiral up the body##amSpiral", &a.spiral);
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Blend between routines (s)##amXf", &a.crossfade, 0.0f, 5.0f, "%.1f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Back to the normal camera (s)##amRet", &a.returnTime, 0.0f, 3.0f, "%.1f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Distances in interiors##amInt", &a.interiorScale, 0.3f, 1.0f, "%.2f");
        ImGuiMCP::TextColored(grey, "Smaller circles indoors. Walls are handled below.");

        // 0.12.0
        ImGuiMCP::SeparatorText("Walls");
        ImGuiMCP::TextColored(grey,
            "The idle camera, the Scene Camera orbit and the blends to a saved shot (scene, pose, furniture) stop in front "
            "of walls and ease back out when the way is clear. People are passed through. Your own free-fly is never stopped.");
        {
            auto& c        = CamCollision::Get();
            bool  wChanged = false;
            wChanged |= ImGuiMCP::Checkbox("Stop at walls##cwOn", &c.enabled);
            ImGuiMCP::SetNextItemWidth(200.0f);
            wChanged |= ImGuiMCP::SliderFloat("Distance kept from a wall##cwMargin", &c.margin, 4.0f, 60.0f, "%.0f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            wChanged |= ImGuiMCP::SliderFloat("Ease back out (s)##cwRet", &c.returnSec, 0.1f, 3.0f, "%.1f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            wChanged |= ImGuiMCP::SliderFloat("Ease in to a wall (s)##cwApp", &c.approachSec, 0.0f, 2.0f, "%.2f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            wChanged |= ImGuiMCP::SliderFloat("Ignore objects smaller than##cwSmall", &c.ignoreSmall, 0.0f, 200.0f, "%.0f");
            ImGuiMCP::TextColored(grey, "Sconces, books and things on a table are not walls. 0 = everything counts.");
            wChanged |= ImGuiMCP::Checkbox("Pass through large items (furniture, big props)##cwLarge", &c.passLarge);
            wChanged |= ImGuiMCP::Checkbox("Pass through walls (floors and ceilings still stop it)##cwWalls", &c.passWalls);
            ImGuiMCP::TextColored(grey, "Walls: %s", CamCollision::MenuStatus().c_str());
            if (wChanged) {
                CamCollision::Clamp();
                changed = true;
            }
        }

        ImGuiMCP::SeparatorText("Dolly zoom");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Framing (units visible at you)##amFrame", &a.dollyFraming, 40.0f, 400.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Closest distance to you##amMinDist", &a.dollyMinDist, 40.0f, 300.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Narrow FOV (camera far)##amNarrow", &a.dollyNarrowFOV, 10.0f, 60.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Wide FOV (camera close)##amWide", &a.dollyWideFOV, 50.0f, 130.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Seconds per in-and-out##amPeriod", &a.dollyPeriod, 2.0f, 30.0f, "%.1f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderInt("In-and-outs before the next routine##amCycles", &a.dollyCycles, 1, 10);
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Camera height above the ground##amDH", &a.dollyCamHeight, 0.0f, 200.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Speed round you (degrees/s)##amDSpin", &a.dollySpin, -90.0f, 90.0f, "%.0f");
        ImGuiMCP::TextColored(grey, "Camera distance = framing / (2 tan(FOV / 2)): your size stays the same while the FOV sweeps.");
        ImGuiMCP::TextColored(grey, "It looks up at your face from that height, and rises only when it comes too close to stay low.");

        ImGuiMCP::SeparatorText("Spiral");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Far radius (between areas)##amFar", &a.spiralFar, 60.0f, 500.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Close radius (on an area)##amClose", &a.spiralClose, 25.0f, 200.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("FOV##amSpFov", &a.spiralFOV, 30.0f, 110.0f, "%.0f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Seconds per area##amArea", &a.spiralAreaSec, 0.8f, 10.0f, "%.1f");
        {
            bool other = a.spiralSpeed < 0.0f;
            if (ImGuiMCP::Checkbox("Go round the other way##amDir", &other)) {
                a.spiralSpeed = other ? -40.0f : 40.0f;
                changed       = true;
            }
        }
        ImGuiMCP::TextColored(grey, "Opens on your face, pulls back, drops to the floor and comes in; then floor to face in two loops:");
        ImGuiMCP::TextColored(grey, "bum (from behind), genitals, breasts, face (from the front). Seconds per area = pace of the whole path.");

        ImGuiMCP::Separator();
        if (ImGuiMCP::Button("Test the idle camera (runs when this menu closes)##amTest")) {
            AmbientCam::TestVanityOnMenuClose();
        }

        ImGuiMCP::SeparatorText("Furniture camera");
        ImGuiMCP::TextColored(grey,
            "A camera shot saved per furniture. Sit down, turn on free-fly, fly to where you want the camera, press the "
            "Scene Camera 'Save camera shot' key (FreeCam > Scene Camera). From then on, sitting on that furniture "
            "blends the camera there.");
        changed |= ImGuiMCP::Checkbox("Furniture camera on##amFurnOn", &a.furnEnabled);
        ImGuiMCP::SetNextItemWidth(200.0f);
        changed |= ImGuiMCP::SliderFloat("Delay after sitting down (s)##amFurnDelay", &a.furnDelay, 0.0f, 6.0f, "%.1f");
        changed |= ImGuiMCP::Checkbox("Movement / jump / activate keys give the camera back##amFurnMove", &a.furnMoveCancels);
        if (ms.furniture[0]) {
            ImGuiMCP::Text("Sitting on: %s", ms.furniture);
            ImGuiMCP::Text("Saved shot for it: %s", ms.furnHasShot ? "yes" : "no");
        } else {
            ImGuiMCP::TextColored(grey, "Not on furniture.");
        }
        if (ms.lastEvent[0]) ImGuiMCP::TextColored(grey, "Last: %s", ms.lastEvent);

        // 0.16.0
        {
            auto& cv = ConvoCam::Get();
            bool  cvChanged = false;
            const auto cs = ConvoCam::GetMenuStatus();
            ImGuiMCP::SeparatorText("Conversation camera");
            ImGuiMCP::TextColored(grey,
                "While you talk to someone the camera frames the conversation like a film: over your shoulder while the NPC "
                "speaks, the reverse shot while it is your turn. Every move is damped, so it glides and never cuts or races. "
                "When the conversation ends it eases back to the game's camera.");
            ImGuiMCP::Text("Now: %s", cs.state[0] ? cs.state : "-");
            if (cs.shot[0]) ImGuiMCP::Text("Shot: %s", cs.shot);
            if (cs.speaker[0]) ImGuiMCP::Text("Speaking with: %s", cs.speaker);
            cvChanged |= ImGuiMCP::Checkbox("Conversation camera on##cvOn", &cv.enabled);
            cvChanged |= ImGuiMCP::Checkbox("Third person only##cvTp", &cv.thirdPersonOnly);
            cvChanged |= ImGuiMCP::Checkbox("Reverse shot while it is your turn##cvAlt", &cv.alternate);
            bool twoShot = cv.reverseKind == 1;
            if (ImGuiMCP::Checkbox("Wide two-shot instead of over the NPC's shoulder##cvKind", &twoShot)) {
                cv.reverseKind = twoShot ? 1 : 0;
                cvChanged      = true;
            }
            cvChanged |= ImGuiMCP::Checkbox("Other shoulder##cvSide", &cv.swapSide);
            cvChanged |= ImGuiMCP::Checkbox("No mouse edge-pan in any dialogue (the camera turns it off while it films)##cvEdge", &cv.noEdgePan);
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("Settle on the first shot (s)##cvIn", &cv.blendIn, 0.3f, 6.0f, "%.1f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("Change of shot takes (s)##cvSw", &cv.shotSwitch, 0.3f, 6.0f, "%.1f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("A shot stays at least (s)##cvMin", &cv.minShotSec, 0.5f, 15.0f, "%.1f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("Back to the game's camera (s)##cvOut", &cv.blendOut, 0.0f, 6.0f, "%.1f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("Distance behind the near speaker##cvDist", &cv.distance, 40.0f, 400.0f, "%.0f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("Sideways (shoulder)##cvSh", &cv.shoulder, -100.0f, 100.0f, "%.0f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("Height above the eyes##cvH", &cv.height, -40.0f, 80.0f, "%.0f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("Framed speaker off centre##cvLs", &cv.lookShift, -80.0f, 80.0f, "%.0f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("FOV over the shoulder##cvF1", &cv.fovNpc, 20.0f, 100.0f, "%.0f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("FOV two-shot##cvF2", &cv.fovWide, 20.0f, 110.0f, "%.0f");
            ImGuiMCP::SetNextItemWidth(200.0f);
            cvChanged |= ImGuiMCP::SliderFloat("Handheld sway##cvDr", &cv.drift, 0.0f, 3.0f, "%.1f");
            if (cs.lastEvent[0]) ImGuiMCP::TextColored(grey, "Last: %s", cs.lastEvent);
            if (cvChanged) {
                ConvoCam::Clamp();
                changed = true;
            }
        }

        if (changed) {
            AmbientCam::Clamp();
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
          << "; 1 = the wheel eases the FOV in instead of jumping; fSmoothTime = seconds (0.02 .. 0.6)\nbSmooth="
          << (s_smoothFov ? 1 : 0) << "\nfSmoothTime=" << fl(s_fovSmoothTime) << "\n"
          << "; FOV range limits\nfMin=" << fl(s_fovMin) << "\nfMax=" << fl(s_fovMax) << "\n\n";
        f << "[Roll]\n; Radians per second while key held\nfSpeed=" << fl(s_rollSpeed) << "\n"
          << "; Q = roll counter-clockwise (0x10 = 16)\niKeyCCW=" << s_rollCCWKey << "\n"
          << "; E = roll clockwise (0x12 = 18)\niKeyCW=" << s_rollCWKey << "\n\n";
        f << "[Hotkeys]\n; DX scancode for free fly toggle (0 = unmapped)\niFreeFlyKey=" << s_freeFlyKey << "\n"
          << "; R = reset FOV + roll (0x13 = 19)\niResetKey=" << s_resetKey << "\n"
          << "; Keyboard shortcuts for freeze time / screenshot (0 = unmapped: Shift+Middle Click / Middle Click)\n"
          << "iFreezeTimeKey=" << s_freezeTimeKey << "\niScreenshotKey=" << s_screenshotKey << "\n"
          << "; Hold to fly at 1/5 speed. DX scancode, 0x38 = Left Alt (56). 0 = off\niSlowKey=" << s_slowKey << "\n"
          << "; Hold to fly faster (fFastMultiplier times the speed). DX scancode. 0 = off\niFastKey=" << s_fastKey << "\n\n";
        f << "[Camera]\nfSpeed=" << fl(s_cameraSpeed) << "\n; 1.5 .. 10\nfFastMultiplier=" << fl(s_fastMult)
          << "\nbHideHUD=" << (s_hideHUD ? 1 : 0)
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
          << "\n; Orbit: radius 30..500 units, height = tilt -20..75 degrees, speed -90..90 degrees/s (sign = direction), FOV 20..120\n"
          << "fOrbitRadius=" << fl(sc.orbitRadius) << "\nfOrbitHeight=" << fl(sc.orbitHeight)
          << "\nfOrbitSpeed=" << fl(sc.orbitSpeed) << "\nfOrbitFOV=" << fl(sc.orbitFOV)
          << "\n; Orbit aim -60..60 units (point circled / looked at, up or down), sway 0..15 degrees (tilt breathing),\n"
             "; target 0 = auto (scene actors, else the posed actor, else you), 1 = you only\n"
          << "fOrbitAim=" << fl(sc.orbitAim) << "\nfOrbitSway=" << fl(sc.orbitSway) << "\niOrbitTarget=" << sc.orbitTarget
          << "\n; Eye view: FOV 30..130, near clip 1..10, camera in front of the eyes -5..10, 1 = hide the partner's head\n"
          << "fEyeFOV=" << fl(sc.eyeFOV) << "\nfEyeNearClip=" << fl(sc.eyeNearClip) << "\nfEyeForward=" << fl(sc.eyeForward)
          << "\nbEyeHideHead=" << (sc.eyeHideHead ? 1 : 0)
          << "\n; 1 = blend to a saved shot at scene start / stage change, and when a pose starts (free-fly only)\n"
          << "bAutoStageShots=" << (sc.autoStageShots ? 1 : 0) << "\nbAutoPoseShots=" << (sc.autoPoseShots ? 1 : 0)
          << "\n; 1 = a stage (or an NPC's pose) with a saved shot turns the free camera on by itself\n"
          << "bAutoEnterShots=" << (sc.autoEnterShots ? 1 : 0)
          << "\n; seconds, 0..3\nfShotBlend=" << fl(sc.shotBlend) << "\n";
        f.flush();
        if (!f) {
            SKSE::log::error("FreeCamMenu: writing the new {} failed part-way", a_path);
            return;
        }
        SKSE::log::info("FreeCamMenu: first run - created {} from the code defaults", a_path);
    }

    void SetCameraSpeed(float a_speed) {
        if (std::isfinite(a_speed)) s_cameraSpeed = std::clamp(a_speed, 0.5f, 50.0f);
        SaveINI();
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
        s_fastKey      = readInt("Hotkeys", "iFastKey", 0);
        s_fastMult     = readFloat("Camera", "fFastMultiplier", 4.0f);
        s_smoothFov    = readInt("FOV", "bSmooth", 1) != 0;
        s_fovSmoothTime = readFloat("FOV", "fSmoothTime", 0.12f);
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
            sc.orbitAim       = readFloat("SceneCam", "fOrbitAim", def.orbitAim);
            sc.orbitSway      = readFloat("SceneCam", "fOrbitSway", def.orbitSway);
            sc.orbitTarget    = readInt("SceneCam", "iOrbitTarget", def.orbitTarget);
            sc.orbitRoll      = readFloat("SceneCam", "fOrbitRoll", def.orbitRoll);
            sc.orbitRollSpeed = readFloat("SceneCam", "fOrbitRollSpeed", def.orbitRollSpeed);
            sc.eyeFOV         = readFloat("SceneCam", "fEyeFOV", def.eyeFOV);
            sc.eyeNearClip    = readFloat("SceneCam", "fEyeNearClip", def.eyeNearClip);
            sc.eyeForward     = readFloat("SceneCam", "fEyeForward", def.eyeForward);
            sc.eyeHideHead    = readInt("SceneCam", "bEyeHideHead", def.eyeHideHead ? 1 : 0) != 0;
            sc.autoStageShots = readInt("SceneCam", "bAutoStageShots", def.autoStageShots ? 1 : 0) != 0;
            sc.autoPoseShots  = readInt("SceneCam", "bAutoPoseShots", def.autoPoseShots ? 1 : 0) != 0;
            sc.autoEnterShots = readInt("SceneCam", "bAutoEnterShots", def.autoEnterShots ? 1 : 0) != 0;
            sc.shotBlend      = readFloat("SceneCam", "fShotBlend", def.shotBlend);
            SceneCam::ClampConfig();
        }

        // 0.11.0 Idle + Furniture camera (an existing TFCam.ini without these sections gets the code defaults).
        {
            auto&                      a = AmbientCam::Get();
            const AmbientCam::Settings d{};
            a.vanityEnabled   = readInt("Idle", "bEnabled", d.vanityEnabled ? 1 : 0) != 0;
            a.vanityStart     = readFloat("Idle", "fStartDelay", d.vanityStart);
            a.vanityDuration  = readFloat("Idle", "fDuration", d.vanityDuration);
            a.handOffSandbox  = readInt("Idle", "bHandOffSandbox", d.handOffSandbox ? 1 : 0) != 0;
            a.dolly           = readInt("Idle", "bDolly", d.dolly ? 1 : 0) != 0;
            a.spiral          = readInt("Idle", "bSpiral", d.spiral ? 1 : 0) != 0;
            a.crossfade       = readFloat("Idle", "fCrossfade", d.crossfade);
            a.returnTime      = readFloat("Idle", "fReturnTime", d.returnTime);
            a.interiorScale   = readFloat("Idle", "fInteriorScale", d.interiorScale);
            a.dollyFraming    = readFloat("Idle", "fDollyFraming", d.dollyFraming);
            a.dollyMinDist    = readFloat("Idle", "fDollyMinDistance", d.dollyMinDist);
            a.dollyNarrowFOV  = readFloat("Idle", "fDollyNarrowFOV", d.dollyNarrowFOV);
            a.dollyWideFOV    = readFloat("Idle", "fDollyWideFOV", d.dollyWideFOV);
            a.dollyPeriod     = readFloat("Idle", "fDollyPeriod", d.dollyPeriod);
            a.dollyCycles     = readInt("Idle", "iDollyCycles", d.dollyCycles);
            a.dollyCamHeight  = readFloat("Idle", "fDollyCameraHeight", d.dollyCamHeight);
            a.dollySpin       = readFloat("Idle", "fDollySpin", d.dollySpin);
            a.spiralFar       = readFloat("Idle", "fSpiralFar", d.spiralFar);
            a.spiralClose     = readFloat("Idle", "fSpiralClose", d.spiralClose);
            a.spiralFOV       = readFloat("Idle", "fSpiralFOV", d.spiralFOV);
            a.spiralAreaSec   = readFloat("Idle", "fSpiralAreaSeconds", d.spiralAreaSec);
            a.spiralSpeed     = readFloat("Idle", "fSpiralSpeed", d.spiralSpeed);
            a.furnEnabled     = readInt("Furniture", "bEnabled", d.furnEnabled ? 1 : 0) != 0;
            a.furnDelay       = readFloat("Furniture", "fDelay", d.furnDelay);
            a.furnMoveCancels = readInt("Furniture", "bMoveCancels", d.furnMoveCancels ? 1 : 0) != 0;
            AmbientCam::Clamp();
        }

        // 0.16.0 conversation camera (code defaults when the section is missing).
        {
            auto&                      c = ConvoCam::Get();
            const ConvoCam::Settings   d{};
            c.enabled         = readInt("Conversation", "bEnabled", d.enabled ? 1 : 0) != 0;
            c.thirdPersonOnly = readInt("Conversation", "bThirdPersonOnly", d.thirdPersonOnly ? 1 : 0) != 0;
            c.blendIn         = readFloat("Conversation", "fBlendIn", d.blendIn);
            c.blendOut        = readFloat("Conversation", "fBlendOut", d.blendOut);
            c.shotSwitch      = readFloat("Conversation", "fShotSwitch", d.shotSwitch);
            c.minShotSec      = readFloat("Conversation", "fMinShotSeconds", d.minShotSec);
            c.distance        = readFloat("Conversation", "fDistance", d.distance);
            c.shoulder        = readFloat("Conversation", "fShoulder", d.shoulder);
            c.height          = readFloat("Conversation", "fHeight", d.height);
            c.lookShift       = readFloat("Conversation", "fLookShift", d.lookShift);
            c.fovNpc          = readFloat("Conversation", "fFovNpc", d.fovNpc);
            c.fovWide         = readFloat("Conversation", "fFovWide", d.fovWide);
            c.alternate       = readInt("Conversation", "bAlternate", d.alternate ? 1 : 0) != 0;
            c.reverseKind     = readInt("Conversation", "iReverseKind", d.reverseKind);
            c.swapSide        = readInt("Conversation", "bSwapSide", d.swapSide ? 1 : 0) != 0;
            c.drift           = readFloat("Conversation", "fDrift", d.drift);
            c.noEdgePan       = readInt("Conversation", "bNoEdgePan", d.noEdgePan ? 1 : 0) != 0;
            ConvoCam::Clamp();
            SKSE::log::info("ConvoCam: conversation camera {} - {} shot, settle {:.1f} s, shot change {:.1f} s",
                c.enabled ? "on" : "off", c.reverseKind == 1 ? "two-shot reverse" : "over-the-shoulder reverse", c.blendIn, c.shotSwitch);
        }

        // 0.17.0 background blur + camera path (code defaults when the sections are missing).
        {
            auto&                  b = CamBlur::Get();
            const CamBlur::Settings bd{};
            b.enabled   = readInt("Blur", "bEnabled", bd.enabled ? 1 : 0) != 0;
            b.strength  = readFloat("Blur", "fStrength", bd.strength);
            b.inFreeFly = readInt("Blur", "bInFreeFly", bd.inFreeFly ? 1 : 0) != 0;
            b.style     = readInt("Blur", "iStyle", bd.style);
            b.sharp     = readFloat("Blur", "fSharpMiddle", bd.sharp);
            CamBlur::Clamp();
            auto&                  c = CamPath::Get();
            const CamPath::Settings cd{};
            c.curved      = readInt("CameraPath", "bCurved", cd.curved ? 1 : 0) != 0;
            c.snap        = readInt("CameraPath", "bCut", cd.snap ? 1 : 0) != 0;
            c.duration    = readFloat("CameraPath", "fDuration", cd.duration);
            c.loop        = readInt("CameraPath", "iLoop", cd.loop);
            c.look        = readInt("CameraPath", "iLook", cd.look);
            c.focus       = readInt("CameraPath", "iFocus", cd.focus);
            c.focusHeight = readFloat("CameraPath", "fFocusHeight", cd.focusHeight);
            c.followFocus = readInt("CameraPath", "bFollowFocus", cd.followFocus ? 1 : 0) != 0;
            c.addKey      = readInt("CameraPath", "iAddPointKey", cd.addKey);
            c.playKey     = readInt("CameraPath", "iPlayKey", cd.playKey);
            CamPath::Clamp();
            auto&                 af = FovFx::Get();  // 0.18.0
            const FovFx::Settings ad{};
            af.enabled     = readInt("ActionFOV", "bEnabled", ad.enabled ? 1 : 0) != 0;
            af.strength    = readFloat("ActionFOV", "fStrength", ad.strength);
            af.bow         = readInt("ActionFOV", "bBow", ad.bow ? 1 : 0) != 0;
            af.power       = readInt("ActionFOV", "bPowerAttacks", ad.power ? 1 : 0) != 0;
            af.spells      = readInt("ActionFOV", "bSpells", ad.spells ? 1 : 0) != 0;
            af.spellLevel  = readInt("ActionFOV", "iSpellLevel", ad.spellLevel);
            af.shouts      = readInt("ActionFOV", "bShouts", ad.shouts ? 1 : 0) != 0;
            af.speedPlayer = readInt("ActionFOV", "bSpeed", ad.speedPlayer ? 1 : 0) != 0;
            af.speedCamera = readInt("ActionFOV", "bCameraSpeed", ad.speedCamera ? 1 : 0) != 0;
            af.aimCam      = readInt("ActionFOV", "bArcherCamera", ad.aimCam ? 1 : 0) != 0;
            af.aimTurn     = readFloat("ActionFOV", "fArcherTurn", ad.aimTurn);
            af.aimBack     = readFloat("ActionFOV", "fArcherDistance", ad.aimBack);
            af.aimUp       = readFloat("ActionFOV", "fArcherUp", ad.aimUp);
            af.aimDot      = readInt("ActionFOV", "bArcherDot", ad.aimDot ? 1 : 0) != 0;
            af.aimDotSize  = readFloat("ActionFOV", "fArcherDotSize", ad.aimDotSize);
            af.aimBlur     = readInt("ActionFOV", "bArcherBlur", ad.aimBlur ? 1 : 0) != 0;
            af.aimVignette = readFloat("ActionFOV", "fArcherVignette", ad.aimVignette);
            AimDot::Get().enabled = readInt("AimDot", "bHitTheDot", AimDot::Settings{}.enabled ? 1 : 0) != 0;
            FovFx::Clamp();
            SKSE::log::info("FovFx: action FOV {}", af.enabled ? "on" : "off");
            SKSE::log::info("CamBlur: background blur {} ({}, strength {:.2f}, sharp middle {:.2f}{})", b.enabled ? "on" : "off",
                b.style == CamBlur::kStyleEnb ? "ENB depth of field" : "edges", b.strength, b.sharp,
                b.inFreeFly ? ", also in free-fly" : "");
        }

        // 0.12.0 walls for the automatic cameras (code defaults when the section is missing).
        {
            auto&                        c = CamCollision::Get();
            const CamCollision::Settings d{};
            c.enabled   = readInt("Collision", "bEnabled", d.enabled ? 1 : 0) != 0;
            c.margin    = readFloat("Collision", "fMargin", d.margin);
            c.returnSec = readFloat("Collision", "fReturnSeconds", d.returnSec);
            c.approachSec = readFloat("Collision", "fApproachSeconds", d.approachSec);
            c.ignoreSmall = readFloat("Collision", "fIgnoreSmallerThan", d.ignoreSmall);
            c.passLarge   = readInt("Collision", "bPassLargeItems", d.passLarge ? 1 : 0) != 0;
            c.passWalls   = readInt("Collision", "bPassWalls", d.passWalls ? 1 : 0) != 0;
            c.largeItem   = readFloat("Collision", "fLargeItemRadius", d.largeItem);
            c.layer     = readInt("Collision", "iLayer", d.layer);
            CamCollision::Clamp();
            SKSE::log::info("CamCollision: walls {} for the automatic cameras - margin {:.0f}, ease in {:.2f} s, ease out {:.1f} s, ray layer {}",
                c.enabled ? "on" : "off", c.margin, c.approachSec, c.returnSec, c.layer);
        }

        // 0.13.1: a hand-edited TFCam.ini could hold values the menu sliders never allow (or text that is not a number).
        // std::clamp with fMin > fMax is undefined, so the FOV limits are fixed first. Ranges are the sliders' ranges.
        {
            auto range = [](float& v, float lo, float hi, float def) {
                const float before = v;
                v = std::isfinite(v) ? std::clamp(v, lo, hi) : def;
                return v != before;
            };
            bool fixed = false;
            fixed |= range(fovMin, 1.0f, 170.0f, 10.0f);
            fixed |= range(fovMax, 2.0f, 179.0f, 150.0f);
            if (fovMin >= fovMax) {
                fovMin = 10.0f;
                fovMax = 150.0f;
                fixed  = true;
            }
            s_fovMin = fovMin;
            s_fovMax = fovMax;
            fixed |= range(s_cameraSpeed, 0.5f, 50.0f, 14.5f);
            fixed |= range(s_fovStep, 0.5f, 10.0f, 5.0f);
            fixed |= range(s_rollSpeed, 0.1f, 5.0f, 1.6f);
            fixed |= range(s_lightRadius, 100.0f, 5000.0f, 1000.0f);
            fixed |= range(s_lightFade, 0.1f, 10.0f, 1.7f);
            const int r0 = s_lightColorR, g0 = s_lightColorG, b0 = s_lightColorB;
            s_lightColorR = std::clamp(s_lightColorR, 0, 255);
            s_lightColorG = std::clamp(s_lightColorG, 0, 255);
            s_lightColorB = std::clamp(s_lightColorB, 0, 255);
            fixed |= (r0 != s_lightColorR || g0 != s_lightColorG || b0 != s_lightColorB);
            fixed |= range(s_fastMult, 1.5f, 10.0f, 4.0f);
            fixed |= range(s_fovSmoothTime, 0.02f, 0.6f, 0.12f);
            s_fastKey = std::clamp(s_fastKey, 0, 255);
            s_lmbAction = std::clamp(s_lmbAction, 0, 10);  // the Combo has 11 entries
            s_rmbAction = std::clamp(s_rmbAction, 0, 10);

            auto& ce = CellEntryCam::GetSettings();
            const CellEntryCam::Settings ced{};
            fixed |= range(ce.zoom, -0.2f, 1.0f, ced.zoom);
            fixed |= range(ce.zoomSpeed, 0.1f, 5.0f, ced.zoomSpeed);
            fixed |= range(ce.rotation, 0.0f, 360.0f, ced.rotation);
            fixed |= range(ce.rotationSpeed, 10.0f, 360.0f, ced.rotationSpeed);
            fixed |= range(ce.startDelay, 0.0f, 3.0f, ced.startDelay);
            if (fixed) {
                SKSE::log::warn("FreeCamMenu: some values in {} were outside the range the menu allows (or not a number) "
                                "and were brought back inside it", ini);
            }
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
        settings.fastKey       = static_cast<std::uint32_t>(s_fastKey);
        settings.fastMultiplier = s_fastMult;
        settings.smoothFov     = s_smoothFov;
        settings.fovSmoothTime = s_fovSmoothTime;
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
        SKSEMenuFramework::AddSectionItem("Idle & Furniture", RenderAmbient);  // 0.11.0
        SKSEMenuFramework::AddSectionItem("Camera Path", RenderCamPath);       // 0.17.0
        SKSEMenuFramework::AddSectionItem("Action FOV", RenderActionFov);      // 0.18.0
        SKSEMenuFramework::AddInputEvent(OnSMFInput);

        SKSE::log::info("FreeCamMenu: settings section registered");
    }
}
