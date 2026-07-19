#include "FreeCamController.h"
#include "FreezeTime.h"
#include "FreeCamMenu.h"
#include "CameraLight.h"
#include "HUDHider.h"

#include <RE/I/INISettingCollection.h>
#include <RE/A/AttackBlockHandler.h>
#include <RE/M/MouseMoveEvent.h>

#include <cmath>
#include <Windows.h>

namespace FreeCam {

    static Settings s_settings;
    static float    s_rollAngle = 0.0f;
    static float    s_baseFOV   = 0.0f;
    static bool     s_altSlow   = false;  // Alt slow-mode active
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

    static void SetCameraSpeed(float speed) {
        auto* ini = RE::INISettingCollection::GetSingleton();
        if (!ini) return;
        auto* setting = ini->GetSetting("fFreeCameraTranslationSpeed:Camera");
        if (setting) {
            setting->data.f = speed;
        }
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

    // --- FreeCameraState::GetRotation hook ---

    struct GetRotationHook {
        static void thunk(RE::TESCameraState* a_this, RE::NiQuaternion& a_rotation) {
            func(a_this, a_rotation);

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
    static void ExecuteMouseAction(int action) {
        auto* cam = RE::PlayerCamera::GetSingleton();
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

            // Menu-exit restore: a game menu just closed → re-assert our saved free-cam
            // transform IF the camera was actually yanked (another mod reset it). Guards:
            //  - only with a VALID capture from THIS free-cam session (never restore a
            //    stale/zero position on entry — that was the 0,0,0 / far-jump bug), and
            //  - only when the camera moved a real distance from our save (so benign menu
            //    closes / HUD toggles don't cause a 1-frame hitch).
            if (s_menuRestorePending && a_this) {
                s_menuRestorePending = false;
                if (s_menuSaveValid) {
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
            if (!AnyMenuOpen() && (lmb || rmb) && a_this) {
                auto base = reinterpret_cast<std::uintptr_t>(a_this);
                *reinterpret_cast<std::int16_t*>(base + kOff_verticalDirection) = 0;
                reinterpret_cast<float*>(base + kOff_zUpDown)[0] = 0.0f;
                reinterpret_cast<float*>(base + kOff_zUpDown)[1] = 0.0f;
            }

            func(a_this, a_next);  // vanilla Update

            // Continuous move actions (LMB/RMB remapped) — applied AFTER Update
            // by adding to translation. Only when blockAttacks is on (remap active).
            if (s_settings.blockAttacks && !AnyMenuOpen() && a_this) {
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
            const bool inDlgCam = s_settings.dialogueCam && a_this && InDialogue();
            if (!inDlgCam) s_dlgSeeded = false;

            if (inDlgCam) {
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

                // Look — only while the hold-to-look key (Left Alt) is down.
                if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
                    constexpr float kSens     = 0.0025f;
                    constexpr float kPitchLim = 1.55f;  // ~89°, avoid gimbal flip
                    s_dlgYaw   += s_dlgMouseDX * kSens;
                    s_dlgPitch += s_dlgMouseDY * kSens;
                    if (s_dlgPitch >  kPitchLim) s_dlgPitch =  kPitchLim;
                    if (s_dlgPitch < -kPitchLim) s_dlgPitch = -kPitchLim;
                }
                s_dlgMouseDX = 0.0f;   // consume every frame (held or not)
                s_dlgMouseDY = 0.0f;

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
                // No dedicated up/down keys: pitch the view (Alt+mouse) and W/S
                // climbs or dives, since the forward vector carries the pitch.
            }

            // Save the live free-cam transform as the menu-restore anchor — while no
            // menu is up, OR (dialogue free-cam) while we're driving the camera during
            // dialogue, so the menu-close restore sees no movement and never yanks us
            // back to where the camera was before dialogue opened.
            if (a_this && (!AnyMenuOpen() || (s_settings.dialogueCam && InDialogue()))) {
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
            func(a_this);
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (cam) s_baseFOV = cam->worldFOV;
            s_rollAngle = 0.0f;
            // Start the menu-restore anchor fresh — never restore a previous session's
            // (or zero) position on entry. The first Update frame captures the real spot.
            s_menuSaveValid = false;
            s_menuRestorePending = false;
            HUDHider::OnFreeCamEnter();

            SKSE::log::info("FreeCam entered, FOV={:.1f}", s_baseFOV);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // --- FreeCameraState::End hook ---

    struct FreeCamEndHook {
        static void thunk(RE::TESCameraState* a_this) {
            float savedFOV = s_baseFOV;
            s_rollAngle = 0.0f;
            s_baseFOV   = 0.0f;

            // Restore alt-slow speed before exiting
            if (s_altSlow) {
                SetCameraSpeed(FreeCamMenu::GetCameraSpeed());
                s_altSlow = false;
            }

            // Turn off camera light and restore HUD before exiting free cam
            CameraLight::Cleanup();
            HUDHider::OnFreeCamExit();

            func(a_this);

            if (savedFOV > 0.0f) {
                if (auto* cam = RE::PlayerCamera::GetSingleton()) {
                    cam->worldFOV = savedFOV;
                    cam->firstPersonFOV = savedFOV;
                    SKSE::log::info("FreeCam: FOV restored to {:.1f}", savedFOV);
                }
            }

            // Note: the engine re-enables gameplay controls on a normal free-cam exit
            // (confirmed: attacks work after a plain enter→exit). We deliberately do NOT
            // call ControlMap::ToggleControls ourselves here — in this load order that
            // corrupts the input-handler table and crashes on the next input poll.

            FreezeTime::Restore();
            s_menuSaveValid = false;   // invalidate menu-restore anchor on exit
            s_menuRestorePending = false;
            SKSE::log::info("FreeCam exited");
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
            } else {
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

            bool active = IsActive();
            if (active) UpdateFrameTimer();

            for (auto* evt = *a_events; evt; evt = evt->next) {
                // --Claude: accumulate mouse movement for the dialogue free-cam
                // look. Consumed+cleared each frame by the Update hook (only
                // applied while the hold-to-look key is down).
                if (active && s_settings.dialogueCam &&
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
                    (!AnyMenuOpen() || (s_settings.dialogueCam && InDialogue()))) {
                    int flyKey = FreeCamMenu::GetFreeFlyKey();
                    if (flyKey > 0 && code == static_cast<std::uint32_t>(flyKey)) {
                        auto* cam = RE::PlayerCamera::GetSingleton();
                        if (cam) {
                            cam->ToggleFreeCameraMode(false);
                        }
                        active = IsActive();
                        ConsumeButton(btn);
                        continue;
                    }

                }

                // Everything below only works when free cam is active
                if (!active) continue;

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
                        ExecuteMouseAction(action);
                    ZeroFreeCamVertical();
                    ConsumeButton(btn);
                    continue;
                }

                // Alt slow-mode: check state on every input event
                {
                    bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
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

                    if (code == RE::BSWin32MouseDevice::Key::kWheelUp) {
                        bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (shiftHeld) {
                            CameraLight::ScrollUp();
                        } else {
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
                        } else {
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

                    if (!AnyMenuOpen() && btn->IsPressed()) {
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
                            ResetAll();
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
