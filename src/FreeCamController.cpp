#include "FreeCamController.h"
#include "FreezeTime.h"
#include "FreeCamMenu.h"
#include "CameraLight.h"
#include "HUDHider.h"
#include "FcfwBridge.h"
#include "SlccBridge.h"
#include "SceneTracker.h"

#include <RE/I/INISettingCollection.h>
#include <RE/A/AttackBlockHandler.h>
#include <RE/M/MouseMoveEvent.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <Windows.h>

namespace FreeCam {

    static Settings s_settings;
    static float    s_rollAngle = 0.0f;
    static float    s_baseFOV   = 0.0f;
    static bool     s_altSlow   = false;  // Alt slow-mode active
    static bool     s_slowHeld  = false;  // configurable slow key currently held (fed by the input sink)
    // FreeCameraState field offsets (capture + freeze the transform).
    static constexpr std::ptrdiff_t kOff_translation       = 0x30; // NiPoint3
    static constexpr std::ptrdiff_t kOff_rotation          = 0x3C; // float[2] x=pitch,y=yaw
    static constexpr std::ptrdiff_t kOff_zUpDown           = 0x44;
    static constexpr std::ptrdiff_t kOff_verticalDirection = 0x4C;

    // Menu-exit camera restore: other mods (e.g. Show-Player-In-Inventory) reposition
    // the camera on menu open and RESET it on menu close — which yanks the free cam
    // back onto the player. We continuously save the live free-cam transform (while no
    // menu is up) and re-assert it the frame AFTER a menu closes, so we win that fight.
    static RE::NiPoint3  s_menuSaveTrans{0.0f, 0.0f, 0.0f};
    static float         s_menuSaveRot[2] = {0.0f, 0.0f};  // pitch, yaw
    static float         s_menuSaveFOV = 0.0f;
    static bool          s_menuRestorePending = false;
    static bool          s_menuSaveValid = false;  // have we captured a real position this session?

    // 0.7.7: this free-cam session was begun by TFCam / vanilla tfc (no FCFW timeline at Begin).
    static bool          s_sessionDriven = false;

    // 0.7.7 SLCC hand-over: pose to start the next TFCam-driven session from (applied on its first Update).
    static bool          s_entryPosePending = false;
    static RE::NiPoint3  s_entryPos{0.0f, 0.0f, 0.0f};
    static float         s_entryPitch = 0.0f;
    static float         s_entryYaw   = 0.0f;

    static void SetCameraSpeed(float speed) {
        auto* ini = RE::INISettingCollection::GetSingleton();
        if (!ini) return;
        auto* setting = ini->GetSetting("fFreeCameraTranslationSpeed:Camera");
        if (setting) {
            setting->data.f = speed;
        }
    }

    // ---- 0.7.9 diagnostics + scripted-tfc guard state ---------------------------------------------------------
    // The Begin/End hooks do NOT always run on the main thread: PapyrusUtil's MiscUtil.ToggleFreeCamera (SexLab P+,
    // Prism) and ConsoleUtil.ExecuteCommand("tfc") (Poser Hotkeys Plus) call into the camera from Papyrus VM threads
    // (proven 2026-09-26: SKSEMenuFramework.log shows TFCam's HUD RestoreAll on threads 7528/19496/25188, main = 7960).
    // Everything below is safe to touch from any thread.
    static std::atomic<std::uint32_t> s_mainThreadId{ 0 };

    // Last key-down TFCam's input sink saw on the keyboard / gamepad (written on the main thread by the sink).
    struct LastPress {
        std::uint32_t code      = 0;
        int           device    = -1;
        char          event[32] = {};
        std::int64_t  qpc       = 0;
    };
    static std::mutex                s_lastPressLock;
    static LastPress                 s_lastPress;
    static std::atomic<std::int64_t> s_lastJumpQpc{ 0 };       // last press of the Jump key (user event or mapped key)
    static std::atomic<bool>         s_scriptTfcSession{ false };  // this free cam was opened by a script's console tfc

    static std::int64_t QpcNow() {
        LARGE_INTEGER t;
        ::QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    static double MsSince(std::int64_t a_qpc) {
        static const double freq = [] {
            LARGE_INTEGER f;
            ::QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();
        return static_cast<double>(QpcNow() - a_qpc) * 1000.0 / freq;
    }

    void NoteMainThread() {
        s_mainThreadId = static_cast<std::uint32_t>(::GetCurrentThreadId());
    }

    std::string ThreadTag() {
        const auto id   = static_cast<std::uint32_t>(::GetCurrentThreadId());
        const auto main = s_mainThreadId.load();
        if (main == 0) return std::format("thread {}", id);
        return id == main ? std::format("thread {} (main)", id)
                          : std::format("thread {} (NOT the main thread: a Papyrus VM thread or another worker)", id);
    }

    // Who called into the camera, as module+offset return addresses (SkyrimSE.exe offsets map to Address Library
    // IDs offline). A few microseconds per free cam toggle; never used per frame. The first entry is TFCam's own hook.
    __declspec(noinline) static std::string CallerChain(unsigned long a_frames) {
        void*       frames[32] = {};
        const auto  count      = ::CaptureStackBackTrace(1, (std::min)(a_frames, 32ul), frames, nullptr);
        std::string out;
        for (unsigned short i = 0; i < count; ++i) {
            const auto  addr = reinterpret_cast<std::uintptr_t>(frames[i]);
            HMODULE     mod  = nullptr;
            std::string entry;
            if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(frames[i]), &mod) && mod) {
                wchar_t           path[MAX_PATH] = {};
                const DWORD       len            = ::GetModuleFileNameW(mod, path, MAX_PATH);
                std::wstring_view full(path, len);
                const auto        slash = full.find_last_of(L"\\/");
                const auto        base  = slash == std::wstring_view::npos ? full : full.substr(slash + 1);
                for (const wchar_t c : base) entry.push_back(c < 0x80 ? static_cast<char>(c) : '?');
                entry += std::format("+0x{:X}", addr - reinterpret_cast<std::uintptr_t>(mod));
            } else {
                entry = std::format("0x{:X}", addr);
            }
            if (!out.empty()) out += " < ";
            out += entry;
        }
        return out.empty() ? std::string("(no frames)") : out;
    }

    static std::string DescribeLastPress() {
        LastPress p;
        {
            std::lock_guard lock(s_lastPressLock);
            p = s_lastPress;
        }
        if (p.qpc == 0) return "none seen yet";
        const char* dev = p.device == static_cast<int>(RE::INPUT_DEVICE::kKeyboard) ? "keyboard" :
                          p.device == static_cast<int>(RE::INPUT_DEVICE::kGamepad)  ? "gamepad" : "device";
        return std::format("{} 0x{:X} '{}' {:.0f} ms ago", dev, p.code, static_cast<const char*>(p.event), MsSince(p.qpc));
    }

    // Input sink, main thread, before any eat below can clear the user event.
    static void RecordPress(RE::ButtonEvent* a_btn, RE::INPUT_DEVICE a_device, std::uint32_t a_code) {
        const auto  now = QpcNow();
        const auto& ue  = a_btn->QUserEvent();
        {
            std::lock_guard lock(s_lastPressLock);
            s_lastPress.code   = a_code;
            s_lastPress.device = static_cast<int>(a_device);
            std::snprintf(s_lastPress.event, sizeof(s_lastPress.event), "%s", ue.c_str() ? ue.c_str() : "");
            s_lastPress.qpc = now;
        }
        auto* events = RE::UserEvents::GetSingleton();
        bool  jump   = events && ue == events->jump;
        if (!jump && a_device == RE::INPUT_DEVICE::kKeyboard) {
            // Poser Hotkeys Plus tests the raw key against Input.GetMappedKey("Jump"), whatever the context maps it to.
            if (auto* map = RE::ControlMap::GetSingleton()) {
                const auto mapped = map->GetMappedKey("Jump"sv, RE::INPUT_DEVICE::kKeyboard);
                jump              = mapped != 0xFF && a_code == mapped;
            }
        }
        if (jump) s_lastJumpQpc = now;
    }

    bool JumpPressedWithin(double a_seconds, double& a_msAgo) {
        const auto t = s_lastJumpQpc.load();
        if (t == 0) {
            a_msAgo = -1.0;
            return false;
        }
        a_msAgo = MsSince(t);
        return a_msAgo >= 0.0 && a_msAgo <= a_seconds * 1000.0;
    }

    void NoteScriptTfcOpenedSession() { s_scriptTfcSession = true; }
    bool SessionOpenedByScriptTfc() { return s_scriptTfcSession.load(); }

    // Poser Hotkeys Plus' DisableFreeCam runs MiscUtil.SetFreeCameraSpeed(10) right before its tfc; when that tfc is
    // refused, put TFCam's own speed back.
    void ReapplyCameraSpeed() {
        const float speed = FreeCamMenu::GetCameraSpeed();
        SetCameraSpeed(s_altSlow ? speed / 5.0f : speed);
    }

    static LARGE_INTEGER s_qpcFreq = {};
    static LARGE_INTEGER s_qpcLast = {};
    static float         s_frameDt = 0.016f;

    // --Claude: accumulated mouse delta for dialogue free-cam look. Written by the
    // input sink (main thread), consumed+cleared every frame by the Update hook.
    static float s_dlgMouseDX = 0.0f;
    static float s_dlgMouseDY = 0.0f;

    static void UpdateFrameTimer() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (s_qpcLast.QuadPart != 0 && s_qpcFreq.QuadPart != 0) {
            s_frameDt = static_cast<float>(now.QuadPart - s_qpcLast.QuadPart)
                      / static_cast<float>(s_qpcFreq.QuadPart);
            if (s_frameDt > 0.1f) s_frameDt = 0.016f;
        }
        s_qpcLast = now;
    }

    Settings& GetSettings() { return s_settings; }

    bool IsActive() {
        auto* cam = RE::PlayerCamera::GetSingleton();
        return cam && cam->IsInFreeCameraMode();
    }

    bool TFCamDriving() {
        return IsActive() && !FcfwBridge::FcfwOwnsCamera();
    }

    bool CaptureFreeCamPose(RE::NiPoint3& a_pos, float& a_pitch, float& a_yaw) {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || !cam->IsInFreeCameraMode()) return false;
        auto* state = cam->currentState.get();
        if (!state) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(state);
        a_pos = *reinterpret_cast<RE::NiPoint3*>(base + kOff_translation);
        const float* rot = reinterpret_cast<float*>(base + kOff_rotation);
        a_pitch = rot[0];
        a_yaw   = rot[1];
        const bool ok = std::isfinite(a_pos.x) && std::isfinite(a_pos.y) && std::isfinite(a_pos.z) &&
                        std::isfinite(a_pitch) && std::isfinite(a_yaw) &&
                        (a_pos.x != 0.0f || a_pos.y != 0.0f || a_pos.z != 0.0f);
        SKSE::log::info("FreeCam: hand-over pose captured ({:.1f}, {:.1f}, {:.1f}) pitch={:.3f} yaw={:.3f}{}",
            a_pos.x, a_pos.y, a_pos.z, a_pitch, a_yaw, ok ? "" : " - rejected");
        return ok;
    }

    void SetEntryPose(const RE::NiPoint3& a_pos, float a_pitch, float a_yaw) {
        s_entryPos         = a_pos;
        s_entryPitch       = a_pitch;
        s_entryYaw         = a_yaw;
        s_entryPosePending = true;
    }

    void ClearEntryPose() {
        s_entryPosePending = false;
    }

    float GetRollDegrees() {
        return s_rollAngle * (180.0f / 3.14159265f);
    }

    static void ResetCamera() {
        if (s_baseFOV > 0.0f) {
            if (auto* cam = RE::PlayerCamera::GetSingleton())
                cam->worldFOV = s_baseFOV;
        }
        s_rollAngle = 0.0f;
    }

    void ResetAll() {
        SKSE::log::info("FreeCam: ResetAll() — roll={:.2f} baseFOV={:.1f}", s_rollAngle, s_baseFOV);
        ResetCamera();
    }

    // 0.7.9: reset while an FCFW timeline (SLCC) drives the camera - roll only, the FOV stays SLCC's.
    static void ResetRoll() {
        SKSE::log::info("FreeCam: roll reset ({:.2f} rad) - FCFW timeline {} keeps its FOV", s_rollAngle,
            FcfwBridge::ActiveTimelineID());
        s_rollAngle = 0.0f;
    }

    // --- FreeCameraState::GetRotation hook ---

    struct GetRotationHook {
        static void thunk(RE::TESCameraState* a_this, RE::NiQuaternion& a_rotation) {
            func(a_this, a_rotation);

            // 0.7.9: the roll also applies on top of an FCFW (SLCC) camera. Checked in FCFW's source (8b4df64):
            // TimelineManager::Update rewrites the state's translation + pitch/yaw and its roll (a write_call on
            // FromEulerAnglesZXY INSIDE this vanilla GetRotation, 49814/50744+0x1B) from the timeline every frame, and
            // only ever reads back the state's pitch/yaw fields and its own roll variable. SLCC hooks the same call
            // site and keeps its own post-layer rotation (its log: baseRot/finalRot + roll). This multiply touches
            // only the returned quaternion, after all of them, so nothing feeds back and nothing is fought over.
            if (s_rollAngle != 0.0f) {
                float halfAngle = s_rollAngle * 0.5f;
                float cH = std::cos(halfAngle);
                float sH = std::sin(halfAngle);

                RE::NiQuaternion qRoll;
                qRoll.w = cH;
                qRoll.x = 0.0f;
                qRoll.y = sH;
                qRoll.z = 0.0f;

                RE::NiQuaternion q = a_rotation;
                a_rotation.w = q.w * qRoll.w - q.x * qRoll.x - q.y * qRoll.y - q.z * qRoll.z;
                a_rotation.x = q.w * qRoll.x + q.x * qRoll.w + q.y * qRoll.z - q.z * qRoll.y;
                a_rotation.y = q.w * qRoll.y - q.x * qRoll.z + q.y * qRoll.w + q.z * qRoll.x;
                a_rotation.z = q.w * qRoll.z + q.x * qRoll.y - q.y * qRoll.x + q.z * qRoll.w;
            }

            // "Block mouse buttons" mode: no movement here. LMB/RMB are consumed
            // in the input sink and the vertical they'd cause is undone in the
            // Update hook. (Dolly forward/back removed per user request.)
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // Run a remapped mouse-button action (LMB/RMB → one of our functions).
    // 0.7.7: the camera-writing actions (FOV, reset) only while TFCam drives the camera.
    // 0.7.9: under an FCFW (SLCC) camera, Reset still clears TFCam's roll (the FOV stays SLCC's).
    static void ExecuteMouseAction(int action, bool a_driving) {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!a_driving && action == kResetCam) {
            ResetRoll();
            return;
        }
        if (!a_driving && (action == kFovIn || action == kFovOut)) {
            return;
        }
        switch (action) {
            case kScreenshot:
                keybd_event(VK_SNAPSHOT, 0x2C, 0, 0);
                keybd_event(VK_SNAPSHOT, 0x2C, KEYEVENTF_KEYUP, 0);
                break;
            case kFreezeTime:
                FreezeTime::Toggle();
                break;
            case kToggleLight:
                CameraLight::Toggle();
                break;
            case kFovIn:
                if (cam) cam->worldFOV = std::clamp(cam->worldFOV - s_settings.fovStep,
                                                    s_settings.fovMin, s_settings.fovMax);
                break;
            case kFovOut:
                if (cam) cam->worldFOV = std::clamp(cam->worldFOV + s_settings.fovStep,
                                                    s_settings.fovMin, s_settings.fovMax);
                break;
            case kResetCam:
                ResetAll();
                break;
            default:
                break;
        }
    }

    // --- FreeCameraState::Update hook ---
    // While a mouse button is HELD, freeze the camera's vertical so the
    // (blocked) button can't raise/lower the camera.
    // Only while held → keyboard up/down still works normally otherwise.
    static bool MenuBlocksCamera() {
        auto* ui = RE::UI::GetSingleton();
        return ui && (ui->IsMenuOpen("UIListMenu") ||
                      ui->IsMenuOpen("UIWheelMenu") ||
                      ui->IsMenuOpen("UITextEntryMenu"));
    }

    // True when a menu/overlay is up — used to DISABLE our mouse-button block so
    // the player can use menus normally (inventory, MCM, console, and the SKSE
    // Menu Framework overlay where the checkbox lives). Cursor Menu is open
    // whenever the mouse cursor is shown (covers the SMF overlay + most menus).
    static bool AnyMenuOpen() {
        // SKSE Menu Framework overlay (our config window) — the SMF overlay does not
        // always register a game "Cursor Menu", so check it explicitly. While it's
        // open we suspend mouse handling so the user can click the menu.
        if (FreeCamMenu::IsOverlayOpen()) return true;
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        if (ui->GameIsPaused()) return true;
        if (ui->IsMenuOpen("Cursor Menu")) return true;
        if (ui->IsMenuOpen("Console")) return true;
        if (ui->IsMenuOpen("RaceSex Menu")) return true;
        return false;
    }

    // --Claude: true while the vanilla Dialogue Menu is up. The game suppresses
    // free-cam movement input in this state, so the dialogue free-cam feature
    // drives the camera manually (see the Update hook).
    static bool InDialogue() {
        auto* ui = RE::UI::GetSingleton();
        return ui && ui->IsMenuOpen("Dialogue Menu");
    }

    // --Claude: true while RaceMenu (the "RaceSex Menu") is up. Like dialogue, the
    // game suppresses free-cam movement input under this menu, so when the user opts
    // in we drive the camera manually (same WASD-move / hold-Alt-to-look path as the
    // dialogue free-cam) — letting them fly around the character while editing.
    static bool InRaceMenu() {
        auto* ui = RE::UI::GetSingleton();
        return ui && ui->IsMenuOpen("RaceSex Menu");
    }

    // FreeCameraState internal member offsets (from FreeCameraFramework RE):
    //   0x44 BSTPoint2<float> zUpDown          (accumulated vertical)
    //   0x4C std::int16_t     verticalDirection (per-frame up/down input)
    // Vertical = verticalDirection → zUpDown → translation.z, all inside Update.
    // Zeroing verticalDirection BEFORE the original Update means no vertical input
    // is ever processed → nothing accumulates internally → no snap on release.
    // (Offsets kOff_translation/rotation/zUpDown/verticalDirection are defined up top.)

    // Zero the free cam's vertical input on the CURRENT state (used from the input
    // sink too, to catch very fast clicks the per-frame Update check can miss).
    static void ZeroFreeCamVertical() {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || !cam->IsInFreeCameraMode()) return;
        auto* state = cam->currentState.get();
        if (!state) return;
        auto base = reinterpret_cast<std::uintptr_t>(state);
        *reinterpret_cast<std::int16_t*>(base + kOff_verticalDirection) = 0;
        reinterpret_cast<float*>(base + kOff_zUpDown)[0] = 0.0f;
        reinterpret_cast<float*>(base + kOff_zUpDown)[1] = 0.0f;
    }

    // NOTE on attack blocking: we previously disabled the Fighting control group via
    // ControlMap::ToggleControls(kFighting). That CRASHED in free cam (2026-06-02) —
    // leaving Fighting disabled while the free camera's input context is active makes
    // the engine's per-frame input poll (Main::Update) read a bad control-handler
    // entry, especially alongside TrueDirectionalMovement/SmoothCam which hook attack
    // input. So we do NOT mutate ControlMap. Instead, blockAttacks neutralizes the
    // LMB/RMB ButtonEvents at the input sink (see ProcessEvent) — no engine-state
    // mutation, no crash. Trade-off: consuming the buttons also stops mouse-driven
    // camera vertical while active (use keyboard up/down).

    struct FreeCamUpdateHook {
        static void thunk(RE::TESCameraState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_next) {
            bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

            // 0.7.7: this Update runs for EVERY FreeCameraState, including the one an FCFW timeline
            // (SLCC) drives. Every camera write below is TFCam's only while no timeline owns it.
            const bool driving = !FcfwBridge::FcfwOwnsCamera();
            SlccBridge::Pump();

            // 0.7.9: TFCam's roll also rides on an FCFW camera now. If an FCFW session starts or ends while the free
            // cam stays on (no Begin/End in between), the roll belonged to the other camera: clear it.
            static bool s_prevDriving = true;
            if (driving != s_prevDriving) {
                if (s_rollAngle != 0.0f) {
                    SKSE::log::info("FreeCam: roll cleared - an FCFW timeline {} the camera without a free cam exit",
                        driving ? "released" : "took");
                    s_rollAngle = 0.0f;
                }
                s_prevDriving = driving;
            }

            // 0.7.7 SLCC hand-over: start where SLCC's camera was, not behind the player. Written
            // before the vanilla Update, the same way as the menu-exit restore below.
            if (s_entryPosePending && a_this && driving) {
                s_entryPosePending = false;
                auto base = reinterpret_cast<std::uintptr_t>(a_this);
                *reinterpret_cast<RE::NiPoint3*>(base + kOff_translation) = s_entryPos;
                float* rot = reinterpret_cast<float*>(base + kOff_rotation);
                rot[0] = s_entryPitch;
                rot[1] = s_entryYaw;
                SKSE::log::info("FreeCam: started from SLCC's last camera pose ({:.1f}, {:.1f}, {:.1f})",
                    s_entryPos.x, s_entryPos.y, s_entryPos.z);
            }

            // Menu-exit restore: a game menu just closed → re-assert our saved free-cam
            // transform IF the camera was actually yanked (another mod reset it). Guards:
            //  - only with a VALID capture from THIS free-cam session (never restore a
            //    stale/zero position on entry — that was the 0,0,0 / far-jump bug), and
            //  - only when the camera moved a real distance from our save (so benign menu
            //    closes / HUD toggles don't cause a 1-frame hitch).
            if (s_menuRestorePending && a_this) {
                s_menuRestorePending = false;
                if (s_menuSaveValid && driving) {
                    auto base = reinterpret_cast<std::uintptr_t>(a_this);
                    auto* cur = reinterpret_cast<RE::NiPoint3*>(base + kOff_translation);
                    float dx = cur->x - s_menuSaveTrans.x;
                    float dy = cur->y - s_menuSaveTrans.y;
                    float dz = cur->z - s_menuSaveTrans.z;
                    if (dx * dx + dy * dy + dz * dz > 2500.0f) {  // > ~50 units → was reset
                        *cur = s_menuSaveTrans;
                        float* rot = reinterpret_cast<float*>(base + kOff_rotation);
                        rot[0] = s_menuSaveRot[0];
                        rot[1] = s_menuSaveRot[1];
                        if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && s_menuSaveFOV > 0.0f) {
                            cam->worldFOV = s_menuSaveFOV;
                        }
                    }
                }
            }

            // Block the mouse-button-driven vertical at its source.
            // LMB/RMB are always remapped — vanilla up/down is replaced by
            // the remap system (user can assign Move Up/Down if desired).
            if (driving && !AnyMenuOpen() && (lmb || rmb) && a_this) {
                auto base = reinterpret_cast<std::uintptr_t>(a_this);
                *reinterpret_cast<std::int16_t*>(base + kOff_verticalDirection) = 0;
                reinterpret_cast<float*>(base + kOff_zUpDown)[0] = 0.0f;
                reinterpret_cast<float*>(base + kOff_zUpDown)[1] = 0.0f;
            }

            func(a_this, a_next);  // vanilla Update

            // Continuous move actions (LMB/RMB remapped) — applied AFTER Update
            // by adding to translation. Only when blockAttacks is on (remap active).
            if (driving && s_settings.blockAttacks && !AnyMenuOpen() && a_this) {
                auto applyMove = [&](int action, bool held) {
                    if (!held) return;

                    auto base = reinterpret_cast<std::uintptr_t>(a_this);
                    float* trans = reinterpret_cast<float*>(base + kOff_translation);

                    float speed = 10.0f;
                    if (auto* ini = RE::INISettingCollection::GetSingleton()) {
                        if (auto* s = ini->GetSetting("fFreeCameraTranslationSpeed:Camera"))
                            speed = s->data.f;
                    }
                    float amt = speed * s_frameDt * 30.0f;

                    if (action == kMoveForward || action == kMoveBackward) {
                        float dir = (action == kMoveForward) ? 1.0f : -1.0f;
                        float* rot = reinterpret_cast<float*>(base + kOff_rotation);
                        float pitch = rot[0], yaw = rot[1];
                        float cp = std::cos(pitch), sp = std::sin(pitch);
                        trans[0] += std::sin(yaw) * cp * dir * amt;
                        trans[1] += std::cos(yaw) * cp * dir * amt;
                        trans[2] += -sp * dir * amt;
                    } else if (action == kMoveUp) {
                        trans[2] += amt;
                    } else if (action == kMoveDown) {
                        trans[2] -= amt;
                    }
                };
                applyMove(s_settings.lmbAction, lmb);
                applyMove(s_settings.rmbAction, rmb);
            }

            // --- Dialogue free-cam (--Claude) --------------------------------
            // The engine suppresses free-cam movement input while the Dialogue
            // Menu is up. When the user opts in, we drive the camera ourselves:
            //   WASD          → move (write translation directly)
            //   hold Alt + mouse → look (yaw/pitch). Release Alt to free the
            //                      mouse for clicking dialogue options.
            // Pitching the view then holding W/S also climbs/descends, since the
            // forward vector carries the pitch component.
            // Seed accumulators from the live rotation the first frame we take over,
            // and clear the seed whenever we're not driving, so re-entry re-seeds.
            static float s_dlgYaw = 0.0f, s_dlgPitch = 0.0f;
            static bool  s_dlgSeeded = false;
            // --Claude: the same manual-drive path serves BOTH menus that suppress free-cam
            // input — the Dialogue Menu and RaceMenu (RaceSex Menu). Writing our own absolute
            // yaw/pitch/translation each frame is harmless even where vanilla doesn't re-aim.
            const bool inMenuCam = a_this && driving &&
                ((s_settings.dialogueCam && InDialogue()) || (s_settings.raceMenuCam && InRaceMenu()));
            if (!inMenuCam) s_dlgSeeded = false;

            if (inMenuCam) {
                auto base = reinterpret_cast<std::uintptr_t>(a_this);
                float* trans = reinterpret_cast<float*>(base + kOff_translation);
                float* rot   = reinterpret_cast<float*>(base + kOff_rotation);

                // The vanilla Update re-aims the free-cam rotation at the speaker every
                // frame during dialogue, which snapped an additive look straight back.
                // So we hold our OWN absolute yaw/pitch and write it authoritatively.
                if (!s_dlgSeeded) {
                    s_dlgYaw   = rot[1];
                    s_dlgPitch = rot[0];
                    s_dlgSeeded = true;
                }

                // Look — hold Left Alt + mouse (where the menu passes mouse deltas), OR keyboard:
                // Q/E turn left/right, PageUp/PageDown tilt up/down. The keyboard path needs no
                // mouse, so it works in RaceMenu too (the mouse there is the UI cursor). NOTE: if
                // the menu pins the camera's aim on its subject (RaceMenu keeps it on the character),
                // these rotation writes get overridden and look won't visibly change — that's a menu
                // limitation, not a key that isn't firing.
                constexpr float kPitchLim = 1.55f;  // ~89°, avoid gimbal flip
                if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
                    constexpr float kSens = 0.0025f;
                    s_dlgYaw   += s_dlgMouseDX * kSens;
                    s_dlgPitch += s_dlgMouseDY * kSens;
                }
                s_dlgMouseDX = 0.0f;   // consume every frame (held or not)
                s_dlgMouseDY = 0.0f;
                {
                    float lookAmt = 1.6f * s_frameDt;  // keyboard look rate (rad/sec)
                    if ((GetAsyncKeyState('Q')      & 0x8000) != 0) s_dlgYaw   -= lookAmt; // turn left
                    if ((GetAsyncKeyState('E')      & 0x8000) != 0) s_dlgYaw   += lookAmt; // turn right
                    if ((GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0) s_dlgPitch -= lookAmt; // PageUp = up
                    if ((GetAsyncKeyState(VK_NEXT)  & 0x8000) != 0) s_dlgPitch += lookAmt; // PageDown = down
                }
                if (s_dlgPitch >  kPitchLim) s_dlgPitch =  kPitchLim;
                if (s_dlgPitch < -kPitchLim) s_dlgPitch = -kPitchLim;

                // Write our orientation back every frame, overriding the vanilla
                // per-frame reset so the view the user set actually sticks.
                rot[1] = s_dlgYaw;
                rot[0] = s_dlgPitch;

                // Move.
                float speed = 10.0f;
                if (auto* ini = RE::INISettingCollection::GetSingleton()) {
                    if (auto* s = ini->GetSetting("fFreeCameraTranslationSpeed:Camera"))
                        speed = s->data.f;
                }
                float amt = speed * s_frameDt * 30.0f;
                float pitch = rot[0], yaw = rot[1];
                float cp = std::cos(pitch), sp = std::sin(pitch);
                auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };

                if (down('W') || down('S')) {
                    float dir = down('W') ? 1.0f : -1.0f;
                    trans[0] += std::sin(yaw) * cp * dir * amt;
                    trans[1] += std::cos(yaw) * cp * dir * amt;
                    trans[2] += -sp * dir * amt;
                }
                if (down('A') || down('D')) {
                    float dir = down('D') ? 1.0f : -1.0f;   // strafe along camera-right
                    trans[0] += std::cos(yaw) * dir * amt;
                    trans[1] += -std::sin(yaw) * dir * amt;
                }
                // --Claude: dedicated vertical — Space = up, Left Ctrl = down. This is straight
                // translation.z, independent of look, so it works even where the menu pins the
                // camera's aim (RaceMenu): you can rise/descend around the character regardless.
                if (down(VK_SPACE) && !s_settings.disableSpace) trans[2] += amt;
                if (down(VK_CONTROL)) trans[2] -= amt;
            }

            // Save the live free-cam transform as the menu-restore anchor — while no
            // menu is up, OR (dialogue free-cam) while we're driving the camera during
            // dialogue, so the menu-close restore sees no movement and never yanks us
            // back to where the camera was before dialogue opened.
            if (a_this && driving && (!AnyMenuOpen() || (s_settings.dialogueCam && InDialogue())
                                                     || (s_settings.raceMenuCam && InRaceMenu()))) {
                auto base = reinterpret_cast<std::uintptr_t>(a_this);
                s_menuSaveTrans = *reinterpret_cast<RE::NiPoint3*>(base + kOff_translation);
                float* rot = reinterpret_cast<float*>(base + kOff_rotation);
                s_menuSaveRot[0] = rot[0];
                s_menuSaveRot[1] = rot[1];
                if (auto* cam = RE::PlayerCamera::GetSingleton()) s_menuSaveFOV = cam->worldFOV;
                s_menuSaveValid = true;  // we now have a real position to restore to
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // --- FreeCameraState::Begin hook ---

    struct FreeCamBeginHook {
        static void thunk(RE::TESCameraState* a_this) {
            const std::string chain = CallerChain(24);  // 0.7.9 diagnostic: who turned free cam on
            func(a_this);
            auto* cam = RE::PlayerCamera::GetSingleton();
            s_rollAngle = 0.0f;
            // Start the menu-restore anchor fresh — never restore a previous session's
            // (or zero) position on entry. The first Update frame captures the real spot.
            s_menuSaveValid = false;
            s_menuRestorePending = false;

            // 0.7.7: FCFW (SLCC) enters this same FreeCameraState for its timelines. Only a session
            // nobody else owns is TFCam's: base FOV for the exit write-back and the HUD hide.
            const auto timeline = FcfwBridge::ActiveTimelineID();
            s_sessionDriven = (timeline == 0);
            if (s_sessionDriven) {
                s_baseFOV = cam ? cam->worldFOV : 0.0f;
                HUDHider::OnFreeCamEnter();
                SKSE::log::info("FreeCam entered, FOV={:.1f} [{}]", s_baseFOV, ThreadTag());
            } else {
                s_baseFOV = 0.0f;
                s_entryPosePending = false;
                SKSE::log::info("FreeCam entered by FCFW timeline {} (SLCC / FCFW drives it) - TFCam camera "
                                "features stand down except roll, input blocks stay [{}]", timeline, ThreadTag());
            }
            SKSE::log::info("FreeCam enter: called from {}", chain);
            SlccBridge::OnFreeCamBegin(s_sessionDriven);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // --- FreeCameraState::End hook ---

    struct FreeCamEndHook {
        static void thunk(RE::TESCameraState* a_this) {
            // 0.7.9 diagnostic (permanent, cheap): the caller chain names the DLL that ended free cam, and the last
            // key TFCam's input sink saw names the key behind it. 2026-09-26: exits with no TFCam cause ran on Papyrus
            // VM threads - Poser Hotkeys Plus' Jump handler running ConsoleUtil.ExecuteCommand("tfc").
            const std::string chain = CallerChain(24);
            const bool        scriptSession = s_scriptTfcSession.exchange(false);

            // 0.7.7: s_baseFOV is only set for a TFCam-driven session. If an FCFW timeline owns the
            // camera at exit (SLCC stopping its playback), FCFW restores its own saved FOV.
            const bool fcfwOwned   = FcfwBridge::FcfwOwnsCamera();
            const bool wasTFCam    = s_sessionDriven;
            float savedFOV = s_baseFOV;
            s_rollAngle = 0.0f;
            s_baseFOV   = 0.0f;
            s_sessionDriven    = false;
            s_entryPosePending = false;

            // Restore alt-slow speed before exiting
            if (s_altSlow) {
                SetCameraSpeed(FreeCamMenu::GetCameraSpeed());
                s_altSlow = false;
            }

            // Turn off camera light and restore HUD before exiting free cam.
            // (HUDHider only restores what it hid, so this is safe for FCFW sessions too.)
            CameraLight::Cleanup();
            HUDHider::OnFreeCamExit();

            func(a_this);

            if (savedFOV > 0.0f && !fcfwOwned) {
                if (auto* cam = RE::PlayerCamera::GetSingleton()) {
                    cam->worldFOV = savedFOV;
                    cam->firstPersonFOV = savedFOV;
                    SKSE::log::info("FreeCam: FOV restored to {:.1f}", savedFOV);
                }
            } else if (savedFOV > 0.0f) {
                SKSE::log::info("FreeCam: FOV write-back skipped - FCFW timeline {} owns the camera at exit",
                    FcfwBridge::ActiveTimelineID());
            }

            // Note: the engine re-enables gameplay controls on a normal free-cam exit
            // (confirmed: attacks work after a plain enter→exit). We deliberately do NOT
            // call ControlMap::ToggleControls ourselves here — in this load order that
            // corrupts the input-handler table and crashes on the next input poll.

            FreezeTime::Restore();
            s_menuSaveValid = false;   // invalidate menu-restore anchor on exit
            s_menuRestorePending = false;

            // --Claude 2026-07-24 (user report: "exiting tfcam, player turning was locked
            // to the mouse"): a free-cam exit can strand ThirdPersonState with
            // freeRotationEnabled=false — the state where mouse yaw steers the ACTOR
            // instead of orbiting the camera. Re-assert it one tick after the state
            // transition settles. Logged so a recurrence tells us if this was the cause.
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([]() {
                    auto* cam = RE::PlayerCamera::GetSingleton();
                    if (!cam) return;
                    auto& third = cam->cameraStates[RE::CameraState::kThirdPerson];
                    if (third && cam->currentState.get() == third.get()) {
                        auto* tps = static_cast<RE::ThirdPersonState*>(third.get());
                        if (!tps->freeRotationEnabled) {
                            tps->freeRotationEnabled = true;
                            SKSE::log::info("FreeCam: third-person free rotation was OFF after exit - re-enabled");
                        }
                    }
                });
            }

            SKSE::log::info("FreeCam exited ({} session{}{}) [{}]", wasTFCam ? "TFCam" : "FCFW",
                fcfwOwned ? ", FCFW timeline active at exit" : "",
                scriptSession ? ", opened by a script's console tfc" : "", ThreadTag());
            SKSE::log::info("FreeCam exit: called from {}", chain);
            SKSE::log::info("FreeCam exit: last key/button press TFCam saw: {}", DescribeLastPress());
            SlccBridge::OnFreeCamEnd(fcfwOwned);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // Zero a button event in place so the free camera's internal input reader
    // sees it as not-pressed. `continue` alone only skips OUR handling — the
    // camera still reads the intact event and applies its up/down movement.
    static void ConsumeButton(RE::ButtonEvent* btn) {
        btn->value = 0.0f;
        btn->heldDownSecs = 0.0f;
    }

    // --- Menu open/close watcher (menu-exit camera restore + FavoritesMenu kill) ---
    class MenuWatch : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        static MenuWatch* GetSingleton() { static MenuWatch instance; return &instance; }
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_evt,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
            if (!a_evt || !IsActive())
                return RE::BSEventNotifyControl::kContinue;

            if (a_evt->opening) {
                if (a_evt->menuName == RE::FavoritesMenu::MENU_NAME) {
                    auto* queue = RE::UIMessageQueue::GetSingleton();
                    if (queue) {
                        queue->AddMessage(RE::FavoritesMenu::MENU_NAME,
                            RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
                    }
                }
            } else if (TFCamDriving()) {
                // 0.7.7: never re-assert a transform over an FCFW (SLCC) camera.
                s_menuRestorePending = true;
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // --- Attack block hook (surgical, crash-free) ---
    // Hooks the vanilla AttackBlockHandler::ProcessButton (vtable index 4). When
    // "Block attacks" is on and free cam is active, we swallow the call so no
    // attack / block / power-attack fires. This is the SAFE replacement for the
    // ControlMap approach (which crashed in free cam). It blocks ONLY attacking —
    // mouse-button camera up/down still works (that's a separate code path), and
    // TrueDirectionalMovement does NOT hook this handler, so there's no conflict.
    struct AttackBlockHook {
        static void thunk(RE::AttackBlockHandler* a_this, RE::ButtonEvent* a_event,
                          RE::PlayerControlsData* a_data) {
            bool active = IsActive();
            if (s_settings.blockAttacks && active) {
                return;  // box checked + in free cam → no attack. Unchecked → falls through.
            }
            func(a_this, a_event, a_data);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // 0.7.5: the 0.7.3 Jump eat in InputListener never worked. That sink is added with
    // AddEventSink, i.e. AFTER PlayerControls, so JumpHandler had already jumped by the time
    // we zeroed the event (the same is true of bDisableSpace). Refuse the jump at the handler
    // itself instead, like the attack block above - covers a remapped key and the gamepad too.
    //
    // 0.7.6: gated on bDisableSpace. 0.7.5 blocked unconditionally, so the "Disable Space"
    // checkbox changed nothing: ticked or not, the jump was refused here.
    struct JumpBlockHook {
        static void thunk(RE::JumpHandler* a_this, RE::ButtonEvent* a_event,
                          RE::PlayerControlsData* a_data) {
            if (s_settings.disableSpace && IsActive()) {
                return;
            }
            func(a_this, a_event, a_data);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // 0.7.6: bDisableShift at the handlers. The 0.7.1 ConsumeButton in InputListener has the
    // same ordering problem as the Jump eat: PlayerControls has already run Sprint / Run /
    // ToggleRun (and the free camera's own input handler) by the time our sink sees the event.
    // Matched by the physical key, so it follows Shift whichever of these the controlmap binds
    // it to (vanilla: Run; a common remap: Sprint).
    static bool IsShiftKey(const RE::ButtonEvent* a_event) {
        if (!a_event || a_event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) return false;
        const auto code = a_event->GetIDCode();
        return code == 0x2A || code == 0x36;  // Left Shift / Right Shift
    }

    // Decided once per press, on the key-down edge, and kept until that press is released.
    // Sprint and Run are held-state handlers and ToggleRun flips a flag: letting through the
    // release of a press we swallowed (or swallowing the release of a press that got through,
    // e.g. Shift already held when free cam starts) would leave the player sprinting or flip
    // walk/run. The latch remembers WHICH key it swallowed, so the combo case below (the event
    // arrives under the other key, not Shift) releases on that key's own key-up.
    //
    // a_comboToo (ToggleRun only): a press made while Shift is physically held also counts.
    // A Shift+key binding (the reporter's controlmap: Toggle Always Run = 0x2a+0x2e, Shift+C)
    // reaches the handler under the other key, so IsShiftKey alone never matched it and Shift+C
    // still flipped always-run while flying.
    struct ShiftGate {
        bool             swallowing = false;
        RE::INPUT_DEVICE device     = RE::INPUT_DEVICE::kNone;
        std::uint32_t    code       = 0;
        bool Blocks(const RE::ButtonEvent* a_event, bool a_comboToo) {
            if (!a_event) return false;
            const auto dev = a_event->GetDevice();
            const auto id  = a_event->GetIDCode();
            if (swallowing && dev == device && id == code) {
                if (a_event->IsUp()) swallowing = false;  // the release of the press we swallowed
                return true;
            }
            if (!a_event->IsDown()) return false;
            const bool shiftPress = IsShiftKey(a_event) ||
                (a_comboToo && dev == RE::INPUT_DEVICE::kKeyboard &&
                 (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
            if (shiftPress && s_settings.disableShift && IsActive()) {
                swallowing = true;
                device     = dev;
                code       = id;
                return true;
            }
            return false;
        }
    };

    template <class Handler, bool ComboToo = false>
    struct ShiftBlockHook {
        static void thunk(Handler* a_this, RE::ButtonEvent* a_event, RE::PlayerControlsData* a_data) {
            if (gate.Blocks(a_event, ComboToo)) {
                return;
            }
            func(a_this, a_event, a_data);
        }
        static inline ShiftGate gate;
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // --- Input event sink ---

    class InputListener : public RE::BSTEventSink<RE::InputEvent*> {
    public:
        static InputListener* GetSingleton() {
            static InputListener instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            RE::InputEvent* const* a_events,
            RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (!a_events)
                return RE::BSEventNotifyControl::kContinue;

            // 0.7.7: our own injected SLCC Director key press - not user input.
            if (SlccBridge::IsInjecting())
                return RE::BSEventNotifyControl::kContinue;
            SlccBridge::Pump();

            bool active = IsActive();
            // 0.7.7: camera-writing hotkeys (FOV wheel, roll, reset, FOV/reset remaps) key on this;
            // the input blocks (attack / jump / activate / shift / Tab / roll-key eat) stay on `active`.
            bool driving = active && !FcfwBridge::FcfwOwnsCamera();
            if (active) UpdateFrameTimer();

            for (auto* evt = *a_events; evt; evt = evt->next) {
                // --Claude: accumulate mouse movement for the dialogue free-cam
                // look. Consumed+cleared each frame by the Update hook (only
                // applied while the hold-to-look key is down).
                if (active && (s_settings.dialogueCam || s_settings.raceMenuCam) &&
                    evt->GetEventType() == RE::INPUT_EVENT_TYPE::kMouseMove) {
                    auto* mm = static_cast<RE::MouseMoveEvent*>(evt);
                    s_dlgMouseDX += static_cast<float>(mm->mouseInputX);
                    s_dlgMouseDY += static_cast<float>(mm->mouseInputY);
                    continue;
                }
                if (evt->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;

                auto* btn = evt->AsButtonEvent();
                if (!btn) continue;

                auto device = btn->GetDevice();
                auto code   = btn->GetIDCode();

                // 0.7.9: remember the last key-down (free cam exit diagnostic + scripted-tfc guard), before any
                // eat below zeroes the event or clears its user event.
                if (btn->IsDown() &&
                    (device == RE::INPUT_DEVICE::kKeyboard || device == RE::INPUT_DEVICE::kGamepad)) {
                    RecordPress(btn, device, code);
                }

                // -------------------------------------------------------
                // Key capture for press-to-bind (eats all input while active)
                // -------------------------------------------------------
                if (FreeCamMenu::IsCapturingKey()) {
                    if (device == RE::INPUT_DEVICE::kKeyboard && btn->IsDown()) {
                        int keyCode = static_cast<int>(code);
                        if (keyCode == 0x01) {
                            FreeCamMenu::CancelCapture();
                        } else {
                            FreeCamMenu::OnKeyCaptured(keyCode);
                        }
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }

                // -------------------------------------------------------
                // Global hotkeys (work in any camera state)
                // -------------------------------------------------------
                if (device == RE::INPUT_DEVICE::kKeyboard && btn->IsDown() &&
                    (!AnyMenuOpen() || (s_settings.dialogueCam && InDialogue())
                                    || (s_settings.raceMenuCam && InRaceMenu()))) {
                    // 0.7.8: SexLab P+'s "Toggle Free Camera" key is watched (never eaten): P+ toggles
                    // free cam itself through PapyrusUtil, and SlccBridge follows that toggle.
                    if (!AnyMenuOpen()) SlccBridge::NoteKeyDown(code);

                    int flyKey = FreeCamMenu::GetFreeFlyKey();
                    if (flyKey > 0 && code == static_cast<std::uint32_t>(flyKey)) {
                        if (SlccBridge::IsPplusFreeCamKey(code)) {
                            // Same key as SexLab P+'s free camera hotkey: P+ already toggles free cam on
                            // it (sslSystemConfig.OnKeyDown, through SKSE's key registration, which sees
                            // the key before this sink), so a second toggle here would undo P+'s. TFCam
                            // follows P+'s toggle instead (SlccBridge Begin/End).
                            static bool s_warned = false;
                            if (!s_warned) {
                                SKSE::log::warn("Free-fly key 0x{:X} is also SexLab P+'s Toggle Free Camera key - "
                                                "TFCam follows P+'s toggle instead of toggling itself", code);
                                s_warned = true;
                            }
                        } else {
                            // 0.7.7: plain toggle as before, unless an FCFW timeline (SLCC) owns the camera.
                            // 0.7.8: during a player SexLab scene with SLCC, the TFCam -> SLCC -> Off cycle.
                            SlccBridge::RequestCycle("free-fly key");
                            active  = IsActive();
                            driving = active && !FcfwBridge::FcfwOwnsCamera();
                            ConsumeButton(btn);
                            continue;
                        }
                    }

                }

                // Slow-key state is tracked globally so a release outside free cam never leaves
                // it stuck held for the next activation.
                if (device == RE::INPUT_DEVICE::kKeyboard && s_settings.slowKey != 0 &&
                    code == s_settings.slowKey) {
                    s_slowHeld = btn->IsPressed();
                }

                // Everything below only works when free cam is active
                if (!active) continue;

                // 0.7.6: the Shift / Space / Jump eats moved to the END of this loop (see there).
                // Up here, ending in `continue`, they swallowed any TFCam hotkey bound to Shift or
                // Space before the roll/reset/freeze/screenshot block could see it (the 0.7.4 trap).

                // 0.7.4 FIX: computed once, ahead of every user-event eat below. A roll key must
                // never be swallowed by one of those `continue`s — they sit above the roll block,
                // so anything they eat never rolls. E is the vanilla Activate binding, which is
                // exactly how roll-clockwise died in 0.7.2. Roll keys fall through; the roll block
                // consumes them itself, so they still do not reach the engine.
                const bool isRollKey =
                    device == RE::INPUT_DEVICE::kKeyboard &&
                    (code == s_settings.rollCCWKey || code == s_settings.rollCWKey);

                // 0.7.2 (Nexus request): eat the Activate user event while flying. In tfc the
                // activation ray comes from the camera, so E over a chair sat the player down and
                // E over a cave door loaded the interior. Matched by user event, so it covers a
                // remapped key and the gamepad button too.
                //
                // 0.7.4 FIX: `!isRollKey` (declared above) — with bDisableActivate=1 this eat
                // swallowed E, the vanilla Activate binding, so roll-clockwise never ran.
                if (s_settings.disableActivate && !AnyMenuOpen() && !isRollKey) {
                    auto* ue = RE::UserEvents::GetSingleton();
                    if (ue && btn->QUserEvent() == ue->activate) {
                        ConsumeButton(btn);
                        btn->userEvent = "";
                        continue;
                    }
                }

                // LMB/RMB in free cam: when blockAttacks is on, consume the
                // button (prevents attacks) and fire any remapped action.
                // When blockAttacks is off, let the event pass through so
                // attacks work — the Update hook still zeros vertical movement
                // via GetAsyncKeyState regardless.
                if (s_settings.blockAttacks && !AnyMenuOpen() &&
                    device == RE::INPUT_DEVICE::kMouse &&
                    (code == RE::BSWin32MouseDevice::Key::kLeftButton ||
                     code == RE::BSWin32MouseDevice::Key::kRightButton)) {
                    int action = (code == RE::BSWin32MouseDevice::Key::kLeftButton)
                                     ? s_settings.lmbAction : s_settings.rmbAction;
                    // One-shot actions fire on the press edge. Continuous move
                    // actions (kMoveForward+) are handled in the Update hook
                    // while held, so skip them here.
                    if (btn->IsDown() &&
                        (action > kNone && action < kMoveForward))
                        ExecuteMouseAction(action, driving);
                    if (driving) ZeroFreeCamVertical();
                    ConsumeButton(btn);
                    continue;
                }

                // Slow-mode (configurable key, default Left Alt): check state on every input event
                {
                    bool altHeld = s_slowHeld;
                    if (altHeld && !s_altSlow) {
                        SetCameraSpeed(FreeCamMenu::GetCameraSpeed() / 5.0f);
                        s_altSlow = true;
                    } else if (!altHeld && s_altSlow) {
                        SetCameraSpeed(FreeCamMenu::GetCameraSpeed());
                        s_altSlow = false;
                    }
                }

                // -------------------------------------------------------
                // Mouse: Shift+MMB = freeze, MMB = screenshot, Wheel = FOV
                // -------------------------------------------------------
                if (device == RE::INPUT_DEVICE::kMouse && btn->IsDown() && !AnyMenuOpen()) {
                    if (code == RE::BSWin32MouseDevice::Key::kMiddleButton) {
                        bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (shiftHeld && s_settings.freezeTimeKey == 0) {
                            // Shift+MMB freeze only when no keyboard key is bound
                            FreezeTime::Toggle();
                            SKSE::log::info("Freeze toggled via Shift+MMB");
                        } else if (!shiftHeld && s_settings.screenshotKey == 0) {
                            // MMB screenshot only when no keyboard key is bound
                            keybd_event(VK_SNAPSHOT, 0x2C, 0, 0);
                            keybd_event(VK_SNAPSHOT, 0x2C, KEYEVENTF_KEYUP, 0);
                            SKSE::log::info("Screenshot triggered (PrintScreen)");
                        }
                    }

                    // 0.7.7: plain wheel = FOV only while TFCam drives; under SLCC the wheel is SLCC's zoom.
                    if (code == RE::BSWin32MouseDevice::Key::kWheelUp) {
                        bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (shiftHeld) {
                            CameraLight::ScrollUp();
                        } else if (driving) {
                            auto* cam = RE::PlayerCamera::GetSingleton();
                            if (cam) {
                                cam->worldFOV = std::clamp(
                                    cam->worldFOV - s_settings.fovStep,
                                    s_settings.fovMin, s_settings.fovMax);
                            }
                        }
                    }
                    else if (code == RE::BSWin32MouseDevice::Key::kWheelDown) {
                        bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (shiftHeld) {
                            CameraLight::ScrollDown();
                        } else if (driving) {
                            auto* cam = RE::PlayerCamera::GetSingleton();
                            if (cam) {
                                cam->worldFOV = std::clamp(
                                    cam->worldFOV + s_settings.fovStep,
                                    s_settings.fovMin, s_settings.fovMax);
                            }
                        }
                    }
                }

                // -------------------------------------------------------
                // Keyboard: Q/E = roll, R = reset, Tab block
                // -------------------------------------------------------
                if (device == RE::INPUT_DEVICE::kKeyboard) {
                    bool consumed = false;

                    // 0.7.7: roll only while TFCam drives (the roll keys are still eaten below).
                    // 0.7.9: roll also on top of an FCFW (SLCC) camera - see GetRotationHook. FOV and position
                    // stay SLCC's.
                    if (active && !AnyMenuOpen() && btn->IsPressed()) {
                        if (code == s_settings.rollCCWKey) {
                            s_rollAngle -= s_settings.rollSpeed * s_frameDt;
                            consumed = true;
                        } else if (code == s_settings.rollCWKey) {
                            s_rollAngle += s_settings.rollSpeed * s_frameDt;
                            consumed = true;
                        }
                    }

                    // Block Tab from reaching Wheeler/FavoritesMenu
                    if (code == 0x0F) {
                        consumed = true;
                    }

                    // Block roll keys from reaching engine (prevents FavoritesMenu)
                    if (code == s_settings.rollCCWKey || code == s_settings.rollCWKey) {
                        consumed = true;
                    }

                    if (!AnyMenuOpen() && btn->IsDown()) {
                        if (code == s_settings.resetKey) {
                            // 0.7.9: under an FCFW (SLCC) camera the reset key clears TFCam's roll only.
                            if (driving) {
                                ResetAll();
                            } else {
                                ResetRoll();
                            }
                            consumed = true;
                        }
                        if (s_settings.freezeTimeKey > 0 && code == s_settings.freezeTimeKey) {
                            FreezeTime::Toggle();
                            SKSE::log::info("Freeze toggled via keyboard key");
                            consumed = true;
                        }
                        if (s_settings.screenshotKey > 0 && code == s_settings.screenshotKey) {
                            keybd_event(VK_SNAPSHOT, 0x2C, 0, 0);
                            keybd_event(VK_SNAPSHOT, 0x2C, KEYEVENTF_KEYUP, 0);
                            SKSE::log::info("Screenshot triggered via keyboard key");
                            consumed = true;
                        }
                    }

                    if (consumed) {
                        ConsumeButton(btn);
                        btn->userEvent = "";
                        continue;
                    }
                }

                // 0.7.1 / 0.7.3, reworked 0.7.6: with Disable Shift / Disable Space ticked, zero
                // those keys (and, for Space, the Jump user event on any key or the gamepad) for
                // the sinks registered after this one. The engine itself is stopped at the
                // handlers (JumpBlockHook / ShiftBlockHook) - this sink runs after PlayerControls,
                // so it is too late for that. Last in the loop so a TFCam hotkey bound to one of
                // these keys has already been handled above. Unticked = the event passes untouched.
                //
                // 0.7.9: not Space / Jump during a player SexLab scene. Space is SexLab P+'s Advance hotkey there
                // (a Papyrus key registration, i.e. SKSE's own input sink), and the player cannot jump in a scene
                // anyway - so whatever order the sinks run in, P+ always gets the key.
                if (!AnyMenuOpen()) {
                    const bool isKeyboard = device == RE::INPUT_DEVICE::kKeyboard;
                    const bool isShift = isKeyboard && (code == 0x2A || code == 0x36);
                    const bool isSpace = isKeyboard && code == 0x39;
                    auto* ue = RE::UserEvents::GetSingleton();
                    const bool isJump = ue && btn->QUserEvent() == ue->jump;
                    if ((s_settings.disableShift && isShift) ||
                        (s_settings.disableSpace && (isSpace || isJump) && !SceneTracker::PlayerSceneActive())) {
                        ConsumeButton(btn);
                        btn->userEvent = "";
                    }
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        InputListener() = default;
    };

    // --- Installation ---

    void Install() {
        QueryPerformanceFrequency(&s_qpcFreq);

        REL::Relocation<std::uintptr_t> fcsVtable(RE::VTABLE_FreeCameraState[0]);

        FreeCamBeginHook::func = fcsVtable.write_vfunc(0x1, FreeCamBeginHook::thunk);
        SKSE::log::info("FreeCameraState::Begin hooked (vtable[1])");

        FreeCamEndHook::func = fcsVtable.write_vfunc(0x2, FreeCamEndHook::thunk);
        SKSE::log::info("FreeCameraState::End hooked (vtable[2])");

        FreeCamUpdateHook::func = fcsVtable.write_vfunc(0x3, FreeCamUpdateHook::thunk);
        SKSE::log::info("FreeCameraState::Update hooked (vtable[3])");

        GetRotationHook::func = fcsVtable.write_vfunc(0x4, GetRotationHook::thunk);
        SKSE::log::info("FreeCameraState::GetRotation hooked (vtable[4])");

        // Surgical attack block: hook AttackBlockHandler::ProcessButton (vtable[4]).
        REL::Relocation<std::uintptr_t> abhVtable(RE::VTABLE_AttackBlockHandler[0]);
        AttackBlockHook::func = abhVtable.write_vfunc(0x4, AttackBlockHook::thunk);
        SKSE::log::info("AttackBlockHandler::ProcessButton hooked (vtable[4])");

        REL::Relocation<std::uintptr_t> jumpVtable(RE::VTABLE_JumpHandler[0]);
        JumpBlockHook::func = jumpVtable.write_vfunc(0x4, JumpBlockHook::thunk);
        SKSE::log::info("JumpHandler::ProcessButton hooked (vtable[4])");

        // 0.7.6: Disable Shift - every handler Shift can drive while flying, ProcessButton = [4].
        REL::Relocation<std::uintptr_t> sprintVtable(RE::VTABLE_SprintHandler[0]);
        ShiftBlockHook<RE::SprintHandler>::func =
            sprintVtable.write_vfunc(0x4, ShiftBlockHook<RE::SprintHandler>::thunk);
        REL::Relocation<std::uintptr_t> runVtable(RE::VTABLE_RunHandler[0]);
        ShiftBlockHook<RE::RunHandler>::func =
            runVtable.write_vfunc(0x4, ShiftBlockHook<RE::RunHandler>::thunk);
        // ToggleRun also swallows a press made while Shift is held (Shift+key combo binding).
        REL::Relocation<std::uintptr_t> toggleRunVtable(RE::VTABLE_ToggleRunHandler[0]);
        ShiftBlockHook<RE::ToggleRunHandler, true>::func =
            toggleRunVtable.write_vfunc(0x4, ShiftBlockHook<RE::ToggleRunHandler, true>::thunk);
        // The free camera's own input handler (second vtable, the PlayerInputHandler base). It
        // carries a run-speed flag (FreeCameraState::useRunSpeed, +0x4E); what sets it was not
        // verified (SkyrimSE.exe .text is encrypted on disk), so this only matters if it is Shift.
        REL::Relocation<std::uintptr_t> fcsInputVtable(RE::VTABLE_FreeCameraState[1]);
        ShiftBlockHook<RE::PlayerInputHandler>::func =
            fcsInputVtable.write_vfunc(0x4, ShiftBlockHook<RE::PlayerInputHandler>::thunk);
        SKSE::log::info("Sprint/Run/ToggleRun/FreeCameraState ProcessButton hooked (vtable[4]) for Disable Shift");

        // Menu open/close watcher (menu-exit camera restore).
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatch::GetSingleton());
            SKSE::log::info("Menu watcher registered (menu-exit camera restore)");
        }

        // Input listener for roll keys, Tab blocking, mouse remaps.
        if (auto* idm = RE::BSInputDeviceManager::GetSingleton()) {
            idm->AddEventSink(InputListener::GetSingleton());
            SKSE::log::info("Input listener registered");
        }

    }
}
