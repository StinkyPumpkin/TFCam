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

        Settings s_cfg;

        bool         s_req = false;
        RE::NiPoint3 s_pivot{};
        const char*  s_who = "";

        float  s_comp      = 0.0f;  // how far the camera is pulled in now (units)
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
                if (IsCharacterLayer(layer) || (ref && ref->IsActor()) || frac * len < kSkipNear) {
                    start = frac + 1.0f / len;  // step 1 unit past it and look again
                    continue;
                }
                out.hit   = true;
                out.dist  = frac * len;
                out.ref   = ref ? ref->GetFormID() : 0;
                out.layer = static_cast<int>(layer);
                return out;
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
            s_comp = 0.0f;
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
            s_comp = 0.0f;
            return;
        }
        const RE::NiPoint3 dir = v * (1.0f / d);

        // The camera point plus one either side of it, level with it (a thin wedge from the look point), so the near
        // plane stays out of a wall the camera passes beside. No probes above / below: the spiral's floor-level shots
        // would hit the floor with them.
        RE::NiPoint3 side{ dir.y, -dir.x, 0.0f };
        if (const float s = Len(side); s > 0.01f) {
            side = side * (1.0f / s);
        } else {
            side = { 1.0f, 0.0f, 0.0f };  // straight above / below the look point
        }
        const float        r        = s_cfg.margin * kProbeFrac;
        const RE::NiPoint3 probes[] = { cam, cam + side * r, cam - side * r };

        Hit   nearest;
        float allowed = d;
        for (const auto& probe : probes) {
            const Hit h = Cast(world, s_pivot, probe);
            if (!h.hit) continue;
            const float probeLen = Len(probe - s_pivot);
            const float along    = probeLen > 0.0f ? h.dist * (d / probeLen) : h.dist;  // that hit on the camera's line
            const float a        = (std::max)(along - s_cfg.margin, (std::min)(kMinDist, d));
            if (a < allowed) {
                allowed = a;
                nearest = h;
            }
        }
        const float  need = (std::max)(d - allowed, 0.0f);
        const double now  = Now();
        if (need >= s_comp) {
            if (need > 0.5f && s_comp <= 0.5f && now >= s_nextLog) {
                s_nextLog = now + 0.5;
                const auto text = std::format("{}: pulled in {:.0f} of {:.0f} units by {:08X} (layer {})", s_who, need, d,
                    nearest.ref, nearest.layer);
                SKSE::log::info("CamCollision: {}", text);
                NotePush(text);
            }
            s_comp      = need;
            s_holdUntil = now + kHoldSec;
        } else if (now >= s_holdUntil) {
            s_comp = need + (s_comp - need) * (1.0f - ExpWeight(dt, s_cfg.returnSec / 4.32f));
        }
        if (s_comp > 0.05f) {
            const float dist   = (std::max)(d - s_comp, (std::min)(kMinDist, d));
            a_fcs->translation = s_pivot + dir * dist;
        }
    }

    std::string MenuStatus() {
        std::lock_guard lock(s_statusLock);
        if (s_pushes == 0) return "no wall in the way yet this session";
        return std::format("{} time(s) this session; last: {}", s_pushes, s_last);
    }
}
