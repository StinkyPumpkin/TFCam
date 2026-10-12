#include "AmbientCam.h"
#include "CamCollision.h"
#include "CellEntryCam.h"
#include "FreeCamController.h"
#include "FreeCamMenu.h"
#include "SceneCam.h"
#include "SceneShots.h"
#include "SceneTracker.h"
#include "SlccBridge.h"

#include <RE/I/INISettingCollection.h>
#include <RE/M/MouseMoveEvent.h>
#include <RE/T/ThumbstickEvent.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace AmbientCam {

    namespace {
        constexpr float kPi         = 3.14159265358979f;
        constexpr float kDegToRad   = kPi / 180.0f;
        constexpr float kLn2        = 0.69314718f;
        constexpr float kPitchLimit = 1.55f;

        Settings s_cfg;

        // ---- math ------------------------------------------------------------------------------------------------
        float WrapPi(float a) { return std::remainder(a, 2.0f * kPi); }

        float SmoothStep(float x) {
            x = std::clamp(x, 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        }

        float ExpWeight(float dt, float halfLife) { return dt <= 0.0f ? 0.0f : 1.0f - std::exp(-kLn2 * dt / halfLife); }

        RE::NiPoint3 Mix(const RE::NiPoint3& a, const RE::NiPoint3& b, float w) {
            return { a.x + (b.x - a.x) * w, a.y + (b.y - a.y) * w, a.z + (b.z - a.z) * w };
        }

        // TFCam's convention (see SceneCam.cpp): forward = (sin yaw cos pitch, cos yaw cos pitch, -sin pitch).
        void LookAngles(const RE::NiPoint3& d, float& yaw, float& pitch) {
            yaw   = std::atan2(d.x, d.y);
            pitch = std::atan2(-d.z, std::hypot(d.x, d.y));
        }

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

        // Own frame time for the drive: capped at 0.1 s, 0 while the game is paused.
        float FrameDt() {
            static double s_last = 0.0;
            const double  now    = Now();
            float         dt     = s_last > 0.0 ? static_cast<float>(now - s_last) : 0.0f;
            s_last               = now;
            dt                   = std::clamp(dt, 0.0f, 0.1f);
            auto* ui             = RE::UI::GetSingleton();
            if (ui && ui->GameIsPaused()) dt = 0.0f;
            return dt;
        }

        std::string Lower(std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // ---- camera ------------------------------------------------------------------------------------------------
        RE::PlayerCamera* Cam() { return RE::PlayerCamera::GetSingleton(); }

        float GetFOV() {
            auto* cam = Cam();
            return cam ? cam->GetRuntimeData2().worldFOV : 0.0f;
        }

        void SetFOV(float fov) {
            auto* cam = Cam();
            if (cam && fov > 0.0f && std::isfinite(fov)) cam->GetRuntimeData2().worldFOV = fov;
        }

        struct Pose {
            RE::NiPoint3 pos{};
            float        pitch = 0.0f;
            float        yaw   = 0.0f;
            float        roll  = 0.0f;
            float        fov   = 0.0f;
        };

        Pose ReadPose(const RE::FreeCameraState* fcs) {
            Pose p;
            p.pos   = fcs->translation;
            p.pitch = fcs->rotation.x;
            p.yaw   = fcs->rotation.y;
            p.roll  = FreeCam::GetRollRadians();
            p.fov   = GetFOV();
            return p;
        }

        void WritePose(RE::FreeCameraState* fcs, const Pose& p) {
            fcs->translation = p.pos;
            fcs->rotation.x  = std::clamp(p.pitch, -kPitchLimit, kPitchLimit);
            fcs->rotation.y  = WrapPi(p.yaw);
            FreeCam::SetRollRadians(p.roll);
            SetFOV(p.fov);
        }

        Pose LerpPose(const Pose& a, const Pose& b, float w) {
            Pose p;
            p.pos   = Mix(a.pos, b.pos, w);
            p.yaw   = a.yaw + WrapPi(b.yaw - a.yaw) * w;
            p.pitch = a.pitch + (b.pitch - a.pitch) * w;
            p.roll  = a.roll + WrapPi(b.roll - a.roll) * w;
            p.fov   = a.fov + (b.fov - a.fov) * w;
            return p;
        }

        // Free camera that TFCam drives right now.
        RE::FreeCameraState* LiveFreeCam() {
            auto* cam = Cam();
            if (!cam || !cam->IsInFreeCameraMode() || !FreeCam::TFCamDriving()) return nullptr;
            auto* state = cam->currentState.get();
            return state ? static_cast<RE::FreeCameraState*>(state) : nullptr;
        }

        // ---- log / status -------------------------------------------------------------------------------------------
        std::mutex  s_statusLock;
        MenuStatus  s_status{};
        std::string s_lastEvent;

        void Event(std::string text) {
            SKSE::log::info("AmbientCam: {}", text);
            s_lastEvent = std::move(text);
        }

        void CopyStr(char* dst, std::size_t size, const std::string& src) {
            const std::size_t n = (std::min)(src.size(), size - 1);
            std::memcpy(dst, src.data(), n);
            dst[n] = '\0';
        }

        // ---- the player's body (Dolly / Spiral subject) --------------------------------------------------------------
        struct Names {
            RE::BSFixedString pelvis{ "NPC Pelvis [Pelv]" };
            RE::BSFixedString head{ "NPC Head [Head]" };
            RE::BSFixedString footL{ "NPC L Foot [Lft ]" };
            RE::BSFixedString footR{ "NPC R Foot [Rft ]" };
        };

        const Names& N() {
            static const Names names;
            return names;
        }

        RE::NiAVObject* Node(RE::NiAVObject* root, const RE::BSFixedString& name) {
            return root ? root->GetObjectByName(name) : nullptr;
        }

        struct Subject {
            bool         ok = false;
            RE::NiPoint3 feet{};
            RE::NiPoint3 pelvis{};
            RE::NiPoint3 head{};
            RE::NiPoint3 top{};  // top of the head
            float        heading = 0.0f;
        };

        Subject s_subj;  // smoothed
        bool    s_subjSeeded = false;

        bool ReadSubject(Subject& out) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return false;
            auto*        root  = player->Get3D(false);
            const auto   pos   = player->GetPosition();
            const float  scale = std::clamp(player->GetScale(), 0.3f, 3.0f);
            out.feet           = pos;
            out.heading        = player->GetAngleZ();
            auto* head         = Node(root, N().head);
            auto* pelvis       = Node(root, N().pelvis);
            out.head           = head ? head->world.translate : RE::NiPoint3{ pos.x, pos.y, pos.z + 122.0f * scale };
            out.pelvis         = pelvis ? pelvis->world.translate : RE::NiPoint3{ pos.x, pos.y, pos.z + 66.0f * scale };
            if (root) {
                // Ground level: the reference origin, or the ankles if they hang lower (sitting / kneeling poses).
                for (const auto* name : { &N().footL, &N().footR }) {
                    if (auto* foot = Node(root, *name)) out.feet.z = (std::min)(out.feet.z, foot->world.translate.z - 3.0f);
                }
            }
            const float body = (std::max)(out.head.z - out.feet.z, 20.0f);
            out.top          = { out.head.x, out.head.y, out.head.z + 0.1f * body };
            out.ok           = std::isfinite(out.head.z) && std::isfinite(out.feet.z) && std::isfinite(out.pelvis.z);
            return out.ok;
        }

        void UpdateSubject(float dt) {
            Subject raw;
            if (!ReadSubject(raw)) return;
            if (!s_subjSeeded || dt <= 0.0f) {
                if (!s_subjSeeded) s_subj = raw;
                s_subjSeeded = true;
                if (dt <= 0.0f) return;
            }
            const float w   = ExpWeight(dt, 0.2f);
            s_subj.feet     = Mix(s_subj.feet, raw.feet, w);
            s_subj.pelvis   = Mix(s_subj.pelvis, raw.pelvis, w);
            s_subj.head     = Mix(s_subj.head, raw.head, w);
            s_subj.top      = Mix(s_subj.top, raw.top, w);
            s_subj.heading  = raw.heading;
            s_subj.ok       = true;
        }

        // Height at a fraction of the body: 0 = feet, 0.5 = pelvis, 1 = top of the head.
        float BodyZ(float frac) {
            frac = std::clamp(frac, 0.0f, 1.0f);
            return frac <= 0.5f ? s_subj.feet.z + (s_subj.pelvis.z - s_subj.feet.z) * (frac / 0.5f)
                                : s_subj.pelvis.z + (s_subj.top.z - s_subj.pelvis.z) * ((frac - 0.5f) / 0.5f);
        }

        // The body's centre line at a height (feet origin to head: follows a lean).
        RE::NiPoint3 AxisAt(float z) {
            const float span = (std::max)(s_subj.head.z - s_subj.feet.z, 1.0f);
            const float t    = std::clamp((z - s_subj.feet.z) / span, 0.0f, 1.0f);
            return { s_subj.feet.x + (s_subj.head.x - s_subj.feet.x) * t, s_subj.feet.y + (s_subj.head.y - s_subj.feet.y) * t, z };
        }

        bool InInterior() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell   = player ? player->GetParentCell() : nullptr;
            return cell && cell->IsInteriorCell();
        }

        // 0.12.0: the point on the camera's line of sight nearest the body's centre line (the face for the dolly, the
        // centre line at camera height for the spiral) - where the wall check casts from. Looking away from the body
        // (rare, mid-crossfade): the centre line at camera height.
        RE::NiPoint3 SightPoint(const RE::NiPoint3& a_pos, float a_yaw, float a_pitch) {
            const RE::NiPoint3 axis  = AxisAt(std::clamp(a_pos.z, s_subj.feet.z, s_subj.top.z));
            const float        horiz = std::hypot(a_pos.x - axis.x, a_pos.y - axis.y);
            const float        cp    = std::cos(a_pitch);
            if (cp < 0.2f) return axis;
            const float        t = horiz / cp;
            const RE::NiPoint3 p{ a_pos.x + std::sin(a_yaw) * cp * t, a_pos.y + std::cos(a_yaw) * cp * t, a_pos.z - std::sin(a_pitch) * t };
            const RE::NiPoint3 onAxis = AxisAt(std::clamp(p.z, s_subj.feet.z, s_subj.top.z));
            return std::hypot(p.x - onAxis.x, p.y - onAxis.y) < 40.0f ? p : axis;
        }

        // ---- routines ------------------------------------------------------------------------------------------------
        enum class Routine { kDolly, kSpiral };

        // 0.11.1 spiral: from the floor to the face in two loops, settling on the four areas the user wants to see.
        // loops = position round the body (0 = in front of the player, 0.5 = behind), frac = body height (0 feet,
        // 0.5 pelvis, 1 top of the head), close = close-radius multiplier.
        struct Key {
            const char* name;
            float       loops;
            float       frac;
            float       close;
        };
        constexpr Key kKeys[] = {
            { "floor", 0.00f, 0.02f, 2.20f },     // start low and wide, then rise at once
            { "bum", 0.50f, 0.47f, 0.95f },       // from behind
            { "genitals", 1.00f, 0.45f, 0.90f },  // from the front
            { "breasts", 1.85f, 0.72f, 1.00f },   // front three-quarter
            { "face", 2.00f, 0.95f, 1.30f },      // from the front
        };
        constexpr int kKeyCount = static_cast<int>(sizeof(kKeys) / sizeof(kKeys[0]));

        float AreaSec() { return (std::max)(s_cfg.spiralAreaSec, 0.8f); }

        // One segment: the move round the body to the next area plus the settle on it.
        float SegmentSec(int i) { return AreaSec() * (1.0f + 2.0f * (kKeys[i + 1].loops - kKeys[i].loops)); }

        float SpiralLength() {
            float sum = AreaSec();  // the hold on the face
            for (int i = 0; i + 1 < kKeyCount; ++i) sum += SegmentSec(i);
            return sum;
        }

        // Slows near both ends of a segment (0.3x) and hurries in the middle (1.7x), never stopping.
        float Glide(float s) {
            s = std::clamp(s, 0.0f, 1.0f);
            return s - 0.7f / (2.0f * kPi) * std::sin(2.0f * kPi * s);
        }

        // 0.12.1: the first half of Glide stretched over the whole move - slow only where it starts, full speed at its end.
        float GlideIn(float s) { return 2.0f * Glide(0.5f * std::clamp(s, 0.0f, 1.0f)); }

        float FarRadius() { return s_cfg.spiralFar * (InInterior() ? s_cfg.interiorScale : 1.0f); }

        float KeyRadius(const Key& k, float farR) { return (std::min)(s_cfg.spiralClose * k.close, farR); }

        // 0.11.2 opening before the spiral: on the face from the front, back out to the far radius, down to the floor,
        // then in to the spiral's first key. hold = the stay on the face first (0 when the spiral plays again: its own
        // hold on the face comes just before). The zooms take less time when there is little to zoom (interiors).
        // 0.12.1: no stop at the feet - the zoom in is the last inSec of the drop, the drop lands at full speed and the
        // spiral already leaves the floor during its last FeetBlendSec (SpiralPose rounds that corner).
        struct Opening {
            float hold = 0, outSec = 0, dropSec = 0, inSec = 0;
            float rFace = 0, rFar = 0, rFloor = 0;
            float Length() const { return hold + outSec + dropSec; }
        };

        float FeetBlendSec() { return 0.5f * AreaSec(); }

        Opening SpiralOpening(float hold) {
            Opening o;
            o.rFar     = FarRadius();
            o.rFace    = KeyRadius(kKeys[kKeyCount - 1], o.rFar);
            o.rFloor   = KeyRadius(kKeys[0], o.rFar);
            auto zoom  = [&](float dr) { return AreaSec() * std::clamp(std::fabs(dr) / (0.4f * o.rFar), 0.25f, 1.0f); };
            o.hold     = hold;
            o.outSec   = zoom(o.rFar - o.rFace);
            o.dropSec  = 3.0f * AreaSec();
            o.inSec    = zoom(o.rFar - o.rFloor);
            return o;
        }

        float RoutineLength(Routine r, float spiralHold) {
            return r == Routine::kDolly ? (std::max)(s_cfg.dollyPeriod, 2.0f) * static_cast<float>((std::max)(s_cfg.dollyCycles, 1))
                                        : SpiralOpening(spiralHold).Length() - FeetBlendSec() + SpiralLength();
        }

        const char* RoutineName(Routine r) { return r == Routine::kDolly ? "Dolly zoom" : "Spiral"; }

        Pose DollyPose(float t, float az0) {
            const float period = (std::max)(s_cfg.dollyPeriod, 2.0f);
            const float phase  = 0.5f - 0.5f * std::cos(2.0f * kPi * t / period);  // 0 far/narrow -> 1 close/wide -> 0
            const float scale  = InInterior() ? s_cfg.interiorScale : 1.0f;
            const float framing = s_cfg.dollyFraming;
            const float maxDist = 420.0f * scale;
            // Keep the exact dolly relation but never farther than maxDist: raise the narrow end instead.
            const float minFov = 2.0f * std::atan(framing / (2.0f * maxDist)) / kDegToRad;
            const float narrow = (std::max)(s_cfg.dollyNarrowFOV, minFov);
            const float wide   = (std::max)(s_cfg.dollyWideFOV, narrow + 5.0f);
            const float fov    = narrow + (wide - narrow) * phase;
            const float dist   = framing / (2.0f * std::tan(0.5f * fov * kDegToRad));

            // 0.11.1: low near the ground, tilted up at the face while it circles. When it is too close to stay that low,
            // it rises just enough to keep the distance, so the dolly relation stays exact.
            const RE::NiPoint3 face     = Mix(s_subj.pelvis, s_subj.head, 0.97f);
            const float        az       = az0 + s_cfg.dollySpin * kDegToRad * t;
            const float        minHoriz = 0.35f * dist;
            float              dz       = (s_subj.feet.z + s_cfg.dollyCamHeight) - face.z;  // < 0: below the face
            float              horiz    = minHoriz;
            if (dist * dist - dz * dz >= minHoriz * minHoriz) {
                horiz = std::sqrt(dist * dist - dz * dz);
            } else {
                dz = (dz < 0.0f ? -1.0f : 1.0f) * std::sqrt((std::max)(dist * dist - horiz * horiz, 0.0f));
            }
            Pose p;
            p.pos = { face.x + std::sin(az) * horiz, face.y + std::cos(az) * horiz, face.z + dz };
            LookAngles({ face.x - p.pos.x, face.y - p.pos.y, face.z - p.pos.z }, p.yaw, p.pitch);
            p.roll = 0.0f;
            p.fov  = fov;
            return p;
        }

        // On a ring round the body at height z, looking level at its centre line.
        Pose RingPose(float z, float az, float radius) {
            const RE::NiPoint3 axis = AxisAt(z);
            Pose               p;
            p.pos   = { axis.x + std::sin(az) * radius, axis.y + std::cos(az) * radius, z };
            p.yaw   = std::atan2(axis.x - p.pos.x, axis.y - p.pos.y);
            p.pitch = 0.0f;
            p.roll  = 0.0f;
            p.fov   = s_cfg.spiralFOV;
            return p;
        }

        // Where a spiral pose sits on its ring: height, angle round the body, distance from the centre line.
        struct Ring {
            float z = 0, az = 0, radius = 0;
        };

        // The face and floor keys both sit in front of the player (whole loops), so the opening stays at az0.
        Ring OpeningRing(const Opening& o, float t, float az0) {
            const Key& faceKey  = kKeys[kKeyCount - 1];
            const Key& floorKey = kKeys[0];
            float      frac     = faceKey.frac;
            float      radius   = o.rFace;
            if (t < o.hold) {
                // on the face
            } else if ((t -= o.hold) < o.outSec) {
                radius = o.rFace + (o.rFar - o.rFace) * Glide(t / o.outSec);
            } else {
                t -= o.outSec;
                frac = faceKey.frac + (floorKey.frac - faceKey.frac) * GlideIn(t / o.dropSec);
                const float in = t - (o.dropSec - o.inSec);
                radius = in <= 0.0f ? o.rFar : o.rFar + (o.rFloor - o.rFar) * GlideIn(in / o.inSec);
            }
            return { BodyZ(frac), az0, radius };
        }

        // t = seconds since the spiral left the floor key.
        Ring SpiralRing(float t, float az0, float farR) {
            const float dir = s_cfg.spiralSpeed < 0.0f ? -1.0f : 1.0f;

            int   seg  = kKeyCount - 1;  // past the last segment = the hold on the face
            float s    = 1.0f;
            float done = 0.0f;
            for (int i = 0; i + 1 < kKeyCount; ++i) {
                const float len = SegmentSec(i);
                if (t < done + len) {
                    seg = i;
                    s   = (t - done) / len;
                    break;
                }
                done += len;
            }
            const bool  hold = seg >= kKeyCount - 1;
            const Key&  a    = kKeys[hold ? kKeyCount - 1 : seg];
            const Key&  b    = kKeys[hold ? kKeyCount - 1 : seg + 1];
            const float e    = hold ? 1.0f : Glide(s);

            const float z      = BodyZ(a.frac + (b.frac - a.frac) * e);
            const float loops  = a.loops + (b.loops - a.loops) * e;
            const float base   = KeyRadius(a, farR) + (KeyRadius(b, farR) - KeyRadius(a, farR)) * e;
            const float swell  = hold ? 0.0f : std::sin(kPi * s);  // out between areas, in on them
            const float radius = base + (farR - base) * 0.5f * swell * swell;
            const float az     = az0 + dir * 2.0f * kPi * loops;
            return { z, az, radius };
        }

        Pose SpiralPose(float t, float az0, float openingHold) {
            const Opening open  = SpiralOpening(openingHold);
            const float   blend = FeetBlendSec();
            const float   start = open.Length() - blend;  // the spiral leaves the floor before the drop lands there
            if (t < start) {
                const Ring r = OpeningRing(open, t, az0);
                return RingPose(r.z, r.az, r.radius);
            }
            Ring r = SpiralRing(t - start, az0, open.rFar);
            if (t < open.Length()) {
                // Round the corner at the feet: fade from the falling camera into the rising spiral, neither one slowing.
                const Ring  d = OpeningRing(open, t, az0);
                const float w = SmoothStep((t - start) / blend);
                r             = { d.z + (r.z - d.z) * w, d.az + (r.az - d.az) * w, d.radius + (r.radius - d.radius) * w };
            }
            return RingPose(r.z, r.az, r.radius);
        }

        // 0.11.2: the crossfade into a routine goes round the body, not through it (from the normal camera behind the
        // player to the opening in front of the face). Position in cylinder coordinates about the body's centre line,
        // heading kept relative to the direction towards the body. Both angle steps are tracked from frame to frame so
        // a half-turn blend cannot flip to the other side on the way.
        struct OrbitBlend {
            bool  started = false;
            float turn    = 0.0f;  // azimuth step a -> b
            float look    = 0.0f;  // step of the heading relative to the body
        };

        float KeepSide(float d, float last, bool started) {
            d = WrapPi(d);
            if (started) {
                if (d - last > kPi) d -= 2.0f * kPi;
                else if (last - d > kPi) d += 2.0f * kPi;
            }
            return d;
        }

        Pose OrbitLerp(const Pose& a, const Pose& b, float w, OrbitBlend& ob) {
            auto polar = [](const Pose& p, float& az, float& r) {
                const RE::NiPoint3 c = AxisAt(p.pos.z);
                r  = std::hypot(p.pos.x - c.x, p.pos.y - c.y);
                az = std::atan2(p.pos.x - c.x, p.pos.y - c.y);
            };
            float azA = 0.0f, rA = 0.0f, azB = 0.0f, rB = 0.0f;
            polar(a, azA, rA);
            polar(b, azB, rB);
            if (rA < 1.0f) azA = azB;  // on the centre line: no side to come from
            if (rB < 1.0f) azB = azA;
            const float relA = WrapPi(a.yaw - azA - kPi);  // 0 = facing the body
            const float relB = WrapPi(b.yaw - azB - kPi);
            ob.turn    = KeepSide(azB - azA, ob.turn, ob.started);
            ob.look    = KeepSide(relB - relA, ob.look, ob.started);
            ob.started = true;

            const float        az = azA + ob.turn * w;
            const float        r  = rA + (rB - rA) * w;
            const float        z  = a.pos.z + (b.pos.z - a.pos.z) * w;
            const RE::NiPoint3 c  = AxisAt(z);
            Pose               p;
            p.pos   = { c.x + std::sin(az) * r, c.y + std::cos(az) * r, z };
            p.yaw   = WrapPi(az + kPi + relA + ob.look * w);
            p.pitch = a.pitch + (b.pitch - a.pitch) * w;
            p.roll  = a.roll + WrapPi(b.roll - a.roll) * w;
            p.fov   = a.fov + (b.fov - a.fov) * w;
            return p;
        }

        // ---- session: a free camera TFCam started for the idle camera / the furniture camera -----------------------
        enum class Kind { kNone, kVanity, kFurniture };

        // The camera from before the session, in the player's frame, to blend back to.
        struct PreShot {
            float x = 0, y = 0, z = 0;
            float yawRel = 0, pitch = 0, roll = 0, fov = 0;
        };

        struct Session {
            bool    owned   = false;
            Kind    kind    = Kind::kNone;
            bool    havePre = false;
            PreShot pre;
            double  startedAt = 0.0;
        };
        Session s_sess;

        struct Return {
            bool  active  = false;
            bool  fromSet = false;
            Pose  from;
            float t       = 0.0f;
            float dur     = 0.0f;
            bool  handOff = false;
        };
        Return s_ret;

        // Idle camera state.
        enum class VState { kOff, kStarting, kRunning };
        struct Vanity {
            VState  state = VState::kOff;
            int     routineIdx = 0;
            float   rt = 0.0f;          // seconds into the routine
            float   elapsed = 0.0f;     // seconds the idle camera has run
            float   az0 = 0.0f;         // azimuth the routine starts from
            float   spiralHold = 0.0f;  // the spiral opening's stay on the face (see SpiralOpening)
            bool    xfade = false;
            Pose    xfadeFrom;
            float   xt = 0.0f;
            OrbitBlend xfadeOrbit;
            Pose    last;
            bool    haveLast = false;
        };
        Vanity s_v;

        double s_lastActivity = 0.0;      // Now() at the last real input / blocked moment
        bool   s_needInput    = false;    // the idle camera ended on its own: not again until the player does something
        std::atomic<bool> s_testRequested{ false };

        // Furniture.
        RE::ObjectRefHandle s_furnHandle;      // from the enter event (fallback for Actor::GetOccupiedFurniture)
        bool                s_furnPending = false;
        double              s_furnAt      = 0.0;
        std::string         s_furnKey;
        bool                s_furnActive  = false;  // the furniture camera runs (session kind = furniture)

        // Hand-over to Sandbox When Idle.
        double s_handoffLogUntil = 0.0;
        double s_handoffNextLog  = 0.0;

        std::vector<Routine> EnabledRoutines() {
            std::vector<Routine> r;
            if (s_cfg.dolly) r.push_back(Routine::kDolly);
            if (s_cfg.spiral) r.push_back(Routine::kSpiral);
            return r;
        }

        bool SwiLoaded() { return ::GetModuleHandleA("po3_SandboxWhenIdle.dll") != nullptr; }

        float* IdleTimer() {
            auto* cam = Cam();
            return cam ? &cam->GetRuntimeData2().idleTimer : nullptr;
        }

        float AutoVanityDelay() {
            if (auto* ini = RE::INISettingCollection::GetSingleton()) {
                if (auto* s = ini->GetSetting("fAutoVanityModeDelay:Camera")) return s->data.f;
            }
            return 120.0f;
        }

        // ---- blocking conditions ------------------------------------------------------------------------------------
        bool AnyMenuOpen() {
            if (FreeCamMenu::IsOverlayOpen()) return true;
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return true;
            if (ui->GameIsPaused()) return true;
            for (const char* name : { "Cursor Menu", "Console", "Dialogue Menu", "Loading Menu", "MainMenu", "RaceSex Menu", "Sleep/Wait Menu",
                                      "MessageBoxMenu", "Journal Menu" }) {
                if (ui->IsMenuOpen(name)) return true;
            }
            return false;
        }

        // nullptr = nothing in the way. a_session: our own free camera is already running (it is not "in the way").
        const char* BlockReason(bool a_session) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Is3DLoaded()) return "no player";
            if (!a_session && FreeCam::IsActive()) return "free camera is on";
            if (AnyMenuOpen()) return "a menu is open";
            if (SceneTracker::PlayerSceneActive()) return "player in a SexLab scene";
            if (CellEntryCam::IsRunning()) return "cell-entry camera running";
            if (player->IsInCombat()) return "in combat";
            if (player->IsDead()) return "dead";
            if (player->AsActorState()->IsSwimming() || player->AsActorState()->IsFlying()) return "swimming / flying";
            if (!a_session) {
                if (auto* cm = RE::ControlMap::GetSingleton(); cm && !cm->IsMovementControlsEnabled()) return "controls disabled";
                auto* cam = Cam();
                auto* st  = cam ? cam->currentState.get() : nullptr;
                if (!st) return "no camera state";
                using CS = RE::CameraStates;
                const auto id = st->id;
                if (id != CS::kFirstPerson && id != CS::kThirdPerson && id != CS::kFurniture && id != CS::kMount) return "special camera state";
            }
            return nullptr;
        }

        // ---- furniture identity -------------------------------------------------------------------------------------
        std::string FormKey(const RE::TESForm* form) {
            const auto* file = form->GetFile(0);
            const std::string name = file ? std::string(file->GetFilename()) : std::string("unknown");
            return Lower(std::format("{}|{:06x}", name, form->GetLocalFormID()));
        }

        RE::TESObjectREFR* OccupiedFurniture() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return nullptr;
            if (auto ref = player->GetOccupiedFurniture().get()) return ref.get();
            if (auto ref = s_furnHandle.get(); ref && player->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) return ref.get();
            return nullptr;
        }

        // ---- session helpers ------------------------------------------------------------------------------------------
        void ResetSession() {
            s_sess = Session{};
            s_ret  = Return{};
            s_v.state = VState::kOff;
            s_v.haveLast = false;
            s_v.xfade = false;
            s_furnActive = false;
        }

        PreShot ToPre(const Pose& p) {
            auto*       player = RE::PlayerCharacter::GetSingleton();
            const auto  a      = player->GetPosition();
            const float h      = player->GetAngleZ();
            const RE::NiPoint3 fwd{ std::sin(h), std::cos(h), 0.0f };
            const RE::NiPoint3 right{ std::cos(h), -std::sin(h), 0.0f };
            const RE::NiPoint3 v{ p.pos.x - a.x, p.pos.y - a.y, p.pos.z - a.z };
            PreShot s;
            s.x      = v.x * right.x + v.y * right.y;
            s.y      = v.x * fwd.x + v.y * fwd.y;
            s.z      = v.z;
            s.yawRel = WrapPi(p.yaw - h);
            s.pitch  = p.pitch;
            s.roll   = p.roll;
            s.fov    = p.fov;
            return s;
        }

        Pose FromPre(const PreShot& s) {
            auto*       player = RE::PlayerCharacter::GetSingleton();
            const auto  a      = player->GetPosition();
            const float h      = player->GetAngleZ();
            const RE::NiPoint3 fwd{ std::sin(h), std::cos(h), 0.0f };
            const RE::NiPoint3 right{ std::cos(h), -std::sin(h), 0.0f };
            Pose p;
            p.pos   = { a.x + right.x * s.x + fwd.x * s.y, a.y + right.y * s.x + fwd.y * s.y, a.z + s.z };
            p.yaw   = WrapPi(h + s.yawRel);
            p.pitch = s.pitch;
            p.roll  = s.roll;
            p.fov   = s.fov;
            return p;
        }

        // Last step of a session: give the free camera back, and (idle camera that ran its time) let Sandbox When Idle in.
        void FinishReturn(bool handOff) {
            SKSE::log::info("AmbientCam: giving the free camera back");
            SlccBridge::LeaveFreeFly("Ambient camera");
            ResetSession();
            s_lastActivity = Now();
            if (handOff) {
                s_needInput = true;
                if (s_cfg.handOffSandbox && SwiLoaded()) {
                    if (float* timer = IdleTimer()) {
                        const float delay = AutoVanityDelay();
                        const float before = *timer;
                        *timer = delay + 1.0f;
                        s_handoffLogUntil = Now() + 3.0;
                        s_handoffNextLog  = Now() + 0.5;
                        Event(std::format("idle camera done - idleTimer {:.1f} -> {:.1f} (fAutoVanityModeDelay {:.0f}) so Sandbox When Idle can start",
                            before, *timer, delay));
                    }
                } else {
                    Event(s_cfg.handOffSandbox ? "idle camera done - Sandbox When Idle is not installed" : "idle camera done");
                }
            }
        }

        void BeginReturn(float dur, const char* why, bool handOff = false) {
            if (!s_sess.owned || s_ret.active) return;
            s_furnPending = false;
            s_v.state     = VState::kOff;
            s_furnActive  = false;
            if (!Cam() || !Cam()->IsInFreeCameraMode()) {
                Event(std::format("{} - the free camera is already off", why));
                ResetSession();
                s_lastActivity = Now();
                return;
            }
            if (dur <= 0.0f || !s_sess.havePre || !LiveFreeCam()) {
                Event(std::format("{} - camera back now", why));
                FinishReturn(handOff);
                return;
            }
            s_ret.active  = true;
            s_ret.fromSet = false;
            s_ret.t       = 0.0f;
            s_ret.dur     = dur;
            s_ret.handOff = handOff;
            Event(std::format("{} - camera back over {:.1f} s", why, dur));
        }

        void StartVanity(const char* why) {
            auto routines = EnabledRoutines();
            if (routines.empty()) return;
            s_sess            = Session{};
            s_sess.owned      = true;
            s_sess.kind       = Kind::kVanity;
            s_sess.startedAt  = Now();
            s_v               = Vanity{};
            s_v.state         = VState::kStarting;
            s_subjSeeded      = false;
            Event(std::format("idle camera starting ({})", why));
            SlccBridge::RequestEnterFreeFly("Idle camera");
        }

        // ---- per frame ----------------------------------------------------------------------------------------------
        void DriveReturn(RE::FreeCameraState* fcs, float dt) {
            if (!s_ret.fromSet) {
                s_ret.from    = ReadPose(fcs);
                s_ret.fromSet = true;
            }
            s_ret.t += dt;
            const float w = s_ret.dur > 0.0f ? SmoothStep(s_ret.t / s_ret.dur) : 1.0f;
            WritePose(fcs, LerpPose(s_ret.from, FromPre(s_sess.pre), w));
            if (Subject now; ReadSubject(now)) CamCollision::Request(now.head, "idle camera, back to the normal camera");
            if (w >= 1.0f) {
                const bool handOff = s_ret.handOff;
                FinishReturn(handOff);
            }
        }

        void DriveVanity(RE::FreeCameraState* fcs, float dt) {
            auto routines = EnabledRoutines();
            if (routines.empty()) {
                BeginReturn(s_cfg.returnTime, "no idle camera routine is enabled");
                return;
            }
            UpdateSubject(dt);
            if (!s_subj.ok) return;

            // Fresh into the spiral: stay on the face until the crossfade has landed there, then a moment more.
            const float freshHold = s_cfg.crossfade + 0.5f * AreaSec();

            if (s_v.state == VState::kStarting) {
                s_v.state      = VState::kRunning;
                s_v.routineIdx = 0;
                s_v.rt         = 0.0f;
                s_v.elapsed    = 0.0f;
                s_v.az0        = s_subj.heading + (routines[0] == Routine::kDolly ? 0.5f : 0.0f);
                s_v.spiralHold = freshHold;
                s_v.xfade      = true;
                s_v.xfadeFrom  = ReadPose(fcs);
                s_v.xt         = 0.0f;
                s_v.xfadeOrbit = OrbitBlend{};
                Event(std::format("idle camera running - first routine {}", RoutineName(routines[0])));
            }

            s_v.elapsed += dt;
            s_v.rt += dt;
            if (s_cfg.vanityDuration > 0.0f && s_v.elapsed >= s_cfg.vanityDuration) {
                BeginReturn(s_cfg.returnTime, "idle camera time is up", true);
                return;
            }

            s_v.routineIdx %= static_cast<int>(routines.size());
            Routine routine = routines[static_cast<std::size_t>(s_v.routineIdx)];
            if (s_v.rt >= RoutineLength(routine, s_v.spiralHold)) {
                const Routine previous = routine;
                s_v.routineIdx = (s_v.routineIdx + 1) % static_cast<int>(routines.size());
                routine        = routines[static_cast<std::size_t>(s_v.routineIdx)];
                s_v.rt         = 0.0f;
                s_v.az0        = s_subj.heading + (routine == Routine::kDolly ? 0.5f : 0.0f);
                s_v.spiralHold = previous == Routine::kSpiral ? 0.0f : freshHold;
                if (s_v.haveLast) {
                    s_v.xfade      = true;
                    s_v.xfadeFrom  = s_v.last;
                    s_v.xt         = 0.0f;
                    s_v.xfadeOrbit = OrbitBlend{};
                }
                Event(std::format("idle camera routine: {}", RoutineName(routine)));
            }

            Pose p = routine == Routine::kDolly ? DollyPose(s_v.rt, s_v.az0) : SpiralPose(s_v.rt, s_v.az0, s_v.spiralHold);
            if (s_v.xfade) {
                s_v.xt += dt;
                const float w = s_cfg.crossfade > 0.0f ? SmoothStep(s_v.xt / s_cfg.crossfade) : 1.0f;
                p             = OrbitLerp(s_v.xfadeFrom, p, w, s_v.xfadeOrbit);
                if (w >= 1.0f) s_v.xfade = false;
            }
            s_v.last     = p;
            s_v.haveLast = true;
            WritePose(fcs, p);
            CamCollision::Request(SightPoint(p.pos, p.yaw, p.pitch), "idle camera");
        }

        // ---- furniture start ------------------------------------------------------------------------------------------
        void TryStartFurniture() {
            if (!s_cfg.furnEnabled) return;
            std::string key, label;
            if (!CurrentFurniture(key, label) || key != s_furnKey) {
                Event(std::format("furniture camera for {}: no longer on it", s_furnKey));
                return;
            }
            if (!SceneCam::HasFurnitureShot()) {
                Event(std::format("furniture camera for {}: no saved shot", s_furnKey));
                return;
            }
            // Not the "controls disabled" / camera-state checks of the idle camera: sandboxing (AI-driven) players sit too.
            const char* block = (!s_sess.owned && FreeCam::IsActive()) ? "your own free camera is on" : BlockReason(true);
            if (block) {
                Event(std::format("furniture camera for {}: held back ({})", key, block));
                return;
            }
            if (!s_sess.owned) {
                s_sess           = Session{};
                s_sess.owned     = true;
                s_sess.startedAt = Now();
            }
            s_sess.kind  = Kind::kFurniture;
            s_v.state    = VState::kOff;
            s_furnActive = true;
            Event(std::format("furniture camera: {} - going to its saved shot", label));
            if (!SceneCam::RecallFurnitureShot("furniture camera")) {
                Event("furniture camera: the shot could not be applied");
                BeginReturn(0.0f, "furniture shot refused");
            }
        }

        // ---- tick (main thread, ~4 per second) ------------------------------------------------------------------------
        std::atomic<bool> s_tickQueued{ false };

        void PublishStatus(const char* block) {
            MenuStatus st{};
            const double now = Now();
            if (s_sess.owned && s_sess.kind == Kind::kFurniture) {
                CopyStr(st.state, sizeof(st.state), s_ret.active ? "Furniture camera - returning" : "Furniture camera running");
            } else if (s_sess.owned && s_sess.kind == Kind::kVanity) {
                CopyStr(st.state, sizeof(st.state), s_ret.active ? "Idle camera - returning"
                                                    : s_v.state == VState::kStarting ? "Idle camera - starting"
                                                    : std::format("Idle camera running ({:.0f} s)", s_v.elapsed));
            } else if (!s_cfg.vanityEnabled) {
                CopyStr(st.state, sizeof(st.state), "Idle camera is off");
            } else if (s_needInput) {
                CopyStr(st.state, sizeof(st.state), "Idle camera done - waiting for you to press something");
            } else if (block) {
                CopyStr(st.state, sizeof(st.state), std::format("Waiting - {}", block));
            } else {
                const double left = s_cfg.vanityStart - (now - s_lastActivity);
                CopyStr(st.state, sizeof(st.state), left > 0 ? std::format("Idle for {:.0f} s - starts in {:.0f} s", now - s_lastActivity, left)
                                                              : std::string("Starting"));
            }
            if (s_sess.owned && s_sess.kind == Kind::kVanity) {
                const auto r = EnabledRoutines();
                if (!r.empty()) CopyStr(st.routine, sizeof(st.routine), RoutineName(r[static_cast<std::size_t>(s_v.routineIdx) % r.size()]));
            }
            std::string key, label;
            if (CurrentFurniture(key, label)) {
                CopyStr(st.furniture, sizeof(st.furniture), label);
                st.furnHasShot = SceneCam::HasFurnitureShot();
            }
            st.swiLoaded = SwiLoaded();
            if (float* timer = IdleTimer()) {
                CopyStr(st.timer, sizeof(st.timer), std::format("{:.1f} (auto-vanity delay {:.0f})", *timer, AutoVanityDelay()));
            }
            CopyStr(st.lastEvent, sizeof(st.lastEvent), s_lastEvent);
            std::lock_guard lock(s_statusLock);
            s_status = st;
        }

        void Tick() {
            s_tickQueued = false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            const double now = Now();
            if (s_lastActivity <= 0.0) s_lastActivity = now;
            if (!player || !player->Is3DLoaded()) {
                s_lastActivity = now;
                return;
            }

            // Diagnostics: the engine's idle timer right after the hand-over.
            if (s_handoffLogUntil > now && now >= s_handoffNextLog) {
                s_handoffNextLog = now + 0.5;
                if (float* timer = IdleTimer()) SKSE::log::info("AmbientCam: hand-over, idleTimer now {:.2f}", *timer);
            }

            // Our own session.
            if (s_sess.owned) {
                const bool free = Cam() && Cam()->IsInFreeCameraMode();
                if (!free && now - s_sess.startedAt > 2.0) {
                    Event("the free camera never started");
                    ResetSession();
                    s_lastActivity = now;
                } else if (!s_ret.active) {
                    const char* block = BlockReason(true);
                    // Furniture: only a scene / combat / menu ends it. The idle camera also stops at a menu or combat.
                    if (block && free) BeginReturn(0.0f, block);
                }
            }

            // A furniture camera waiting for its delay.
            if (s_furnPending && now >= s_furnAt) {
                s_furnPending = false;
                TryStartFurniture();
            }

            // Idle camera start.
            const char* block = BlockReason(s_sess.owned);
            if (!s_sess.owned) {
                if (block) s_lastActivity = now;
                const bool test = s_testRequested.load();
                if (test && !block && !FreeCamMenu::IsOverlayOpen()) {
                    s_testRequested = false;
                    StartVanity("test");
                } else if (s_cfg.vanityEnabled && !block && !s_needInput && !s_furnPending &&
                           now - s_lastActivity >= s_cfg.vanityStart) {
                    // The engine's idle countdown, for the log (does it count up or down? see the hand-over line).
                    if (float* timer = IdleTimer()) SKSE::log::info("AmbientCam: idle {:.0f} s, engine idleTimer {:.1f}", now - s_lastActivity, *timer);
                    StartVanity("idle");
                }
            }
            PublishStatus(block);
        }

        void QueueTick() {
            if (s_tickQueued.exchange(true)) return;
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([] { Tick(); });
            } else {
                s_tickQueued = false;
            }
        }

        // ---- furniture events ------------------------------------------------------------------------------------------
        void OnFurnitureEvent(bool enter, RE::ObjectRefHandle handle) {
            if (enter) {
                s_furnHandle = handle;
                if (!s_cfg.furnEnabled) return;
                auto ref = handle.get();
                if (!ref || !ref->GetBaseObject()) return;
                const std::string key = "furn:" + FormKey(ref->GetBaseObject());
                if (!SceneShots::Has(key)) return;
                s_furnKey     = key;
                s_furnPending = true;
                s_furnAt      = Now() + (std::max)(s_cfg.furnDelay, 0.0f);
                Event(std::format("sat on {} - a saved shot exists, camera in {:.1f} s", key, s_cfg.furnDelay));
                QueueTick();
            } else {
                s_furnHandle  = {};
                s_furnPending = false;
                if (s_sess.owned && s_sess.kind == Kind::kFurniture) BeginReturn(s_cfg.returnTime, "the player left the furniture");
            }
        }

        class FurnSink final : public RE::BSTEventSink<RE::TESFurnitureEvent> {
        public:
            static FurnSink* Get() {
                static FurnSink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESFurnitureEvent* evt, RE::BSTEventSource<RE::TESFurnitureEvent>*) override {
                if (!evt || !evt->actor || !evt->actor->IsPlayerRef() || !evt->targetFurniture) return RE::BSEventNotifyControl::kContinue;
                const bool enter = evt->type.get() == RE::TESFurnitureEvent::FurnitureEventType::kEnter;
                const auto handle = evt->targetFurniture->GetHandle();
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([enter, handle] { OnFurnitureEvent(enter, handle); });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        bool IsTFCamKey(const RE::ButtonEvent* btn) {
            if (btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) return false;
            const auto  code = btn->GetIDCode();
            const auto& fs   = FreeCam::GetSettings();
            return code == fs.rollCCWKey || code == fs.rollCWKey || code == fs.resetKey || code == fs.freezeTimeKey ||
                   code == fs.screenshotKey || code == fs.slowKey;
        }

        bool IsMoveEvent(const RE::ButtonEvent* btn) {
            const char* ue = btn->QUserEvent().c_str();
            if (!ue) return false;
            const std::string_view u(ue);
            return u == "Forward" || u == "Back" || u == "Strafe Left" || u == "Strafe Right" || u == "Move" || u == "Activate" ||
                   u == "Jump" || u == "Sneak" || u == "Sprint";
        }
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Public
    // ----------------------------------------------------------------------------------------------------------------

    Settings& Get() { return s_cfg; }

    void Clamp() {
        auto range = [](float& v, float lo, float hi, float def) { v = std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
        range(s_cfg.vanityStart, 5.0f, 600.0f, 25.0f);
        range(s_cfg.vanityDuration, 0.0f, 1800.0f, 75.0f);
        range(s_cfg.crossfade, 0.0f, 5.0f, 1.5f);
        range(s_cfg.returnTime, 0.0f, 3.0f, 0.7f);
        range(s_cfg.interiorScale, 0.3f, 1.0f, 0.6f);
        range(s_cfg.dollyFraming, 40.0f, 400.0f, 110.0f);
        range(s_cfg.dollyNarrowFOV, 10.0f, 60.0f, 28.0f);
        range(s_cfg.dollyWideFOV, 50.0f, 130.0f, 85.0f);
        range(s_cfg.dollyPeriod, 2.0f, 30.0f, 9.0f);
        s_cfg.dollyCycles = std::clamp(s_cfg.dollyCycles, 1, 10);
        range(s_cfg.dollyCamHeight, 0.0f, 200.0f, 25.0f);
        range(s_cfg.dollySpin, -90.0f, 90.0f, 10.0f);
        range(s_cfg.spiralFar, 60.0f, 500.0f, 170.0f);
        range(s_cfg.spiralClose, 25.0f, 200.0f, 55.0f);
        range(s_cfg.spiralFOV, 30.0f, 110.0f, 65.0f);
        range(s_cfg.spiralAreaSec, 0.8f, 10.0f, 3.2f);
        range(s_cfg.spiralSpeed, -180.0f, 180.0f, 40.0f);
        range(s_cfg.furnDelay, 0.0f, 10.0f, 1.2f);
    }

    void Install() {
        Clamp();
        s_lastActivity = Now();
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESFurnitureEvent>(FurnSink::Get());
        }
        // A plain thread paces the checks (a task that re-queues itself would spin inside one frame).
        std::thread([] {
            while (true) {
                ::Sleep(250);
                QueueTick();
            }
        }).detach();
        SKSE::log::info("AmbientCam: ready - idle camera {} (starts after {:.0f} s, runs {:.0f} s), furniture camera {}; Sandbox When Idle {}",
            s_cfg.vanityEnabled ? "on" : "off", s_cfg.vanityStart, s_cfg.vanityDuration, s_cfg.furnEnabled ? "on" : "off",
            SwiLoaded() ? "installed" : "not installed");
    }

    void Drive(RE::FreeCameraState* fcs, bool driving) {
        const float dt = FrameDt();
        if (!fcs) return;
        if (!s_sess.owned) return;
        if (!driving) {
            Event("another camera took the free camera");
            ResetSession();
            s_lastActivity = Now();
            return;
        }
        // The camera from before the session = where the free camera started (the first frame it updates).
        if (!s_sess.havePre) {
            s_sess.pre     = ToPre(ReadPose(fcs));
            s_sess.havePre = true;
        }
        if (s_ret.active) {
            DriveReturn(fcs, dt);
        } else if (s_sess.kind == Kind::kVanity && s_v.state != VState::kOff) {
            DriveVanity(fcs, dt);
        }
        // Furniture: SceneCam's blend moved the camera; from there on it is free-fly.
    }

    bool OwnsMotion() {
        return s_sess.owned && (s_ret.active || (s_sess.kind == Kind::kVanity && s_v.state != VState::kOff));
    }

    void OnInput(RE::InputEvent* const* events) {
        if (!events) return;
        bool activity = false;
        bool move     = false;
        for (auto* evt = *events; evt; evt = evt->next) {
            switch (evt->GetEventType()) {
            case RE::INPUT_EVENT_TYPE::kMouseMove: {
                auto* mm = static_cast<RE::MouseMoveEvent*>(evt);
                if (std::abs(mm->mouseInputX) + std::abs(mm->mouseInputY) >= 3) activity = true;
                break;
            }
            case RE::INPUT_EVENT_TYPE::kThumbstick: {
                auto* ts = static_cast<RE::ThumbstickEvent*>(evt);
                if (std::fabs(ts->xValue) + std::fabs(ts->yValue) > 0.25f) {
                    activity = true;
                    if (ts->GetIDCode() == RE::ThumbstickEvent::InputType::kLeftThumbstick) move = true;
                }
                break;
            }
            case RE::INPUT_EVENT_TYPE::kButton: {
                auto* btn = evt->AsButtonEvent();
                if (!btn || !btn->IsDown()) break;
                activity = true;
                if (!IsTFCamKey(btn) && IsMoveEvent(btn)) move = true;
                break;
            }
            default:
                break;
            }
        }
        if (!activity) return;
        s_lastActivity = Now();
        s_needInput    = false;
        if (s_sess.owned && s_sess.kind == Kind::kVanity && !s_ret.active) {
            BeginReturn(s_cfg.returnTime, "input");
        } else if (move && s_sess.owned && s_sess.kind == Kind::kFurniture && s_cfg.furnMoveCancels && !s_ret.active) {
            BeginReturn(s_cfg.returnTime, "movement key");
        }
    }

    void OnFreeCamEnd() {
        if (s_sess.owned) {
            Event("the free camera ended - session closed");
            ResetSession();
            s_lastActivity = Now();
        }
    }

    void OnGameLoad(const char* why) {
        if (s_sess.owned) Event(std::format("session dropped ({})", why));
        ResetSession();
        s_furnPending  = false;
        s_furnHandle   = {};
        s_needInput    = false;
        s_lastActivity = Now();
    }

    void OnPlayerSceneStart() {
        if (s_sess.owned) BeginReturn(0.0f, "a SexLab scene started");
        s_furnPending = false;
    }

    void TestVanityOnMenuClose() { s_testRequested = true; }

    bool CurrentFurniture(std::string& key, std::string& label) {
        auto* ref = OccupiedFurniture();
        if (!ref) return false;
        auto* base = ref->GetBaseObject();
        if (!base) return false;
        key = "furn:" + FormKey(base);
        const char* name = base->GetName();
        label = std::format("{} [{}]", name && *name ? name : "furniture", key.substr(5));
        return true;
    }

    MenuStatus GetMenuStatus() {
        std::lock_guard lock(s_statusLock);
        return s_status;
    }
}
