#include "CamCollision.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <string>

namespace CamCollision {

    namespace {
        constexpr float kLn2       = 0.69314718f;
        constexpr float kMinDist   = 6.0f;   // never closer to the look point than this
        constexpr float kSkipNear  = 6.0f;   // hits this close to the look point: it sits inside them, pass on
        constexpr float kProbeFrac = 0.6f;   // side probes at margin * this round the camera
        constexpr float kHoldSec   = 0.25f;  // stay pulled in this long after the last push before easing out
        constexpr int   kMaxPasses = 8;      // characters / near hits skipped per ray
        constexpr float kMaxSide   = 40.0f;  // 0.15.1: widest side probe, whatever the FOV
        constexpr float kMinCushion = 6.0f;  // 0.15.1: while easing in, the camera is never nearer a wall than margin - cushion

        Settings s_cfg;

        bool         s_req = false;
        RE::NiPoint3 s_pivot{};
        const char*  s_who = "";

        RE::NiPoint3 s_off{};  // 0.15.2: how far the camera is moved off what the driver wrote (slid along walls), now
        double s_holdUntil = 0.0;

        // Log / menu.
        double      s_nextLog = 0.0;
        int         s_pushes  = 0;
        std::mutex  s_statusLock;
        std::string s_last;

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

        // Own frame time: capped at 0.1 s, 0 while the game is paused.
        float FrameDt() {
            static double s_lastT = 0.0;
            const double  now     = Now();
            float         dt      = s_lastT > 0.0 ? static_cast<float>(now - s_lastT) : 0.0f;
            s_lastT               = now;
            dt                    = std::clamp(dt, 0.0f, 0.1f);
            auto* ui              = RE::UI::GetSingleton();
            if (ui && ui->GameIsPaused()) dt = 0.0f;
            return dt;
        }

        float ExpWeight(float dt, float halfLife) { return dt <= 0.0f ? 0.0f : 1.0f - std::exp(-kLn2 * dt / halfLife); }

        float Len(const RE::NiPoint3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

        // 0.15.1: how far the near plane's corners reach sideways, from the live world camera (the game's frustum, which also
        // follows the eye view's near clip) and the current FOV. The FOV is taken as the vertical one: if it is the horizontal one
        // this is wider than needed, never narrower. Logged once so the numbers can be checked in game.
        float NearPlaneReach(float& a_near) {
            a_near       = 15.0f;
            float aspect = 16.0f / 9.0f;
            float fov    = 70.0f;
            if (auto* cam = RE::PlayerCamera::GetSingleton()) {
                const float f = cam->GetRuntimeData2().worldFOV;
                if (std::isfinite(f) && f > 5.0f && f < 170.0f) fov = f;
            }
            if (!REL::Module::IsVR()) {
                if (auto* nc = RE::Main::WorldRootCamera()) {
                    const auto& fr = nc->GetRuntimeData2().viewFrustum;
                    if (std::isfinite(fr.fNear) && fr.fNear > 0.5f && fr.fNear < 100.0f) a_near = fr.fNear;
                    const float w = fr.fRight - fr.fLeft;
                    const float h = fr.fTop - fr.fBottom;
                    if (std::isfinite(w) && std::isfinite(h) && w > 0.0f && h > 0.0f) aspect = std::clamp(w / h, 1.0f, 3.0f);
                    static bool s_logged = false;
                    if (!s_logged) {
                        s_logged = true;
                        SKSE::log::info("CamCollision: frustum l {:.3f} r {:.3f} b {:.3f} t {:.3f} near {:.2f} far {:.0f}, worldFOV {:.1f}",
                            fr.fLeft, fr.fRight, fr.fBottom, fr.fTop, fr.fNear, fr.fFar, fov);
                    }
                }
            }
            return a_near * std::tan(fov * 0.5f * 0.01745329f) * aspect;
        }

        // 0.15.3: a sconce on the wall or an item on a table is not a wall: the camera went over every one of them. Loose clutter and
        // debris never count; neither does any object whose bounding sphere is smaller than ignoreSmall (terrain and anything
        // without a 3D bound still counts).
        bool IsSmallProp(RE::COL_LAYER a_layer, RE::TESObjectREFR* a_ref) {
            if (a_layer == RE::COL_LAYER::kClutter || a_layer == RE::COL_LAYER::kDebrisSmall || a_layer == RE::COL_LAYER::kDebrisLarge) return true;
            if (!(s_cfg.ignoreSmall > 0.0f) || !a_ref) return false;
            auto* root = a_ref->Get3D();
            if (!root) return false;
            const float radius = root->worldBound.radius;
            return std::isfinite(radius) && radius > 0.1f && radius < s_cfg.ignoreSmall;
        }

        // 0.15.4: the two tickable pass-throughs, decided from the hit object's size and the surface it was hit on. A large item is
        // anything from ignoreSmall up to largeItem; above that it is architecture, and only its upright surfaces are "walls"
        // (floors and ceilings keep the camera in the room).
        bool IsPassThrough(RE::COL_LAYER a_layer, RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_normal) {
            if (!s_cfg.passLarge && !s_cfg.passWalls) return false;
            float radius = 0.0f;
            if (a_ref) {
                if (auto* root = a_ref->Get3D()) radius = root->worldBound.radius;
            }
            if (!std::isfinite(radius) || radius <= 0.1f) return false;  // no size: it counts
            if (radius < s_cfg.largeItem) return s_cfg.passLarge;
            if (!s_cfg.passWalls) return false;
            if (a_layer != RE::COL_LAYER::kStatic && a_layer != RE::COL_LAYER::kAnimStatic) return false;
            const float nl = Len(a_normal);
            return nl > 0.5f && std::fabs(a_normal.z / nl) < 0.7f;  // an upright surface
        }

        // 10-30: the game's own camera bodies - the third-person camera's collision sphere (kCameraSphere, no reference) sits
        // where the camera was last frame, so a probe toward where it goes next met it every frame and the archer camera
        // lurched in and out while the string was held. Nothing else lives on these layers.
        bool IsCameraLayer(RE::COL_LAYER a_layer) { return a_layer == RE::COL_LAYER::kCameraSphere || a_layer == RE::COL_LAYER::kCameraPick; }

        bool IsCharacterLayer(RE::COL_LAYER a_layer) {
            switch (a_layer) {
            case RE::COL_LAYER::kBiped:
            case RE::COL_LAYER::kCharController:
            case RE::COL_LAYER::kDeadBip:
            case RE::COL_LAYER::kBipedNoCC:
                return true;
            default:
                return false;
            }
        }

        struct Hit {
            bool          hit   = false;
            float         dist  = 0.0f;  // from the ray start
            RE::FormID    ref   = 0;
            int           layer = -1;
            RE::NiPoint3  normal{};  // 0.15.2: the surface normal at the hit
        };

        // First solid hit on the way from a_from to a_to. Characters and anything right at the start are passed through.
        Hit Cast(RE::bhkWorld* a_world, const RE::NiPoint3& a_from, const RE::NiPoint3& a_to) {
            Hit         out;
            const auto  delta = a_to - a_from;
            const float len   = Len(delta);
            if (!(len > 1.0f) || !std::isfinite(len)) return out;
            const float scale = RE::bhkWorld::GetWorldScale();
            float       start = 0.0f;  // fraction of the ray already passed
            for (int pass = 0; pass < kMaxPasses && start < 1.0f; ++pass) {
                RE::bhkPickData pick{};
                pick.rayInput.from                        = RE::hkVector4((a_from + delta * start) * scale);
                pick.rayInput.to                          = RE::hkVector4(a_to * scale);
                pick.rayInput.enableShapeCollectionFilter = true;
                pick.rayInput.filterInfo.SetCollisionLayer(static_cast<RE::COL_LAYER>(s_cfg.layer));
                a_world->PickObject(pick);  // main thread, like P+'s and MuJointFix's ray casts (no extra world lock)
                if (!pick.rayOutput.HasHit()) return out;
                const float frac  = start + (1.0f - start) * std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f);
                const auto* coll  = pick.rayOutput.rootCollidable;
                const auto  layer = coll ? coll->GetCollisionLayer() : RE::COL_LAYER::kUnidentified;
                auto*       ref   = coll ? RE::TESHavokUtilities::FindCollidableRef(*coll) : nullptr;
                const RE::NiPoint3 hitNormal{ pick.rayOutput.normal.quad.m128_f32[0], pick.rayOutput.normal.quad.m128_f32[1],
                    pick.rayOutput.normal.quad.m128_f32[2] };
                if (IsCameraLayer(layer) || IsCharacterLayer(layer) || (ref && ref->IsActor()) || IsSmallProp(layer, ref) || IsPassThrough(layer, ref, hitNormal) ||
                    frac * len < kSkipNear) {
                    start = frac + 1.0f / len;  // step 1 unit past it and look again
                    continue;
                }
                out.hit   = true;
                out.dist  = frac * len;
                out.ref   = ref ? ref->GetFormID() : 0;
                out.layer = static_cast<int>(layer);
                out.normal = { pick.rayOutput.normal.quad.m128_f32[0], pick.rayOutput.normal.quad.m128_f32[1],
                    pick.rayOutput.normal.quad.m128_f32[2] };
                return out;
            }
            return out;
        }

        float Dot(const RE::NiPoint3& a, const RE::NiPoint3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

        struct Solved {
            RE::NiPoint3 pos;
            Hit          nearest;
            bool         moved = false;
        };

        // 0.15.2: where the camera may be. A wall between the look point and the camera does not pull the camera toward the
        // look point: the camera is pushed out along the wall's normal to a_margin in front of its plane, so the move along the
        // wall carries on (a slide) and the camera does not dip in and swing back out as it passes. Up to 4 rounds (a corner is
        // two walls); when a hit has no usable plane, or the line is still blocked after the rounds, it pulls in along the line.
        Solved Solve(RE::bhkWorld* a_world, const RE::NiPoint3& a_pivot, const RE::NiPoint3& a_cam, const RE::NiPoint3& a_side,
            float a_r, float a_margin) {
            Solved out;
            out.pos                      = a_cam;
            const RE::NiPoint3 offsets[] = { { 0.0f, 0.0f, 0.0f }, a_side * a_r, a_side * (-a_r) };
            for (int round = 0; round < 4; ++round) {
                RE::NiPoint3 bestPush{};
                float        bestLen = 0.0f;
                Hit          bestHit;
                for (const auto& o : offsets) {
                    const RE::NiPoint3 from = a_pivot + o;
                    const RE::NiPoint3 to   = out.pos + o;
                    const Hit          h    = Cast(a_world, from, to);
                    if (!h.hit) continue;
                    const RE::NiPoint3 rd  = to - from;
                    const float        len = Len(rd);
                    if (!(len > 1.0f)) continue;
                    RE::NiPoint3 push{};
                    if (const float nl = Len(h.normal); nl > 0.5f) {
                        RE::NiPoint3 n = h.normal * (1.0f / nl);
                        if (Dot(n, rd) > 0.0f) n = n * -1.0f;  // face the look point
                        const RE::NiPoint3 hitPt = from + rd * (h.dist / len);
                        const float        need  = a_margin - Dot(to - hitPt, n);
                        if (need > 0.1f && need <= len) push = n * need;
                    }
                    if (Len(push) < 0.1f) {  // no usable plane: pull in along the ray
                        const float allowed = (std::max)(h.dist - a_margin, (std::min)(kMinDist, len));
                        push                = rd * (-(len - allowed) / len);
                    }
                    if (const float pl = Len(push); pl > bestLen) {
                        bestLen = pl;
                        bestPush = push;
                        bestHit  = h;
                    }
                }
                if (bestLen < 0.25f) break;
                out.pos     = out.pos + bestPush;
                out.moved   = true;
                out.nearest = bestHit;
            }
            if (out.moved) {  // the centre line must be clear in the end
                const Hit h = Cast(a_world, a_pivot, out.pos);
                if (h.hit) {
                    const RE::NiPoint3 rd  = out.pos - a_pivot;
                    const float        len = Len(rd);
                    if (len > kMinDist) {
                        out.pos     = a_pivot + rd * ((std::max)(h.dist - a_margin, kMinDist) / len);
                        out.nearest = h;
                    }
                }
            }
            return out;
        }

        void NotePush(std::string a_text) {
            std::lock_guard lock(s_statusLock);
            ++s_pushes;
            s_last = std::move(a_text);
        }
    }

    Settings& Get() { return s_cfg; }

    void Clamp() {
        auto range = [](float& v, float lo, float hi, float def) { v = std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
        range(s_cfg.margin, 4.0f, 60.0f, 16.0f);
        range(s_cfg.returnSec, 0.1f, 3.0f, 0.8f);
        range(s_cfg.approachSec, 0.0f, 2.0f, 0.3f);
        range(s_cfg.ignoreSmall, 0.0f, 200.0f, 50.0f);
        range(s_cfg.largeItem, 60.0f, 600.0f, 200.0f);
        if (s_cfg.layer < 1 || s_cfg.layer > 46) s_cfg.layer = 36;
    }

    void Request(const RE::NiPoint3& a_pivot, const char* a_who) {
        if (!std::isfinite(a_pivot.x) || !std::isfinite(a_pivot.y) || !std::isfinite(a_pivot.z)) return;
        s_req   = true;
        s_pivot = a_pivot;
        s_who   = a_who ? a_who : "";
    }

    void Apply(RE::FreeCameraState* a_fcs, bool a_driving) {
        const float dt  = FrameDt();
        const bool  req = s_req;
        s_req           = false;
        if (!req || !a_fcs || !a_driving || !s_cfg.enabled) {
            s_off = {};
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell   = player ? player->GetParentCell() : nullptr;
        auto* world  = cell ? cell->GetbhkWorld() : nullptr;
        if (!world) return;

        const RE::NiPoint3 cam = a_fcs->translation;
        const RE::NiPoint3 v   = cam - s_pivot;
        const float        d   = Len(v);
        if (!(d > kMinDist) || !std::isfinite(d)) {
            s_off = {};
            return;
        }
        const RE::NiPoint3 dir = v * (1.0f / d);

        // The camera line plus one tube either side of it, level with it and as far out as the near plane's corners reach,
        // so the near plane stays out of a wall the camera passes beside. The side rays run parallel to the camera line (not
        // as a wedge from the look point): a wall beside the subject, or one the camera only passes along, does not count -
        // only a wall that stands between the look point and the camera does. No probes above / below: the spiral's
        // floor-level shots would hit the floor with them.
        RE::NiPoint3 side{ dir.y, -dir.x, 0.0f };
        if (const float sl = Len(side); sl > 0.01f) {
            side = side * (1.0f / sl);
        } else {
            side = { 1.0f, 0.0f, 0.0f };  // straight above / below the look point
        }
        float       nearClip = 15.0f;
        const float reach    = NearPlaneReach(nearClip);
        const float r        = std::clamp(reach, s_cfg.margin * kProbeFrac, kMaxSide);
        // The camera body is kept at least the near clip (plus a hand) off the wall as well.
        const float margin = (std::max)(s_cfg.margin, nearClip + 4.0f);

        const Solved       sol    = Solve(world, s_pivot, cam, side, r, margin);
        const RE::NiPoint3 target = sol.pos - cam;
        const float        tl     = Len(target);
        const float        ol     = Len(s_off);
        const double       now    = Now();
        const RE::NiPoint3 diff   = target - s_off;
        if (tl >= ol - 0.01f) {
            if (tl > 0.5f && ol <= 0.5f && now >= s_nextLog) {
                s_nextLog = now + 0.5;
                const auto text = std::format("{}: moved {:.0f} units off the wall of {:08X} (layer {}, side probes {:.0f}, kept {:.0f}, camera {:.0f} from the look point)",
                    s_who, tl, sol.nearest.ref, sol.nearest.layer, r, margin, d);
                SKSE::log::info("CamCollision: {}", text);
                NotePush(text);
            }
            // Ease in instead of snapping. How far the camera may trail the wall is capped (the cushion), so it never gets
            // nearer the wall than half the kept distance, however fast it moves into it.
            if (s_cfg.approachSec <= 0.01f) {
                s_off = target;
            } else {
                s_off = s_off + diff * ExpWeight(dt, s_cfg.approachSec / 4.32f);
                const RE::NiPoint3 rem = target - s_off;
                const float        rl  = Len(rem);
                const float        cushion = (std::max)(margin * 0.5f, kMinCushion);
                if (rl > cushion) s_off = target - rem * (cushion / rl);
            }
            s_holdUntil = now + kHoldSec;
        } else if (now >= s_holdUntil) {
            s_off = s_off + diff * ExpWeight(dt, s_cfg.returnSec / 4.32f);
        }

        if (Len(s_off) > 0.05f) {
            const RE::NiPoint3 np = cam + s_off;
            // Keep looking at the same point from the new spot, so the subject stays where the driver framed it.
            auto angles = [](const RE::NiPoint3& dv, float& yaw, float& pitch) {
                yaw   = std::atan2(dv.x, dv.y);
                pitch = std::atan2(-dv.z, std::hypot(dv.x, dv.y));
            };
            float y0 = 0, p0 = 0, y1 = 0, p1 = 0;
            angles(s_pivot - cam, y0, p0);
            angles(s_pivot - np, y1, p1);
            constexpr float kPi = 3.14159265f;
            float           dy  = y1 - y0;
            while (dy > kPi) dy -= 2.0f * kPi;
            while (dy < -kPi) dy += 2.0f * kPi;
            a_fcs->rotation.y += dy;
            a_fcs->rotation.x = std::clamp(a_fcs->rotation.x + (p1 - p0), -1.55f, 1.55f);
            a_fcs->translation = np;
        } else {
            s_off = {};
        }
    }

    RE::NiPoint3 ClearSpot(const RE::NiPoint3& a_pivot, const RE::NiPoint3& a_cam) {
        if (!s_cfg.enabled) return a_cam;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell   = player ? player->GetParentCell() : nullptr;
        auto* world  = cell ? cell->GetbhkWorld() : nullptr;
        if (!world) return a_cam;
        const RE::NiPoint3 v = a_cam - a_pivot;
        const float        d = Len(v);
        if (!(d > kMinDist) || !std::isfinite(d)) return a_cam;
        const RE::NiPoint3 dir = v * (1.0f / d);
        RE::NiPoint3       side{ dir.y, -dir.x, 0.0f };
        if (const float sl = Len(side); sl > 0.01f) {
            side = side * (1.0f / sl);
        } else {
            side = { 1.0f, 0.0f, 0.0f };
        }
        float       nearClip = 15.0f;
        const float reach    = NearPlaneReach(nearClip);
        const float r        = std::clamp(reach, s_cfg.margin * kProbeFrac, kMaxSide);
        const float margin   = (std::max)(s_cfg.margin, nearClip + 4.0f);
        const Solved sol     = Solve(world, a_pivot, a_cam, side, r, margin);
        if (sol.moved && Now() >= s_nextLog) {
            s_nextLog       = Now() + 0.5;
            const auto text = std::format("archer camera: moved {:.0f} units off the wall of {:08X} (layer {})", Len(sol.pos - a_cam),
                sol.nearest.ref, sol.nearest.layer);
            SKSE::log::info("CamCollision: {}", text);
            NotePush(text);
        }
        return sol.pos;
    }

    std::string MenuStatus() {
        std::lock_guard lock(s_statusLock);
        if (s_pushes == 0) return "no wall in the way yet this session";
        return std::format("{} time(s) this session; last: {}", s_pushes, s_last);
    }
}
