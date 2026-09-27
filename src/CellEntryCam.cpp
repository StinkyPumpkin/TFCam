#include "CellEntryCam.h"
#include "FreeCamController.h"
#include "FreeCamMenu.h"
#include "SceneTracker.h"

#include <RE/M/MouseMoveEvent.h>
#include <RE/T/ThumbstickEvent.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numbers>
#include <Windows.h>

namespace CellEntryCam {

    namespace {
        Settings s_settings;

        // ---- timing: QueryPerformanceCounter, so a paused game or another camera state never counts ----
        LARGE_INTEGER s_qpcFreq{};
        double Now() {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return static_cast<double>(t.QuadPart) / static_cast<double>(s_qpcFreq.QuadPart);
        }

        constexpr double kPendingTimeout = 8.0;   // armed but never got a third-person frame to start in
        constexpr double kStallTimeout   = 0.5;   // no Update for this long = third person left / paused
        constexpr double kFirstFrameWait = 1.5;   // first third-person frame after arming must come this soon
        constexpr float  kMaxFrameDt     = 0.1f;

        // ---- state. Main thread only (menu sink, input sink and the camera Update all run there), except
        //      the two atomics the SKSE Menu Framework page touches. ----
        bool   s_saveLoad    = false;     // the loading screen now showing is a save load / new game
        RE::TESObjectCELL* s_cellBefore = nullptr;

        bool   s_pending     = false;     // armed, waiting for the start delay
        double s_pendingAt   = 0.0;       // Now() when armed
        float  s_readyFor    = 0.0f;      // seconds of startable third-person frames so far
        int    s_pendingFrames = 0;       // third-person frames seen since arming
        std::atomic<bool> s_testRequested{ false };

        bool   s_active      = false;     // orbiting
        float  s_t           = 0.0f;      // seconds into the move
        float  s_zoomDur     = 0.0f;
        float  s_rotDur      = 0.0f;
        float  s_startYaw    = 0.0f;
        float  s_startZoom   = 0.0f;
        float  s_goalZoom    = 0.0f;
        float  s_sweep       = 0.0f;      // signed radians
        double s_lastUpdate  = 0.0;

        std::mutex s_lastLock;
        char       s_last[192] = "Nothing yet this session.";

        void SetLast(const char* a_fmt, auto... a_args) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), a_fmt, a_args...);
            {
                std::scoped_lock lk(s_lastLock);
                std::snprintf(s_last, sizeof(s_last), "%s", buf);
            }
            SKSE::log::info("CellEntryCam: {}", buf);
        }

        float SmoothStep(float x) {
            x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
            return x * x * (3.0f - 2.0f * x);
        }

        float WrapPi(float a) {
            constexpr float kPi = std::numbers::pi_v<float>;
            while (a > kPi) a -= 2.0f * kPi;
            while (a < -kPi) a += 2.0f * kPi;
            return a;
        }

        void Disarm() {
            s_pending = false;
            s_readyFor = 0.0f;
            s_pendingFrames = 0;
        }

        void Stop(const char* a_why) {
            const bool was = s_active || s_pending;
            s_active = false;
            Disarm();
            if (was) SetLast("stopped - %s", a_why);
        }

        void Arm(const char* a_why) {
            // Third person only: the move is driven by the third-person camera's own update, and a
            // first-person arrival must not fire later when the player switches views.
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (cam && cam->IsInFirstPerson()) {
                SetLast("skipped - %s, but in first person", a_why);
                return;
            }
            s_active = false;
            s_pending = true;
            s_pendingAt = Now();
            s_readyFor = 0.0f;
            s_pendingFrames = 0;
            SetLast("armed - %s", a_why);
        }

        // What must hold on a frame for the move to start (or keep waiting).
        const char* BlockReason() {
            if (FreeCam::IsActive()) return "free camera is on";
            if (FreeCamMenu::IsOverlayOpen()) return "SKSE menu open";
            if (SceneTracker::PlayerSceneActive()) return "player in a SexLab scene";
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->GameIsPaused()) return "game paused";
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return "no player";
            if (s_settings.skipInCombat && player->IsInCombat()) return "in combat";
            return nullptr;
        }

        void Start(RE::ThirdPersonState* a_tps) {
            const auto& s = s_settings;
            constexpr float kDeg = std::numbers::pi_v<float> / 180.0f;
            s_startYaw  = a_tps->freeRotation.x;
            s_startZoom = a_tps->targetZoomOffset;
            s_goalZoom  = s.zoom < -0.2f ? -0.2f : (s.zoom > 1.0f ? 1.0f : s.zoom);
            s_zoomDur   = (s.changeZoom && s.zoomSpeed > 0.0f) ? std::fabs(s_goalZoom - s_startZoom) / s.zoomSpeed : 0.0f;
            // +x swings the camera round the player's left side (camera yaw = player heading + freeRotation.x)
            s_sweep     = (s.direction == 1 ? -1.0f : 1.0f) * s.rotation * kDeg;
            s_rotDur    = s.rotationSpeed > 0.0f ? std::fabs(s.rotation) / s.rotationSpeed : 0.0f;
            s_t         = 0.0f;
            s_active    = true;
            Disarm();
            SetLast("started - zoom %.2f -> %.2f over %.1fs, then %.0f deg %s over %.1fs",
                s_startZoom, s.changeZoom ? s_goalZoom : s_startZoom, s_zoomDur, s.rotation,
                s.direction == 1 ? "right" : "left", s_rotDur);
        }

        void Drive(RE::ThirdPersonState* a_tps, float a_dt) {
            s_t += a_dt;
            // zoom first ...
            if (s_settings.changeZoom) {
                const float z = s_zoomDur > 0.0f
                    ? s_startZoom + (s_goalZoom - s_startZoom) * SmoothStep(s_t / s_zoomDur)
                    : s_goalZoom;
                a_tps->targetZoomOffset  = z;
                a_tps->currentZoomOffset = z;
            }
            // ... then the orbit. Free rotation = the camera turns round the player, the player stays put.
            const float tr = s_t - s_zoomDur;
            const float k = s_rotDur > 0.0f ? SmoothStep(tr / s_rotDur) : (tr >= 0.0f ? 1.0f : 0.0f);
            a_tps->freeRotationEnabled = true;
            a_tps->freeRotation.x = WrapPi(s_startYaw + s_sweep * k);

            if (s_t >= s_zoomDur + s_rotDur) {
                s_active = false;
                SetLast("finished (%.1fs) - camera handed back", s_t);
            }
        }

        // Called before the game's own ThirdPersonState::Update, every third-person frame.
        void OnThirdPersonUpdate(RE::ThirdPersonState* a_tps) {
            const double now = Now();
            float dt = s_lastUpdate > 0.0 ? static_cast<float>(now - s_lastUpdate) : 0.0f;
            const bool stalled = s_lastUpdate > 0.0 && (now - s_lastUpdate) > kStallTimeout;
            s_lastUpdate = now;
            if (dt > kMaxFrameDt) dt = kMaxFrameDt;

            if (s_testRequested.load() && !FreeCamMenu::IsOverlayOpen()) {
                s_testRequested.store(false);
                Arm("test from the settings page");
            }

            if (s_active) {
                if (stalled) { Stop("left third person / game paused mid-move"); return; }
                if (const char* why = BlockReason()) { Stop(why); return; }
                Drive(a_tps, dt);
                return;
            }

            if (!s_pending) return;
            if (now - s_pendingAt > kPendingTimeout) { Stop("no third-person gameplay within 8s"); return; }
            // Third person from the loading screen on: the first frame must come at once, and any gap
            // (first person, a different camera, a pause) drops it instead of firing later.
            if (s_pendingFrames == 0) {
                if (now - s_pendingAt > kFirstFrameWait) { Stop("not in third person after the loading screen"); return; }
            } else if (stalled) {
                Stop("left third person before it started");
                return;
            }
            ++s_pendingFrames;
            if (BlockReason()) { s_readyFor = 0.0f; return; }
            s_readyFor += dt;
            if (s_readyFor >= s_settings.startDelay) Start(a_tps);
        }

        struct UpdateHook {
            static void thunk(RE::ThirdPersonState* a_this, RE::BSTSmartPointer<RE::TESCameraState>& a_next) {
                if (a_this && (s_active || s_pending || s_testRequested.load())) {
                    OnThirdPersonUpdate(a_this);
                } else {
                    s_lastUpdate = Now();
                }
                func(a_this, a_next);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // Loading screens: note the cell before, decide on the way out.
        class LoadWatch final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
        public:
            static LoadWatch* Get() { static LoadWatch w; return &w; }

            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_evt,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
                if (!a_evt || a_evt->menuName != RE::LoadingMenu::MENU_NAME) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (a_evt->opening) {
                    s_cellBefore = player ? player->GetParentCell() : nullptr;
                    Stop("loading screen");
                    return RE::BSEventNotifyControl::kContinue;
                }

                const bool saveLoad = s_saveLoad;
                s_saveLoad = false;
                const auto& s = s_settings;
                if (!s.enabled) return RE::BSEventNotifyControl::kContinue;

                auto* cell = player ? player->GetParentCell() : nullptr;
                if (!cell) return RE::BSEventNotifyControl::kContinue;
                const bool interior = cell->IsInteriorCell();
                const char* name = cell->GetFullName();
                if (!name || !*name) name = cell->GetFormEditorID();
                if (!name || !*name) name = interior ? "interior" : "exterior";

                if (saveLoad) {
                    if (s.afterSaveLoad) Arm("save loaded");
                    else SetLast("skipped - a save load (option off), %s", name);
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (cell == s_cellBefore) {
                    SetLast("skipped - same cell after the loading screen (%s)", name);
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (interior ? !s.onInteriors : !s.onExteriors) {
                    SetLast("skipped - %s (%s entries are off)", name, interior ? "interior" : "exterior");
                    return RE::BSEventNotifyControl::kContinue;
                }
                char why[160];
                std::snprintf(why, sizeof(why), "entered %s (%s)", name, interior ? "interior" : "exterior");
                Arm(why);
                return RE::BSEventNotifyControl::kContinue;
            }
        };
    }

    Settings& GetSettings() { return s_settings; }

    bool IsRunning() { return s_active || s_pending; }

    const char* LastEvent() {
        static thread_local char copy[192];
        std::scoped_lock lk(s_lastLock);
        std::snprintf(copy, sizeof(copy), "%s", s_last);
        return copy;
    }

    void TestOnMenuClose() { s_testRequested.store(true); }

    void OnPreLoadGame() {
        s_saveLoad = true;
        Stop("save loading");
    }

    void OnInput(RE::InputEvent* const* a_events) {
        if (!a_events || !(s_active || s_pending)) return;
        for (auto* evt = *a_events; evt; evt = evt->next) {
            switch (evt->GetEventType()) {
            case RE::INPUT_EVENT_TYPE::kMouseMove: {
                auto* mm = static_cast<RE::MouseMoveEvent*>(evt);
                if (mm->mouseInputX != 0 || mm->mouseInputY != 0) { Stop("mouse moved"); return; }
                break;
            }
            case RE::INPUT_EVENT_TYPE::kThumbstick: {
                auto* ts = static_cast<RE::ThumbstickEvent*>(evt);
                const bool moved = std::fabs(ts->xValue) + std::fabs(ts->yValue) > 0.15f;
                if (!moved) break;
                if (ts->GetIDCode() == RE::ThumbstickEvent::InputType::kRightThumbstick) { Stop("right stick moved"); return; }
                if (s_settings.keysCancel) { Stop("left stick moved"); return; }
                break;
            }
            case RE::INPUT_EVENT_TYPE::kButton: {
                auto* btn = evt->AsButtonEvent();
                if (!btn || !btn->IsDown()) break;
                const bool wheel = btn->GetDevice() == RE::INPUT_DEVICE::kMouse &&
                                   (btn->GetIDCode() == 8 || btn->GetIDCode() == 9);
                if (wheel) { Stop("mouse wheel"); return; }
                if (s_settings.keysCancel) { Stop("key / button pressed"); return; }
                break;
            }
            default:
                break;
            }
        }
    }

    void Install() {
        QueryPerformanceFrequency(&s_qpcFreq);
        if (REL::Module::IsVR()) {
            SKSE::log::info("CellEntryCam: not installed on VR (ThirdPersonState layout differs)");
            return;
        }
        // Update: slot 3 on SE and AE (1.6 and 1.7), 4 on VR - same as FreeCameraState's.
        const std::size_t kUpdateSlot = REL::Relocate<std::size_t>(0x3, 0x3, 0x4);
        REL::Relocation<std::uintptr_t> vtbl(RE::VTABLE_ThirdPersonState[0]);
        UpdateHook::func = vtbl.write_vfunc(kUpdateSlot, UpdateHook::thunk);
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(LoadWatch::Get());
        }
        SKSE::log::info("CellEntryCam: ThirdPersonState::Update hooked (vtable[{}]), loading-screen watcher registered",
            kUpdateSlot);
    }
}
