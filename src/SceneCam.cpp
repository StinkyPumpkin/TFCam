#include "SceneCam.h"
#include "CamCollision.h"
#include "FcfwBridge.h"
#include "SceneShots.h"
#include "SceneTracker.h"
#include "FreeCamController.h"
#include "FreeCamMenu.h"
#include "SlccBridge.h"
#include "AmbientCam.h"
#include "CamFocus.h"
#include "CamPath.h"
#include "ConvoCam.h"

#include <RE/I/INIPrefSettingCollection.h>
#include <RE/I/INISettingCollection.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SceneCam {

    namespace {
        using TFCAM_API::SceneCamMode;

        constexpr float  kPi             = 3.14159265358979f;
        constexpr float  kDegToRad       = kPi / 180.0f;
        constexpr float  kRadToDeg       = 180.0f / kPi;
        constexpr float  kLn2            = 0.69314718f;
        constexpr float  kLookLimit      = 85.0f * kDegToRad;  // eye view: mouse look either way
        constexpr float  kPitchLimit     = 1.55f;              // ~89 degrees, no flip
        constexpr float  kOrbitEntrySec  = 1.0f;               // orbit start: elevation / distance / FOV blend in
        constexpr float  kTargetHalfLife = 0.25f;              // orbit target smoothing
        constexpr float  kChaseHalfLife  = 0.2f;               // live setting changes (wheel, SLUI steppers)
        constexpr double kPendingTimeout = 1.2;                // a request waits this long for TFCam free-fly
        constexpr double kAutoWindow     = 5.0;                // a stage shot is applied if the free cam arrives within this;
                                                               // 0.13.0: an automatic free cam starts within this once clear
        constexpr double kSaveDebounce   = 0.75;               // ini save after the last wheel notch / SLUI change
        constexpr float  kOrbitElMin     = -80.0f * kDegToRad; // 0.15.0 mouse tilt: looking up from below ...
        constexpr float  kOrbitElMax     = 85.0f * kDegToRad;  // ... to a top view (the vanilla pitch clamp stays clear)
        constexpr float  kFloorMargin    = 12.0f;              // 0.15.0: orbit / preset camera at least this above the floor
        constexpr float  kPresetHalfLife = 0.3f;               // 0.15.0: preset view chase (a new view, a new stage)
        constexpr float  kOrbitTiltSens  = 0.0007f;            // 0.15.0: orbit tilt, radians per raw mouse unit

        Config s_cfg;

        // ---- math ------------------------------------------------------------------------------------------------
        float WrapPi(float a_angle) { return std::remainder(a_angle, 2.0f * kPi); }

        float SmoothStep(float a_x) {
            a_x = std::clamp(a_x, 0.0f, 1.0f);
            return a_x * a_x * (3.0f - 2.0f * a_x);
        }

        // Frame-rate independent exponential follow.
        float ExpWeight(float a_dt, float a_halfLife) {
            return a_dt <= 0.0f ? 0.0f : 1.0f - std::exp(-kLn2 * a_dt / a_halfLife);
        }

        float Dot(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b) { return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z; }

        RE::NiPoint3 Cross(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b) {
            return { a_a.y * a_b.z - a_a.z * a_b.y, a_a.z * a_b.x - a_a.x * a_b.z, a_a.x * a_b.y - a_a.y * a_b.x };
        }

        float Length(const RE::NiPoint3& a_v) { return std::sqrt(Dot(a_v, a_v)); }

        bool Normalize(RE::NiPoint3& a_v) {
            const float len = Length(a_v);
            if (!std::isfinite(len) || len < 1e-5f) return false;
            a_v.x /= len;
            a_v.y /= len;
            a_v.z /= len;
            return true;
        }

        RE::NiPoint3 Mix(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, float a_w) {
            return { a_a.x + (a_b.x - a_a.x) * a_w, a_a.y + (a_b.y - a_a.y) * a_w, a_a.z + (a_b.z - a_a.z) * a_w };
        }

        // TFCam's camera convention: forward = (sin yaw * cos pitch, cos yaw * cos pitch, -sin pitch), so a positive
        // pitch looks down. Inverse: the yaw / pitch that look along a_dir.
        void LookAngles(const RE::NiPoint3& a_dir, float& a_yaw, float& a_pitch) {
            a_yaw   = std::atan2(a_dir.x, a_dir.y);
            a_pitch = std::atan2(-a_dir.z, std::hypot(a_dir.x, a_dir.y));
        }

        std::string Lower(std::string a_s) {
            for (auto& c : a_s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return a_s;
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

        // Own frame time for the drive (TFCam's s_frameDt only ticks in the input sink): capped at 0.1 s, 0 while the
        // game is paused so nothing moves or catches up behind a pausing menu.
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

        // ---- camera ----------------------------------------------------------------------------------------------
        RE::PlayerCamera* Cam() { return RE::PlayerCamera::GetSingleton(); }

        float GetFOV() {
            auto* cam = Cam();
            return cam ? cam->GetRuntimeData2().worldFOV : 0.0f;
        }

        void SetFOV(float a_fov) {
            auto* cam = Cam();
            if (cam && a_fov > 0.0f && std::isfinite(a_fov)) cam->GetRuntimeData2().worldFOV = a_fov;
        }

        // The live free camera state, or null when the free camera is off.
        RE::FreeCameraState* CurrentFreeCam() {
            auto* cam = Cam();
            if (!cam || !cam->IsInFreeCameraMode()) return nullptr;
            auto* state = cam->currentState.get();
            return state ? static_cast<RE::FreeCameraState*>(state) : nullptr;
        }

        struct Pose {
            RE::NiPoint3 pos{};
            float        pitch = 0.0f;
            float        yaw   = 0.0f;
            float        roll  = 0.0f;
            float        fov   = 0.0f;
        };

        Pose ReadPose(const RE::FreeCameraState* a_fcs) {
            Pose p;
            p.pos   = a_fcs->translation;
            p.pitch = a_fcs->rotation.x;
            p.yaw   = a_fcs->rotation.y;
            p.roll  = FreeCam::GetRollRadians();
            p.fov   = GetFOV();
            return p;
        }

        void WritePose(RE::FreeCameraState* a_fcs, const Pose& a_p, bool a_roll) {
            a_fcs->translation = a_p.pos;
            a_fcs->rotation.x  = a_p.pitch;
            a_fcs->rotation.y  = WrapPi(a_p.yaw);
            if (a_roll) FreeCam::SetRollRadians(a_p.roll);
            SetFOV(a_p.fov);
        }

        Pose LerpPose(const Pose& a_a, const Pose& a_b, float a_w) {
            Pose p;
            p.pos   = Mix(a_a.pos, a_b.pos, a_w);
            p.yaw   = a_a.yaw + WrapPi(a_b.yaw - a_a.yaw) * a_w;
            p.pitch = a_a.pitch + (a_b.pitch - a_a.pitch) * a_w;
            p.roll  = a_a.roll + WrapPi(a_b.roll - a_a.roll) * a_w;
            p.fov   = a_a.fov + (a_b.fov - a_a.fov) * a_w;
            return p;
        }

        // ---- skeleton ----------------------------------------------------------------------------------------------
        // Built on first use (main thread, in game): BSFixedString needs the game's string cache.
        struct Names {
            RE::BSFixedString pelvis{ "NPC Pelvis [Pelv]" };
            RE::BSFixedString spine2{ "NPC Spine2 [Spn2]" };
            RE::BSFixedString spine1{ "NPC Spine1 [Spn1]" };
            RE::BSFixedString head{ "NPC Head [Head]" };
            RE::BSFixedString neck{ "NPC Neck [Neck]" };
            RE::BSFixedString faceGen{ "BSFaceGenNiNodeSkinned" };
            // Left / right eye node pairs, then single eye-centre bones (SLCC's lookup order, slcc map fact 8).
            RE::BSFixedString eyePairs[5][2] = {
                { "NPC L Eye [LEye]", "NPC R Eye [REye]" },
                { "NPC LEye", "NPC REye" },
                { "NPC L Eye", "NPC R Eye" },
                { "L Eye", "R Eye" },
                { "LeftEye", "RightEye" },
            };
            RE::BSFixedString eyeCentre[4] = { "NPC EyeBone [Eye]", "NPCEyeBone", "EyeBone", "EyeCenter" };
        };

        const Names& N() {
            static const Names names;
            return names;
        }

        RE::NiAVObject* Node(RE::NiAVObject* a_root, const RE::BSFixedString& a_name) {
            return a_root ? a_root->GetObjectByName(a_name) : nullptr;
        }

        // Midpoint of pelvis and spine2 (else spine1, else the reference + 60 up).
        RE::NiPoint3 TorsoPoint(RE::Actor* a_actor) {
            if (auto* root = a_actor->Get3D(false)) {
                auto* pelvis = Node(root, N().pelvis);
                auto* spine2 = Node(root, N().spine2);
                if (pelvis && spine2) return Mix(pelvis->world.translate, spine2->world.translate, 0.5f);
                if (auto* spine1 = Node(root, N().spine1)) return spine1->world.translate;
            }
            auto pos = a_actor->GetPosition();
            pos.z += 60.0f;
            return pos;
        }

        using ActorPtr = RE::NiPointer<RE::Actor>;

        // The player's scene actors in position order (partners only: without the player).
        std::vector<ActorPtr> SceneActors(bool a_partnersOnly) {
            std::vector<ActorPtr> out;
            const auto&           scene = SceneTracker::GetPlayerScene();
            if (!scene.inScene) return out;
            auto* player = RE::PlayerCharacter::GetSingleton();
            for (const auto& handle : scene.actors) {
                auto actor = handle.get();
                if (!actor) continue;
                if (a_partnersOnly && actor.get() == player) continue;
                out.push_back(std::move(actor));
            }
            return out;
        }

        // ---- state (main thread) -----------------------------------------------------------------------------------
        SceneCamMode s_mode = TFCAM_API::kModeFree;  // Free / Orbit / Eye; reported as None while TFCam does not drive

        // Free-mode blend to a fixed pose (saved shot, back from eye view).
        struct Blend {
            bool  active = false;
            Pose  from;
            Pose  to;
            float t   = 0.0f;
            float dur = 0.0f;
        };
        Blend s_blend;

        struct OrbitState {
            RE::NiPoint3 target{};   // smoothed torso centre
            float        az     = 0.0f;  // radians, direction from the target to the camera (0 = +Y)
            float        el     = 0.0f;  // radians above the target
            float        dist   = 0.0f;
            float        fov    = 0.0f;
            float        el0    = 0.0f;  // at entry
            float        dist0  = 0.0f;
            float        fov0   = 0.0f;
            float        yaw0   = 0.0f;
            float        pitch0 = 0.0f;
            float        t      = 0.0f;  // seconds since entry
            float        breath = 0.0f;  // elevation breathing phase, degrees
            float        entryFOV = 0.0f;  // restored when the orbit ends
            float        aim    = 0.0f;  // 0.13.0: look-at height offset, chased toward the setting
            float        tilt   = 0.0f;  // 0.15.0: the mouse's up / down on top of the tilt setting, radians
            float        roll   = 0.0f;  // 0.17.0: the orbit's own roll (FreeCam extra roll), radians, chased
            float        rollPhase = 0.0f;  // 0.17.0: degrees turned so far at orbitRollSpeed
        };
        OrbitState s_orbit;

        // 0.15.0 preset view: fixed views round the player's scene. The camera chases the chosen view in orbit terms
        // (azimuth / elevation / distance round the smoothed scene centre), so a new view swings round the actors and
        // never through them. aimW blends the look: 0 = at the centre (the built-in views), 1 = fixed angles (the
        // stage's saved shot, and the camera's own angles at entry, turned towards the centre from there).
        struct PresetState {
            int          view     = 1;  // 0 = the stage's saved shot, 1..kPresetCount = kPresets[view - 1]
            RE::NiPoint3 target{};      // smoothed torso centre
            float        az       = 0.0f;
            float        el       = 0.0f;
            float        dist     = 0.0f;
            float        fixYaw   = 0.0f;
            float        fixPitch = 0.0f;
            float        aimW     = 1.0f;
            float        roll     = 0.0f;
            float        fov      = 0.0f;
            float        entryFOV = 0.0f;  // restored when preset view ends
        };
        PresetState s_preset;
        bool        s_placing = false;  // 0.15.0 "set cam pos": the middle mouse button saves the stage's shot
        float       s_mouseDY = 0.0f;   // 0.15.0: raw mouse Y since the last orbit frame (OnMouseMove)

        // Azimuth round from the player's front (+ = her right), elevation above the centre, distance, FOV.
        struct PresetDef {
            const char* name;
            float       az;
            float       el;
            float       dist;
            float       fov;
        };
        constexpr PresetDef kPresets[] = {
            { "Front", 0.0f, 15.0f, 140.0f, 70.0f },
            { "Front right", 45.0f, 20.0f, 130.0f, 70.0f },
            { "Right", 90.0f, 15.0f, 120.0f, 70.0f },
            { "Back right", 135.0f, 25.0f, 130.0f, 70.0f },
            { "Back", 180.0f, 15.0f, 140.0f, 70.0f },
            { "Back left", 225.0f, 25.0f, 130.0f, 70.0f },
            { "Left", 270.0f, 15.0f, 120.0f, 70.0f },
            { "Front left", 315.0f, 20.0f, 130.0f, 70.0f },
            { "Top", 0.0f, 75.0f, 170.0f, 70.0f },
            { "Low", 30.0f, -10.0f, 110.0f, 70.0f },
        };
        constexpr int kPresetCount = static_cast<int>(std::size(kPresets));

        const char* PresetName(int a_view) {
            if (a_view == 0) return "Saved shot";
            return a_view >= 1 && a_view <= kPresetCount ? kPresets[a_view - 1].name : "";
        }

        struct EyeState {
            RE::ActorHandle target;
            RE::FormID      targetID  = 0;
            int             preferred = 0;  // partner index (NextEyeTarget)
            Pose            pre;            // the camera before eye view (blended back to on exit)
            Pose            last;
            bool            haveLast  = false;
            float           yawOff    = 0.0f;  // the user's mouse look on top of the head
            float           pitchOff  = 0.0f;
            double          nullSince = -1.0;
            bool            blending  = false;
            Pose            blendFrom;
            float           blendT    = 0.0f;
            // Hidden head (FaceGen node) - the exact previous AppCulled bit is put back.
            RE::NiPointer<RE::NiAVObject> culled;
            bool                          culledPrev = false;
            // Near clip.
            bool  nearApplied  = false;
            bool  haveIni      = false;
            float savedIni     = 0.0f;
            bool  haveFrustum  = false;
            float savedFrustum = 0.0f;
            float written      = 0.0f;
        };
        EyeState s_eye;

        enum class Req { kNone, kOrbit, kEye, kRecall, kFurniture, kAutoShot, kPreset, kPlace };
        Req    s_pending      = Req::kNone;  // waits for TFCam free-fly (entered on the request)
        double s_pendingUntil = 0.0;

        const char* Name(Req a_req) {
            switch (a_req) {
            case Req::kOrbit:  return "orbit";
            case Req::kEye:    return "eye view";
            case Req::kRecall: return "go to shot";
            case Req::kFurniture: return "furniture shot";
            case Req::kAutoShot: return "automatic shot";
            case Req::kPreset: return "preset view";
            case Req::kPlace:  return "set cam pos";
            default:           return "none";
            }
        }

        struct ActivePose {
            RE::ActorHandle actor;
            RE::FormID      id = 0;
            std::string     event;  // lower case
            std::string     key;    // "pose:<event>"
        };
        ActivePose s_pose;

        std::string s_autoKey;  // stage key whose saved shot is applied once TFCam drives in Free mode
        double      s_autoUntil = 0.0;
        double      s_saveAt    = -1.0;  // debounced TFCam.ini [SceneCam] save
        std::string s_lastEvent;

        // 0.13.0 (autoEnterShots): a committed stage key / an NPC pose with a saved shot turns TFCam free-fly on.
        struct AutoEnter {
            std::string key;              // stage / pose key, "" = nothing armed
            bool        pose   = false;
            double      until  = 0.0;     // dropped after this; pushed on while a menu is in the way
            bool        waited = false;   // the wait is logged once
        };
        AutoEnter   s_autoEnter;
        std::string s_pendingShotKey;        // Req::kAutoShot: the shot to blend to once TFCam drives
        bool        s_autoEnterOff = false;  // the user left the free camera in this player scene: no more entries
        bool        s_autoOwnsFree = false;  // TFCam turned this free camera on for an NPC pose: given back at its end
        bool        s_expectOwnEnd = false;  // the next free-cam end is TFCam's own (pose camera / AmbientCam hand-back)

        std::mutex                 s_statusLock;
        TFCAM_API::SceneCamStatus  s_status{};
        TFCAM_API::SceneCamStatus3 s_status3{};  // 0.13.0, API v3
        TFCAM_API::SceneCamPreset4 s_status4{};  // 0.15.0, API v4
        int                       s_statusSceneShots = 0;
        std::string               s_statusEvent;

        std::atomic<bool> s_tickQueued{ false };
        bool              s_inTick = false;

        // Set by OnFreeCamEnd, cleared by the next Drive (the Update hook runs only on a live free camera). The End's
        // inline path calls OnFreeCamEnd before the vanilla End, while IsInFreeCameraMode() still says yes, and outside a
        // scene / pose nothing republishes afterwards: without this the status kept reporting Free with no camera.
        bool s_freeCamEnded = false;

        void Event(std::string a_text) {
            SKSE::log::info("SceneCam: {}", a_text);
            s_lastEvent = std::move(a_text);
        }

        void Notify(const char* a_text) { RE::SendHUDMessage::ShowHUDMessage(a_text); }

        // A refused request: logged, and shown when the user caused it.
        void Refuse(const char* a_hud, std::string_view a_why) {
            Event(std::format("refused - {}", a_why));
            if (a_hud) Notify(a_hud);
        }

        // ---- saved shots ---------------------------------------------------------------------------------------------
        struct ShotContext {
            bool         ok    = false;
            bool         stage = false;  // stage shot (else pose / furniture shot)
            const char*  what  = "pose";  // "stage" / "pose" / "furniture", for messages
            std::string  key;
            std::string  label;
            RE::NiPoint3 anchor{};
            float        heading = 0.0f;
        };

        std::string StageKey(const SceneTracker::PlayerScene& a_scene) {
            return std::format("stage:{}|{}|{}", a_scene.sceneId, a_scene.stageId, a_scene.playerSlot);
        }

        // Anchor = the player's reference: P+ places every actor at the scene base + its stage offset, so the player's
        // ref is a rigid transform of the base and a shot relative to it frames the same wherever the scene plays.
        ShotContext StageContext() {
            ShotContext c;
            const auto& scene  = SceneTracker::GetPlayerScene();
            auto*       player = RE::PlayerCharacter::GetSingleton();
            if (!scene.inScene || !scene.stageKnown || !player) return c;
            c.ok      = true;
            c.stage   = true;
            c.what    = "stage";
            c.key     = StageKey(scene);
            c.label   = scene.sceneName.empty() ? scene.sceneId : scene.sceneName;
            c.anchor  = player->GetPosition();
            c.heading = player->GetAngleZ();
            return c;
        }

        ShotContext PoseContext() {
            ShotContext c;
            if (!s_pose.id) return c;
            auto actor = s_pose.actor.get();
            if (!actor) return c;
            c.ok      = true;
            c.key     = s_pose.key;
            c.label   = s_pose.event;
            c.anchor  = actor->GetPosition();
            c.heading = actor->GetAngleZ();
            return c;
        }

        // 0.11.0: the furniture the player sits on. Anchored on the player, whose reference sits at the furniture's marker
        // (so one shot per furniture base form fits every marker and every placement).
        ShotContext FurnitureContext() {
            ShotContext c;
            auto*       player = RE::PlayerCharacter::GetSingleton();
            std::string key, label;
            if (!player || !AmbientCam::CurrentFurniture(key, label)) return c;
            c.ok      = true;
            c.what    = "furniture";
            c.key     = key;
            c.label   = label;
            c.anchor  = player->GetPosition();
            c.heading = player->GetAngleZ();
            return c;
        }

        // In the player's scene with a known stage: the stage shot; otherwise the active pose's shot; otherwise the
        // furniture the player sits on.
        ShotContext CurrentContext() {
            auto c = StageContext();
            if (c.ok) return c;
            c = PoseContext();
            return c.ok ? c : FurnitureContext();
        }

        // Anchor frame: forward = (sin h, cos h, 0), right = (cos h, -sin h, 0), up = +Z.
        SceneShots::Shot PoseToShot(const Pose& a_p, const ShotContext& a_c) {
            const RE::NiPoint3 fwd{ std::sin(a_c.heading), std::cos(a_c.heading), 0.0f };
            const RE::NiPoint3 right{ std::cos(a_c.heading), -std::sin(a_c.heading), 0.0f };
            const RE::NiPoint3 v{ a_p.pos.x - a_c.anchor.x, a_p.pos.y - a_c.anchor.y, a_p.pos.z - a_c.anchor.z };
            SceneShots::Shot   s;
            s.x      = Dot(v, right);
            s.y      = Dot(v, fwd);
            s.z      = v.z;
            s.yawRel = WrapPi(a_p.yaw - a_c.heading) * kRadToDeg;
            s.pitch  = a_p.pitch * kRadToDeg;
            s.roll   = WrapPi(a_p.roll) * kRadToDeg;
            s.fov    = a_p.fov;
            s.label  = a_c.label;
            return s;
        }

        // The shots file is plain text people may edit: SceneShots only reads finite numbers, and the values are kept
        // in a sane range here (offset within kMaxShotOffset of the anchor, FOV inside TFCam's own FOV limits).
        constexpr float kMaxShotOffset = 5000.0f;

        Pose ShotToPose(const SceneShots::Shot& a_s, const ShotContext& a_c) {
            const RE::NiPoint3 fwd{ std::sin(a_c.heading), std::cos(a_c.heading), 0.0f };
            const RE::NiPoint3 right{ std::cos(a_c.heading), -std::sin(a_c.heading), 0.0f };
            RE::NiPoint3       local{ a_s.x, a_s.y, a_s.z };
            if (const float len = Length(local); !std::isfinite(len)) {  // finite parts whose squares overflow
                local = {};
            } else if (len > kMaxShotOffset) {
                local = local * (kMaxShotOffset / len);
            }
            Pose p;
            p.pos   = { a_c.anchor.x + right.x * local.x + fwd.x * local.y, a_c.anchor.y + right.y * local.x + fwd.y * local.y,
                  a_c.anchor.z + local.z };
            p.yaw   = WrapPi(a_c.heading + a_s.yawRel * kDegToRad);
            p.pitch = std::clamp(a_s.pitch * kDegToRad, -kPitchLimit, kPitchLimit);
            p.roll  = WrapPi(a_s.roll * kDegToRad);
            const auto& fs   = FreeCam::GetSettings();
            const float fLo  = std::clamp(fs.fovMin, 1.0f, 170.0f);
            const float fHi  = std::clamp(fs.fovMax, fLo, 170.0f);
            p.fov            = a_s.fov > 1.0f ? std::clamp(a_s.fov, fLo, fHi) : GetFOV();
            return p;
        }

        std::string NowText() {
            const std::time_t t = std::time(nullptr);
            std::tm           tm{};
            localtime_s(&tm, &t);
            char buf[32] = {};
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
            return buf;
        }

        void StartBlend(const Pose& a_from, const Pose& a_to, float a_dur) {
            s_blend.active = true;
            s_blend.from   = a_from;
            s_blend.to     = a_to;
            s_blend.t      = 0.0f;
            s_blend.dur    = (std::max)(a_dur, 0.0f);
        }

        // ---- near clip (eye view) -------------------------------------------------------------------------------------
        RE::Setting* NearSetting() {
            static RE::Setting* s_setting = nullptr;
            static bool         s_looked  = false;
            if (!s_looked) {
                s_looked = true;
                if (auto* ini = RE::INISettingCollection::GetSingleton()) s_setting = ini->GetSetting("fNearDistance:Display");
                if (!s_setting) {
                    if (auto* pref = RE::INIPrefSettingCollection::GetSingleton()) s_setting = pref->GetSetting("fNearDistance:Display");
                }
                SKSE::log::info("SceneCam: fNearDistance:Display {}", s_setting
                    ? std::format("found ({:.2f})", s_setting->data.f)
                    : std::string("not found - eye view changes only the world camera's frustum near plane"));
            }
            return s_setting;
        }

        // The world camera's frustum (flat runtimes; VR keeps per-eye frustums elsewhere and is left alone).
        RE::NiCamera* WorldCamera() { return REL::Module::IsVR() ? nullptr : RE::Main::WorldRootCamera(); }

        void ApplyNearClip() {
            const float v = s_cfg.eyeNearClip;
            if (!s_eye.nearApplied) {
                s_eye.nearApplied = true;
                s_eye.haveIni     = false;
                s_eye.haveFrustum = false;
                if (auto* s = NearSetting()) {
                    s_eye.savedIni = s->data.f;
                    s_eye.haveIni  = true;
                }
                if (auto* nc = WorldCamera()) {
                    s_eye.savedFrustum = nc->GetRuntimeData2().viewFrustum.fNear;
                    s_eye.haveFrustum  = true;
                }
                SKSE::log::info("SceneCam: near clip {:.2f} for eye view (saved fNearDistance {}, frustum near {})", v,
                    s_eye.haveIni ? std::format("{:.2f}", s_eye.savedIni) : std::string("n/a"),
                    s_eye.haveFrustum ? std::format("{:.2f}", s_eye.savedFrustum) : std::string("n/a"));
            }
            // Re-asserted every frame: the frustum may be rebuilt from the setting at any time.
            if (auto* s = NearSetting()) s->data.f = v;
            if (auto* nc = WorldCamera()) {
                auto& frustum = nc->GetRuntimeData2().viewFrustum;
                if (frustum.fNear != v) frustum.fNear = v;
            }
            s_eye.written = v;
        }

        void RestoreNearClip() {
            if (!s_eye.nearApplied) return;
            s_eye.nearApplied = false;
            if (auto* s = NearSetting(); s && s_eye.haveIni) s->data.f = s_eye.savedIni;
            if (auto* nc = WorldCamera(); nc && s_eye.haveFrustum) {
                auto& frustum = nc->GetRuntimeData2().viewFrustum;
                // Only if it still holds our value: the engine may have rebuilt it from the restored setting already.
                if (std::fabs(frustum.fNear - s_eye.written) < 0.001f) frustum.fNear = s_eye.savedFrustum;
            }
            SKSE::log::info("SceneCam: near clip restored (fNearDistance {}, frustum near {})",
                s_eye.haveIni ? std::format("{:.2f}", s_eye.savedIni) : std::string("n/a"),
                s_eye.haveFrustum ? std::format("{:.2f}", s_eye.savedFrustum) : std::string("n/a"));
            s_eye.haveIni     = false;
            s_eye.haveFrustum = false;
        }

        // ---- hidden head (eye view) ---------------------------------------------------------------------------------
        void RestoreHeadCull() {
            if (!s_eye.culled) return;
            s_eye.culled->SetAppCulled(s_eye.culledPrev);
            SKSE::log::info("SceneCam: partner's head shown again (AppCulled back to {})", s_eye.culledPrev);
            s_eye.culled.reset();
        }

        // Culls the target's FaceGen node (head, hair, eyes, mouth). Re-run every frame: a 3D reload makes a new node.
        void MaintainHeadCull(RE::Actor* a_actor) {
            if (!s_cfg.eyeHideHead || !a_actor) {
                RestoreHeadCull();
                return;
            }
            RE::NiAVObject* node = a_actor->GetFaceNodeSkinned();
            if (!node) node = Node(a_actor->Get3D(false), N().faceGen);
            if (!node) {
                static RE::FormID s_warned = 0;
                if (s_warned != a_actor->GetFormID()) {
                    s_warned = a_actor->GetFormID();
                    SKSE::log::info("SceneCam: {:08X} has no FaceGen node - its head stays visible", s_warned);
                }
                return;
            }
            if (s_eye.culled.get() == node) {
                if (!node->GetAppCulled()) node->SetAppCulled(true);
                return;
            }
            RestoreHeadCull();
            s_eye.culled     = RE::NiPointer<RE::NiAVObject>(node);
            s_eye.culledPrev = node->GetAppCulled();
            node->SetAppCulled(true);
            SKSE::log::info("SceneCam: {} ({:08X}) head hidden for eye view (AppCulled was {})", a_actor->GetName(),
                a_actor->GetFormID(), s_eye.culledPrev);
        }

        // ---- eye frame -----------------------------------------------------------------------------------------------
        struct EyeFrame {
            RE::NiPoint3 eye{};
            RE::NiPoint3 fwd{};
            RE::NiPoint3 up{};
            std::string  source;
        };

        // Head frame without bone axis conventions when the eyes exist: right = R eye - L eye, up = head - neck
        // (made square to right), forward = up x right (z up, right-handed: an actor standing facing +Y gives right +X,
        // up +Z, forward +Y). No eye pair (the normal case: XPMSSE has only NPCEyeBone): NPC Head [Head]'s own fixed
        // local axes, never chosen from the current pose (a lying / all-fours partner picked the spine axis that way).
        bool ComputeEyeFrame(RE::Actor* a_actor, EyeFrame& a_out) {
            auto* root = a_actor->Get3D(false);
            if (!root) return false;
            const auto& n    = N();
            auto*       head = Node(root, n.head);
            auto*       neck = Node(root, n.neck);

            RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
            if (head && neck) {
                RE::NiPoint3 u = head->world.translate - neck->world.translate;
                if (Normalize(u)) up = u;
            }

            for (const auto& pair : n.eyePairs) {
                auto* le = Node(root, pair[0]);
                auto* re = Node(root, pair[1]);
                if (!le || !re) continue;
                RE::NiPoint3 right = re->world.translate - le->world.translate;
                if (!Normalize(right)) continue;
                RE::NiPoint3 u = up - right * Dot(up, right);
                if (!Normalize(u)) continue;
                RE::NiPoint3 f = Cross(u, right);
                if (!Normalize(f)) continue;
                a_out.eye    = Mix(le->world.translate, re->world.translate, 0.5f);
                a_out.fwd    = f;
                a_out.up     = u;
                a_out.source = std::format("eye nodes '{}' / '{}'", pair[0].c_str(), pair[1].c_str());
                return true;
            }

            if (!head) return false;
            // NPC Head [Head]'s local axes: +X right, +Y out of the face, +Z out of the top of the head. Read from the
            // installed skeletons (XPMSSE male / GT SOFTBODY female, and the werewolf, draugr, troll and giant ones):
            // the eye bones sit at head-local (~0, +8..+22, +4..+9), and the human bind pose has the head at identity
            // rotation facing +Y. world.rotate's column k is local axis k in world space (CommonLib NiMatrix3: v' = M v).
            const auto&  m = head->world.rotate;
            RE::NiPoint3 f{ m.entry[0][1], m.entry[1][1], m.entry[2][1] };
            RE::NiPoint3 u{ m.entry[0][2], m.entry[1][2], m.entry[2][2] };
            if (!Normalize(f)) return false;
            u = u - f * Dot(u, f);
            if (!Normalize(u)) return false;
            a_out.fwd = f;
            a_out.up  = u;
            for (const auto& name : n.eyeCentre) {
                if (auto* centre = Node(root, name)) {
                    a_out.eye    = centre->world.translate;
                    a_out.source = std::format("eye centre bone '{}' + NPC Head [Head] axes", name.c_str());
                    return true;
                }
            }
            a_out.eye    = head->world.translate + f * 7.0f + u * 2.0f;
            a_out.source = "NPC Head [Head] + 7 forward + 2 up";
            return true;
        }

        // Yaw / pitch (no roll) looking along the face. atan2(fwd.x, fwd.y) has no stable value when the face points
        // nearly straight up or down (partner on their back / face down): a small head bob would swing the yaw and spin
        // the picture. Near there the heading comes from the head's up axis instead - looking up, the yaw points away
        // from the top of the head, looking down, along it - so the top of the head stays at the top of the picture.
        // That equals the head's horizontal right axis turned 90 degrees (slcc map fact 10). It is blended in only for
        // a steep face (|fwd.z| 0.88..0.98, about 62..79 degrees up / down): for an upright head both point the same way
        // so the yaw is unchanged, and a head rolled sideways (lying on the side) keeps its exact view below that.
        void EyeAngles(const RE::NiPoint3& a_fwd, const RE::NiPoint3& a_up, float& a_yaw, float& a_pitch) {
            const float w  = SmoothStep((std::fabs(a_fwd.z) - 0.88f) / 0.1f);
            float       hx = a_fwd.x - a_up.x * a_fwd.z * w;
            float       hy = a_fwd.y - a_up.y * a_fwd.z * w;
            float       hl = std::hypot(hx, hy);
            if (!std::isfinite(hl) || hl < 1e-4f) {  // only where an upside-down head flips over: plain face direction
                hx = a_fwd.x;
                hy = a_fwd.y;
                hl = std::hypot(hx, hy);
            }
            if (!std::isfinite(hl) || hl < 1e-6f) {
                a_yaw   = 0.0f;
                a_pitch = a_fwd.z > 0.0f ? -kPitchLimit : kPitchLimit;
                return;
            }
            a_yaw = std::atan2(hx, hy);
            // Pitch in the vertical plane of that yaw (positive looks down). Past straight up / down (an upside-down
            // head) it goes beyond 90 degrees and the caller's clamp holds it at the pole.
            a_pitch = std::atan2(-a_fwd.z, (a_fwd.x * hx + a_fwd.y * hy) / hl);
        }

        bool EyePose(RE::Actor* a_actor, Pose& a_p, bool a_log) {
            EyeFrame fr;
            if (!ComputeEyeFrame(a_actor, fr)) return false;
            float yaw   = 0.0f;
            float pitch = 0.0f;
            EyeAngles(fr.fwd, fr.up, yaw, pitch);
            a_p.pos   = fr.eye + fr.fwd * s_cfg.eyeForward;
            a_p.yaw   = WrapPi(yaw + s_eye.yawOff);
            a_p.pitch = std::clamp(pitch + s_eye.pitchOff, -kPitchLimit, kPitchLimit);
            a_p.roll  = FreeCam::GetRollRadians();
            a_p.fov   = s_cfg.eyeFOV;
            if (a_log) {
                const float        h = a_actor->GetAngleZ();
                const RE::NiPoint3 heading{ std::sin(h), std::cos(h), 0.0f };
                SKSE::log::info("SceneCam: eye frame for {} ({:08X}) from {}: dot(forward, actor heading) = {:.2f} (about 1 "
                                "for an actor standing straight), forward ({:.2f}, {:.2f}, {:.2f}), up ({:.2f}, {:.2f}, {:.2f}), "
                                "yaw {:.1f} pitch {:.1f} deg before the mouse offset",
                    a_actor->GetName(), a_actor->GetFormID(), fr.source, Dot(fr.fwd, heading), fr.fwd.x, fr.fwd.y, fr.fwd.z,
                    fr.up.x, fr.up.y, fr.up.z, yaw * kRadToDeg, pitch * kRadToDeg);
            }
            return true;
        }

        bool HasEyeFrame(RE::Actor* a_actor) {
            EyeFrame fr;
            return ComputeEyeFrame(a_actor, fr);
        }

        // The player's scene partners with 3D, in position order: the eye view candidates.
        std::vector<ActorPtr> LoadedPartners() {
            auto partners = SceneActors(true);
            std::erase_if(partners, [](const ActorPtr& a) { return !a->Get3D(false); });
            return partners;
        }

        // Index of the first partner from a_start on (wrapping round the list) whose skeleton gives an eye frame, -1 when
        // none does. A creature without NPC Head [Head] / eye nodes is skipped, so it never blocks a partner with a head.
        int FirstWithHead(const std::vector<ActorPtr>& a_partners, int a_start) {
            const int n = static_cast<int>(a_partners.size());
            for (int k = 0; k < n; ++k) {
                const int i = ((a_start + k) % n + n) % n;
                if (HasEyeFrame(a_partners[i].get())) return i;
                SKSE::log::info("SceneCam: eye view skips {} ({:08X}) - no NPC Head [Head] / eye nodes",
                    a_partners[i]->GetName(), a_partners[i]->GetFormID());
            }
            return -1;
        }

        // ---- orbit target ----------------------------------------------------------------------------------------------
        // The player's scene actors; else the posed actor; else the player (0.13.0 a_playerOnly: the player). Centre of
        // their torso points. 0.15.0 a_floor: the lowest of their feet (the floor the orbit / preset camera stays above).
        bool OrbitTargetRaw(RE::NiPoint3& a_out, std::string* a_what, bool a_playerOnly = false, float* a_floor = nullptr) {
            std::vector<ActorPtr> list;
            const char*           what = "the scene's actors";
            if (!a_playerOnly) {
                for (auto& actor : SceneActors(false)) {
                    if (actor->Get3D(false)) list.push_back(actor);
                }
                if (list.empty() && s_pose.id) {
                    auto actor = s_pose.actor.get();
                    if (actor && actor->Get3D(false)) {
                        list.push_back(std::move(actor));
                        what = "the posed actor";
                    }
                }
            }
            if (list.empty()) {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    list.push_back(ActorPtr(player));
                    what = "the player";
                }
            }
            if (list.empty()) return false;
            RE::NiPoint3 sum{};
            float        floor = (std::numeric_limits<float>::max)();
            for (auto& actor : list) {
                const auto p = TorsoPoint(actor.get());
                sum.x += p.x;
                sum.y += p.y;
                sum.z += p.z;
                floor = (std::min)(floor, actor->GetPosition().z);
            }
            const float n = static_cast<float>(list.size());
            a_out         = { sum.x / n, sum.y / n, sum.z / n };
            if (a_floor) *a_floor = floor;
            if (a_what) *a_what = std::format("{} ({})", what, list.size());
            return true;
        }

        // 0.17.0: the orbit's target - the console selection with iOrbitTarget = 2 while one is picked, else as before.
        bool OrbitSubject(RE::NiPoint3& a_out, std::string* a_what, float* a_floor) {
            if (s_cfg.orbitTarget == 2) {
                if (auto ref = CamFocus::ConsoleRef(); ref && CamFocus::RefPoint(ref.get(), a_out, a_floor)) {
                    if (a_what) *a_what = "the console selection " + CamFocus::Describe(ref.get());
                    return true;
                }
            }
            return OrbitTargetRaw(a_out, a_what, s_cfg.orbitTarget == 1, a_floor);
        }

        // ---- modes -------------------------------------------------------------------------------------------------------
        void LeaveOrbit(std::string_view a_why, bool a_restoreFOV) {
            if (s_mode != TFCAM_API::kModeOrbit) return;
            s_mode = TFCAM_API::kModeFree;
            if (a_restoreFOV && CurrentFreeCam() && FreeCam::TFCamDriving()) SetFOV(s_orbit.entryFOV);
            Event(std::format("orbit off ({}) - the camera stays where it is{}", a_why,
                a_restoreFOV ? std::format(", FOV back to {:.0f}", s_orbit.entryFOV) : std::string()));
        }

        void LeavePreset(std::string_view a_why, bool a_restoreFOV) {
            if (s_mode != TFCAM_API::kModePreset) return;
            s_mode = TFCAM_API::kModeFree;
            if (a_restoreFOV && CurrentFreeCam() && FreeCam::TFCamDriving()) SetFOV(s_preset.entryFOV);
            Event(std::format("preset view off ({}) - the camera stays where it is{}", a_why,
                a_restoreFOV ? std::format(", FOV back to {:.0f}", s_preset.entryFOV) : std::string()));
        }

        void EndPlacing(std::string_view a_why) {
            if (!s_placing) return;
            s_placing = false;
            Event(std::format("set cam pos off ({})", a_why));
        }

        void CleanupEye() {
            RestoreHeadCull();
            RestoreNearClip();
        }

        // a_blendBack: blend to the camera from before eye view (FOV included). Without it the caller moves on from
        // here (orbit / a saved shot blend from the current pose and FOV).
        void LeaveEye(std::string_view a_why, bool a_blendBack) {
            if (s_mode != TFCAM_API::kModeEye) return;
            CleanupEye();
            s_mode         = TFCAM_API::kModeFree;
            s_eye.blending = false;
            auto* fcs      = FreeCam::TFCamDriving() ? CurrentFreeCam() : nullptr;
            if (fcs && a_blendBack) {
                StartBlend(ReadPose(fcs), s_eye.pre, s_cfg.shotBlend);
            }
            Event(std::format("eye view off ({}){}", a_why,
                fcs && a_blendBack ? " - blending back to the camera from before it" : ""));
            s_eye.target   = {};
            s_eye.targetID = 0;
        }

        // No camera to move any more (free cam ended, another camera took it, game load): only undo our changes.
        void DropModes(std::string_view a_why, bool a_restoreFOV) {
            const bool any = s_mode != TFCAM_API::kModeFree || s_blend.active || s_placing;
            if (a_restoreFOV && Cam() && Cam()->IsInFreeCameraMode()) {
                if (s_mode == TFCAM_API::kModeOrbit) SetFOV(s_orbit.entryFOV);
                if (s_mode == TFCAM_API::kModeEye) SetFOV(s_eye.pre.fov);
                if (s_mode == TFCAM_API::kModePreset) SetFOV(s_preset.entryFOV);
            }
            s_placing      = false;
            if (s_mode == TFCAM_API::kModeEye) CleanupEye();
            s_mode         = TFCAM_API::kModeFree;
            s_blend.active = false;
            s_eye.blending = false;
            s_eye.target   = {};
            s_eye.targetID = 0;
            if (any) Event(std::format("scene camera back to plain free-fly ({})", a_why));
        }

        void SetEyeTarget(RE::Actor* a_actor, int a_index) {
            RestoreHeadCull();
            s_eye.target     = a_actor->GetHandle();
            s_eye.targetID   = a_actor->GetFormID();
            s_eye.preferred  = a_index;
            s_eye.yawOff     = 0.0f;
            s_eye.pitchOff   = 0.0f;
            s_eye.nullSince  = -1.0;
            s_eye.haveLast   = false;
            MaintainHeadCull(a_actor);
        }

        // Switch partner while in eye view, blending from where the camera is.
        void RetargetEye(RE::Actor* a_actor, int a_index, std::string_view a_why) {
            SetEyeTarget(a_actor, a_index);
            if (auto* fcs = CurrentFreeCam()) {
                s_eye.blending  = true;
                s_eye.blendFrom = ReadPose(fcs);
                s_eye.blendT    = 0.0f;
            }
            Pose probe;
            EyePose(a_actor, probe, true);
            Event(std::format("eye view: looking through {} ({:08X}) ({})", a_actor->GetName(), a_actor->GetFormID(), a_why));
        }

        void EnterOrbit(RE::FreeCameraState* a_fcs, std::string_view a_source) {
            CamPath::Stop("orbit turned on");  // 0.17.0
            // From eye view: start where the eyes were, and give the pre-eye-view FOV back when the orbit ends.
            const bool  fromEye      = s_mode == TFCAM_API::kModeEye;
            const float preEyeFOV    = s_eye.pre.fov;
            const bool  fromPreset   = s_mode == TFCAM_API::kModePreset;  // 0.15.0: its pre-preset FOV comes back too
            const float prePresetFOV = s_preset.entryFOV;
            LeaveEye("orbit turned on", false);
            LeavePreset("orbit turned on", false);
            EndPlacing("orbit turned on");
            s_blend.active = false;
            RE::NiPoint3 target{};
            std::string  what;
            if (!OrbitSubject(target, &what, nullptr)) {
                Refuse("Orbit: nothing to orbit", "orbit: no target (no player?)");
                return;
            }
            const Pose         cur    = ReadPose(a_fcs);
            const RE::NiPoint3 centre = target + RE::NiPoint3{ 0.0f, 0.0f, s_cfg.orbitAim };
            RE::NiPoint3       off    = cur.pos - centre;
            float        dist = Length(off);
            float        az   = 0.0f;
            float        el   = 0.0f;
            if (!std::isfinite(dist) || dist < 1.0f) {
                auto* player = RE::PlayerCharacter::GetSingleton();
                az           = player ? player->GetAngleZ() : 0.0f;
                el           = s_cfg.orbitHeight * kDegToRad;
                dist         = s_cfg.orbitRadius;
            } else {
                az = std::atan2(off.x, off.y);
                el = std::asin(std::clamp(off.z / dist, -1.0f, 1.0f));
            }
            s_orbit          = OrbitState{};
            s_orbit.target   = target;
            s_orbit.az       = az;
            s_orbit.el       = el;
            s_orbit.el0      = el;
            s_orbit.dist     = dist;
            s_orbit.dist0    = dist;
            s_orbit.fov      = cur.fov;
            s_orbit.fov0     = cur.fov;
            s_orbit.yaw0     = cur.yaw;
            s_orbit.pitch0   = cur.pitch;
            s_orbit.entryFOV = fromEye && preEyeFOV > 0.0f ? preEyeFOV : (fromPreset && prePresetFOV > 0.0f ? prePresetFOV : cur.fov);
            s_orbit.aim      = s_cfg.orbitAim;
            s_orbit.roll     = FreeCam::GetExtraRoll();  // 0.17.0: from wherever the last orbit's roll eased back to
            s_mouseDY        = 0.0f;
            s_mode           = TFCAM_API::kModeOrbit;
            s_autoOwnsFree   = false;  // the user took this free camera over: it stays on when an NPC pose ends

            // Pitch sign check (camera convention: positive pitch looks down). When the camera already faces the target
            // both pitches have the same sign.
            float lookYaw   = 0.0f;
            float lookPitch = 0.0f;
            LookAngles(centre - cur.pos, lookYaw, lookPitch);
            SKSE::log::info("SceneCam: pitch check at orbit start - camera pitch {:.3f} yaw {:.3f}; look-at to the target "
                            "pitch {:.3f} yaw {:.3f} (camera {:.0f} units away, {:.0f} deg {} it)",
                cur.pitch, WrapPi(cur.yaw), lookPitch, lookYaw, dist, std::fabs(el * kRadToDeg), el >= 0.0f ? "above" : "below");
            Event(std::format("orbit on ({}) round {} - radius {:.0f}, tilt {:.0f}, speed {:.0f} deg/s, FOV {:.0f}, aim {:+.0f}, "
                              "sway {:.0f}", a_source, what, s_cfg.orbitRadius, s_cfg.orbitHeight, s_cfg.orbitSpeed,
                s_cfg.orbitFOV, s_cfg.orbitAim, s_cfg.orbitSway));
        }

        bool EnterEye(RE::FreeCameraState* a_fcs, std::string_view a_source) {
            CamPath::Stop("eye view turned on");  // 0.17.0
            auto partners = LoadedPartners();
            if (partners.empty()) {
                Refuse("Eye view needs a partner in your SexLab scene", "eye view: no loaded partner in the player's scene");
                return false;
            }
            // The preferred partner, else the next one with a head (a creature without NPC Head [Head] is skipped).
            const int idx = FirstWithHead(partners, s_eye.preferred);
            if (idx < 0) {
                Refuse(partners.size() == 1 ? "Eye view: no head found on the partner's skeleton"
                                            : "Eye view: no head found on any partner's skeleton",
                    std::format("eye view: none of the {} loaded partner(s) has NPC Head [Head] / eye nodes", partners.size()));
                return false;
            }
            // From orbit: blend from the orbit's pose and FOV; leaving eye view later returns to the pre-orbit FOV.
            const bool  fromOrbit = s_mode == TFCAM_API::kModeOrbit || s_mode == TFCAM_API::kModePreset;
            const float orbitFOV  = s_mode == TFCAM_API::kModePreset ? s_preset.entryFOV : s_orbit.entryFOV;
            LeaveOrbit("eye view turned on", false);
            LeavePreset("eye view turned on", false);
            EndPlacing("eye view turned on");
            s_blend.active  = false;
            s_eye.pre       = ReadPose(a_fcs);
            s_eye.blendFrom = s_eye.pre;
            if (fromOrbit && orbitFOV > 0.0f) s_eye.pre.fov = orbitFOV;
            SetEyeTarget(partners[idx].get(), idx);
            s_eye.blending  = true;
            s_eye.blendT    = 0.0f;
            ApplyNearClip();
            s_mode         = TFCAM_API::kModeEye;
            s_autoOwnsFree = false;  // the user took this free camera over
            Pose probe;
            EyePose(partners[idx].get(), probe, true);
            Event(std::format("eye view on ({}): looking through {} ({:08X}), FOV {:.0f}, near clip {:.1f}, head {}", a_source,
                partners[idx]->GetName(), partners[idx]->GetFormID(), s_cfg.eyeFOV, s_cfg.eyeNearClip,
                s_cfg.eyeHideHead ? "hidden" : "shown"));
            return true;
        }

        // ---- 0.15.0 preset view / set cam pos ---------------------------------------------------------------------------
        // The current stage's saved shot (the preset view's view 0).
        const SceneShots::Shot* StageShot(ShotContext& a_ctx) {
            a_ctx = StageContext();
            return a_ctx.ok ? SceneShots::Find(a_ctx.key) : nullptr;
        }

        // a_view: 0 = the stage's saved shot (the front view when there is none), 1..kPresetCount, -1 = the saved shot
        // when there is one, else the front view.
        void EnterPreset(RE::FreeCameraState* a_fcs, std::string_view a_source, int a_view) {
            CamPath::Stop("preset view turned on");  // 0.17.0
            // The FOV from before orbit / eye view comes back when preset view ends.
            float keepFOV = 0.0f;
            if (s_mode == TFCAM_API::kModeEye) keepFOV = s_eye.pre.fov;
            if (s_mode == TFCAM_API::kModeOrbit) keepFOV = s_orbit.entryFOV;
            LeaveEye("preset view turned on", false);
            LeaveOrbit("preset view turned on", false);
            EndPlacing("preset view turned on");
            s_blend.active = false;
            RE::NiPoint3 target{};
            if (!OrbitTargetRaw(target, nullptr)) {
                Refuse("Preset view: nothing to look at", "preset view: no target (no player?)");
                return;
            }
            const Pose         cur  = ReadPose(a_fcs);
            const RE::NiPoint3 off  = cur.pos - target;
            const float        dist = Length(off);
            s_preset        = PresetState{};
            s_preset.target = target;
            if (std::isfinite(dist) && dist >= 1.0f) {
                s_preset.az   = std::atan2(off.x, off.y);
                s_preset.el   = std::asin(std::clamp(off.z / dist, -1.0f, 1.0f));
                s_preset.dist = dist;
            } else {
                auto* player  = RE::PlayerCharacter::GetSingleton();
                s_preset.az   = player ? player->GetAngleZ() : 0.0f;
                s_preset.el   = kPresets[0].el * kDegToRad;
                s_preset.dist = kPresets[0].dist;
            }
            s_preset.fixYaw   = cur.yaw;
            s_preset.fixPitch = cur.pitch;
            s_preset.aimW     = 1.0f;
            s_preset.roll     = cur.roll;
            s_preset.fov      = cur.fov;
            s_preset.entryFOV = keepFOV > 0.0f ? keepFOV : cur.fov;
            ShotContext ctx;
            const bool  hasShot = StageShot(ctx) != nullptr;
            s_preset.view       = a_view < 0 ? (hasShot ? 0 : 1) : (a_view == 0 && !hasShot ? 1 : std::clamp(a_view, 0, kPresetCount));
            s_mode              = TFCAM_API::kModePreset;
            s_autoOwnsFree      = false;  // the user took this free camera over
            Event(std::format("preset view on ({}): {}{}", a_source, PresetName(s_preset.view),
                hasShot ? " - this stage has a saved shot" : ""));
        }

        bool StepPresetView(int a_step, std::string_view a_source) {
            if (s_mode != TFCAM_API::kModePreset) {
                Refuse("Preset view is not on", std::format("preset step ({}): preset view is not on", a_source));
                return false;
            }
            ShotContext ctx;
            const int   first = StageShot(ctx) ? 0 : 1;
            const int   n     = kPresetCount + 1 - first;
            const int   idx   = (((std::max)(s_preset.view - first, 0) + a_step) % n + n) % n;
            s_preset.view     = first + idx;
            Notify(std::format("Camera: {} ({}/{})", PresetName(s_preset.view), idx + 1, n).c_str());
            Event(std::format("preset view: {} ({})", PresetName(s_preset.view), a_source));
            return true;
        }

        void BeginPlacing(std::string_view a_source) {
            LeaveOrbit("set cam pos", false);
            LeaveEye("set cam pos", false);
            LeavePreset("set cam pos", false);
            s_blend.active = false;
            s_autoKey.clear();
            s_autoOwnsFree = false;
            s_placing      = true;
            Notify("Set cam pos: fly the camera there, then the middle mouse button saves it for this stage");
            Event(std::format("set cam pos on ({}) - the middle mouse button saves the stage's shot", a_source));
        }

        // TFCam free-fly is needed first: request it and remember what to do once TFCam drives.
        bool WaitForFreeCam(Req a_req) {
            if (FreeCam::TFCamDriving() && CurrentFreeCam()) return false;
            s_pending      = a_req;
            s_pendingUntil = Now() + kPendingTimeout;
            Event(std::format("{}: entering TFCam free-fly first", Name(a_req)));
            SlccBridge::RequestEnterFreeFly("Scene Camera");
            ScheduleTick();
            return true;
        }

        bool RequestOrbit(std::int32_t a_state, std::string_view a_source) {
            const bool on = a_state < 0 ? (s_mode != TFCAM_API::kModeOrbit && s_pending != Req::kOrbit) : a_state != 0;
            if (!on) {
                if (s_pending == Req::kOrbit) {
                    s_pending = Req::kNone;
                    Event(std::format("orbit request cancelled ({})", a_source));
                }
                LeaveOrbit(a_source, true);
                return true;
            }
            if (s_mode == TFCAM_API::kModeOrbit) return true;
            if (WaitForFreeCam(Req::kOrbit)) return true;
            EnterOrbit(CurrentFreeCam(), a_source);
            return s_mode == TFCAM_API::kModeOrbit;
        }

        bool RequestEye(std::int32_t a_state, std::string_view a_source) {
            const bool on = a_state < 0 ? (s_mode != TFCAM_API::kModeEye && s_pending != Req::kEye) : a_state != 0;
            if (!on) {
                if (s_pending == Req::kEye) {
                    s_pending = Req::kNone;
                    Event(std::format("eye view request cancelled ({})", a_source));
                }
                LeaveEye(a_source, true);
                return true;
            }
            if (s_mode == TFCAM_API::kModeEye) return true;
            if (!SceneTracker::PlayerSceneActive() || SceneActors(true).empty()) {
                Refuse("Eye view needs a partner in your SexLab scene",
                    std::format("eye view ({}): {}", a_source,
                        SceneTracker::PlayerSceneActive() ? "no partner known in the player's scene" : "the player is not in a SexLab scene"));
                return false;
            }
            if (WaitForFreeCam(Req::kEye)) return true;
            return EnterEye(CurrentFreeCam(), a_source);
        }

        bool RequestPreset(std::int32_t a_state, std::string_view a_source) {
            const bool on = a_state < 0 ? (s_mode != TFCAM_API::kModePreset && s_pending != Req::kPreset) : a_state != 0;
            if (!on) {
                if (s_pending == Req::kPreset) {
                    s_pending = Req::kNone;
                    Event(std::format("preset view request cancelled ({})", a_source));
                }
                LeavePreset(a_source, true);
                return true;
            }
            if (s_mode == TFCAM_API::kModePreset) return true;
            if (!SceneTracker::PlayerSceneActive()) {
                Refuse("Preset views need your SexLab scene", std::format("preset view ({}): the player is not in a SexLab scene", a_source));
                return false;
            }
            if (WaitForFreeCam(Req::kPreset)) return true;
            EnterPreset(CurrentFreeCam(), a_source, -1);
            return s_mode == TFCAM_API::kModePreset;
        }

        bool RequestPlacing(std::int32_t a_state, std::string_view a_source) {
            const bool on = a_state < 0 ? (!s_placing && s_pending != Req::kPlace) : a_state != 0;
            if (!on) {
                if (s_pending == Req::kPlace) {
                    s_pending = Req::kNone;
                    Event(std::format("set cam pos request cancelled ({})", a_source));
                }
                EndPlacing(a_source);
                return true;
            }
            if (s_placing) return true;
            if (!StageContext().ok) {
                Refuse("Set cam pos needs your SexLab scene", std::format("set cam pos ({}): no stage of the player's scene known", a_source));
                return false;
            }
            if (WaitForFreeCam(Req::kPlace)) return true;
            BeginPlacing(a_source);
            return true;
        }

        bool NextEye(std::string_view a_source) {
            if (s_mode != TFCAM_API::kModeEye) {
                Refuse("Eye view is not on", std::format("next partner ({}): eye view is not on", a_source));
                return false;
            }
            auto partners = LoadedPartners();
            if (partners.empty()) {
                LeaveEye("no loaded partner left", true);
                return false;
            }
            int cur = -1;
            for (std::size_t i = 0; i < partners.size(); ++i) {
                if (partners[i]->GetFormID() == s_eye.targetID) cur = static_cast<int>(i);
            }
            // Partners without a head are skipped; when only the current one has a head this lands back on it.
            const int next = FirstWithHead(partners, cur + 1);
            if (next < 0) {
                Refuse("Eye view: no partner with a head to switch to",
                    std::format("next partner ({}): no loaded partner has NPC Head [Head] / eye nodes", a_source));
                return false;
            }
            RetargetEye(partners[next].get(), next, std::format("next partner, {}", a_source));
            return true;
        }

        bool DoSave(std::string_view a_source) {
            const auto ctx = CurrentContext();
            if (!ctx.ok) {
                Refuse("No scene stage, pose or furniture to save the shot for", std::format("save shot ({}): no stage / pose", a_source));
                return false;
            }
            auto* fcs = FreeCam::TFCamDriving() ? CurrentFreeCam() : nullptr;
            if (!fcs) {
                Refuse("Turn the free camera on to save a camera shot", std::format("save shot ({}): TFCam free camera is not on", a_source));
                return false;
            }
            auto shot    = PoseToShot(ReadPose(fcs), ctx);
            shot.savedAt = NowText();
            if (!SceneShots::Put(ctx.key, shot)) {
                Refuse("Camera shot could not be saved (see TFCam.log)", std::format("save shot {}: file write failed", ctx.key));
                return false;
            }
            Notify(std::format("Camera shot saved for this {}", ctx.what).c_str());
            Event(std::format("shot saved ({}) {} - local ({:.1f}, {:.1f}, {:.1f}) yaw {:.1f} pitch {:.1f} roll {:.1f} FOV {:.1f}",
                a_source, ctx.key, shot.x, shot.y, shot.z, shot.yawRel, shot.pitch, shot.roll, shot.fov));
            return true;
        }

        bool DoRecall(std::string_view a_source) {
            const auto ctx = CurrentContext();
            if (!ctx.ok) {
                Refuse("No scene stage, pose or furniture to go to a shot for", std::format("go to shot ({}): no stage / pose", a_source));
                return false;
            }
            const auto* shot = SceneShots::Find(ctx.key);
            if (!shot) {
                Refuse(std::format("No saved camera shot for this {}", ctx.what).c_str(),
                    std::format("go to shot ({}): none saved for {}", a_source, ctx.key));
                return false;
            }
            if (WaitForFreeCam(Req::kRecall)) return true;
            LeaveOrbit("going to a saved shot", false);
            LeaveEye("going to a saved shot", false);
            LeavePreset("going to a saved shot", false);
            StartBlend(ReadPose(CurrentFreeCam()), ShotToPose(*shot, ctx), s_cfg.shotBlend);
            Event(std::format("go to shot ({}) {} over {:.1f} s", a_source, ctx.key, s_cfg.shotBlend));
            return true;
        }

        bool DoDelete(std::string_view a_source) {
            const auto ctx = CurrentContext();
            if (!ctx.ok) {
                Refuse("No scene stage, pose or furniture to delete a shot for", std::format("delete shot ({}): no stage / pose", a_source));
                return false;
            }
            if (!SceneShots::Has(ctx.key)) {
                Refuse(std::format("No saved camera shot for this {}", ctx.what).c_str(),
                    std::format("delete shot ({}): none saved for {}", a_source, ctx.key));
                return false;
            }
            if (!SceneShots::Erase(ctx.key)) {  // it exists (checked above): the file write failed, the shot is kept
                Refuse("Camera shot could not be deleted (see TFCam.log)", std::format("delete shot {}: file write failed", ctx.key));
                return false;
            }
            Notify(std::format("Camera shot deleted for this {}", ctx.what).c_str());
            Event(std::format("shot deleted ({}) {}", a_source, ctx.key));
            return true;
        }

        void DeleteSceneShots() {
            const auto& scene = SceneTracker::GetPlayerScene();
            if (scene.sceneId.empty()) {
                Refuse("No SexLab scene to delete camera shots for", "delete scene shots: the scene id is not known");
                return;
            }
            const int n = SceneShots::EraseWithPrefix("stage:" + scene.sceneId + "|");
            if (n < 0) {
                Refuse("Camera shots could not be deleted (see TFCam.log)",
                    std::format("delete scene shots '{}': file write failed", scene.sceneId));
                return;
            }
            Notify(std::format("Deleted {} camera shot(s) for this scene", n).c_str());
            Event(std::format("deleted {} shot(s) for scene '{}'", n, scene.sceneId));
        }

        // ---- 0.13.0 automatic free camera (autoEnterShots) ------------------------------------------------------------
        constexpr RE::FormID kPlayerID = 0x14;

        bool NpcPoseActive() { return s_pose.id != 0 && s_pose.id != kPlayerID; }

        // nullptr = nothing in the way of turning the free camera on by itself. Entering free-fly under a focus menu
        // pushes its input context under the menu's (the SLUI Studio steps aside before its own camera commands).
        const char* AutoEnterBlock() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return "no UI";
            if (ui->GameIsPaused()) return "the game is paused";
            for (const char* name :
                { "PrismaUI_FocusMenu", "Cursor Menu", "Console", "Dialogue Menu", "MessageBoxMenu", "Loading Menu" }) {
                if (ui->IsMenuOpen(name)) return name;
            }
            if (FreeCamMenu::IsOverlayOpen()) return "SKSE Menu Framework";
            return nullptr;
        }

        void ArmAutoEnter(const std::string& a_key, bool a_pose) {
            s_autoEnter       = AutoEnter{};
            s_autoEnter.key   = a_key;
            s_autoEnter.pose  = a_pose;
            s_autoEnter.until = Now() + kAutoWindow;
            Event(std::format("auto: {} has a saved shot - the free camera turns on for it once nothing is in the way", a_key));
            ScheduleTick();
        }

        // The player's SexLab scene has the camera now (P+ AutoTFC keeps a free camera that is already on, the user flies
        // it): a free camera TFCam turned on for an NPC pose is not TFCam's to turn off any more, and a pose shot still
        // waiting for one is dropped (ApplyAutoShot would take it over again).
        void HandPoseCamToScene(std::string_view a_why) {
            const bool pendingPose = s_pending == Req::kAutoShot && s_pendingShotKey.starts_with("pose:");
            if (!s_autoOwnsFree && !pendingPose) return;
            s_autoOwnsFree = false;
            if (pendingPose) {
                s_pending = Req::kNone;
                s_pendingShotKey.clear();
            }
            Event(std::format("the free camera TFCam turned on for a pose stays on - the player's scene has it ({})", a_why));
        }

        // Gives back a free camera TFCam turned on by itself for an NPC pose. Never from the Update hook (a toggle there
        // would replace the camera state inside its own Update): SetActivePose and the tick only.
        void LeaveOwnFreeCam(std::string_view a_why) {
            const bool pendingPose = s_pending == Req::kAutoShot && s_pendingShotKey.starts_with("pose:");
            if (!s_autoOwnsFree && !pendingPose) return;
            // Never during the player's scene: SLUI clears the pose when the posed NPC joins it, and turning the free
            // camera off then would throw the player out of the camera her scene uses.
            if (SceneTracker::PlayerSceneActive()) {
                HandPoseCamToScene(a_why);
                return;
            }
            // Requested but not up yet: ApplyAutoShot takes it over and the next tick gives it back.
            if (!s_autoOwnsFree && !FreeCam::TFCamDriving()) return;
            s_autoOwnsFree = false;
            if (pendingPose) {
                s_pending = Req::kNone;
                s_pendingShotKey.clear();
            }
            if (s_mode != TFCAM_API::kModeFree || !FreeCam::TFCamDriving()) return;
            Event(std::format("giving back the free camera TFCam turned on for a pose ({})", a_why));
            s_expectOwnEnd = true;
            SlccBridge::LeaveFreeFly("Scene Camera: the pose ended");
        }

        void SetActivePose(std::uint32_t a_id, const std::string& a_event) {
            if (a_id == 0 || a_event.empty()) {
                if (s_pose.id) Event(std::format("pose cleared ({:08X} '{}')", s_pose.id, s_pose.event));
                s_pose = ActivePose{};
                if (s_autoEnter.pose) s_autoEnter = AutoEnter{};
                LeaveOwnFreeCam("the pose stopped");
                return;
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id);
            if (!actor) {
                SKSE::log::info("SceneCam: SetActivePose {:08X} '{}' - not an actor, no pose", a_id, a_event);
                s_pose = ActivePose{};
                if (s_autoEnter.pose) s_autoEnter = AutoEnter{};
                LeaveOwnFreeCam("the pose stopped");
                return;
            }
            // A repeat (SLUI re-reports the target's pose) does not arm the automatic free camera again.
            const bool repeat = s_pose.id == a_id && s_pose.event == Lower(a_event);
            s_pose.actor = actor->GetHandle();
            s_pose.id    = a_id;
            s_pose.event = Lower(a_event);
            s_pose.key   = "pose:" + s_pose.event;
            Event(std::format("pose '{}' on {} ({:08X})", s_pose.event, actor->GetName(), a_id));
            // Automatic: only into a free camera TFCam already drives, never turning one on.
            if (s_cfg.autoPoseShots && s_mode == TFCAM_API::kModeFree) {
                auto*      fcs = FreeCam::TFCamDriving() ? CurrentFreeCam() : nullptr;
                const auto ctx = PoseContext();
                if (fcs && ctx.ok) {
                    if (const auto* shot = SceneShots::Find(ctx.key)) {
                        StartBlend(ReadPose(fcs), ShotToPose(*shot, ctx), s_cfg.shotBlend);
                        Event(std::format("auto: blending to the saved shot for {}", ctx.key));
                    }
                }
            }
            // 0.13.0: only an NPC pose turns the free camera on - the player ends her pose by moving, which free-fly
            // would take from her - and a free camera TFCam turned on for an NPC pose is given back for hers.
            if (a_id == kPlayerID) {
                if (s_autoEnter.pose) s_autoEnter = AutoEnter{};
                LeaveOwnFreeCam("a pose on the player");
            } else if (!repeat && s_cfg.autoEnterShots && s_cfg.autoPoseShots && !FreeCam::IsActive() &&
                       SceneShots::Has(s_pose.key)) {
                ArmAutoEnter(s_pose.key, true);
            } else if (!repeat && s_autoEnter.pose) {
                s_autoEnter = AutoEnter{};
            }
        }

        // The tick: turn TFCam free-fly on for the armed shot once nothing is in the way. At most once per arming (a
        // committed stage key / a new NPC pose); a menu in the way pushes the deadline on, anything else lets it run out.
        void ServiceAutoEnter(double a_now) {
            if (s_autoEnter.key.empty()) return;
            const std::string key  = s_autoEnter.key;
            const bool        pose = s_autoEnter.pose;
            const auto        drop = [&key](std::string_view a_why) {
                Event(std::format("auto free camera for {}: dropped - {}", key, a_why));
                s_autoEnter = AutoEnter{};
            };
            const auto&       scene   = SceneTracker::GetPlayerScene();
            const std::string current = pose ? (NpcPoseActive() ? s_pose.key : std::string())
                                             : (scene.inScene && scene.stageKnown ? StageKey(scene) : std::string());
            if (!s_cfg.autoEnterShots || !(pose ? s_cfg.autoPoseShots : s_cfg.autoStageShots)) {
                drop("turned off");
                return;
            }
            if (current != key) {
                drop(pose ? "the pose changed" : "the stage changed");
                return;
            }
            if (!pose && s_autoEnterOff) {
                drop("you left the free camera in this scene");
                return;
            }
            if (FreeCam::IsActive()) {
                drop(FreeCam::TFCamDriving() ? "the free camera is on already" : "an FCFW timeline / SLCC has the camera");
                return;
            }
            if (!SceneShots::Has(key)) {
                drop("the shot was deleted");
                return;
            }
            if (const char* block = AutoEnterBlock()) {
                s_autoEnter.until = a_now + kAutoWindow;
                if (!s_autoEnter.waited) {
                    s_autoEnter.waited = true;
                    Event(std::format("auto free camera for {}: waiting - {} is open", key, block));
                }
                return;
            }
            auto*       player = RE::PlayerCharacter::GetSingleton();
            const char* wait   = nullptr;
            if (s_pending != Req::kNone) {
                wait = "another scene camera request is waiting";
            } else if (s_mode != TFCAM_API::kModeFree) {
                wait = "orbit / eye view is on";
            } else if (FcfwBridge::FcfwOwnsCamera() || SlccBridge::SlccOwnsCamera()) {
                wait = "an FCFW timeline / SLCC owns the camera";
            } else if (!player || !player->Is3DLoaded()) {
                wait = "the player is not loaded";
            }
            if (wait) {
                if (a_now > s_autoEnter.until) drop(wait);
                return;
            }
            s_autoEnter = AutoEnter{};
            s_autoKey.clear();  // Req::kAutoShot blends to the shot once TFCam drives
            s_pendingShotKey = key;
            Event(std::format("auto: turning the free camera on for the saved shot of {}", key));
            WaitForFreeCam(Req::kAutoShot);
        }

        // ---- per frame (Update hook) --------------------------------------------------------------------------------
        // 0.13.0: the free camera TFCam turned on for a saved shot is up: blend to the shot of the stage / pose now.
        void ApplyAutoShot(RE::FreeCameraState* a_fcs) {
            const std::string key = std::move(s_pendingShotKey);
            s_pendingShotKey.clear();
            const bool pose = key.starts_with("pose:");
            if (pose) {
                // The tick gives it back when the NPC pose ends - unless the player's scene has started (its camera then).
                s_autoOwnsFree = !SceneTracker::PlayerSceneActive();
                if (!NpcPoseActive()) {
                    Event(std::format("auto: free camera on for {}, but that pose has ended", key));
                    return;
                }
            }
            const auto  ctx  = pose ? PoseContext() : StageContext();
            const auto* shot = ctx.ok ? SceneShots::Find(ctx.key) : nullptr;
            if (!shot) {
                Event(std::format("auto: free camera on for {}, but there is no saved shot for {} now", key,
                    ctx.ok ? ctx.key : std::string("the current stage / pose")));
                return;
            }
            StartBlend(ReadPose(a_fcs), ShotToPose(*shot, ctx), s_cfg.shotBlend);
            Event(std::format("auto: free camera on - blending to the saved shot for {}", ctx.key));
        }

        void ServicePending(RE::FreeCameraState* a_fcs) {
            if (s_pending == Req::kNone) return;
            const Req req = s_pending;
            s_pending     = Req::kNone;
            switch (req) {
            case Req::kOrbit:
                if (s_mode != TFCAM_API::kModeOrbit) EnterOrbit(a_fcs, "after entering free-fly");
                break;
            case Req::kEye:
                if (s_mode != TFCAM_API::kModeEye) EnterEye(a_fcs, "after entering free-fly");
                break;
            case Req::kRecall:
                DoRecall("after entering free-fly");
                break;
            case Req::kFurniture:
                RecallFurnitureShot("furniture camera, after entering free-fly");
                break;
            case Req::kAutoShot:
                ApplyAutoShot(a_fcs);
                break;
            case Req::kPreset:
                if (s_mode != TFCAM_API::kModePreset) EnterPreset(a_fcs, "after entering free-fly", -1);
                break;
            case Req::kPlace:
                if (!s_placing) BeginPlacing("after entering free-fly");
                break;
            default:
                break;
            }
        }

        // A stage key changed while TFCam was (or soon is) driving in Free mode: blend to its saved shot.
        void ServiceAuto(RE::FreeCameraState* a_fcs) {
            if (s_autoKey.empty() || CamPath::Playing()) return;  // 0.17.0: a playing camera path keeps the camera
            if (s_mode != TFCAM_API::kModeFree || s_placing || Now() > s_autoUntil) {
                s_autoKey.clear();
                return;
            }
            const auto ctx = StageContext();
            const std::string key = std::move(s_autoKey);
            s_autoKey.clear();
            if (!ctx.ok || ctx.key != key) return;
            if (const auto* shot = SceneShots::Find(key)) {
                StartBlend(ReadPose(a_fcs), ShotToPose(*shot, ctx), s_cfg.shotBlend);
                Event(std::format("auto: blending to the saved shot for {}", key));
            }
        }

        void DriveBlend(RE::FreeCameraState* a_fcs, float a_dt) {
            s_blend.t += a_dt;
            const float w = s_blend.dur > 0.0f ? SmoothStep(s_blend.t / s_blend.dur) : 1.0f;
            WritePose(a_fcs, LerpPose(s_blend.from, s_blend.to, w), true);
            // 0.12.0: keep a saved shot (anchored on a moved actor / other furniture) out of walls.
            if (RE::NiPoint3 pivot{}; OrbitTargetRaw(pivot, nullptr)) CamCollision::Request(pivot, "blend to a saved shot");
            if (w >= 1.0f) {
                s_blend.active = false;
                SKSE::log::info("SceneCam: blend finished - free-fly from here");
            }
        }

        // 0.15.0: never under the floor - the camera slides along it instead (it still looks at the centre).
        void KeepAboveFloor(RE::NiPoint3& a_pos, bool a_haveFloor, float a_floor) {
            if (a_haveFloor && a_pos.z < a_floor + kFloorMargin) a_pos.z = a_floor + kFloorMargin;
        }

        // The game's "Invert Y Axis" (the orbit tilt follows it like the normal look).
        bool InvertY() {
            auto* prefs = RE::INIPrefSettingCollection::GetSingleton();
            auto* s     = prefs ? prefs->GetSetting("bInvertYValues:Controls") : nullptr;
            return s && s->data.b;
        }

        void DriveOrbit(RE::FreeCameraState* a_fcs, float a_dt) {
            RE::NiPoint3 raw{};
            float        floor     = 0.0f;
            const bool   haveFloor = OrbitSubject(raw, nullptr, &floor);
            if (haveFloor) {
                s_orbit.target = Mix(s_orbit.target, raw, ExpWeight(a_dt, kTargetHalfLife));
            }
            s_orbit.t += a_dt;
            s_orbit.az     = WrapPi(s_orbit.az + s_cfg.orbitSpeed * kDegToRad * a_dt);
            // Breathing: +-orbitSway degrees (0.13.0; was a fixed 8) over three orbit periods.
            s_orbit.breath = std::fmod(s_orbit.breath + a_dt * std::fabs(s_cfg.orbitSpeed) / 3.0f, 360.0f);
            // 0.15.0 mouse tilt: the raw mouse Y moves the orbit the way the view would turn - mouse down (looking
            // down) raises the camera to a top view, mouse up lowers it to looking up from below. Raw mouse, not what
            // the vanilla Update pitched: the vanilla free camera clamps its own pitch, and near a top / bottom view
            // that clamp read as mouse movement and threw the orbit to the top. Never under the floor: the tilt stops
            // there instead of winding up.
            const float base =
                std::clamp(s_cfg.orbitHeight + s_cfg.orbitSway * std::sin(s_orbit.breath * kDegToRad), -85.0f, 85.0f) *
                kDegToRad;
            const float dy = std::exchange(s_mouseDY, 0.0f);
            if (std::isfinite(dy)) s_orbit.tilt += (InvertY() ? -dy : dy) * kOrbitTiltSens;
            float lo = kOrbitElMin;
            if (haveFloor && s_orbit.dist > 1.0f) {
                const float centreZ = s_orbit.target.z + s_orbit.aim;
                lo = (std::max)(lo, std::asin(std::clamp((floor + kFloorMargin - centreZ) / s_orbit.dist, -1.0f, 1.0f)));
            }
            const float elGoal = std::clamp(base + s_orbit.tilt, (std::min)(lo, kOrbitElMax), kOrbitElMax);
            s_orbit.tilt       = elGoal - base;
            const float entry = s_orbit.t < kOrbitEntrySec ? SmoothStep(s_orbit.t / kOrbitEntrySec) : 1.0f;
            if (s_orbit.t < kOrbitEntrySec) {
                s_orbit.el   = s_orbit.el0 + (elGoal - s_orbit.el0) * entry;
                s_orbit.dist = s_orbit.dist0 + (s_cfg.orbitRadius - s_orbit.dist0) * entry;
                s_orbit.fov  = s_orbit.fov0 + (s_cfg.orbitFOV - s_orbit.fov0) * entry;
            } else {
                const float w = ExpWeight(a_dt, kChaseHalfLife);
                s_orbit.el += (elGoal - s_orbit.el) * w;
                s_orbit.dist += (s_cfg.orbitRadius - s_orbit.dist) * w;
                s_orbit.fov += (s_cfg.orbitFOV - s_orbit.fov) * w;
            }
            s_orbit.aim += (s_cfg.orbitAim - s_orbit.aim) * ExpWeight(a_dt, kChaseHalfLife);
            // 0.13.0 aim: the orbit circles and looks at a point above / below the target, so the tilt stays the angle
            // the camera looks down at. Walls are still probed from the torso centre (always inside the actors).
            const RE::NiPoint3 centre = s_orbit.target + RE::NiPoint3{ 0.0f, 0.0f, s_orbit.aim };
            const float  ce = std::cos(s_orbit.el);
            const RE::NiPoint3 dir{ std::sin(s_orbit.az) * ce, std::cos(s_orbit.az) * ce, std::sin(s_orbit.el) };
            Pose p;
            p.pos  = centre + dir * s_orbit.dist;
            KeepAboveFloor(p.pos, haveFloor, floor);
            LookAngles(centre - p.pos, p.yaw, p.pitch);
            if (entry < 1.0f) {  // turn towards the target over the same second, no snap
                p.yaw   = s_orbit.yaw0 + WrapPi(p.yaw - s_orbit.yaw0) * entry;
                p.pitch = s_orbit.pitch0 + (p.pitch - s_orbit.pitch0) * entry;
            }
            p.roll = FreeCam::GetRollRadians();
            p.fov  = s_orbit.fov;
            WritePose(a_fcs, p, false);
            // 0.17.0 roll: a fixed Dutch angle that keeps turning at orbitRollSpeed degrees per second, chased so a new
            // setting (or the orbit's start) rolls in. On top of the free camera's own Q / E roll.
            s_orbit.rollPhase    = std::fmod(s_orbit.rollPhase + s_cfg.orbitRollSpeed * a_dt, 360.0f);
            const float rollGoal = WrapPi((s_cfg.orbitRoll + s_orbit.rollPhase) * kDegToRad);
            s_orbit.roll         = WrapPi(s_orbit.roll + WrapPi(rollGoal - s_orbit.roll) * ExpWeight(a_dt, kChaseHalfLife));
            FreeCam::SetExtraRoll(s_orbit.roll);
            CamCollision::Request(s_orbit.target, "scene orbit");  // 0.12.0
        }

        // 0.15.0: the chosen view, chased round the smoothed scene centre.
        void DrivePreset(RE::FreeCameraState* a_fcs, float a_dt) {
            RE::NiPoint3 raw{};
            float        floor     = 0.0f;
            const bool   haveFloor = OrbitTargetRaw(raw, nullptr, false, &floor);
            if (haveFloor) s_preset.target = Mix(s_preset.target, raw, ExpWeight(a_dt, kTargetHalfLife));
            const RE::NiPoint3 centre = s_preset.target;

            ShotContext ctx;
            const auto* shot = StageShot(ctx);
            if (s_preset.view == 0 && !shot) {
                s_preset.view = 1;
                Event("preset view: no saved shot for this stage - front view");
            }
            float gAz = 0.0f, gEl = 0.0f, gDist = 0.0f, gFov = 0.0f, gRoll = 0.0f, gAim = 0.0f;
            if (s_preset.view == 0) {
                const Pose         sp  = ShotToPose(*shot, ctx);
                const RE::NiPoint3 off = sp.pos - centre;
                gDist                  = (std::max)(Length(off), 1.0f);
                gAz                    = std::atan2(off.x, off.y);
                gEl                    = std::asin(std::clamp(off.z / gDist, -1.0f, 1.0f));
                gFov                   = sp.fov;
                gRoll                  = sp.roll;
                gAim                   = 1.0f;
                const float w          = ExpWeight(a_dt, kPresetHalfLife);
                s_preset.fixYaw += WrapPi(sp.yaw - s_preset.fixYaw) * w;
                s_preset.fixPitch += (sp.pitch - s_preset.fixPitch) * w;
            } else {
                const auto& d      = kPresets[s_preset.view - 1];
                auto*       player = RE::PlayerCharacter::GetSingleton();
                gAz                = (player ? player->GetAngleZ() : 0.0f) + d.az * kDegToRad;
                gEl                = d.el * kDegToRad;
                gDist              = d.dist;
                gFov               = d.fov;
            }
            const float w = ExpWeight(a_dt, kPresetHalfLife);
            s_preset.az   = WrapPi(s_preset.az + WrapPi(gAz - s_preset.az) * w);
            s_preset.el += (gEl - s_preset.el) * w;
            s_preset.dist += (gDist - s_preset.dist) * w;
            s_preset.fov += (gFov - s_preset.fov) * w;
            s_preset.roll += WrapPi(gRoll - s_preset.roll) * w;
            s_preset.aimW += (gAim - s_preset.aimW) * w;

            const float        ce = std::cos(s_preset.el);
            const RE::NiPoint3 dir{ std::sin(s_preset.az) * ce, std::cos(s_preset.az) * ce, std::sin(s_preset.el) };
            Pose               p;
            p.pos = centre + dir * s_preset.dist;
            if (s_preset.view != 0) KeepAboveFloor(p.pos, haveFloor, floor);  // a saved shot stays where it was put
            float lookYaw = 0.0f, lookPitch = 0.0f;
            LookAngles(centre - p.pos, lookYaw, lookPitch);
            p.yaw   = lookYaw + WrapPi(s_preset.fixYaw - lookYaw) * s_preset.aimW;
            p.pitch = std::clamp(lookPitch + (s_preset.fixPitch - lookPitch) * s_preset.aimW, -kPitchLimit, kPitchLimit);
            p.roll  = s_preset.roll;
            p.fov   = s_preset.fov;
            WritePose(a_fcs, p, true);
            CamCollision::Request(centre, "scene preset");
        }

        void DriveEye(RE::FreeCameraState* a_fcs, float a_prePitch, float a_preYaw, float a_dt) {
            // What the vanilla Update turned this frame is the user's mouse / stick look: keep it as an offset.
            s_eye.yawOff   = std::clamp(s_eye.yawOff + WrapPi(a_fcs->rotation.y - a_preYaw), -kLookLimit, kLookLimit);
            s_eye.pitchOff = std::clamp(s_eye.pitchOff + (a_fcs->rotation.x - a_prePitch), -kLookLimit, kLookLimit);

            auto       actor  = s_eye.target.get();
            const bool loaded = actor && actor->Get3D(false);
            Pose       p;
            if (!loaded || !EyePose(actor.get(), p, false)) {
                const double now = Now();
                if (s_eye.nullSince < 0.0) s_eye.nullSince = now;
                if (now - s_eye.nullSince > 0.5) {
                    Notify(loaded ? "Eye view ended: no head found on the partner's skeleton"
                                  : "Eye view ended: the partner is not loaded");
                    LeaveEye(loaded ? "no head frame on the partner for 0.5 s" : "the partner's 3D was gone for 0.5 s", true);
                    return;
                }
                if (s_eye.haveLast) WritePose(a_fcs, s_eye.last, false);  // hold still meanwhile
                return;
            }
            s_eye.nullSince = -1.0;
            ApplyNearClip();
            MaintainHeadCull(actor.get());
            if (s_eye.blending) {
                s_eye.blendT += a_dt;
                const float w = s_cfg.shotBlend > 0.0f ? SmoothStep(s_eye.blendT / s_cfg.shotBlend) : 1.0f;
                p             = LerpPose(s_eye.blendFrom, p, w);
                p.roll        = FreeCam::GetRollRadians();
                if (w >= 1.0f) s_eye.blending = false;
            }
            WritePose(a_fcs, p, false);
            s_eye.last     = p;
            s_eye.haveLast = true;
        }

        // ---- status ------------------------------------------------------------------------------------------------------
        void CopyStr(char* a_dst, std::size_t a_size, const std::string& a_src) {
            const std::size_t n = (std::min)(a_src.size(), a_size - 1);
            std::memcpy(a_dst, a_src.data(), n);
            a_dst[n] = '\0';
        }

        TFCAM_API::SceneCamSettings3 Settings3() {
            TFCAM_API::SceneCamSettings3 s{};
            s.size           = sizeof(s);
            s.orbitRadius    = s_cfg.orbitRadius;
            s.orbitTilt      = s_cfg.orbitHeight;
            s.orbitSpeed     = s_cfg.orbitSpeed;
            s.orbitFOV       = s_cfg.orbitFOV;
            s.eyeFOV         = s_cfg.eyeFOV;
            s.orbitAim       = s_cfg.orbitAim;
            s.orbitSway      = s_cfg.orbitSway;
            s.orbitTarget    = s_cfg.orbitTarget;
            s.autoEnterShots = s_cfg.autoEnterShots ? 1 : 0;
            return s;
        }

        void PublishStatus() {
            TFCAM_API::SceneCamStatus st{};
            st.size = sizeof(st);
            st.mode = static_cast<std::uint8_t>(FreeCam::TFCamDriving() && !s_freeCamEnded ? s_mode : TFCAM_API::kModeNone);

            const auto& scene = SceneTracker::GetPlayerScene();
            st.inPlayerScene  = scene.inScene;
            st.stageKnown     = scene.stageKnown;
            st.playerSlot     = scene.playerSlot;
            CopyStr(st.sceneId, sizeof(st.sceneId), scene.sceneId);
            CopyStr(st.stageId, sizeof(st.stageId), scene.stageId);

            std::uint32_t partners = 0;
            for (std::size_t i = 0; i < scene.actorIDs.size() && i < scene.actors.size(); ++i) {
                if (scene.actorIDs[i] == 0 || scene.actorIDs[i] == 0x14) continue;
                ++partners;
            }
            // Eye view needs a loaded partner whose skeleton gives an eye frame (a creature without a head does not
            // count). This runs every frame while flying: the node lookups are redone when the actor list changes and
            // otherwise at most every 0.25 s.
            static std::vector<RE::FormID> s_eIDs;
            static double                  s_eAt = -1.0;
            static bool                    s_eOk = false;
            if (const double now = Now(); scene.actorIDs != s_eIDs || now - s_eAt >= 0.25 || now < s_eAt) {
                s_eIDs = scene.actorIDs;
                s_eAt  = now;
                s_eOk  = false;
                for (std::size_t i = 0; i < scene.actorIDs.size() && i < scene.actors.size() && !s_eOk; ++i) {
                    if (scene.actorIDs[i] == 0 || scene.actorIDs[i] == 0x14) continue;
                    auto actor = scene.actors[i].get();
                    s_eOk      = actor && actor->Get3D(false) && HasEyeFrame(actor.get());
                }
            }
            st.partnerCount = partners;
            st.eyeAvailable = scene.inScene && s_eOk;
            st.eyeTarget    = s_mode == TFCAM_API::kModeEye ? s_eye.targetID : 0;
            st.poseActive   = s_pose.id != 0;
            CopyStr(st.poseKey, sizeof(st.poseKey), s_pose.key);

            // Shot lookups only when the key, the pose or the file changed.
            static std::uint32_t s_cSerial  = ~0u;
            static std::uint32_t s_cVersion = 0;
            static std::string   s_cPose;
            static bool          s_cStage   = false;
            static bool          s_cPoseHas = false;
            static int           s_cCount   = 0;
            if (scene.keySerial != s_cSerial || SceneShots::Version() != s_cVersion || s_pose.key != s_cPose) {
                s_cSerial  = scene.keySerial;
                s_cVersion = SceneShots::Version();
                s_cPose    = s_pose.key;
                s_cStage   = scene.stageKnown && SceneShots::Has(StageKey(scene));
                s_cPoseHas = !s_pose.key.empty() && SceneShots::Has(s_pose.key);
                s_cCount   = scene.sceneId.empty() ? 0 : SceneShots::CountWithPrefix("stage:" + scene.sceneId + "|");
            }
            st.hasStageShot = s_cStage;
            st.hasPoseShot  = s_cPoseHas;
            st.settings     = { s_cfg.orbitRadius, s_cfg.orbitHeight, s_cfg.orbitSpeed, s_cfg.orbitFOV, s_cfg.eyeFOV };

            TFCAM_API::SceneCamStatus3 st3{};
            static_assert(sizeof(st3.sceneId) == sizeof(st.sceneId) && sizeof(st3.stageId) == sizeof(st.stageId) &&
                          sizeof(st3.poseKey) == sizeof(st.poseKey));
            st3.size           = sizeof(st3);
            st3.mode           = st.mode;
            st3.freeCamActive  = FreeCam::TFCamDriving() && !s_freeCamEnded ? 1 : 0;
            st3.inPlayerScene  = st.inPlayerScene;
            st3.stageKnown     = st.stageKnown;
            st3.hasStageShot   = st.hasStageShot;
            st3.poseActive     = st.poseActive;
            st3.hasPoseShot    = st.hasPoseShot;
            st3.eyeAvailable   = st.eyeAvailable;
            st3.eyeTarget      = st.eyeTarget;
            st3.partnerCount   = static_cast<std::int32_t>(st.partnerCount);
            st3.playerSlot     = st.playerSlot;
            st3.sceneShotCount = s_cCount;
            std::memcpy(st3.sceneId, st.sceneId, sizeof(st3.sceneId));
            std::memcpy(st3.stageId, st.stageId, sizeof(st3.stageId));
            std::memcpy(st3.poseKey, st.poseKey, sizeof(st3.poseKey));
            st3.settings = Settings3();

            TFCAM_API::SceneCamPreset4 st4{};
            st4.size      = sizeof(st4);
            st4.hasShot   = st.hasStageShot ? 1 : 0;
            st4.viewCount = kPresetCount + st4.hasShot;
            st4.view      = st.mode == TFCAM_API::kModePreset ? s_preset.view : -1;
            st4.placing   = s_placing ? 1 : 0;
            CopyStr(st4.name, sizeof(st4.name), st4.view >= 0 ? PresetName(st4.view) : "");

            std::lock_guard lock(s_statusLock);
            s_status           = st;
            s_status3          = st3;
            s_status4          = st4;
            s_statusSceneShots = s_cCount;
            s_statusEvent      = s_lastEvent;
        }

        // ---- tick (SKSE task, once per frame while something is going on) --------------------------------------------
        bool NeedsTick() {
            return SceneTracker::NeedsTick() || s_pending != Req::kNone || s_saveAt > 0.0 || s_pose.id != 0 ||
                   !s_autoKey.empty() || !s_autoEnter.key.empty() || s_autoOwnsFree;
        }

        void Tick() {
            SceneTracker::Tick();
            const double now = Now();
            if (s_pending != Req::kNone && now > s_pendingUntil && !FreeCam::TFCamDriving()) {
                const Req req = s_pending;
                s_pending     = Req::kNone;
                s_pendingShotKey.clear();
                Refuse(req == Req::kAutoShot ? nullptr : "Scene camera: the free camera did not start",
                    std::format("{} - TFCam free-fly did not start within {:.1f} s", Name(req), kPendingTimeout));
            }
            if (s_pose.id) {
                auto actor = s_pose.actor.get();
                if (!actor || !actor->Is3DLoaded()) {
                    Event(std::format("pose cleared - actor {:08X} unloaded", s_pose.id));
                    s_pose = ActivePose{};
                }
            }
            if (s_autoOwnsFree && !NpcPoseActive()) LeaveOwnFreeCam("the pose ended");
            ServiceAutoEnter(now);
            if (!s_autoKey.empty() && now > s_autoUntil) s_autoKey.clear();
            if (s_saveAt > 0.0 && now >= s_saveAt) {
                s_saveAt = -1.0;
                FreeCamMenu::SaveSceneCamSettings();
            }
            PublishStatus();
        }

        template <class F>
        void OnMain(F&& a_fn) {
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask(std::function<void()>(std::forward<F>(a_fn)));
            }
        }

        TFCAM_API::SceneCamStatus Snapshot() {
            std::lock_guard lock(s_statusLock);
            return s_status;
        }
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Public
    // ----------------------------------------------------------------------------------------------------------------

    Config& GetConfig() { return s_cfg; }

    void ClampConfig() {
        auto key = [](int& k) { k = std::clamp(k, 0, 255); };
        key(s_cfg.orbitKey);
        key(s_cfg.eyeKey);
        key(s_cfg.eyeNextKey);
        key(s_cfg.shotSaveKey);
        key(s_cfg.shotRecallKey);
        key(s_cfg.shotDeleteKey);
        auto range = [](float& v, float lo, float hi, float def) { v = std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
        range(s_cfg.orbitRadius, 30.0f, 500.0f, 120.0f);
        range(s_cfg.orbitHeight, -20.0f, 75.0f, 20.0f);
        range(s_cfg.orbitSpeed, -90.0f, 90.0f, 10.0f);
        range(s_cfg.orbitFOV, 20.0f, 120.0f, 70.0f);
        range(s_cfg.orbitAim, -60.0f, 60.0f, 0.0f);
        range(s_cfg.orbitSway, 0.0f, 15.0f, 8.0f);
        s_cfg.orbitTarget = std::clamp(s_cfg.orbitTarget, 0, 2);
        range(s_cfg.orbitRoll, -90.0f, 90.0f, 0.0f);
        range(s_cfg.orbitRollSpeed, -90.0f, 90.0f, 0.0f);
        range(s_cfg.eyeFOV, 30.0f, 130.0f, 90.0f);
        range(s_cfg.eyeNearClip, 1.0f, 10.0f, 3.0f);
        range(s_cfg.eyeForward, -5.0f, 10.0f, 1.0f);
        range(s_cfg.shotBlend, 0.0f, 3.0f, 0.6f);
    }

    void OnConfigChanged() {
        ClampConfig();
        ScheduleTick();  // the status (settings) is republished on the main thread
    }

    void Install() {
        {
            // v3 consumers can read the settings before the first publish (a load).
            std::lock_guard lock(s_statusLock);
            s_status3.size       = sizeof(s_status3);
            s_status3.playerSlot = -1;
            s_status3.settings   = Settings3();
        }
        SKSE::log::info("SceneCam: ready - orbit / eye view / saved shots ({}), keys orbit 0x{:X} eye 0x{:X} next 0x{:X} save "
                        "0x{:X} go to 0x{:X} delete 0x{:X}",
            SceneShots::FilePath(), s_cfg.orbitKey, s_cfg.eyeKey, s_cfg.eyeNextKey, s_cfg.shotSaveKey, s_cfg.shotRecallKey,
            s_cfg.shotDeleteKey);
    }

    // Main task -> UI task -> main task: lands once per frame (SKSE drains its task queue until empty, so a task that
    // re-adds itself would spin inside one frame). Runs only while NeedsTick().
    void ScheduleTick() {
        if (s_inTick) return;  // the running tick re-arms itself below
        if (s_tickQueued.exchange(true)) return;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            s_tickQueued = false;
            return;
        }
        tasks->AddTask([] {
            s_tickQueued = false;
            s_inTick     = true;
            Tick();
            s_inTick = false;
            if (NeedsTick()) {
                if (auto* t = SKSE::GetTaskInterface()) {
                    t->AddUITask([] { ScheduleTick(); });
                }
            }
        });
    }

    void Drive(RE::FreeCameraState* a_fcs, float a_prePitch, float a_preYaw, bool a_driving) {
        const float dt = FrameDt();
        if (!a_fcs) return;
        s_freeCamEnded = false;  // a free camera is live again
        // 0.17.0: the orbit's roll eases back to none once the orbit ends (a playing camera path sets its own).
        if (!a_driving) {
            FreeCam::SetExtraRoll(0.0f);
        } else if (s_mode != TFCAM_API::kModeOrbit && !CamPath::Playing()) {
            const float r = FreeCam::GetExtraRoll();
            if (r != 0.0f) FreeCam::SetExtraRoll(std::fabs(r) < 0.002f ? 0.0f : r * (1.0f - ExpWeight(dt, kChaseHalfLife)));
        }
        if (!a_driving) {
            // An FCFW timeline / SLCC took this free camera: hands off, undo what we changed.
            if (s_mode != TFCAM_API::kModeFree || s_blend.active) {
                DropModes("another camera (FCFW timeline / SLCC) took the free camera", false);
            }
            PublishStatus();
            return;
        }
        ServicePending(a_fcs);
        ServiceAuto(a_fcs);
        switch (s_mode) {
        case TFCAM_API::kModeOrbit:
            DriveOrbit(a_fcs, dt);
            break;
        case TFCAM_API::kModePreset:
            DrivePreset(a_fcs, dt);
            break;
        case TFCAM_API::kModeEye:
            DriveEye(a_fcs, a_prePitch, a_preYaw, dt);
            break;
        default:
            if (s_blend.active) DriveBlend(a_fcs, dt);
            break;
        }
        PublishStatus();
    }

    bool SubjectPoint(RE::NiPoint3& a_out, std::string* a_what) { return OrbitTargetRaw(a_out, a_what, false, nullptr); }

    void StandDown(std::string_view a_why) { DropModes(a_why, true); }

    bool BlurSubject(RE::NiPoint3& a_out, const char*& a_what, bool& a_on) {
        a_on = false;
        if (!FreeCam::TFCamDriving()) return false;
        switch (s_mode) {
        case TFCAM_API::kModeOrbit:
            a_on   = true;
            a_out  = s_orbit.target + RE::NiPoint3{ 0.0f, 0.0f, s_orbit.aim };
            a_what = "orbit: its centre";
            return true;
        case TFCAM_API::kModePreset:
            a_on   = true;
            a_out  = s_preset.target;
            a_what = "preset view: the scene's centre";
            return true;
        case TFCAM_API::kModeEye:
            // Eye view looks at you out of a partner's eyes: you stay sharp.
            a_on = true;
            if (auto* player = RE::PlayerCharacter::GetSingleton(); player && player->Get3D(false)) {
                a_out  = TorsoPoint(player);
                a_what = "eye view: you";
                return true;
            }
            a_what = "eye view: screen centre";
            return false;
        default:
            return false;
        }
    }

    bool OwnsMotion() {
        return s_mode == TFCAM_API::kModeOrbit || s_mode == TFCAM_API::kModeEye || s_mode == TFCAM_API::kModePreset ||
               s_blend.active || AmbientCam::OwnsMotion() || ConvoCam::OwnsMotion() || CamPath::OwnsMotion();
    }

    bool OnWheel(int a_dir) {
        if (s_mode == TFCAM_API::kModeOrbit) {
            // 0.15.0: the orbit FOV (the radius moved to Shift + wheel)
            s_cfg.orbitFOV = std::clamp(s_cfg.orbitFOV + static_cast<float>(a_dir) * FreeCam::GetSettings().fovStep, 20.0f, 120.0f);
        } else if (s_mode == TFCAM_API::kModeEye) {
            s_cfg.eyeFOV = std::clamp(s_cfg.eyeFOV + static_cast<float>(a_dir) * FreeCam::GetSettings().fovStep, 30.0f, 130.0f);
        } else if (s_mode == TFCAM_API::kModePreset) {
            StepPresetView(a_dir < 0 ? -1 : 1, "mouse wheel");  // 0.15.0
            PublishStatus();
            return true;
        } else {
            return false;
        }
        s_saveAt = Now() + kSaveDebounce;
        ScheduleTick();
        return true;
    }

    void OnMouseMove(float a_dy) {
        if (s_mode == TFCAM_API::kModeOrbit) s_mouseDY += a_dy;
    }

    bool OnShiftWheel(int a_dir) {
        if (s_mode != TFCAM_API::kModeOrbit) return false;
        s_cfg.orbitRadius = std::clamp(s_cfg.orbitRadius * (a_dir < 0 ? 0.92f : 1.08f), 30.0f, 500.0f);
        s_saveAt          = Now() + kSaveDebounce;
        ScheduleTick();
        return true;
    }

    bool OnMiddleClick() {
        if (!s_placing) return false;
        auto* fcs = FreeCam::TFCamDriving() ? CurrentFreeCam() : nullptr;
        if (fcs && DoSave("set cam pos")) {
            s_placing = false;
            EnterPreset(fcs, "set cam pos saved the shot", 0);
        }
        PublishStatus();
        return true;  // while placing the middle mouse button is never a screenshot
    }

    bool OnHotkey(std::uint32_t a_code) {
        if (a_code == 0) return false;
        const auto is = [a_code](int a_key) { return a_key > 0 && a_code == static_cast<std::uint32_t>(a_key); };
        if (is(s_cfg.orbitKey)) {
            RequestOrbit(-1, "hotkey");
        } else if (is(s_cfg.eyeKey)) {
            if (!SceneTracker::PlayerSceneActive()) return false;  // eye keys belong to the player's scene only
            RequestEye(-1, "hotkey");
        } else if (is(s_cfg.eyeNextKey)) {
            if (!SceneTracker::PlayerSceneActive()) return false;
            NextEye("hotkey");
        } else if (is(s_cfg.shotSaveKey)) {
            DoSave("hotkey");
        } else if (is(s_cfg.shotRecallKey)) {
            DoRecall("hotkey");
        } else if (is(s_cfg.shotDeleteKey)) {
            DoDelete("hotkey");
        } else {
            return false;
        }
        PublishStatus();
        return true;
    }

    void NoteOwnLeave() { s_expectOwnEnd = true; }

    void OnFreeCamEnd() {
        const bool ownLeave = std::exchange(s_expectOwnEnd, false);
        if (s_pending != Req::kNone) {
            Event(std::format("{} request dropped - the free camera ended", Name(s_pending)));
            s_pending = Req::kNone;
            s_pendingShotKey.clear();
        }
        // 0.13.0: the free camera went off in the player's scene (the user's key, P+'s key, the SLCC cycle to Off): saved
        // stage shots leave it off until the scene ends. P+'s own exit at the scene's end is undone by OnPlayerSceneEnd.
        // TFCam's own hand-backs (NoteOwnLeave: the NPC pose camera, the idle / furniture camera at scene start) do not
        // count; their End can run queued, after OnPlayerSceneStart.
        if (!ownLeave && SceneTracker::PlayerSceneActive() && !s_autoEnterOff) {
            s_autoEnterOff = true;
            if (s_cfg.autoEnterShots) {
                Event("free camera off during the player's scene - saved stage shots leave it off until the scene ends");
            }
        }
        s_autoOwnsFree = false;
        DropModes("the free camera ended", true);
        s_freeCamEnded = true;  // reported as None from now on, until a free camera updates again
        PublishStatus();
    }

    void OnPlayerSceneStart() {
        AmbientCam::OnPlayerSceneStart();
        HandPoseCamToScene("the player's scene started");
        s_eye.preferred = 0;
        s_autoEnterOff  = false;
        PublishStatus();
        ScheduleTick();
    }

    void OnSceneChanged(bool a_keyChanged) {
        if (s_mode == TFCAM_API::kModeEye) {
            auto       partners   = LoadedPartners();
            const bool stillThere = std::ranges::any_of(partners, [](const ActorPtr& a) { return a->GetFormID() == s_eye.targetID; });
            if (!stillThere) {
                const int idx = FirstWithHead(partners, 0);
                if (idx >= 0) {
                    RetargetEye(partners[idx].get(), idx, "the previous partner left the scene");
                } else if (partners.empty()) {
                    LeaveEye("the partner left the scene", true);
                } else {
                    Notify("Eye view ended: no partner left with a head");
                    LeaveEye("the partner left the scene and no partner left has NPC Head [Head] / eye nodes", true);
                }
            }
        }
        if (a_keyChanged && s_mode == TFCAM_API::kModePreset) {
            // 0.15.0: a new stage with a saved shot shows it (one without falls back to the front view in the drive)
            ShotContext ctx;
            if (StageShot(ctx) && s_preset.view != 0) {
                s_preset.view = 0;
                Event(std::format("preset view: the saved shot of {}", ctx.key));
            }
        }
        if (a_keyChanged) {
            if (s_cfg.autoStageShots && s_mode == TFCAM_API::kModeFree && !s_placing) {
                s_autoKey   = StageKey(SceneTracker::GetPlayerScene());
                s_autoUntil = Now() + kAutoWindow;
            } else {
                s_autoKey.clear();
            }
            // 0.13.0: no free camera on - a saved shot for the new stage turns it on (the tick, once nothing is in the way).
            const auto& scene = SceneTracker::GetPlayerScene();
            if (s_cfg.autoEnterShots && s_cfg.autoStageShots && !s_autoEnterOff && !FreeCam::IsActive() && scene.stageKnown &&
                SceneShots::Has(StageKey(scene))) {
                ArmAutoEnter(StageKey(scene), false);
            } else if (!s_autoEnter.pose) {
                s_autoEnter = AutoEnter{};
            }
        }
        PublishStatus();
        ScheduleTick();
    }

    void OnPlayerSceneEnd() {
        s_autoKey.clear();
        s_autoEnterOff = false;
        if (!s_autoEnter.pose) s_autoEnter = AutoEnter{};
        if (s_pending == Req::kAutoShot && !s_pendingShotKey.starts_with("pose:")) {
            s_pending = Req::kNone;
            s_pendingShotKey.clear();
        }
        if (s_pending == Req::kEye || s_pending == Req::kPreset || s_pending == Req::kPlace) s_pending = Req::kNone;
        LeaveEye("the player's scene ended", true);
        LeaveOrbit("the player's scene ended", true);
        LeavePreset("the player's scene ended", true);
        EndPlacing("the player's scene ended");
        s_eye.preferred = 0;
        PublishStatus();
    }

    bool HasFurnitureShot() {
        const auto ctx = FurnitureContext();
        return ctx.ok && SceneShots::Has(ctx.key);
    }

    bool RecallFurnitureShot(const char* a_source) {
        const auto ctx = FurnitureContext();
        if (!ctx.ok) return false;
        const auto* shot = SceneShots::Find(ctx.key);
        if (!shot) return false;
        if (WaitForFreeCam(Req::kFurniture)) return true;
        LeaveOrbit("going to a furniture shot", false);
        LeaveEye("going to a furniture shot", false);
        LeavePreset("going to a furniture shot", false);
        StartBlend(ReadPose(CurrentFreeCam()), ShotToPose(*shot, ctx), s_cfg.shotBlend);
        Event(std::format("go to shot ({}) {} over {:.1f} s", a_source, ctx.key, s_cfg.shotBlend));
        return true;
    }

    void OnGameLoad(const char* a_why) {
        s_pending = Req::kNone;
        s_autoKey.clear();
        s_pendingShotKey.clear();
        s_autoEnter    = AutoEnter{};
        s_autoEnterOff = false;
        s_autoOwnsFree = false;
        s_expectOwnEnd = false;
        DropModes(a_why, true);
        if (s_pose.id) Event(std::format("pose cleared ({})", a_why));
        s_pose          = ActivePose{};
        s_eye.preferred = 0;
        PublishStatus();
    }

    // ---- TFCamAPI v2 (any thread) ----------------------------------------------------------------------------------
    // The return value is a prediction from the last status snapshot; the main-thread call checks again and shows the
    // reason when it refuses.

    bool ApiSetOrbit(std::int32_t a_state) {
        OnMain([a_state] {
            RequestOrbit(a_state, "SLUI");
            PublishStatus();
        });
        return true;
    }

    bool ApiSetEyeView(std::int32_t a_state) {
        const auto st = Snapshot();
        const bool ok = a_state == 0 || (a_state < 0 && st.mode == TFCAM_API::kModeEye) || (st.inPlayerScene && st.eyeAvailable);
        OnMain([a_state] {
            RequestEye(a_state, "SLUI");
            PublishStatus();
        });
        return ok;
    }

    bool ApiNextEyeTarget() {
        const bool ok = Snapshot().mode == TFCAM_API::kModeEye;
        OnMain([] {
            NextEye("SLUI");
            PublishStatus();
        });
        return ok;
    }

    void ApiSetSettings(const TFCAM_API::SceneCamSettings& a_settings) {
        const TFCAM_API::SceneCamSettings copy = a_settings;
        OnMain([copy] {
            s_cfg.orbitRadius = copy.orbitRadius;
            s_cfg.orbitHeight = copy.orbitHeight;
            s_cfg.orbitSpeed  = copy.orbitSpeed;
            s_cfg.orbitFOV    = copy.orbitFOV;
            s_cfg.eyeFOV      = copy.eyeFOV;
            ClampConfig();
            s_saveAt = Now() + kSaveDebounce;
            SKSE::log::info("SceneCam: settings from SLUI - radius {:.0f}, height {:.0f}, speed {:.0f}, orbit FOV {:.0f}, eye FOV {:.0f}",
                s_cfg.orbitRadius, s_cfg.orbitHeight, s_cfg.orbitSpeed, s_cfg.orbitFOV, s_cfg.eyeFOV);
            PublishStatus();
            ScheduleTick();
        });
    }

    bool ApiSaveShot() {
        const auto st = Snapshot();
        const bool ok = st.mode != TFCAM_API::kModeNone && ((st.inPlayerScene && st.stageKnown) || st.poseActive);
        OnMain([] {
            DoSave("SLUI");
            PublishStatus();
        });
        return ok;
    }

    bool ApiRecallShot() {
        const auto st = Snapshot();
        const bool ok = (st.inPlayerScene && st.stageKnown) ? st.hasStageShot : (st.poseActive && st.hasPoseShot);
        OnMain([] {
            DoRecall("SLUI");
            PublishStatus();
        });
        return ok;
    }

    bool ApiDeleteShot() {
        const auto st = Snapshot();
        const bool ok = (st.inPlayerScene && st.stageKnown) ? st.hasStageShot : (st.poseActive && st.hasPoseShot);
        OnMain([] {
            DoDelete("SLUI");
            PublishStatus();
        });
        return ok;
    }

    void ApiSetActivePose(std::uint32_t a_actor, const char* a_animEvent) {
        std::string event;
        if (a_animEvent) event.assign(a_animEvent, ::strnlen(a_animEvent, 95));
        OnMain([a_actor, event] {
            SetActivePose(a_actor, event);
            PublishStatus();
            ScheduleTick();
        });
    }

    void ApiGetStatus(TFCAM_API::SceneCamStatus* a_out) {
        if (!a_out || a_out->size < sizeof(TFCAM_API::SceneCamStatus)) return;
        std::lock_guard lock(s_statusLock);
        *a_out      = s_status;
        a_out->size = sizeof(TFCAM_API::SceneCamStatus);
    }

    // ---- TFCamAPI v3 (any thread) ----------------------------------------------------------------------------------
    void ApiSetSettings3(const TFCAM_API::SceneCamSettings3& a_settings) {
        if (a_settings.size != sizeof(TFCAM_API::SceneCamSettings3)) {
            SKSE::log::warn("SceneCam: SetSettings3 with size {} (TFCam expects {}) - ignored", a_settings.size,
                sizeof(TFCAM_API::SceneCamSettings3));
            return;
        }
        const TFCAM_API::SceneCamSettings3 copy = a_settings;
        OnMain([copy] {
            s_cfg.orbitRadius    = copy.orbitRadius;
            s_cfg.orbitHeight    = copy.orbitTilt;
            s_cfg.orbitSpeed     = copy.orbitSpeed;
            s_cfg.orbitFOV       = copy.orbitFOV;
            s_cfg.eyeFOV         = copy.eyeFOV;
            s_cfg.orbitAim       = copy.orbitAim;
            s_cfg.orbitSway      = copy.orbitSway;
            s_cfg.orbitTarget    = copy.orbitTarget;
            s_cfg.autoEnterShots = copy.autoEnterShots != 0;
            ClampConfig();
            s_saveAt = Now() + kSaveDebounce;
            SKSE::log::info("SceneCam: settings from SLUI - radius {:.0f}, tilt {:.0f}, speed {:.0f}, orbit FOV {:.0f}, eye FOV "
                            "{:.0f}, aim {:+.0f}, sway {:.0f}, orbit {}, automatic free camera {}",
                s_cfg.orbitRadius, s_cfg.orbitHeight, s_cfg.orbitSpeed, s_cfg.orbitFOV, s_cfg.eyeFOV, s_cfg.orbitAim,
                s_cfg.orbitSway, s_cfg.orbitTarget == 1 ? "the player" : (s_cfg.orbitTarget == 2 ? "console selection" : "auto"),
                s_cfg.autoEnterShots ? "on" : "off");
            PublishStatus();
            ScheduleTick();
        });
    }

    bool ApiGetStatus3(TFCAM_API::SceneCamStatus3* a_out) {
        if (!a_out || a_out->size != sizeof(TFCAM_API::SceneCamStatus3)) return false;
        std::lock_guard lock(s_statusLock);
        *a_out               = s_status3;
        a_out->size          = sizeof(TFCAM_API::SceneCamStatus3);
        a_out->settings.size = sizeof(TFCAM_API::SceneCamSettings3);
        return true;
    }

    bool ApiDeleteSceneShots() {
        const bool ok = Snapshot().sceneId[0] != '\0';
        RequestDeleteSceneShots();
        return ok;
    }

    // ---- TFCamAPI v4 (any thread) ----------------------------------------------------------------------------------
    bool ApiSetPresetView(std::int32_t a_state) {
        const auto st = Snapshot();
        const bool ok = a_state == 0 || (a_state < 0 && st.mode == TFCAM_API::kModePreset) || st.inPlayerScene;
        OnMain([a_state] {
            RequestPreset(a_state, "SLUI");
            PublishStatus();
        });
        return ok;
    }

    bool ApiStepPreset(std::int32_t a_step) {
        if (a_step == 0) return false;
        const bool ok = Snapshot().mode == TFCAM_API::kModePreset;
        OnMain([a_step] {
            StepPresetView(a_step < 0 ? -1 : 1, "SLUI");
            PublishStatus();
        });
        return ok;
    }

    bool ApiSetShotPlacing(std::int32_t a_state) {
        const auto st = Snapshot();
        const bool ok = a_state == 0 || (st.inPlayerScene && st.stageKnown);
        OnMain([a_state] {
            RequestPlacing(a_state, "SLUI");
            PublishStatus();
        });
        return ok;
    }

    bool ApiGetPreset4(TFCAM_API::SceneCamPreset4* a_out) {
        if (!a_out || a_out->size != sizeof(TFCAM_API::SceneCamPreset4)) return false;
        std::lock_guard lock(s_statusLock);
        if (s_status4.size == 0) {  // nothing published yet
            *a_out           = TFCAM_API::SceneCamPreset4{};
            a_out->view      = -1;
            a_out->viewCount = kPresetCount;
        } else {
            *a_out = s_status4;
        }
        a_out->size = sizeof(TFCAM_API::SceneCamPreset4);
        return true;
    }

    MenuStatus GetMenuStatus() {
        MenuStatus out;
        std::lock_guard lock(s_statusLock);
        out.status     = s_status;
        out.sceneShots = s_statusSceneShots;
        CopyStr(out.lastEvent, sizeof(out.lastEvent), s_statusEvent);
        return out;
    }

    void RequestDeleteSceneShots() {
        OnMain([] {
            DeleteSceneShots();
            PublishStatus();
        });
    }
}
