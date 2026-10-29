#include "FovFx.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace FovFx {

    namespace {
        // degrees at strength 1 (+ = wider / out, - = narrower / in)
        constexpr float kBowPull     = -8.0f;   // reached after drawing for kBowPullTime
        constexpr float kBowPullTime = 1.2f;
        constexpr float kBowRelease  = 7.0f;
        constexpr float kPowerPunch  = -10.0f;
        constexpr float kSpellWarp   = 9.0f;
        constexpr float kShoutWarp   = 14.0f;
        constexpr float kSpeedMax    = 15.0f;   // at kSpeedFull units / second
        constexpr float kSpeedStart  = 350.0f;  // running
        constexpr float kSpeedFull   = 750.0f;   // a sprint (~500) stretches about a third of the way
        constexpr float kRise        = 0.07f;   // a pulse's snap
        constexpr float kFall        = 0.35f;   // ... and its way back (time constant)

        Settings s_settings;

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

        float SmoothStep(float a_x) {
            a_x = std::clamp(a_x, 0.0f, 1.0f);
            return a_x * a_x * (3.0f - 2.0f * a_x);
        }

        // ---- events (any thread) -> the frame (main thread)
        struct Pending {
            SKSE::ActionEvent::Type type;
            RE::FormID              form;
        };
        std::mutex           s_lock;
        std::vector<Pending> s_pending;

        class Sink : public RE::BSTEventSink<SKSE::ActionEvent> {
        public:
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ActionEvent* a_event, RE::BSTEventSource<SKSE::ActionEvent>*) override {
                if (a_event && s_settings.enabled && a_event->actor && a_event->actor->IsPlayerRef()) {
                    std::scoped_lock lock(s_lock);
                    if (s_pending.size() < 32) {
                        s_pending.push_back({ a_event->type.get(), a_event->sourceForm ? a_event->sourceForm->GetFormID() : 0 });
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        Sink s_sink;

        // ---- state (main thread)
        struct Pulse {
            double start;
            float  amp;
        };
        std::vector<Pulse> s_pulses;
        bool               s_bowDrawn  = false;
        double             s_bowStart  = 0.0;
        float              s_bowNow    = 0.0f;  // the pull, smoothed
        double             s_last      = 0.0;
        RE::NiPoint3       s_lastPos{};
        bool               s_havePos   = false;
        float              s_speed     = 0.0f;  // smoothed, units / second
        float              s_stretch   = 0.0f;
        double             s_spellAt   = 0.0;   // concentration spells fire often: one pulse per second
        // the view layer (see Apply)
        struct Sides {
            float l = 0.0f, r = 0.0f, t = 0.0f, b = 0.0f;
        };
        bool  s_applying = false;
        Sides s_base{};
        Sides s_written{};
        float s_peak = 0.0f;  // the biggest change of one effect, degrees

        void Kick(float a_amp, const char* a_what) {
            s_pulses.push_back({ Now(), a_amp });
            if (s_pulses.size() > 16) s_pulses.erase(s_pulses.begin());
            SKSE::log::info("FovFx: {} ({:+.0f} deg)", a_what, a_amp * s_settings.strength);
        }

        bool PowerAttacking(RE::PlayerCharacter* a_player) {
            auto* process = a_player->GetActorRuntimeData().currentProcess;
            auto* high    = process ? process->high : nullptr;
            auto* attack  = high ? high->attackData.get() : nullptr;
            return attack && attack->data.flags.any(RE::AttackData::AttackFlag::kPowerAttack);
        }

        int SpellLevel(RE::FormID a_form) {
            auto* magic  = a_form ? RE::TESForm::LookupByID<RE::MagicItem>(a_form) : nullptr;
            auto* effect = magic ? magic->GetCostliestEffectItem() : nullptr;
            return effect && effect->baseEffect ? effect->baseEffect->GetMinimumSkillLevel() : -1;
        }

        void Handle(const Pending& a_p, RE::PlayerCharacter* a_player) {
            using T = SKSE::ActionEvent::Type;
            const auto& s = s_settings;
            static int s_seen = 0;  // the first events of a session, for checking what fires
            if (s_seen < 40) {
                ++s_seen;
                SKSE::log::info("FovFx: action event {} (form {:08X}{})", static_cast<int>(a_p.type), a_p.form,
                    a_p.type == T::kWeaponSwing ? (PowerAttacking(a_player) ? ", a power attack" : ", not a power attack") : "");
            }
            switch (a_p.type) {
            case T::kBowDraw:
                if (s.bow) {
                    s_bowDrawn = true;
                    s_bowStart = Now();
                }
                break;
            case T::kBowRelease:
                if (s.bow) {
                    s_bowDrawn = false;
                    Kick(kBowRelease, "bow released");
                }
                break;
            case T::kBeginSheathe:
            case T::kEndSheathe:
                s_bowDrawn = false;
                break;
            case T::kWeaponSwing:
                if (s.power && PowerAttacking(a_player)) Kick(kPowerPunch, "power attack");
                break;
            case T::kSpellFire:
                if (s.spells) {
                    const int lvl = SpellLevel(a_p.form);
                    if (lvl >= s.spellLevel && Now() - s_spellAt > 1.0) {
                        s_spellAt = Now();
                        Kick(kSpellWarp, "big spell");
                    }
                }
                break;
            case T::kVoiceFire:
                if (s.shouts) Kick(kShoutWarp, "shout");
                break;
            default:
                break;
            }
        }

        constexpr float kDegToRad = 0.01745329f;
        bool Same(const RE::NiFrustum& a_f, const Sides& a_s) {
            return a_f.fLeft == a_s.l && a_f.fRight == a_s.r && a_f.fTop == a_s.t && a_f.fBottom == a_s.b;
        }
        void Write(RE::NiCamera* a_cam, const Sides& a_s) {
            auto& f   = a_cam->GetRuntimeData2().viewFrustum;
            f.fLeft   = a_s.l;
            f.fRight  = a_s.r;
            f.fTop    = a_s.t;
            f.fBottom = a_s.b;
            RE::NiUpdateData data{};
            a_cam->Update(data);  // its view-projection now, for this frame
        }

        // The offset goes on the render camera's view (its frustum), after everyone has set the FOV - the game's FOV
        // value itself is left alone. A mod that holds the FOV never sees us then: Camera Studio's world-FOV override
        // re-reads it every frame and "rebases" on any outside change, and fought a worldFOV offset down to 40 (10-29).
        // The frustum's sides are tangents at distance 1; sides someone else wrote since our last write are the new base.
        void Apply(float a_offset) {
            auto* cam = RE::Main::WorldRootCamera();
            if (!cam) return;
            const auto& f = cam->GetRuntimeData2().viewFrustum;
            if (f.bOrtho) return;
            const bool  ours = s_applying && Same(f, s_written);
            const Sides base = ours ? s_base : Sides{ f.fLeft, f.fRight, f.fTop, f.fBottom };
            const float hw   = (base.r - base.l) * 0.5f;
            if (std::fabs(a_offset) < 0.05f || hw <= 1e-4f) {
                if (ours) Write(cam, base);
                if (s_applying && std::fabs(s_peak) > 1.0f) {
                    SKSE::log::info("FovFx: view {:.0f} -> {:.0f} degrees wide and back", 2.0f * std::atan(hw) / kDegToRad,
                        2.0f * std::atan(hw) / kDegToRad + s_peak);
                }
                s_applying = false;
                s_peak     = 0.0f;
                return;
            }
            const float half = std::atan(hw);
            const float want = std::clamp(half + a_offset * 0.5f * kDegToRad, 0.1f, 1.45f);
            const float k    = std::tan(want) / hw;
            const Sides out{ base.l * k, base.r * k, base.t * k, base.b * k };
            Write(cam, out);
            s_base     = base;
            s_written  = out;
            s_applying = true;
            const float shown = 2.0f * (want - half) / kDegToRad;
            if (std::fabs(shown) > std::fabs(s_peak)) s_peak = shown;
        }
    }

    Settings& Get() { return s_settings; }

    void Clamp() {
        auto& s = s_settings;
        if (!std::isfinite(s.strength)) s.strength = 1.0f;
        s.strength   = std::clamp(s.strength, 0.0f, 2.0f);
        s.spellLevel = std::clamp(s.spellLevel, 0, 100);
        if (!std::isfinite(s.aimBack)) s.aimBack = 25.0f;
        if (!std::isfinite(s.aimSide)) s.aimSide = 20.0f;
        if (!std::isfinite(s.aimUp)) s.aimUp = 5.0f;
        s.aimBack = std::clamp(s.aimBack, 10.0f, 80.0f);
        s.aimSide = std::clamp(s.aimSide, -30.0f, 30.0f);
        s.aimUp   = std::clamp(s.aimUp, -15.0f, 20.0f);
    }

    void Install() {
        if (auto* src = SKSE::GetActionEventSource()) {
            src->AddEventSink(&s_sink);
            SKSE::log::info("FovFx: action events registered");
        }
    }

    // The archer camera: the view the game made, moved to just behind the eyes (its own aim direction is kept, so the
    // crosshair still aims where you look). In over ~0.12 s as the string comes back, out over ~0.2 s.
    void ApplyAimCam() {
        static double s_aimLast = 0.0;
        static float  s_aimW    = 0.0f;
        const double  now       = Now();
        const float   dt        = s_aimLast > 0.0 ? static_cast<float>(std::clamp(now - s_aimLast, 0.0, 0.1)) : 0.0f;
        s_aimLast               = now;
        auto* cam    = RE::PlayerCamera::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui     = RE::UI::GetSingleton();
        if (!s_settings.enabled || !s_settings.aimCam || !cam || !cam->cameraRoot || !player || cam->IsInFreeCameraMode() ||
            cam->IsInFirstPerson()) {
            s_aimW = 0.0f;
            return;
        }
        if (!(ui && ui->GameIsPaused())) {
            const float target = s_bowDrawn ? 1.0f : 0.0f;
            const float tau    = target > s_aimW ? 0.12f : 0.2f;
            s_aimW += (target - s_aimW) * (dt > 0.0f ? 1.0f - std::exp(-dt / tau) : 0.0f);
        }
        if (s_aimW < 0.002f) return;
        auto* body = player->Get3D2();
        auto* head = body ? body->GetObjectByName("NPC Head [Head]") : nullptr;
        if (!head) return;
        auto*           root = cam->cameraRoot.get();
        RE::NiTransform t    = root->world;
        const auto&     m    = t.rotate.entry;
        const RE::NiPoint3 right{ m[0][0], m[1][0], m[2][0] };
        const RE::NiPoint3 fwd{ m[0][1], m[1][1], m[2][1] };
        const RE::NiPoint3 up{ m[0][2], m[1][2], m[2][2] };
        const float        h = player->GetAngleZ();
        RE::NiPoint3       eye = head->world.translate + RE::NiPoint3{ std::sin(h) * 4.0f, std::cos(h) * 4.0f, 7.0f };
        const RE::NiPoint3 want = eye - fwd * s_settings.aimBack + right * s_settings.aimSide + up * s_settings.aimUp;
        const float        w    = SmoothStep(s_aimW);
        t.translate             = t.translate + (want - t.translate) * w;
        root->local             = root->parent ? root->parent->world.Invert() * t : t;
        RE::NiUpdateData data{};
        root->Update(data);
        static bool s_logged = false;
        if (!s_logged && s_aimW > 0.95f) {
            s_logged = true;
            SKSE::log::info("FovFx: archer camera in - {:.0f} units from the eyes", (want - eye).Length());
        }
        if (s_aimW < 0.05f) s_logged = false;
    }

    void Reset() {
        if (s_applying) {
            auto* cam = RE::Main::WorldRootCamera();
            if (cam && Same(cam->GetRuntimeData2().viewFrustum, s_written)) Write(cam, s_base);
            s_applying = false;
            s_peak     = 0.0f;
        }
        s_pulses.clear();
        s_bowDrawn = false;
        s_bowNow   = 0.0f;
        s_stretch  = 0.0f;
        s_havePos  = false;
    }

    void Tick() {
        const double now = Now();
        const float  dt  = s_last > 0.0 ? static_cast<float>(std::clamp(now - s_last, 0.0, 0.1)) : 0.0f;
        s_last           = now;
        std::vector<Pending> events;
        {
            std::scoped_lock lock(s_lock);
            events.swap(s_pending);
        }
        auto* ui     = RE::UI::GetSingleton();
        auto* cam    = RE::PlayerCamera::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!s_settings.enabled || !cam || !player || (ui && ui->GameIsPaused())) {
            if (s_applying && (!s_settings.enabled || !cam)) Reset();
            return;
        }
        const bool freeCam = cam->IsInFreeCameraMode();
        if (!freeCam) {
            for (const auto& e : events) Handle(e, player);
            // The bow's pull follows the attack state (drawing / held): the BowDraw action event does not always come
            // (10-29: an animation-event draw sent BeginDraw, EndDraw and BowRelease only).
            if (s_settings.bow || s_settings.aimCam) {
                using A           = RE::ATTACK_STATE_ENUM;
                const A    st      = player->AsActorState()->GetAttackState();
                const bool drawing = st == A::kBowDraw || st == A::kBowAttached || st == A::kBowDrawn;
                if (drawing && !s_bowDrawn) s_bowStart = now;
                s_bowDrawn = drawing;
            }
        } else {
            s_bowDrawn = false;  // your actions only count when the camera is on you
        }

        // speed: yours (on foot / on a horse) or the free camera's
        RE::NiPoint3 pos{};
        bool         useSpeed = false;
        if (freeCam && s_settings.speedCamera && cam->cameraRoot) {
            pos      = cam->cameraRoot->world.translate;
            useSpeed = true;
        } else if (!freeCam && s_settings.speedPlayer) {
            pos      = player->GetPosition();
            useSpeed = true;
        }
        float target = 0.0f;
        if (useSpeed && dt > 0.0f) {
            if (s_havePos) {
                const float d = std::hypot(pos.x - s_lastPos.x, pos.y - s_lastPos.y);
                if (d < 4000.0f * dt) {  // not a teleport / load / fast travel
                    const float raw = d / dt;
                    s_speed += (raw - s_speed) * (1.0f - std::exp(-dt / 0.2f));
                }
            }
            s_lastPos = pos;
            s_havePos = true;
            target    = kSpeedMax * SmoothStep((s_speed - kSpeedStart) / (kSpeedFull - kSpeedStart));
        } else {
            s_havePos = false;
            s_speed   = 0.0f;
        }
        s_stretch += (target - s_stretch) * (dt > 0.0f ? 1.0f - std::exp(-dt / 0.25f) : 0.0f);
        static bool s_stretchLogged = false;
        if (!s_stretchLogged && s_stretch > 2.0f) {
            s_stretchLogged = true;
            SKSE::log::info("FovFx: speed stretch ({:.0f} units / s{})", s_speed, freeCam ? ", the free camera" : "");
        } else if (s_stretchLogged && s_stretch < 0.5f) {
            s_stretchLogged = false;
            SKSE::log::info("FovFx: speed stretch over");
        }

        // the bow's pull while drawn, eased back after
        const float bowTarget = s_bowDrawn && s_settings.bow ? kBowPull * SmoothStep(static_cast<float>(now - s_bowStart) / kBowPullTime) : 0.0f;
        s_bowNow += (bowTarget - s_bowNow) * (dt > 0.0f ? 1.0f - std::exp(-dt / (s_bowDrawn ? 0.15f : 0.08f)) : 0.0f);

        float pulses = 0.0f;
        for (const auto& p : s_pulses) {
            const float t = static_cast<float>(now - p.start);
            pulses += t < kRise ? p.amp * SmoothStep(t / kRise) : p.amp * std::exp(-(t - kRise) / kFall);
        }
        std::erase_if(s_pulses, [now](const Pulse& p) { return now - p.start > 3.0; });

        Apply(std::clamp((pulses + s_bowNow + s_stretch) * s_settings.strength, -40.0f, 40.0f));
    }
}
