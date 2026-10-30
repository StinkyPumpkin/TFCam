#include "FovFx.h"
#include "AimDot.h"
#include "CamBlur.h"
#include "CamCollision.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace FovFx {

    namespace {
        // degrees at strength 1 (+ = wider / out, - = narrower / in)
        constexpr float kBowPull     = -8.0f;   // reached after drawing for kBowPullTime
        constexpr float kBowPullTime = 0.6f;    // 10-31: about a pull (was 1.2, from the quiver)
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
                // 10-31: nothing here - this event comes with the arrow leaving the quiver; the attack state in Tick()
                // (kBowAttached: the string being pulled) turns s_bowDrawn on, so the view moves with the pull.
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
        if (!std::isfinite(s.aimTurn)) s.aimTurn = 22.0f;
        if (!std::isfinite(s.aimBack)) s.aimBack = 61.0f;
        if (!std::isfinite(s.aimUp)) s.aimUp = -1.0f;
        s.aimTurn = std::clamp(s.aimTurn, -110.0f, 110.0f);
        s.aimBack = std::clamp(s.aimBack, 10.0f, 400.0f);  // 10-31: further out (was 120)
        s.aimUp   = std::clamp(s.aimUp, -80.0f, 80.0f);    // 10-31: was -20..30
        if (!std::isfinite(s.aimDotSize)) s.aimDotSize = 1.2f;
        s.aimDotSize = std::clamp(s.aimDotSize, 0.5f, 8.0f);
        if (!std::isfinite(s.aimVignette)) s.aimVignette = 0.0f;
        s.aimVignette = std::clamp(s.aimVignette, 0.0f, 1.0f);
    }

    namespace {
        // 10-31: the archer view, for the image pass (main thread)
        bool         s_pubOn = false;
        float        s_pubIn = 0.0f;
        RE::NiPoint3 s_pubAim{};
        bool         s_hwya = false;
    }

    bool ArcherAim(RE::NiPoint3& a_aimAt, float& a_in) {
        a_aimAt = s_pubAim;
        a_in    = s_pubIn;
        return s_pubOn;
    }

    bool HitWhereYouAim() { return s_hwya; }

    float ArcherVignette() { return s_pubOn ? s_settings.aimVignette * std::clamp(s_pubIn, 0.0f, 1.0f) : 0.0f; }

    void Install() {
        if (auto* src = SKSE::GetActionEventSource()) {
            src->AddEventSink(&s_sink);
            SKSE::log::info("FovFx: action events registered");
        }
        s_hwya = ::GetModuleHandleW(L"HitWhereYouAim.dll") != nullptr;
        SKSE::log::info("FovFx: Hit Where You Aim {} - the archer dot sits {}", s_hwya ? "loaded" : "not loaded",
            s_hwya ? "in the middle of the screen" : "on the arrow's own line");
    }

    namespace {
        constexpr float kArrowFar = 6000.0f;  // nothing in the way: the aim point is this far out

        // How far the arrow's line runs from a_from along a_dir before it meets something - you and your bow are passed
        // through. Main thread.
        float ArrowReach(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_from, const RE::NiPoint3& a_dir) {
            auto* cell  = a_player->GetParentCell();
            auto* world = cell ? cell->GetbhkWorld() : nullptr;
            if (!world) return kArrowFar;
            const float scale = RE::bhkWorld::GetWorldScale();
            float       start = 0.0f;
            for (int pass = 0; pass < 4; ++pass) {
                RE::bhkPickData pick{};
                pick.rayInput.from                        = RE::hkVector4((a_from + a_dir * start) * scale);
                pick.rayInput.to                          = RE::hkVector4((a_from + a_dir * kArrowFar) * scale);
                pick.rayInput.enableShapeCollectionFilter = true;
                pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
                world->PickObject(pick);
                if (!pick.rayOutput.HasHit()) return kArrowFar;
                const float d    = start + (kArrowFar - start) * std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f);
                auto*       coll = pick.rayOutput.rootCollidable;
                auto*       ref  = coll ? RE::TESHavokUtilities::FindCollidableRef(*coll) : nullptr;
                if (ref == a_player || d < 5.0f) {
                    start = d + 1.0f;  // your own body or bow: look again past it
                    continue;
                }
                return d;
            }
            return kArrowFar;
        }
    }

    // The archer camera: round the head from behind toward your right side (aimTurn), looking at where the arrow will
    // land - a line from the arrow tip, the way the shot leaves (your aim heading, at the view's height), to the first
    // thing it meets. So the middle of the screen is the aim point even with the view turned, and Hit Where You Aim
    // (which aims through the screen's middle) agrees with it. In over ~0.12 s as the string comes back; after the
    // release the view holds on the aim point kAimHold seconds (the arrow flies in), then goes out over ~0.2 s.
    void ApplyAimCam() {
        constexpr double kAimHold = 0.5;
        static double       s_aimLast   = 0.0;
        static float        s_aimW      = 0.0f;
        static float        s_reach     = 0.0f;  // the aim point's distance, eased so a branch passing does not jerk the view
        static bool         s_wasDrawn  = false;
        static double       s_holdUntil = 0.0;
        static RE::NiPoint3 s_aimAt{};
        static bool         s_haveAim   = false;
        static RE::NiPoint3 s_wallOff{};          // how far a wall has moved the camera in (eased)
        static double       s_wallHold  = 0.0;    // stays in this long after the last push
        static RE::NiPoint3 s_headOff{};          // 10-31: the head's offset from your feet, in your own frame, eased
        static bool         s_haveHead  = false;  //        (the hold animation sways the head; the camera must not sway)
        const double        now         = Now();
        const float         dt          = s_aimLast > 0.0 ? static_cast<float>(std::clamp(now - s_aimLast, 0.0, 0.1)) : 0.0f;
        s_aimLast                       = now;
        auto* cam    = RE::PlayerCamera::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui     = RE::UI::GetSingleton();
        if (!s_settings.enabled || !s_settings.aimCam || !cam || !cam->cameraRoot || !player || cam->IsInFreeCameraMode() ||
            cam->IsInFirstPerson()) {
            s_aimW     = 0.0f;
            s_haveAim  = false;
            s_haveHead = false;
            s_wasDrawn = false;
            s_wallOff  = {};
            s_pubOn    = false;
            s_pubIn    = 0.0f;
            CamBlur::Archer(false, {});
            return;
        }
        if (s_wasDrawn && !s_bowDrawn) s_holdUntil = now + kAimHold;
        s_wasDrawn = s_bowDrawn;
        if (!(ui && ui->GameIsPaused())) {
            const float target = s_bowDrawn || now < s_holdUntil ? 1.0f : 0.0f;
            const float tau    = target > s_aimW ? 0.22f : 0.2f;  // 10-31: in over the pull (was 0.12)
            s_aimW += (target - s_aimW) * (dt > 0.0f ? 1.0f - std::exp(-dt / tau) : 0.0f);
        }
        if (s_aimW < 0.002f) {
            s_haveAim  = false;
            s_haveHead = false;
            s_wallOff  = {};
            s_pubOn    = false;
            s_pubIn    = 0.0f;
            CamBlur::Archer(false, {});
            return;
        }
        auto* body = player->Get3D2();
        auto* head = body ? body->GetObjectByName("NPC Head [Head]") : nullptr;
        if (!head) return;
        auto*              root = cam->cameraRoot.get();
        RE::NiTransform    t    = root->world;
        const auto&        m    = t.rotate.entry;
        const RE::NiPoint3 view{ m[0][1], m[1][1], m[2][1] };  // the game's view (your mouse)
        const float        h  = player->GetAngleZ();
        const float        ah = player->GetAimHeading();  // the way the shot leaves the bow
        const RE::NiPoint3 f{ std::sin(ah), std::cos(ah), 0.0f };
        const RE::NiPoint3 r{ f.y, -f.x, 0.0f };  // your right
        const float        flat = std::hypot(view.x, view.y);
        const RE::NiPoint3 dir{ f.x * flat, f.y * flat, view.z };  // the aim heading at the view's height (unit length)
        // The eyes, steadied (10-31): the head bone sways through the hold animation and the camera hung off it, so the
        // shot swayed too. The head's offset from your feet is kept in your own frame (turning with you) and eased; your
        // feet move the camera at once, the sway does not.
        RE::NiPoint3 eye;
        {
            const RE::NiPoint3 feet = player->GetPosition();
            const RE::NiPoint3 off  = head->world.translate - feet;
            const float        ch = std::cos(h), sh = std::sin(h);
            const RE::NiPoint3 local{ off.x * ch - off.y * sh, off.x * sh + off.y * ch, off.z };  // turned by -h
            if (!s_haveHead || dt <= 0.0f) {
                s_headOff  = local;
                s_haveHead = true;
            } else {
                s_headOff = s_headOff + (local - s_headOff) * (1.0f - std::exp(-dt / 0.3f));
            }
            const RE::NiPoint3 back{ s_headOff.x * ch + s_headOff.y * sh, -s_headOff.x * sh + s_headOff.y * ch, s_headOff.z };  // by +h
            eye = feet + back + RE::NiPoint3{ sh * 4.0f, ch * 4.0f, 7.0f };
        }
        if (s_bowDrawn || !s_haveAim) {  // after the release the aim point stays put
            // the arrow tip: just past the bow hand, on the line from the string hand
            RE::NiPoint3 tip  = eye + dir * 40.0f;
            auto*        grip = body->GetObjectByName("NPC L Hand [LHnd]");
            auto*        nock = body->GetObjectByName("NPC R Hand [RHnd]");
            if (grip && nock) {
                const RE::NiPoint3 along = grip->world.translate - nock->world.translate;
                const float        len   = along.Length();
                tip = grip->world.translate + (len > 1.0f ? along * (10.0f / len) : dir * 10.0f);
            }
            const float reach = ArrowReach(player, tip, dir);
            if (!s_haveAim || s_reach <= 0.0f) {
                s_reach = reach;
            } else if (dt > 0.0f) {  // eased in log space: near and far steps feel the same
                s_reach = std::exp(std::log(s_reach) + (std::log(reach) - std::log(s_reach)) * (1.0f - std::exp(-dt / 0.25f)));
            }
            s_aimAt   = tip + dir * (std::max)(s_reach, 50.0f);
            s_haveAim = true;
        }
        const RE::NiPoint3 aimAt = s_aimAt;
        const float        th    = s_settings.aimTurn * kDegToRad;
        // The camera sits round the eyes in the AIM's frame (10-31): back along the way the arrow flies (pitched with
        // your view), round to your right, up across that. Looking up puts the camera below and behind the eyes, looking
        // down above them - you stay in the shot either way. Before, the offset was flat and a raised aim point left
        // you under the frame.
        const RE::NiPoint3 upP{ r.y * dir.z - r.z * dir.y, r.z * dir.x - r.x * dir.z, r.x * dir.y - r.y * dir.x };  // r x dir
        RE::NiPoint3       want = eye + (r * std::sin(th) - dir * std::cos(th)) * s_settings.aimBack + upP * s_settings.aimUp;
        {
            // Walls (10-29): the free camera's rules (TFCam > Collision) - in front of anything solid between your eyes and
            // the camera, people passed. In quickly, held a moment once clear, out gently; looking at the same aim point.
            const RE::NiPoint3 target = CamCollision::ClearSpot(eye, want) - want;
            const float        tl     = target.Length();
            if (tl >= s_wallOff.Length() - 0.01f) {
                s_wallOff  = s_wallOff + (target - s_wallOff) * (dt > 0.0f ? 1.0f - std::exp(-dt / 0.03f) : 1.0f);
                s_wallHold = now + 0.25;
            } else if (now >= s_wallHold && dt > 0.0f) {
                s_wallOff = s_wallOff + (target - s_wallOff) * (1.0f - std::exp(-dt / 0.15f));
            }
            want = want + s_wallOff;
        }
        // 10-31: when the projectiles go through the middle of the screen anyway (TFCam's own aim or Hit Where You Aim)
        // the view keeps your aim's direction - looking at the arrow line's hit made the world swing whenever that
        // hit jumped between a near tree and the far sky ("the crosshair jumps around", user). The arrow-line look
        // stays for the case where nothing corrects the arrow.
        RE::NiPoint3 look = AimDot::Active() || s_hwya ? dir : aimAt - want;
        look              = look * (1.0f / (std::max)(look.Length(), 1e-3f));
        const float w     = SmoothStep(s_aimW);
        {
            // 10-31: round the eyes on an arc, not a straight line - the line from the game's camera (behind you) to the
            // spot beside your head passed through your shoulder and bow arm (seen 0.4 s into the pull). Direction
            // slerped, distance eased in log space, so the camera stays outside you all the way.
            const RE::NiPoint3 v0 = t.translate - eye, v1 = want - eye;
            const float        l0 = v0.Length(), l1 = v1.Length();
            if (l0 > 1e-3f && l1 > 1e-3f) {
                const RE::NiPoint3 d0 = v0 * (1.0f / l0), d1 = v1 * (1.0f / l1);
                const float        c  = std::clamp(d0.Dot(d1), -1.0f, 1.0f);
                const float        a  = std::acos(c);
                RE::NiPoint3       d;
                if (a > 1e-3f && a < 3.1405f) {
                    const float s = std::sin(a);
                    d             = d0 * (std::sin((1.0f - w) * a) / s) + d1 * (std::sin(w * a) / s);
                } else {
                    d = d0 + (d1 - d0) * w;
                }
                d           = d * (1.0f / (std::max)(d.Length(), 1e-3f));
                t.translate = eye + d * std::exp(std::log(l0) + (std::log(l1) - std::log(l0)) * w);
            } else {
                t.translate = t.translate + (want - t.translate) * w;
            }
        }
        RE::NiPoint3 fwd = view + (look - view) * w;
        fwd              = fwd * (1.0f / (std::max)(fwd.Length(), 1e-3f));
        RE::NiPoint3 right{ fwd.y, -fwd.x, 0.0f };
        const float  rl = std::hypot(right.x, right.y);
        right           = rl > 1e-4f ? right * (1.0f / rl) : RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
        const RE::NiPoint3 up{ right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z, right.x * fwd.y - right.y * fwd.x };
        auto& e  = t.rotate.entry;
        e[0][0] = right.x, e[1][0] = right.y, e[2][0] = right.z;
        e[0][1] = fwd.x, e[1][1] = fwd.y, e[2][1] = fwd.z;
        e[0][2] = up.x, e[1][2] = up.y, e[2][2] = up.z;
        root->local = root->parent ? root->parent->world.Invert() * t : t;
        RE::NiUpdateData data{};
        root->Update(data);
        // 10-31: the image pass draws the dot there and, with the box on, blurs round the edge
        s_pubOn  = true;
        s_pubIn  = s_aimW;
        s_pubAim = aimAt;
        CamBlur::Archer(s_settings.aimBlur, aimAt);
        static bool s_logged = false;
        if (!s_logged && s_aimW > 0.95f) {
            s_logged = true;
            SKSE::log::info("FovFx: archer camera in - {:.0f} units from the eyes, aim point {:.0f} out from the arrow tip",
                (want - eye).Length(), s_reach);
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
                // 10-31: from kBowAttached, not kBowDraw - kBowDraw is the arrow coming out of the quiver; the string is pulled
                // between the arrow attaching and kBowDrawn, and the user wants the view to move WITH the pull.
                const bool drawing = st == A::kBowAttached || st == A::kBowDrawn;
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
