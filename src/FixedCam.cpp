#include "FixedCam.h"
#include "AimDot.h"
#include "CamFocus.h"
#include "CamPath.h"
#include "CamPathPrisma.h"
#include "ConvoCam.h"
#include "FreeCamController.h"
#include "FovFx.h"
#include "FreezeTime.h"
#include "SlccBridge.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <format>
#include <string>

namespace FixedCam {

    namespace {
        constexpr float kLn2         = 0.69314718f;
        constexpr float kWatchHalf   = 0.25f;  // seconds: how quickly the watching camera turns after you
        constexpr float kWatchRaise  = 20.0f;  // units above your middle (chest height)
        constexpr float kPitchLimit  = 1.45f;

        std::atomic<bool> s_on{ false };
        std::atomic<bool> s_watch{ false };
        // main thread
        RE::NiPoint3     s_pos{};
        RE::NiQuaternion s_rot{};
        float            s_fov      = 0.0f;
        float            s_savedFov = 0.0f;
        bool             s_fovSaved = false;
        float            s_yaw = 0.0f, s_pitch = 0.0f;  // the watching camera's own turn
        double           s_last = 0.0;

        double Now() {
            static const double freq = [] {
                LARGE_INTEGER f;
                ::QueryPerformanceFrequency(&f);
                return static_cast<double>(f.QuadPart);
            }();
            LARGE_INTEGER t;
            ::QueryPerformanceCounter(&t);
            return static_cast<double>(t.QuadPart) / freq;
        }

        void Notify(const std::string& a_text) { RE::SendHUDMessage::ShowHUDMessage(a_text.c_str()); }

        void Queue(std::function<void()> a_fn) {
            if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask(std::move(a_fn));
        }

        float& WorldFOV() {
            static float dummy = 0.0f;
            auto*        cam   = RE::PlayerCamera::GetSingleton();
            return cam ? cam->GetRuntimeData2().worldFOV : dummy;
        }

        // A camera rotation looking along yaw / pitch (TFCam's convention: + pitch looks down), no roll. The camera
        // root's axes: X right, Y forward, Z up (columns).
        RE::NiQuaternion LookRotation(float a_yaw, float a_pitch) {
            const RE::NiPoint3 fwd{ std::sin(a_yaw) * std::cos(a_pitch), std::cos(a_yaw) * std::cos(a_pitch), -std::sin(a_pitch) };
            RE::NiPoint3       right{ fwd.y, -fwd.x, 0.0f };
            const float        len = std::hypot(right.x, right.y);
            right                  = len > 1e-5f ? right * (1.0f / len) : RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
            const RE::NiPoint3 up{ right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z, right.x * fwd.y - right.y * fwd.x };
            RE::NiMatrix3      m;
            m.entry[0][0] = right.x, m.entry[1][0] = right.y, m.entry[2][0] = right.z;
            m.entry[0][1] = fwd.x, m.entry[1][1] = fwd.y, m.entry[2][1] = fwd.z;
            m.entry[0][2] = up.x, m.entry[1][2] = up.y, m.entry[2][2] = up.z;
            return RE::NiQuaternion(m);
        }

        void Release(std::string_view a_why) {
            if (!s_on.exchange(false)) return;
            if (s_fovSaved && s_savedFov > 0.0f) WorldFOV() = s_savedFov;
            s_fovSaved = false;
            SKSE::log::info("FixedCam: released ({}) - the camera follows you again", a_why);
        }

        // Every frame of the third-person camera while fixed (main thread).
        void FrameWork() {
            FreezeTime::Tick();
            CamPathPrisma::Tick();  // the PrismaUI editor's state push (while open)
            ConvoCam::FrameCheck();  // a dialogue the NPC started
            if (s_on) {
                if (!s_fovSaved) {  // the free camera's exit has put the game's own FOV back by now
                    s_savedFov = WorldFOV();
                    s_fovSaved = true;
                }
                if (s_fov > 0.0f) WorldFOV() = s_fov;
            }
            FovFx::Tick();  // last: its offset goes on top of everyone's FOV
        }

        RE::NiQuaternion WatchRotation() {
            const double now = Now();
            const float  dt  = s_last > 0.0 ? static_cast<float>(std::clamp(now - s_last, 0.0, 0.1)) : 0.0f;
            s_last           = now;
            RE::NiPoint3 target{};
            auto*        player = RE::PlayerCharacter::GetSingleton();
            if (player && CamFocus::RefPoint(player, target)) {
                target.z += kWatchRaise;
                const RE::NiPoint3 d    = target - s_pos;
                const float        flat = std::hypot(d.x, d.y);
                if (flat > 1.0f || std::fabs(d.z) > 1.0f) {
                    const float yaw   = std::atan2(d.x, d.y);
                    const float pitch = std::clamp(std::atan2(-d.z, flat), -kPitchLimit, kPitchLimit);
                    const float w     = dt <= 0.0f ? 1.0f : 1.0f - std::exp(-kLn2 * dt / kWatchHalf);
                    s_yaw += std::remainder(yaw - s_yaw, 6.2831853f) * w;
                    s_pitch += (pitch - s_pitch) * w;
                }
            }
            return LookRotation(s_yaw, s_pitch);
        }

        // Walking goes by the view (forward = away from the camera): the hidden third-person camera's free rotation is
        // turned so its yaw - what the game and True Directional Movement steer by - is the fixed view's. Its own view
        // is never seen (ours replaces it). 10-29: hard to steer with the keys still going by the camera behind you.
        void SteerByView(const RE::NiMatrix3& a_view) {
            auto* cam    = RE::PlayerCamera::GetSingleton();
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!cam || !player || !cam->currentState) return;
            const auto id = cam->currentState->id;
            if (id != RE::CameraState::kThirdPerson && id != RE::CameraState::kMount) return;  // a horse's is one too
            auto*       tps     = static_cast<RE::ThirdPersonState*>(cam->currentState.get());
            const float viewYaw = std::atan2(a_view.entry[0][1], a_view.entry[1][1]);
            tps->freeRotationEnabled   = true;
            tps->freeRotation.x        = std::remainder(viewYaw - player->GetAngleZ(), 6.2831853f);
            cam->GetRuntimeData2().yaw = viewYaw;
        }

        // The camera root takes the fixed view, after the game has placed it (any camera state but the free camera).
        void ApplyRoot() {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam || !cam->cameraRoot || cam->IsInFreeCameraMode()) return;
            RE::NiTransform t;
            t.rotate    = (s_watch ? WatchRotation() : s_rot).ToRotation();
            t.translate = s_pos;
            t.scale     = 1.0f;
            auto* root  = cam->cameraRoot.get();
            root->local = root->parent ? root->parent->world.Invert() * t : t;
            RE::NiUpdateData data{};
            root->Update(data);
            SteerByView(t.rotate);
        }

        // PlayerCamera::Update's call that updates the camera root (Cinematic Mode's site, + 0x1A6 on SE and AE).
        // Every frame, every camera state - also the full freeze's tick.
        struct CameraUpdateHook {
            static void thunk(RE::TESCamera* a_camera) {
                func(a_camera);
                FrameWork();
                if (s_on) {
                    ApplyRoot();
                } else {
                    FovFx::ApplyAimCam();  // the archer camera (Action FOV)
                }
                AimDot::CrosshairTick();  // the game's crosshair off while the dot is the crosshair
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void Install() {
        if (REL::Module::IsVR()) {
            SKSE::log::info("FixedCam: not on VR");
            return;
        }
        // The exe's code is only readable at run time (on disk it is wrapped), so the site is checked here: a 5-byte
        // call, or no hook at all.
        REL::Relocation<std::uintptr_t> update{ RELOCATION_ID(49852, 50784) };
        const std::uintptr_t site = update.address() + REL::Relocate(0x1A6, 0x1A6);
        const auto*          code = reinterpret_cast<const std::uint8_t*>(site);
        if (code[0] != 0xE8) {
            SKSE::log::warn("FixedCam: PlayerCamera::Update + 0x1A6 is not a call (byte {:02X}) - no fixed camera / frame tick",
                code[0]);
            return;
        }
        const auto target = site + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
        CameraUpdateHook::func = SKSE::GetTrampoline().write_call<5>(site, CameraUpdateHook::thunk);
        SKSE::log::info("FixedCam: PlayerCamera::Update + 0x1A6 hooked (it called SkyrimSE.exe+{:X})",
            target - REL::Module::get().base());
    }

    bool IsOn() { return s_on; }
    bool Watching() { return s_watch; }

    void SetWatch(bool a_on) {
        if (s_watch.exchange(a_on) == a_on) return;
        Queue([a_on] {
            if (a_on) {  // start turning from the fixed view
                const RE::NiMatrix3 m = s_rot.ToRotation();
                const RE::NiPoint3  f{ m.entry[0][1], m.entry[1][1], m.entry[2][1] };
                s_yaw   = std::atan2(f.x, f.y);
                s_pitch = std::atan2(-f.z, std::hypot(f.x, f.y));
                s_last  = 0.0;
            }
            SKSE::log::info("FixedCam: {}", a_on ? "turns to watch you" : "holds its view");
        });
    }

    void RequestFixHere() {
        Queue([] {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam || !cam->IsInFreeCameraMode() || !FreeCam::TFCamDriving() || !cam->currentState) {
                Notify("Fixed camera: fly the free camera to the shot first");
                return;
            }
            if (CamPath::Playing()) CamPath::Stop("the camera was fixed");
            auto* state = cam->currentState.get();
            RE::NiQuaternion q;
            RE::NiPoint3     p;
            state->GetRotation(q);  // with TFCam's roll
            state->GetTranslation(p);
            s_rot      = q;
            s_pos      = p;
            s_fov      = WorldFOV();
            s_fovSaved = false;
            auto* fcs  = static_cast<RE::FreeCameraState*>(state);
            s_yaw      = fcs->rotation.y;
            s_pitch    = std::clamp(static_cast<float>(std::remainder(fcs->rotation.x, 6.2831853f)), -kPitchLimit, kPitchLimit);
            s_last     = 0.0;
            // convention check: the forward axis of the free camera's own rotation vs ours from its yaw / pitch
            const RE::NiMatrix3 a = q.ToRotation();
            const RE::NiMatrix3 b = LookRotation(s_yaw, s_pitch).ToRotation();
            const float dot = a.entry[0][1] * b.entry[0][1] + a.entry[1][1] * b.entry[1][1] + a.entry[2][1] * b.entry[2][1];
            s_on = true;
            SKSE::log::info("FixedCam: fixed at ({:.0f}, {:.0f}, {:.0f}), FOV {:.0f} (forward check {:.3f})", p.x, p.y, p.z, s_fov, dot);
            SlccBridge::LeaveFreeFly("fixed camera");
            Notify("Camera fixed here - play on. Camera Path window: Let the camera go");
        });
    }

    void RequestRelease(std::string_view a_why) {
        Queue([why = std::string(a_why)] { Release(why); });
    }

    // The free camera starts where the fixed view was (Begin placed it at the third-person camera), so a shot can be
    // moved on from there.
    void OnFreeCamBegin() {
        if (!s_on) return;
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (cam && cam->IsInFreeCameraMode() && cam->currentState) {
            auto*               fcs = static_cast<RE::FreeCameraState*>(cam->currentState.get());
            const RE::NiMatrix3 m   = (s_watch ? LookRotation(s_yaw, s_pitch) : s_rot).ToRotation();
            const RE::NiPoint3  f{ m.entry[0][1], m.entry[1][1], m.entry[2][1] };
            fcs->translation = s_pos;
            fcs->rotation.x  = std::atan2(-f.z, std::hypot(f.x, f.y));
            fcs->rotation.y  = std::atan2(f.x, f.y);
        }
        Release("the free camera started - from the fixed view");
    }

    void OnGameLoad() { Release("a game load"); }
}
