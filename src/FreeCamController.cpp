#include "FreeCamController.h"
#include "FreezeTime.h"
#include "FreeCamMenu.h"
#include "CameraLight.h"

#include <cmath>
#include <Windows.h>

namespace FreeCam {

    static Settings s_settings;
    static float    s_rollAngle = 0.0f;
    static float    s_baseFOV   = 0.0f;

    static LARGE_INTEGER s_qpcFreq = {};
    static LARGE_INTEGER s_qpcLast = {};
    static float         s_frameDt = 0.016f;

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
        s_baseFOV   = 0.0f;
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

            // Turn off camera light before exiting free cam
            CameraLight::Cleanup();

            func(a_this);

            if (savedFOV > 0.0f) {
                if (auto* cam = RE::PlayerCamera::GetSingleton()) {
                    cam->worldFOV = savedFOV;
                    cam->firstPersonFOV = savedFOV;
                    SKSE::log::info("FreeCam: FOV restored to {:.1f}", savedFOV);
                }
            }

            FreezeTime::Restore();
            SKSE::log::info("FreeCam exited");
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
                if (device == RE::INPUT_DEVICE::kKeyboard && btn->IsDown()) {
                    int flyKey = FreeCamMenu::GetFreeFlyKey();
                    if (flyKey > 0 && code == static_cast<std::uint32_t>(flyKey)) {
                        auto* cam = RE::PlayerCamera::GetSingleton();
                        if (cam) {
                            cam->ToggleFreeCameraMode(false);
                        }
                        continue;
                    }

                }

                // Everything below only works when free cam is active
                if (!active) continue;

                // -------------------------------------------------------
                // Mouse: Shift+MMB = freeze, MMB = screenshot, Wheel = FOV
                // -------------------------------------------------------
                if (device == RE::INPUT_DEVICE::kMouse && btn->IsDown()) {
                    if (code == RE::BSWin32MouseDevice::Key::kMiddleButton) {
                        bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        if (shiftHeld) {
                            FreezeTime::Toggle();
                            SKSE::log::info("Freeze toggled via Shift+MMB");
                        } else {
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
                // Keyboard: Q/E = roll, R = reset
                // -------------------------------------------------------
                if (device == RE::INPUT_DEVICE::kKeyboard) {
                    if (btn->IsPressed()) {
                        if (code == s_settings.rollCCWKey) {
                            s_rollAngle -= s_settings.rollSpeed * s_frameDt;
                        } else if (code == s_settings.rollCWKey) {
                            s_rollAngle += s_settings.rollSpeed * s_frameDt;
                        }
                    }

                    if (btn->IsDown()) {
                        if (code == s_settings.resetKey) {
                            ResetAll();
                        }
                        if (s_settings.freezeTimeKey > 0 && code == s_settings.freezeTimeKey) {
                            FreezeTime::Toggle();
                            SKSE::log::info("Freeze toggled via keyboard key");
                        }
                        if (s_settings.screenshotKey > 0 && code == s_settings.screenshotKey) {
                            keybd_event(VK_SNAPSHOT, 0x2C, 0, 0);
                            keybd_event(VK_SNAPSHOT, 0x2C, KEYEVENTF_KEYUP, 0);
                            SKSE::log::info("Screenshot triggered via keyboard key");
                        }
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

        GetRotationHook::func = fcsVtable.write_vfunc(0x4, GetRotationHook::thunk);
        SKSE::log::info("FreeCameraState::GetRotation hooked (vtable[4])");

        auto* idm = RE::BSInputDeviceManager::GetSingleton();
        if (idm) {
            idm->AddEventSink(InputListener::GetSingleton());
            SKSE::log::info("Input listener registered");
        }
    }
}
