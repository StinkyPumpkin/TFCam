#include "AimDot.h"
#include "FovFx.h"
#include "TDM_API.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace AimDot {

    namespace {
        constexpr float kDegToRad    = std::numbers::pi_v<float> / 180.0f;
        constexpr float kMaxDist     = 7000.0f;  // 100 m: the ray's reach (Hit Where You Aim's default)
        constexpr float kPenetration = 14.0f;    // 0.2 m past the hit, so the projectile reaches into it
        constexpr float kFixedPitch  = 0.7f;     // degrees up for a projectile with gravity when the solver cannot help
        constexpr std::uint32_t kPlayerHandle = 0x100000;
        constexpr int   kHookCount   = 5;        // launch angles, flames, beams, projectile init, spells

        Settings    s_settings;
        bool        s_hwya      = false;
        int         s_hooks     = 0;
        std::string s_status    = "not installed";
        std::string s_offStatus = "not installed";  // what the menu shows while the box is unticked
        bool        s_logged    = false;

        // True Directional Movement's target lock can aim the player's shots at the locked target (it rewrites the
        // angles in the projectile init). Whether it does is its own Target Lock setting, one for arrows and one for
        // spells: 0 Free Aim (it leaves the shot alone), 1 Predictive, 2 Homing. Only when it aims are the shots left
        // to it. Hit Where You Aim 1.4 hands over on the lock alone; under Free Aim that leaves the arrow aimed by
        // nobody, so it flies wherever the bow happens to point.
        enum class TdmShot { kArrow, kSpell };
        TDM_API::IVTDM1*  s_tdm = nullptr;
        std::atomic<bool> s_loggedTdm{ false };
        std::atomic<bool> s_loggedTdmFree{ false };

        // TDM's aim mode for one kind of shot, from its MCM settings (the defaults file when the player never changed
        // it; TDM's own default is Predictive). Read again at most every 2 s, so a change in its menu is picked up.
        int TdmAimType(TdmShot a_kind) {
            static std::atomic<int>           s_arrow{ 1 }, s_spell{ 1 };
            static std::atomic<std::uint64_t> s_readAt{ 0 };
            const std::uint64_t               now = ::GetTickCount64();
            const std::uint64_t               at  = s_readAt.load();
            if (at == 0 || now - at > 2000) {
                s_readAt = now;
                std::error_code ec;
                const auto      base = std::filesystem::current_path(ec);
                const auto      user = (base / "Data/MCM/Settings/TrueDirectionalMovement.ini").string();
                const auto      def  = (base / "Data/MCM/Config/TrueDirectionalMovement/settings.ini").string();
                auto read = [&](const char* a_key) {
                    int v = static_cast<int>(::GetPrivateProfileIntA("TargetLock", a_key, -1, user.c_str()));
                    if (v < 0) v = static_cast<int>(::GetPrivateProfileIntA("TargetLock", a_key, -1, def.c_str()));
                    return v < 0 ? 1 : v;
                };
                s_arrow = read("uTargetLockArrowAimType");
                s_spell = read("uTargetLockMissileAimType");
            }
            return a_kind == TdmShot::kArrow ? s_arrow.load() : s_spell.load();
        }

        // True = TDM's target lock is on AND it aims this kind of shot, so TFCam keeps out of it.
        bool TdmAims(TdmShot a_kind) {
            if (!s_tdm || !s_tdm->GetTargetLockState()) return false;
            const char* what = a_kind == TdmShot::kArrow ? "arrows" : "spells";
            const int   type = TdmAimType(a_kind);
            if (type == 0) {
                if (!s_loggedTdmFree.exchange(true)) {
                    SKSE::log::info("AimDot: True Directional Movement's target lock is on, but its aim mode for {} is Free Aim - TFCam aims the shot at the dot", what);
                }
                return false;
            }
            if (!s_loggedTdm.exchange(true)) {
                SKSE::log::info("AimDot: True Directional Movement's target lock is on and aims {} ({}) - its aim is kept for the shot", what,
                    type == 2 ? "Homing" : "Predictive");
            }
            return true;
        }

        // ---- pattern scan inside a function (Hit Where You Aim's FindPatternInFunction) --------------------------
        struct Byte {
            std::uint8_t value = 0;
            bool         any   = false;
        };

        std::vector<Byte> Parse(std::string_view a_pat) {
            std::vector<Byte> out;
            std::size_t       i = 0;
            while (i < a_pat.size()) {
                while (i < a_pat.size() && a_pat[i] == ' ') ++i;
                if (i >= a_pat.size()) break;
                Byte b;
                if (a_pat[i] == '?') {
                    b.any = true;
                    while (i < a_pat.size() && a_pat[i] == '?') ++i;
                } else {
                    if (i + 1 >= a_pat.size()) return {};
                    auto hex = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        return -1;
                    };
                    const int hi = hex(a_pat[i]), lo = hex(a_pat[i + 1]);
                    if (hi < 0 || lo < 0) return {};
                    b.value = static_cast<std::uint8_t>(hi * 16 + lo);
                    i += 2;
                }
                out.push_back(b);
            }
            return out;
        }

        std::uintptr_t FindInFunction(std::uintptr_t a_fn, std::string_view a_pat, std::size_t a_max = 0x4000) {
            const auto pat = Parse(a_pat);
            if (pat.empty() || !a_fn) return 0;
            const auto* p = reinterpret_cast<const std::uint8_t*>(a_fn);
            while (reinterpret_cast<std::uintptr_t>(p) + pat.size() < a_fn + a_max) {
                if (*reinterpret_cast<const std::uint32_t*>(p) == 0xCCCCCCC3) return 0;  // ret + padding: the function ended
                bool match = true;
                for (std::size_t i = 0; i < pat.size(); ++i) {
                    if (!pat[i].any && p[i] != pat[i].value) {
                        match = false;
                        break;
                    }
                }
                if (match) return reinterpret_cast<std::uintptr_t>(p);
                ++p;
            }
            return 0;
        }

        // The call (E8) at a_offset past the pattern in the function a_id starts, or 0.
        std::uintptr_t CallSite(REL::RelocationID a_id, std::string_view a_pat, std::size_t a_offset, const char* a_what) {
            const std::uintptr_t fn  = REL::Relocation<std::uintptr_t>(a_id).address();
            const std::uintptr_t hit = FindInFunction(fn, a_pat);
            if (!hit) {
                SKSE::log::warn("AimDot: {} - the pattern was not found in SkyrimSE.exe+{:X}; that part stays off", a_what,
                    fn - REL::Module::get().base());
                return 0;
            }
            const std::uintptr_t site = hit + a_offset;
            if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
                SKSE::log::warn("AimDot: {} - no call at the expected spot (byte {:02X}); that part stays off", a_what,
                    *reinterpret_cast<const std::uint8_t*>(site));
                return 0;
            }
            return site;
        }

        // ---- the aim ---------------------------------------------------------------------------------------------
        bool CameraAllows() {
            auto* pc = RE::PlayerCamera::GetSingleton();
            if (!pc || !pc->currentState) return false;
            auto*      st = pc->currentState.get();
            const auto id = st->id;
            if (id == RE::CameraState::kMount) return true;  // free rotation is fine on a horse
            if (id != RE::CameraState::kFirstPerson && id != RE::CameraState::kThirdPerson) return false;
            if (id == RE::CameraState::kThirdPerson) {
                auto* tps = static_cast<RE::ThirdPersonState*>(st);
                if (tps->GetFreeRotationMode() && tps->freeRotation.x >= 0.1f) return false;  // facing away from the view
            }
            return true;
        }

        struct Ray {
            bool                     ok = false;
            RE::NiPoint3             from{};
            RE::NiPoint3             dir{};
            RE::NiPoint3             point{};          // the first thing the middle of the screen's ray meets, else kMaxDist out
            RE::TESObjectREFR*       ref = nullptr;    // what it met, when that is a reference
            RE::hkpWorldRayCastInput input{};          // the ray as cast (Havok units)
        };

        RE::TESObjectREFR* RefOf(RE::NiAVObject* a_obj) {
            for (int i = 0; a_obj && i < 64; ++i, a_obj = a_obj->parent) {
                if (auto* ref = a_obj->GetUserData()) return ref;
            }
            return nullptr;
        }

        // The middle of the rendered picture, cast with the given collision filter (its group keeps the shooter out
        // the way the shot itself would leave it out).
        Ray CentreRay(RE::CFilter a_filter) {
            Ray   r;
            auto* cam = RE::Main::WorldRootCamera();
            auto* tes = RE::TES::GetSingleton();
            if (!cam || !tes) return r;
            RE::NiPoint3 o{}, d{};
            if (!cam->WindowPointToRay(1000, 1000, o, d, 2000.0f, 2000.0f)) return r;  // the middle of any window size
            const float dl = d.Length();
            if (!(dl > 1e-4f)) return r;
            d = d * (1.0f / dl);
            r.from = cam->world.translate;
            r.dir  = d;
            const float     scale = RE::bhkWorld::GetWorldScale();
            RE::bhkPickData pick{};
            pick.rayInput.from       = RE::hkVector4(r.from * scale);
            pick.rayInput.to         = RE::hkVector4((r.from + d * kMaxDist) * scale);
            pick.rayInput.filterInfo = a_filter;
            r.ref   = RefOf(tes->Pick(pick));
            r.input = pick.rayInput;
            const float frac = pick.rayOutput.HasHit() ? std::clamp(pick.rayOutput.hitFraction, 0.0f, 1.0f) : 1.0f;
            r.point = r.from + d * (kMaxDist * frac);
            r.ok    = true;
            return r;
        }

        Ray CentreRay(std::uint32_t a_group, RE::COL_LAYER a_layer) {
            RE::CFilter filter{ 0 };
            filter.SetCollisionLayer(a_layer);
            filter.SetSystemGroup(a_group);
            return CentreRay(filter);
        }

        // Skyrim's angles: pitch (x) negative = up, yaw (z) = atan2(east, north).
        void Angles(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float& a_pitch, float& a_yaw) {
            const RE::NiPoint3 d  = a_to - a_from;
            const float        h2 = d.x * d.x + d.y * d.y;
            if (h2 + d.z * d.z <= 1e-6f) return;
            a_pitch = -std::atan2(d.z, std::sqrt(h2));
            if (h2 > 1e-6f) a_yaw = std::atan2(d.x, d.y);
        }

        float WorldGravity() {
            static REL::Relocation<const float*> g{ RELOCATION_ID(518746, 405286) };
            return *g;
        }

        // The game's own ballistic prediction (CombatProjectileAimController's): where to aim a projectile of this speed
        // and gravity so it lands on a_target. False when it cannot (no gravity, too far, out of reach).
        bool NativeSolve(RE::NiPoint3& a_out, const RE::NiPoint3& a_from, const RE::NiPoint3& a_target, float a_speed, float a_gravity) {
            if (a_speed <= 0.0f || a_gravity <= 0.0f) return false;
            const float dx = a_target.x - a_from.x, dy = a_target.y - a_from.y, dz = a_target.z - a_from.z;
            const float h2 = dx * dx + dy * dy;
            if (h2 + dz * dz >= 8192.0f * 8192.0f) return false;
            const float s2   = a_speed * a_speed;
            const float disc = s2 * s2 - a_gravity * (a_gravity * h2 + 2.0f * dz * s2);
            if (disc <= 0.0f) return false;
            using func_t = RE::NiPoint3* (*)(RE::NiPoint3*, RE::NiPoint3*, RE::NiPoint3*, RE::NiPoint3*, float, float, bool);
            static REL::Relocation<func_t> func{ RELOCATION_ID(46025, 47289) };
            RE::NiPoint3 from = a_from, target = a_target, vel{};
            return func(&a_out, &from, &target, &vel, a_speed, a_gravity, false) != nullptr;
        }

        bool On() { return s_settings.enabled && !s_hwya && s_hooks > 0; }

        // After the game worked out the player's launch angles: turn them to the dot.
        void Correct(RE::Projectile* a_proj, RE::NiAVObject* a_node, float* a_yaw, float* a_pitch, RE::NiPoint3* a_origin) {
            if (!On() || !a_proj || !a_yaw || !a_pitch || (!a_node && !a_origin) || !CameraAllows()) return;
            const RE::NiPoint3 origin = a_origin ? *a_origin : a_node->world.translate;
            // Line-of-sight layer, not the projectile's own: the projectile layers also meet invisible collision (walls,
            // blockers), which sent the shot at a point nobody can see (Hit Where You Aim 1.3.1). The flight itself
            // keeps the projectile's layer.
            const Ray          ray    = CentreRay(a_proj->GetCollisionGroup(), RE::COL_LAYER::kLOS);
            if (!ray.ok) return;
            if ((ray.point - origin).Dot(ray.dir) <= 0.0f) return;  // the hit is behind the muzzle: leave the game's angles
            const RE::NiPoint3 aim     = ray.point + ray.dir * kPenetration;
            const float        gravity = a_proj->GetGravity();
            RE::NiPoint3       predicted{};
            if (NativeSolve(predicted, origin, aim, a_proj->GetSpeed(), gravity * WorldGravity())) {
                Angles(origin, predicted, *a_pitch, *a_yaw);
            } else {
                Angles(origin, aim, *a_pitch, *a_yaw);
                if (gravity > 0.0f) *a_pitch -= kFixedPitch * kDegToRad;
            }
            if (!s_logged) {
                s_logged = true;
                SKSE::log::info("AimDot: first shot turned to the dot - {:.0f} units out", (aim - origin).Length());
            }
        }

        // The player's projectile launch-angle call (SE 43009 / AE 44200, inside it).
        struct LaunchAngleHook {
            static void thunk(RE::PlayerCharacter* a_player, RE::Projectile* a_proj, RE::NiAVObject* a_node, float* a_yaw, float* a_pitch,
                RE::NiPoint3* a_origin, float a_7, float a_8) {
                func(a_player, a_proj, a_node, a_yaw, a_pitch, a_origin, a_7, a_8);
                Correct(a_proj, a_node, a_yaw, a_pitch, a_origin);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // Flames and beams: the game asks for the aim yaw, then the aim pitch, every update. Yaw does the ray and
        // keeps the pitch for the call right after.
        struct ConcentrationHook {
            using func_t = float (*)(RE::Projectile*, float);
            static inline float  s_pitch = FLT_MAX;
            static inline func_t s_yawFlame = nullptr, s_pitchFlame = nullptr, s_yawBeam = nullptr, s_pitchBeam = nullptr;

            static bool Player(RE::Projectile* a_proj) {
                const auto& h = a_proj->GetProjectileRuntimeData().shooter;
                return h && h.native_handle() == kPlayerHandle;
            }

            static float Yaw(RE::Projectile* a_proj, float a_angle, func_t a_func) {
                if (On() && a_proj && Player(a_proj) && CameraAllows()) {
                    if (TdmAims(TdmShot::kSpell)) {
                        s_pitch = FLT_MAX;
                        return a_func(a_proj, a_angle);
                    }
                    auto& rd    = a_proj->GetProjectileRuntimeData();
                    RE::TESObjectREFR* ref   = rd.shooter.get().get();
                    RE::Actor*         actor = ref ? ref->As<RE::Actor>() : nullptr;
                    auto*      cast  = actor ? actor->GetMagicCaster(rd.castingSource) : nullptr;
                    auto* node  = cast ? cast->GetMagicNode() : nullptr;
                    if (node) {
                        const Ray ray = CentreRay(a_proj->GetCollisionGroup(), RE::COL_LAYER::kSpell);
                        if (ray.ok && (ray.point - node->world.translate).Dot(ray.dir) > 0.0f) {
                            float yaw = a_angle, pitch = FLT_MAX;
                            Angles(node->world.translate, ray.point, pitch, yaw);
                            s_pitch = pitch;
                            return a_func(a_proj, yaw);
                        }
                    }
                    s_pitch = FLT_MAX;
                }
                return a_func(a_proj, a_angle);
            }

            static float Pitch(RE::Projectile* a_proj, float a_angle, func_t a_func) {
                if (On() && a_proj && Player(a_proj) && s_pitch != FLT_MAX && CameraAllows() && !TdmAims(TdmShot::kSpell)) return a_func(a_proj, s_pitch);
                return a_func(a_proj, a_angle);
            }

            static float YawFlame(RE::Projectile* p, float a) { return Yaw(p, a, s_yawFlame); }
            static float PitchFlame(RE::Projectile* p, float a) { return Pitch(p, a, s_pitchFlame); }
            static float YawBeam(RE::Projectile* p, float a) { return Yaw(p, a, s_yawBeam); }
            static float PitchBeam(RE::Projectile* p, float a) { return Pitch(p, a, s_pitchBeam); }
        };

        // Projectile init: True Directional Movement rewrites the angles in here, so the launch's angles are put back
        // after it (and on a horse the arrow's yaw stays within 15 degrees of the bow). With TDM's target lock on and
        // TDM aiming this kind of shot, its rewrite (at the locked target) stands.
        struct ProjectileInitHook {
            static void thunk(RE::Projectile* a_this) {
                RE::NiPoint3 angle = a_this ? a_this->data.angle : RE::NiPoint3{};
                func(a_this);
                if (!a_this || !On()) return;
                const auto& h = a_this->GetProjectileRuntimeData().shooter;
                if (!h || h.native_handle() != kPlayerHandle) return;
                if (TdmAims(a_this->GetFormType() == RE::FormType::ProjectileArrow ? TdmShot::kArrow : TdmShot::kSpell)) return;
                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* pc     = RE::PlayerCamera::GetSingleton();
                if (player && pc && pc->currentState && pc->currentState->id == RE::CameraState::kMount) {
                    const auto st = player->AsActorState()->GetAttackState();
                    auto       biped = player->GetCurrentBiped();
                    auto*      proc  = player->GetActorRuntimeData().currentProcess;
                    if (biped && proc && st >= RE::ATTACK_STATE_ENUM::kBowReleasing && st <= RE::ATTACK_STATE_ENUM::kBowReleased) {
                        if (auto* fire = proc->GetWeaponNode(biped)) {
                            RE::NiPoint3 fd = fire->world.rotate * RE::NiPoint3(0.0f, 1.0f, 0.0f);
                            const float  h2 = fd.x * fd.x + fd.y * fd.y;
                            if (h2 > 1e-6f) {
                                const float bowYaw = std::atan2(fd.x, fd.y);
                                const float diff   = std::remainder(angle.z - bowYaw, 2.0f * std::numbers::pi_v<float>);
                                angle.z            = bowYaw + std::clamp(diff, -15.0f * kDegToRad, 15.0f * kDegToRad);
                            }
                        }
                    }
                }
                auto&       rd    = a_this->GetProjectileRuntimeData();
                const float speed = rd.linearVelocity.Length();
                if (speed <= 1e-6f) return;
                const float sx = std::sin(angle.x), cx = std::cos(angle.x), sz = std::sin(angle.z), cz = std::cos(angle.z);
                const RE::NiPoint3 dir{ sz * cx, cz * cx, -sx };
                a_this->data.angle.x = angle.x;
                a_this->data.angle.z = angle.z;
                rd.linearVelocity    = dir * speed;
                if (auto* obj = a_this->Get3D()) {
                    auto& m     = obj->local.rotate;
                    m.entry[0][0] = cz, m.entry[0][1] = dir.x, m.entry[0][2] = sz * sx;
                    m.entry[1][0] = -sz, m.entry[1][1] = dir.y, m.entry[1][2] = cz * sx;
                    m.entry[2][0] = 0.0f, m.entry[2][1] = dir.z, m.entry[2][2] = cx;
                }
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // Target Actor / Target Location spells (Hit Where You Aim 1.2's spell aim). The game's spell target finder is
        // called twice from SE 33632 / AE 34410 (Target Location first, then Target Actor); in it, SE 33677 / AE 34457
        // looks up the crosshair target and casts a pick from the hand. Inside the PLAYER's finder call the crosshair
        // target is dropped (so the game always casts) and the pick is turned to go through the dot.
        // Hit Where You Aim's 4th spell hook (clearing the caster's desiredTarget) is left out on purpose: it made weapon
        // poison land on the player (HWYA 1.3.1 turned it off as dangerous).
        struct SpellTargetHook {
            using finder_t = RE::MagicTarget* (*)(RE::ActorMagicCaster*, bool, float, RE::NiPoint3*, RE::TESObjectCELL**, RE::bhkPickData*);
            using lookup_t = bool (*)(RE::ObjectRefHandle*, RE::NiPointer<RE::TESObjectREFR>*);
            using pick_t   = void (*)(RE::TESObjectCELL*, RE::bhkPickData*);
            static inline finder_t s_finder = nullptr;
            static inline lookup_t s_lookup = nullptr;
            static inline pick_t   s_pick   = nullptr;
            static inline thread_local int  t_depth = 0;      // > 0: inside the player's finder call
            static inline thread_local bool t_actor = false;  // that call is the Target Actor one
            static inline bool s_loggedLoc = false, s_loggedActor = false, s_loggedMiss = false, s_loggedOutside = false;

            static RE::MagicTarget* Finder(RE::ActorMagicCaster* a_caster, bool a_actor, float a_range, RE::NiPoint3* a_pos,
                RE::TESObjectCELL** a_cell, RE::bhkPickData* a_pick) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (!a_caster || !player || a_caster->GetCasterAsActor() != player) return s_finder(a_caster, a_actor, a_range, a_pos, a_cell, a_pick);
                const bool prev = t_actor;
                t_actor         = a_actor;
                ++t_depth;
                auto* r = s_finder(a_caster, a_actor, a_range, a_pos, a_cell, a_pick);
                --t_depth;
                t_actor = prev;
                return r;
            }

            static bool Mine() { return t_depth > 0 && On() && CameraAllows(); }

            static bool Lookup(RE::ObjectRefHandle* a_handle, RE::NiPointer<RE::TESObjectREFR>* a_out) {
                const bool found = s_lookup(a_handle, a_out);
                if (!a_out || !Mine()) return found;
                a_out->reset();  // no crosshair target: the game casts its pick, and the pick goes through the dot
                return false;
            }

            static void Aim(RE::bhkPickData& a_pick) {
                const float range = a_pick.ray.Length3();  // Havok units
                if (!(range > 1e-4f)) return;
                const Ray c = CentreRay(a_pick.rayInput.filterInfo);
                if (!c.ok) return;
                const float  scale = RE::bhkWorld::GetWorldScale();
                const auto   P     = [](const RE::hkVector4& v) { return RE::NiPoint3{ v.quad.m128_f32[0], v.quad.m128_f32[1], v.quad.m128_f32[2] }; };
                RE::Actor*   actor = c.ref ? c.ref->As<RE::Actor>() : nullptr;
                RE::NiPoint3 from{}, dir{};
                if (t_actor && !actor) {
                    // Target Actor with nobody under the dot: cast along the dot's own ray, so it misses where the dot misses
                    from = P(c.input.from) * (1.0f / scale);
                    dir  = c.dir;
                } else {
                    from = P(a_pick.rayInput.from) * (1.0f / scale);  // the hand
                    if ((c.point - from).Dot(c.dir) <= 0.0f) return;  // the dot's hit is behind the hand: leave the game's pick
                    dir = c.point - from;
                    const float l = dir.Length();
                    if (!(l > 1e-4f)) return;
                    dir = dir * (1.0f / l);
                }
                a_pick.rayInput.from = RE::hkVector4(from * scale);
                a_pick.ray           = RE::hkVector4(dir * range);
                a_pick.rayInput.to   = a_pick.rayInput.from + a_pick.ray;
                // Once per kind (a Target Location cast must not hide the first Target Actor one), with the reach
                // and what is under the dot, so a spell that "does nothing" can be told from one out of range.
                const float reach = range / scale, dist = (c.point - from).Length();
                if (!t_actor && !s_loggedLoc) {
                    s_loggedLoc = true;
                    SKSE::log::info("AimDot: first Target Location spell turned to the dot - reach {:.0f}, dot {:.0f} units away", reach, dist);
                } else if (t_actor && actor && !s_loggedActor) {
                    s_loggedActor = true;
                    const char* name = actor->GetDisplayFullName();
                    SKSE::log::info("AimDot: first Target Actor spell turned to the dot - {} under the dot, {:.0f} units away, reach {:.0f}",
                        name ? name : "?", dist, reach);
                } else if (t_actor && !actor && !s_loggedMiss) {
                    s_loggedMiss = true;
                    SKSE::log::info("AimDot: first Target Actor spell with nobody under the dot - cast along the dot's ray (reach {:.0f})", reach);
                }
            }

            static void Pick(RE::TESObjectCELL* a_cell, RE::bhkPickData* a_pick) {
                if (a_pick && Mine()) {
                    Aim(*a_pick);
                } else if (t_depth == 0 && !s_loggedOutside) {
                    s_loggedOutside = true;
                    SKSE::log::info("AimDot: a spell pick ran outside the player's target finder - left alone");
                }
                s_pick(a_cell, a_pick);
            }
        };

        std::uintptr_t CallTarget(std::uintptr_t a_site) {
            return a_site + 5 + *reinterpret_cast<const std::int32_t*>(a_site + 1);
        }

        bool Ranged(RE::TESForm* a_form) {
            if (!a_form) return false;
            if (a_form->Is(RE::FormType::Spell)) return true;
            if (auto* w = a_form->As<RE::TESObjectWEAP>()) return w->IsBow() || w->IsCrossbow() || w->IsStaff();
            return false;
        }
    }

    Settings& Get() { return s_settings; }

    void Install() {
        if (REL::Module::IsVR()) {
            s_status = "not on VR";
            SKSE::log::info("AimDot: not on VR");
            return;
        }
        s_hwya = ::GetModuleHandleW(L"HitWhereYouAim.dll") != nullptr;
        if (s_hwya) {
            s_status = "Hit Where You Aim is loaded - it aims the projectiles, TFCam stays out";
            SKSE::log::info("AimDot: {}", s_status);
            return;
        }
        if (::GetModuleHandleA("TrueDirectionalMovement.dll")) {
            s_tdm = TDM_API::RequestV1();
            if (s_tdm) {
                SKSE::log::info("AimDot: True Directional Movement found - while its target lock is on, arrows and flames / beams keep its aim "
                    "(arrow aim mode {}, spell aim mode {}; 0 = Free Aim, the dot aims)",
                    TdmAimType(TdmShot::kArrow), TdmAimType(TdmShot::kSpell));
            } else {
                SKSE::log::warn("AimDot: True Directional Movement is loaded but gave no API - its target lock cannot be seen");
            }
        }
        auto& tr = SKSE::GetTrampoline();
        // 1. the player's launch angles
        if (const auto site = CallSite(RELOCATION_ID(43009, 44200),
                "F3 0F 11 ?? 24 38 F3 0F 11 ?? 24 30 48 89 ?? 24 28 48 89 ?? 24 20 4C 8B C5 48 8B D7 48 8B 0D ?? ?? ?? ?? E8", 0x23,
                "launch angles")) {
            LaunchAngleHook::func = tr.write_call<5>(site, LaunchAngleHook::thunk);
            ++s_hooks;
        }
        // 2. flames, 3. beams: yaw at +0xE, pitch at +0x21 past the pattern
        if (const auto site = CallSite(RELOCATION_ID(42727, 43894),
                "48 8B CD E8 ?? ?? ?? ?? 0F 28 C8 48 8B CE E8 ?? ?? ?? ?? 48 8B CD E8 ?? ?? ?? ?? 0F 28 C8 48 8B CE E8 ?? ?? ?? ??", 0xE,
                "flames")) {
            if (*reinterpret_cast<const std::uint8_t*>(site + 0x13) == 0xE8) {
                ConcentrationHook::s_yawFlame   = reinterpret_cast<ConcentrationHook::func_t>(tr.write_call<5>(site, ConcentrationHook::YawFlame));
                ConcentrationHook::s_pitchFlame = reinterpret_cast<ConcentrationHook::func_t>(tr.write_call<5>(site + 0x13, ConcentrationHook::PitchFlame));
                ++s_hooks;
            }
        }
        if (const auto site = CallSite(RELOCATION_ID(42586, 43749),
                "49 8B CE E8 ?? ?? ?? ?? 0F 28 C8 48 8B CE E8 ?? ?? ?? ?? 49 8B CE E8 ?? ?? ?? ?? 0F 28 C8 48 8B CE E8 ?? ?? ?? ??", 0xE,
                "beams")) {
            if (*reinterpret_cast<const std::uint8_t*>(site + 0x13) == 0xE8) {
                ConcentrationHook::s_yawBeam   = reinterpret_cast<ConcentrationHook::func_t>(tr.write_call<5>(site, ConcentrationHook::YawBeam));
                ConcentrationHook::s_pitchBeam = reinterpret_cast<ConcentrationHook::func_t>(tr.write_call<5>(site + 0x13, ConcentrationHook::PitchBeam));
                ++s_hooks;
            }
        }
        // 4. projectile init (after True Directional Movement's rewrite)
        std::uintptr_t init = 0;
        if (!REL::Module::IsAE()) {
            init = REL::Relocation<std::uintptr_t>(REL::ID(43030), 0x3B8).address();
            if (*reinterpret_cast<const std::uint8_t*>(init) != 0xE8) init = 0;
        } else {
            init = CallSite(REL::RelocationID(44222, 44222), "48 8B 01 FF 50 08 48 8B CF E8 ?? ?? ?? ?? 48 8B D7", 0x9, "projectile init");
        }
        if (init) {
            ProjectileInitHook::func = tr.write_call<5>(init, ProjectileInitHook::thunk);
            ++s_hooks;
        }
        // 5. Target Actor / Target Location spells: both finder calls, the pick, and (optional) the crosshair lookup
        {
            constexpr std::string_view kFinder = "48 89 ?? 24 ?? 48 8D 44 24 ?? 48 89 ?? 24 ?? 4C 8D 4C 24 ?? 48 8B ?? E8 ?? ?? ?? ??";
            const auto loc  = CallSite(RELOCATION_ID(33632, 34410), kFinder, 0x17, "spell finder (Target Location)");
            const auto act  = CallSite(RELOCATION_ID(33632, 34410), std::string(kFinder) + " 48 85 C0", 0x17, "spell finder (Target Actor)");
            const auto pick = CallSite(RELOCATION_ID(33677, 34457),
                "48 8B 05 ?? ?? ?? ?? 49 8B ?? 48 8B ?? 60 E8 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ??", 0x0E, "spell pick");
            const auto look = CallSite(RELOCATION_ID(33677, 34457), "48 8D 54 24 ?? 48 8D ?? ?? ?? ?? ?? E8 ?? ?? ?? ??", 0x0C,
                "spell crosshair target");
            if (!loc || !act || !pick) {
                SKSE::log::warn("AimDot: spells - a call site is missing; Target Actor / Target Location spells keep the game's aim");
            } else if (loc >= act || CallTarget(loc) != CallTarget(act)) {
                SKSE::log::warn("AimDot: spells - the two finder calls are not the expected pair ({:X} / {:X}); spells keep the game's aim",
                    loc - REL::Module::get().base(), act - REL::Module::get().base());
            } else {
                SpellTargetHook::s_finder = reinterpret_cast<SpellTargetHook::finder_t>(tr.write_call<5>(act, SpellTargetHook::Finder));
                tr.write_call<5>(loc, SpellTargetHook::Finder);
                SpellTargetHook::s_pick = reinterpret_cast<SpellTargetHook::pick_t>(tr.write_call<5>(pick, SpellTargetHook::Pick));
                ++s_hooks;
                // The crosshair lookup: only when the call is the game's handle lookup (LookupReferenceByHandle) and the
                // handle it is given was just read from the crosshair pick data: shortly before it the game does
                // mov rax, [rip+disp] (the CrosshairPickData pointer) ; mov ecx, [rax+4] (its target handle), copies the
                // handle to a local and passes that (seen in AE 1.6.1170 memory, 11-06).
                const std::uintptr_t lookupFn = REL::Relocation<std::uintptr_t>(RELOCATION_ID(12204, 12332)).address();
                const auto*          xhair    = RE::CrosshairPickData::GetSingleton();
                const auto           base     = REL::Module::get().base();
                bool                 callOk = false, readOk = false;
                if (look && xhair) {
                    callOk = CallTarget(look) == lookupFn;
                    for (std::uintptr_t p = look - 0x40; p + 10 <= look; ++p) {
                        const auto* b = reinterpret_cast<const std::uint8_t*>(p);
                        if (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x05 && b[7] == 0x8B && b[8] == 0x48 && b[9] == 0x04) {
                            const std::uintptr_t slot = p + 7 + *reinterpret_cast<const std::int32_t*>(p + 3);
                            readOk = *reinterpret_cast<const std::uintptr_t*>(slot) == reinterpret_cast<std::uintptr_t>(xhair);
                            if (readOk) break;
                        }
                    }
                }
                const bool lookOk = callOk && readOk;
                if (lookOk) {
                    SpellTargetHook::s_lookup = reinterpret_cast<SpellTargetHook::lookup_t>(tr.write_call<5>(look, SpellTargetHook::Lookup));
                    SKSE::log::info("AimDot: spells - Target Actor / Target Location go through the dot");
                } else {
                    SKSE::log::warn("AimDot: spells - the crosshair lookup did not check out (site {:X}, call {}, crosshair read {}); "
                                    "spells go through the dot, but an actor under the game's own crosshair still wins",
                        look ? look - base : 0, callOk ? "ok" : "other", readOk ? "ok" : "not found");
                }
            }
        }
        s_status    = s_hooks > 0 ? std::format("on - {} of {} hooks in", s_hooks, kHookCount) : "off - none of the hooks fitted this game version";
        s_offStatus = s_hooks > 0 ? std::format("off - tick the box to use it ({} of {} hooks ready)", s_hooks, kHookCount) : s_status;
        SKSE::log::info("AimDot: {} (bHitTheDot={})", s_status, s_settings.enabled ? 1 : 0);
    }

    bool Active() { return On(); }

    bool DotWanted() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui     = RE::UI::GetSingleton();
        if (!player || !player->Is3DLoaded()) return false;
        if (ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME) || ui->IsMenuOpen(RE::Console::MENU_NAME))) return false;
        if (!player->AsActorState()->IsWeaponDrawn()) return false;
        return Ranged(player->GetEquippedObject(false)) || Ranged(player->GetEquippedObject(true));
    }

    bool DotInCharge() { return FovFx::Get().aimDot && (Active() || (FovFx::HitWhereYouAim() && FovFx::Get().enabled)); }

    namespace {
        std::atomic<bool> s_crosshairOff{ false };  // we hid the game's crosshair (UI thread writes it)
        std::atomic<bool> s_pending{ false };       // a UI task is queued, don't queue another

        // UI thread. The HUD's crosshair clip (_root.HUDMovieBaseInstance.Crosshair; vanilla, SkyHUD, Edge UI) is kept
        // hidden. Not its SetCrosshairEnabled switch: the game turns that back on every frame (tested 10-31, 4 tries a
        // second, each found it on again). _visible stays put until the HUD's SetCrosshairTarget / ValidateCrosshair
        // set it again (a new crosshair target, a HUD mode change), so it is checked every frame.
        void SetGameCrosshair(bool a_on) {
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks || s_pending.exchange(true)) return;
            tasks->AddUITask([a_on]() {
                s_pending = false;
                auto* ui = RE::UI::GetSingleton();
                if (!ui) return;
                auto view = ui->GetMovieView(RE::HUDMenu::MENU_NAME);
                if (!view) return;
                RE::GFxValue hud;
                if (!view->GetVariable(&hud, "_root.HUDMovieBaseInstance") || !hud.IsObject()) return;
                if (a_on) {
                    if (!s_crosshairOff.exchange(false)) return;  // never ours to give back
                    hud.Invoke("ValidateCrosshair", nullptr, nullptr, 0);  // the HUD works out its own visibility again
                    SKSE::log::info("AimDot: the game's crosshair is back");
                    return;
                }
                RE::GFxValue clip;
                if (!hud.GetMember("Crosshair", &clip) || !clip.IsDisplayObject()) return;
                RE::GFxValue visible;
                if (clip.GetMember("_visible", &visible) && visible.IsBool() && visible.GetBool()) {
                    clip.SetMember("_visible", RE::GFxValue(false));
                }
                if (!s_crosshairOff.exchange(true)) SKSE::log::info("AimDot: the game's crosshair is off - the dot is the crosshair");
            });
        }
    }

    void CrosshairTick() {
        const bool hide = s_settings.hideGameCrosshair && DotInCharge();
        if (!hide) {
            if (s_crosshairOff.load()) SetGameCrosshair(true);
            return;
        }
        SetGameCrosshair(false);
    }

    const char* Status() { return !s_settings.enabled && !s_hwya && s_hooks > 0 ? s_offStatus.c_str() : s_status.c_str(); }
}
