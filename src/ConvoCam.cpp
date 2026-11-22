#include "ConvoCam.h"
#include "CamCollision.h"
#include "CellEntryCam.h"
#include "FreeCamController.h"
#include "FreeCamMenu.h"
#include "SceneCam.h"
#include "SceneTracker.h"
#include "SlccBridge.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>

namespace ConvoCam {

    namespace {
        constexpr float kPi         = 3.14159265358979f;
        constexpr float kPitchLimit = 1.55f;

        Settings s_cfg;

        // ---- math ------------------------------------------------------------------------------------------------
        float WrapPi(float a) { return std::remainder(a, 2.0f * kPi); }

        float SmoothStep(float x) {
            x = std::clamp(x, 0.0f, 1.0f);
            return x * x * (3.0f - 2.0f * x);
        }

        float ExpWeight(float dt, float halfLife) { return dt <= 0.0f ? 0.0f : 1.0f - std::exp(-0.69314718f * dt / halfLife); }

        float Len2(float x, float y) { return std::sqrt(x * x + y * y); }

        RE::NiPoint3 Mix(const RE::NiPoint3& a, const RE::NiPoint3& b, float w) {
            return { a.x + (b.x - a.x) * w, a.y + (b.y - a.y) * w, a.z + (b.z - a.z) * w };
        }

        // Critically damped spring towards a target with a speed limit (the usual "smooth damp"). a_settle is about the time it
        // takes to arrive. Frame-rate independent.
        float SmoothDamp(float a_cur, float a_target, float& a_vel, float a_settle, float a_maxSpeed, float dt) {
            if (dt <= 0.0f) return a_cur;
            const float smoothTime = (std::max)(a_settle / 2.35f, 0.0001f);
            const float omega      = 2.0f / smoothTime;
            const float x          = omega * dt;
            const float e          = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
            float       change     = a_cur - a_target;
            const float original   = a_target;
            const float maxChange  = a_maxSpeed * smoothTime;
            change                 = std::clamp(change, -maxChange, maxChange);
            const float target     = a_cur - change;
            const float temp       = (a_vel + omega * change) * dt;
            a_vel                  = (a_vel - omega * temp) * e;
            float out              = target + (change + temp) * e;
            if ((original - a_cur > 0.0f) == (out > original)) {
                out   = original;
                a_vel = 0.0f;
            }
            return out;
        }

        // The same on an angle: the short way round.
        float SmoothDampAngle(float a_cur, float a_target, float& a_vel, float a_settle, float a_maxSpeed, float dt) {
            const float d = WrapPi(a_target - a_cur);
            return WrapPi(a_cur + SmoothDamp(0.0f, d, a_vel, a_settle, a_maxSpeed, dt));
        }

        // TFCam's convention: forward = (sin yaw cos pitch, cos yaw cos pitch, -sin pitch).
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

        float g_realDt = 0.0f;  // the last frame's real time, paused or not (a trade menu's shot runs on it)
        bool  g_turnPending = false;  // a change of shot is under way: its time goes in the log when it lands

        // Own frame time for the drive: capped at 0.1 s, 0 while the game is paused.
        float FrameDt() {
            static double s_last = 0.0;
            const double  now    = Now();
            float         dt     = s_last > 0.0 ? static_cast<float>(now - s_last) : 0.0f;
            s_last               = now;
            dt                   = std::clamp(dt, 0.0f, 0.1f);
            g_realDt             = dt;
            auto* ui             = RE::UI::GetSingleton();
            if (ui && ui->GameIsPaused()) dt = 0.0f;
            return dt;
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

        // The camera from before the session, in the player's frame (so it follows a player who moved).
        struct PreShot {
            float x = 0, y = 0, z = 0, yawRel = 0, pitch = 0, roll = 0, fov = 0;
        };

        Pose ReadPose(const RE::FreeCameraState* fcs) {
            Pose p;
            p.pos   = fcs->translation;
            p.pitch = WrapPi(fcs->rotation.x);  // 0.17.1 (Claude): the vanilla free camera keeps its pitch in 0..2 pi
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

        PreShot ToPre(const Pose& p) {
            auto*              player = RE::PlayerCharacter::GetSingleton();
            const auto         a      = player->GetPosition();
            const float        h      = player->GetAngleZ();
            const RE::NiPoint3 fwd{ std::sin(h), std::cos(h), 0.0f };
            const RE::NiPoint3 right{ std::cos(h), -std::sin(h), 0.0f };
            const RE::NiPoint3 v{ p.pos.x - a.x, p.pos.y - a.y, p.pos.z - a.z };
            PreShot            s;
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
            auto*              player = RE::PlayerCharacter::GetSingleton();
            const auto         a      = player->GetPosition();
            const float        h      = player->GetAngleZ();
            const RE::NiPoint3 fwd{ std::sin(h), std::cos(h), 0.0f };
            const RE::NiPoint3 right{ std::cos(h), -std::sin(h), 0.0f };
            Pose               p;
            p.pos   = { a.x + right.x * s.x + fwd.x * s.y, a.y + right.y * s.x + fwd.y * s.y, a.z + s.z };
            p.yaw   = WrapPi(h + s.yawRel);
            p.pitch = s.pitch;
            p.roll  = s.roll;
            p.fov   = s.fov;
            return p;
        }

        // ---- session ------------------------------------------------------------------------------------------------
        enum class St { kOff, kStarting, kRunning, kReturning };
        enum class Shot { kOverPlayer, kReverse };

        struct Session {
            St               st        = St::kOff;
            double           startedAt = 0.0;
            bool             havePre   = false;
            bool             seenMenu  = false;     // the Dialogue Menu itself has been open (the end is then the menu closing)
            double           lastTalk  = 0.0;       // the last time the menu was open or the NPC was saying a line
            bool             dialogueSeen = false;  // the Dialogue Menu has been open since this session armed
            double           armedAt   = 0.0;
            bool             firstPerson = false;  // the game's camera was first person when the conversation began
            PreShot          pre{};
            RE::ObjectRefHandle speaker{};
            std::string      speakerName;
            // smoothed positions of the two speakers' eyes
            bool         seeded = false;
            RE::NiPoint3 player{}, npc{};
            // the camera: cylindrical about the midpoint, plus the point it looks at
            float        ang = 0, rad = 0, z = 0, fov = 0;
            float        vAng = 0, vRad = 0, vZ = 0, vFov = 0;
            RE::NiPoint3 look{};
            RE::NiPoint3 vLook{};
            Shot         shot      = Shot::kOverPlayer;
            double       shotSince = 0.0;
            bool         tradeHold = false;  // a menu opened from the talk (barter, gift ...) holds the camera on one shot
            // --Claude 11-22: a picked line - the camera stays on the player until the NPC's voice starts
            const RE::TESTopicInfo* lastInfo     = nullptr;
            double                  pickAt       = 0.0;
            bool                    npcVoiceSeen = true;
            double                  nextVoiceLog = 0.0;
            // --Claude 11-22: turning the player to face the NPC
            bool turning    = false;
            bool turnLogged = false;
            int  turnBacks  = 0;
            float        tradePush = 0.0f;   // how far the trade shot was moved out to keep the player's head off the NPC
            bool         tradeBlocked = false;  // 11-22: a wall in the way of the shoulder trade shot - face to face for this menu
            float        settle    = 1.6f;  // seconds the damping takes to arrive (the entry is slower than a shot change)
            float        clock     = 0.0f;
            // return
            bool  retFromSet = false;
            Pose  retFrom{};
            float retT   = 0.0f;
            float retDur = 1.0f;
        };
        Session s_s;

        std::mutex  s_statusLock;
        MenuStatus  s_status{};
        std::string s_lastEvent;
        std::atomic<bool> s_tickQueued{ false };

        void Event(std::string text) {
            SKSE::log::info("ConvoCam: {}", text);
            s_lastEvent = std::move(text);
        }

        void CopyStr(char* dst, std::size_t size, const std::string& src) {
            const std::size_t n = (std::min)(src.size(), size - 1);
            std::memcpy(dst, src.data(), n);
            dst[n] = '\0';
        }

        RE::FreeCameraState* LiveFreeCam() {
            auto* cam = Cam();
            if (!cam || !cam->IsInFreeCameraMode() || !FreeCam::TFCamDriving()) return nullptr;
            auto* state = cam->currentState.get();
            return state ? static_cast<RE::FreeCameraState*>(state) : nullptr;
        }

        // --Claude 11-22: the player's head (FaceGen node: head, hair, eyes) hidden while a face to face camera is in or behind it
        // (Distance moved out) - the same cull as the scene camera's eye view. Re-checked every frame (a 3D reload makes a new node).
        RE::NiPointer<RE::NiAVObject> s_headCulled;
        bool                          s_headCulledPrev = false;

        void ShowPlayerHead(const char* a_why) {
            if (!s_headCulled) return;
            s_headCulled->SetAppCulled(s_headCulledPrev);
            SKSE::log::info("ConvoCam: your head shown again ({})", a_why);
            s_headCulled.reset();
        }

        void HidePlayerHead() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            RE::NiAVObject* node = player ? player->GetFaceNodeSkinned() : nullptr;
            if (!node) return;
            if (s_headCulled.get() == node) {
                if (!node->GetAppCulled()) node->SetAppCulled(true);
                return;
            }
            ShowPlayerHead("a new head node");
            s_headCulled     = RE::NiPointer<RE::NiAVObject>(node);
            s_headCulledPrev = node->GetAppCulled();
            node->SetAppCulled(true);
            SKSE::log::info("ConvoCam: your head hidden - the face to face camera is in or behind it");
        }

        void ResetSession() {
            ShowPlayerHead("the conversation camera ended");
            s_s = Session{};
        }

        // The dialogue menu hold (defined above FrameCheck): a Dialogue Menu asked to open waits until the free camera is in.
        void   ReleaseHeld(const std::string& a_why);
        double HeldFor();  // seconds the held menu has waited; -1 = nothing held

        // ---- the conversation ---------------------------------------------------------------------------------------
        bool DialogueOpen() {
            auto* mtm = RE::MenuTopicManager::GetSingleton();
            auto* ui  = RE::UI::GetSingleton();
            return mtm && ui && mtm->menuOpen && ui->IsMenuOpen("Dialogue Menu");
        }

        // A menu the talk opened on top of itself: trading, a gift, the NPC's things, training.
        bool TradeMenuOpen() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) return false;
            for (const char* name : { "BarterMenu", "GiftMenu", "ContainerMenu", "Training Menu" })
                if (ui->IsMenuOpen(name)) return true;
            return false;
        }

        // The NPC is saying a line (the dialogue manager holds a current topic info only then).
        bool NpcTalking() {
            auto* mtm = RE::MenuTopicManager::GetSingleton();
            return mtm && mtm->currentTopicInfo != nullptr;
        }

        // The NPC of this session is saying a line to the player - with or without the Dialogue Menu (scenes speak without it).
        bool OurSpeakerTalking() {
            auto* mtm = RE::MenuTopicManager::GetSingleton();
            if (!mtm || mtm->currentTopicInfo == nullptr) return false;
            auto spk = mtm->speaker.get();
            auto ours = s_s.speaker.get();
            return spk && ours && spk.get() == ours.get();
        }

        // --Claude 11-22: the actor's voice is playing - its high process voice sound handles. The current topic info is set from
        // the moment a line is picked, while a player-voice mod (DBReV) is still saying the player's own line and holds the reply.
        bool VoicePlaying(RE::Actor* a) {
            if (!a) return false;
            auto* proc = a->GetActorRuntimeData().currentProcess;
            auto* high = proc ? proc->high : nullptr;
            if (!high) return false;
            for (auto& h : high->soundHandles) {
                if (h.IsValid() && h.IsPlaying()) return true;
            }
            return false;
        }

        RE::Actor* SpeakerActor() {
            auto* mtm = RE::MenuTopicManager::GetSingleton();
            if (!mtm) return nullptr;
            auto ref = mtm->speaker.get();
            auto* a  = ref ? ref->As<RE::Actor>() : nullptr;
            return (a && !a->IsPlayerRef()) ? a : nullptr;
        }

        // Eye point of an actor: the head node when there is one, else a share of its height.
        RE::NiPoint3 EyePoint(RE::Actor* a) {
            const auto  pos    = a->GetPosition();
            float       height = a->GetHeight();
            if (!(height > 20.0f) || !std::isfinite(height)) height = 120.0f * std::clamp(a->GetScale(), 0.3f, 3.0f);
            RE::NiPoint3 p{ pos.x, pos.y, pos.z + 0.93f * height };
            if (auto* root = a->Get3D(false)) {
                if (auto* head = root->GetObjectByName(RE::BSFixedString("NPC Head [Head]"))) {
                    const auto h = head->world.translate;
                    if (std::isfinite(h.x) && std::isfinite(h.y) && std::isfinite(h.z) && h.z > pos.z + 20.0f) p = h;
                }
            }
            return p;
        }

        // ---- player controls ------------------------------------------------------------------------------------------
        // The engine keeps ONE stored copy of the player's enabled controls (ControlMap::storedControls): a menu stores them
        // and gives them back when it closes, and so does the free camera. A free camera that begins while the Dialogue Menu is
        // open stores the dialogue's already-reduced controls over the dialogue's own copy; when the dialogue closes and the
        // free camera ends after it, the reduced set is what comes back - the player cannot move afterwards (2026-10-24).
        // So the controls as they are in plain gameplay are remembered, written back as the stored copy just before the free
        // camera ends, and checked for a while after.
        std::uint32_t s_gameplayControls = 0;
        bool          s_haveControls     = false;
        double        s_checkControlsUntil = 0.0;

        std::uint32_t* EnabledControls() {
            auto* cm = RE::ControlMap::GetSingleton();
            return cm ? reinterpret_cast<std::uint32_t*>(&cm->GetRuntimeData().enabledControls) : nullptr;
        }

        std::uint32_t* StoredControls() {
            auto* cm = RE::ControlMap::GetSingleton();
            return cm ? reinterpret_cast<std::uint32_t*>(&cm->GetRuntimeData().storedControls) : nullptr;
        }

        bool PlainGameplay() {
            auto* ui = RE::UI::GetSingleton();
            if (!ui || ui->GameIsPaused()) return false;
            for (const char* name : { "Dialogue Menu", "Cursor Menu", "Console", "Loading Menu", "MainMenu", "BarterMenu", "ContainerMenu",
                                      "GiftMenu", "Journal Menu", "MapMenu", "InventoryMenu", "MagicMenu", "StatsMenu", "LevelUp Menu",
                                      "Sleep/Wait Menu", "MessageBoxMenu", "TweenMenu", "Lockpicking Menu", "BookMenu", "RaceSex Menu" }) {
                if (ui->IsMenuOpen(name)) return false;
            }
            return !FreeCam::IsActive();
        }

        void RememberControls() {
            if (!PlainGameplay()) return;
            if (const auto* now = EnabledControls()) {
                constexpr std::uint32_t kMovement = 1u;  // UserEvents::USER_EVENT_FLAG::kMovement
                if (*now & kMovement) {  // a locked state (a script's cutscene) is not what the player should get back
                    s_gameplayControls = *now;
                    s_haveControls     = true;
                }
            }
        }

        // For the log: what the control map holds at a moment of the session.
        void LogControls(const char* a_when) {
            auto* cm = RE::ControlMap::GetSingleton();
            if (!cm) return;
            const auto& rt = cm->GetRuntimeData();
            std::string stack;
            for (const auto id : rt.contextPriorityStack) stack += std::format("{} ", static_cast<int>(id));
            SKSE::log::info("ConvoCam: controls {} - enabled {:#x} stored {:#x}, context stack [ {}], ignoreKeyboardMouse {}", a_when,
                rt.enabledControls.underlying(), rt.storedControls.underlying(), stack, rt.ignoreKeyboardMouse);
        }

        // After it: anything the player had in plain gameplay and does not have now is given back (never taken away).
        void CheckControlsAfter() {
            if (!s_haveControls || Now() > s_checkControlsUntil) return;
            if (!PlainGameplay()) return;
            if (auto* now = EnabledControls()) {
                const std::uint32_t missing = s_gameplayControls & ~*now;
                if (missing) {
                    SKSE::log::info("ConvoCam: controls {:#x} were missing after the conversation - given back ({:#x} -> {:#x})", missing, *now,
                        *now | missing);
                    *now |= missing;
                }
            }
        }

        // Where the game's own camera would be again: behind the player, looking the way the player faces, at the distance and
        // pitch it had before - not the pose it had when the conversation began (the player turns to the NPC, and a swing back to
        // the old direction was the "rolls around, looks at the ground" return).
        Pose ChasePose() {
            auto*       player = RE::PlayerCharacter::GetSingleton();
            const float h      = player->GetAngleZ();
            const RE::NiPoint3 eye = EyePoint(player);
            Pose        p;
            p.yaw  = h;
            p.roll = 0.0f;
            p.fov  = s_s.pre.fov > 1.0f ? s_s.pre.fov : GetFOV();
            if (s_s.firstPerson) {
                p.pos   = { eye.x, eye.y, eye.z };
                p.pitch = 0.0f;
                return p;
            }
            float dist = std::hypot(s_s.pre.x, s_s.pre.y);
            if (!(dist > 40.0f) || !std::isfinite(dist)) dist = 150.0f;
            dist        = std::clamp(dist, 60.0f, 400.0f);
            float pitch = s_s.pre.pitch;
            if (!std::isfinite(pitch) || std::fabs(pitch) > 0.9f) pitch = 0.12f;
            p.pitch = pitch;
            p.pos   = { eye.x - std::sin(h) * dist * std::cos(pitch), eye.y - std::cos(h) * dist * std::cos(pitch), eye.z + dist * std::sin(pitch) };
            return p;
        }

        // ---- shots ----------------------------------------------------------------------------------------------------
        struct Target {
            RE::NiPoint3 pos;
            RE::NiPoint3 look;
            float        fov;
        };

        // Over the shoulder of a foreground speaker F, looking at T.
        Target OverShoulder(const RE::NiPoint3& F, const RE::NiPoint3& T, float side) {
            const float        d  = (std::max)(Len2(T.x - F.x, T.y - F.y), 30.0f);
            const RE::NiPoint3 f{ (T.x - F.x) / d, (T.y - F.y) / d, 0.0f };
            const RE::NiPoint3 r{ f.y, -f.x, 0.0f };
            Target             t;
            t.pos  = { F.x - f.x * s_cfg.distance + r.x * side * s_cfg.shoulder, F.y - f.y * s_cfg.distance + r.y * side * s_cfg.shoulder,
                F.z + s_cfg.height };
            t.look = { T.x - r.x * side * s_cfg.lookShift, T.y - r.y * side * s_cfg.lookShift, T.z - 3.0f };
            t.fov  = s_cfg.fovNpc;
            return t;
        }

        // Both speakers from the side.
        Target TwoShot(const RE::NiPoint3& P, const RE::NiPoint3& N, float side) {
            const float        d = (std::max)(Len2(N.x - P.x, N.y - P.y), 30.0f);
            const RE::NiPoint3 f{ (N.x - P.x) / d, (N.y - P.y) / d, 0.0f };
            const RE::NiPoint3 r{ f.y, -f.x, 0.0f };
            const RE::NiPoint3 m = Mix(P, N, 0.5f);
            Target             t;
            const float        out = d * 0.5f + s_cfg.distance * 0.9f;
            t.pos  = { m.x + r.x * side * out, m.y + r.y * side * out, m.z + s_cfg.height * 0.5f };
            t.look = { m.x, m.y, m.z - 2.0f };
            t.fov  = s_cfg.fovWide;
            return t;
        }

        // --Claude 11-22: from the player's eyes to the NPC. The camera stands a little in front of the player's face on the line
        // to the NPC (the player is turned to face them, faceSpeaker), so their own head is behind it, and looks at the NPC's face
        // and chest: the NPC stands in the open middle of a trade page, where the side shots hid them behind its panels.
        // 11-22: the eye points are the head BONES, which sit at the jaw, and the lens was
        // the over-the-shoulder one. The frame now runs from above the NPC's head to the hips (centre kFaceCentre from the head
        // bone, half height kFaceHalf), the camera at eye height, and the lens is worked out for the real distance (FitFov).
        constexpr float kFaceCentre = -13.0f;
        constexpr float kFaceHalf   = 60.0f;   // head to hips: an FOV of about 95 at ~100 units from the face shows about this much

        // the screen's width / height from the live world camera's frustum (16:9 if it cannot be read)
        float ScreenAspect() {
            if (!REL::Module::IsVR()) {
                if (auto* nc = RE::Main::WorldRootCamera()) {
                    const auto& fr = nc->GetRuntimeData2().viewFrustum;
                    const float w = fr.fRight - fr.fLeft, h = fr.fTop - fr.fBottom;
                    if (std::isfinite(w) && std::isfinite(h) && w > 0.0f && h > 0.0f) return std::clamp(w / h, 1.0f, 3.0f);
                }
            }
            return 16.0f / 9.0f;
        }

        // the game's (horizontal) FOV that shows a_half units above and below the look point from where the camera is
        float FitFov(const Target& t, float a_half) {
            const float dx = t.look.x - t.pos.x, dy = t.look.y - t.pos.y, dz = t.look.z - t.pos.z;
            const float dist = (std::max)(std::sqrt(dx * dx + dy * dy + dz * dz), 20.0f);
            const float tanH = a_half / dist * ScreenAspect();
            return std::clamp(2.0f * std::atan(tanH) * 57.2958f, 40.0f, 100.0f);
        }

        Target FaceToFace(const RE::NiPoint3& P, const RE::NiPoint3& N) {
            constexpr float kFwd = 22.0f;
            const float     d    = (std::max)(Len2(N.x - P.x, N.y - P.y), 30.0f);
            const float     fx = (N.x - P.x) / d, fy = (N.y - P.y) / d;
            const float     fwd = (std::min)(kFwd, d * 0.35f);
            Target          t;
            t.pos  = { P.x + fx * fwd, P.y + fy * fwd, P.z + 6.0f };
            t.look = { N.x, N.y, N.z + kFaceCentre };
            t.fov  = FitFov(t, kFaceHalf);
            return t;
        }

        // --Claude 11-21: the shot under a menu the talk opened (barter, gift, training), from the Trade menus settings.
        Target TradeTarget() {
            const float side = s_cfg.swapSide ? -1.0f : 1.0f;
            Target      t    = (s_cfg.tradeShot == 3 || s_s.tradeBlocked) ? FaceToFace(s_s.player, s_s.npc) :
                               s_cfg.tradeShot == 1 ? TwoShot(s_s.player, s_s.npc, side) : OverShoulder(s_s.player, s_s.npc, side);
            if (s_cfg.tradeShot == 2) {  // closer on the NPC: the camera 40 % of the way to what it looks at, a longer lens
                t.pos = Mix(t.pos, t.look, 0.4f);
                t.fov = (std::max)(20.0f, t.fov - 8.0f);
            }
            const RE::NiPoint3 back{ t.pos.x - t.look.x, t.pos.y - t.look.y, t.pos.z - t.look.z };
            t.pos = { t.look.x + back.x * s_cfg.tradeDistance, t.look.y + back.y * s_cfg.tradeDistance, t.look.z + back.z * s_cfg.tradeDistance };
            if (s_cfg.tradeTurn != 0.0f) {  // round the midpoint of the two
                const auto  m = Mix(s_s.player, s_s.npc, 0.5f);
                const float a = s_cfg.tradeTurn * 0.01745329f, c = std::cos(a), s = std::sin(a);
                const float x = t.pos.x - m.x, y = t.pos.y - m.y;
                t.pos.x = m.x + x * c - y * s;
                t.pos.y = m.y + x * s + y * c;
            }
            // 11-22: over your shoulder, the default shoulder offset runs the line to the NPC only ~17 units past the middle of
            // your head when the two stand ~100 apart, so your head covers them. Under the trade menu the camera goes out to the
            // side (pivoting on the NPC) until that line clears your head by kTradeClear - looked at from above.
            // 11-22: to the side, as the camera looks (+ = right)
            if (s_cfg.tradeSide != 0.0f) {
                const float lx = t.look.x - t.pos.x, ly = t.look.y - t.pos.y, ll = Len2(lx, ly);
                if (ll > 1.0f) {
                    t.pos.x += ly / ll * s_cfg.tradeSide;
                    t.pos.y += -lx / ll * s_cfg.tradeSide;
                }
            }
            s_s.tradePush = 0.0f;
            if ((s_cfg.tradeShot == 0 || s_cfg.tradeShot == 2) && !s_s.tradeBlocked) {  // 11-22: not face to face (in front of the head)
                constexpr float kTradeClear = 30.0f;
                const auto&     P = s_s.player;
                const auto&     N = s_s.npc;
                for (int i = 0; i < 8; ++i) {
                    const float ux = N.x - t.pos.x, uy = N.y - t.pos.y;
                    const float len = Len2(ux, uy);
                    if (len < 1.0f) break;
                    const float nx = ux / len, ny = uy / len;
                    const float along = (P.x - t.pos.x) * nx + (P.y - t.pos.y) * ny;
                    if (along <= 0.0f || along >= len) break;  // you are not between the camera and the NPC
                    const float s = nx * (P.y - t.pos.y) - ny * (P.x - t.pos.x);  // your distance off the line, + = to its left
                    if (std::fabs(s) >= kTradeClear) break;
                    // a sideways step of the camera moves the line at you by (len - along) / len of it (the NPC is the pivot)
                    const float need = (kTradeClear - std::fabs(s)) * len / (std::max)(len - along, 1.0f);
                    const float sgn  = s >= 0.0f ? -1.0f : 1.0f;  // away from your side of the line
                    t.pos.x += -ny * sgn * need;
                    t.pos.y += nx * sgn * need;
                    s_s.tradePush += need;
                }
            }
            t.pos.z += s_cfg.tradeHeight;
            if (s_cfg.tradeFov > 1.0f) {
                t.fov = s_cfg.tradeFov;
            } else if (s_cfg.tradeShot == 3 || s_s.tradeBlocked) {
                t.fov = FitFov(t, kFaceHalf);   // 11-22: face to face - the lens for where Distance / Side / Height put the camera
            }
            return t;
        }

        const char* TradeShotName() {
            if (s_s.tradeBlocked) return "face to face (a wall was in the way)";
            return s_cfg.tradeShot == 3 ? "face to face" : s_cfg.tradeShot == 1 ? "both from the side" : s_cfg.tradeShot == 2 ? "close on the NPC" : "over your shoulder";
        }

        Target ShotTarget(Shot shot) {
            if (s_s.tradeHold) return TradeTarget();
            const float side = s_cfg.swapSide ? -1.0f : 1.0f;
            if (shot == Shot::kOverPlayer) return OverShoulder(s_s.player, s_s.npc, side);
            return s_cfg.reverseKind == 1 ? TwoShot(s_s.player, s_s.npc, side) : OverShoulder(s_s.npc, s_s.player, side);
        }

        const char* ShotName(Shot shot) {
            if (shot == Shot::kOverPlayer) return "over your shoulder";
            return s_cfg.reverseKind == 1 ? "wide two-shot" : "over the NPC's shoulder";
        }

        // ---- start / return -------------------------------------------------------------------------------------------
        void FinishReturn() {
            SKSE::log::info("ConvoCam: giving the free camera back");
            if (FreeCam::TFCamDriving()) SceneCam::NoteOwnLeave();
            LogControls("before the free camera ends");
            s_checkControlsUntil = Now() + 3.0;
            SlccBridge::LeaveFreeFly("Conversation camera");
            ResetSession();
        }

        void BeginReturn(float dur, const char* why) {
            if (s_s.st == St::kOff || s_s.st == St::kReturning) return;
            if (!Cam() || !Cam()->IsInFreeCameraMode()) {
                Event(std::format("{} - the free camera is already off", why));
                ResetSession();
                return;
            }
            if (dur <= 0.0f || !s_s.havePre || !LiveFreeCam()) {
                Event(std::format("{} - camera back now", why));
                FinishReturn();
                return;
            }
            s_s.st         = St::kReturning;
            s_s.retFromSet = false;
            s_s.retT       = 0.0f;
            s_s.retDur     = dur;
            Event(std::format("{} - camera back over {:.1f} s", why, dur));
        }

        // The free camera has to begin BEFORE the Dialogue Menu opens and end AFTER it closes. The engine keeps one stored copy of the
        // player's controls and the input contexts as a stack, so the menu and the free camera must nest, not interleave: a free
        // camera that began under the open menu ended with the menu's reduced controls and left the player unable to move
        // (2026-10-24). So the session arms on the player's activation of the NPC (TESActivateEvent), a moment before the dialogue
        // opens, and gives the camera back if no dialogue comes.
        void Arm(RE::Actor* spk, const char* a_how = nullptr) {
            if (!spk) return;
            s_s             = Session{};
            s_s.st          = St::kStarting;
            s_s.startedAt   = Now();
            s_s.armedAt     = s_s.startedAt;
            s_s.speaker     = spk->CreateRefHandle();
            if (auto* cam = Cam(); cam && cam->currentState) s_s.firstPerson = cam->currentState->id == RE::CameraStates::kFirstPerson;
            const char* nm  = spk->GetDisplayFullName();
            s_s.speakerName = nm ? nm : "";
            if (a_how) {
                Event(std::format("{} {} - camera starting", s_s.speakerName.empty() ? "an NPC" : s_s.speakerName, a_how));
            } else {
                Event(std::format("you activated {} - camera starting", s_s.speakerName.empty() ? "an NPC" : s_s.speakerName));
            }
            LogControls("before the free camera begins");
            SlccBridge::RequestEnterFreeFly("Conversation camera");
        }

        // nullptr = nothing in the way of arming (a_actor: the NPC the player just activated).
        const char* ArmBlock(RE::Actor* a_actor) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!s_cfg.enabled) return "off";
            if (s_s.st != St::kOff) return "a session is running";
            if (!player || !a_actor || a_actor->IsPlayerRef()) return "not an NPC";
            if (a_actor->IsDead() || a_actor->IsInCombat() || player->IsInCombat() || a_actor->IsHostileToActor(player)) return "dead / hostile / in combat";
            if (player->IsSneaking()) return "sneaking (pickpocket)";
            if (DialogueOpen()) return "a dialogue is open already";
            if (!PlainGameplay()) return "a menu is open";
            if (FreeCam::IsActive()) return "free camera is on";
            if (SceneTracker::PlayerSceneActive()) return "player in a SexLab scene";
            if (CellEntryCam::IsRunning()) return "cell-entry camera running";
            auto* cam = Cam();
            auto* st  = cam ? cam->currentState.get() : nullptr;
            if (!st) return "no camera state";
            using CS = RE::CameraStates;
            if (st->id == CS::kThirdPerson) return nullptr;
            if (st->id == CS::kFirstPerson && !s_cfg.thirdPersonOnly) return nullptr;
            return "not in the third-person camera";
        }

        // The player activated something: an NPC about to talk? (Game thread; the work runs on the main thread.)
        class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent> {
        public:
            static ActivateSink* Get() {
                static ActivateSink s;
                return &s;
            }
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* evt, RE::BSTEventSource<RE::TESActivateEvent>*) override {
                if (!evt || !s_cfg.enabled || !evt->actionRef || !evt->objectActivated) return RE::BSEventNotifyControl::kContinue;
                // An NPC activating the player (a force-greet may do this): log only, the per-frame check starts the camera.
                if (!evt->actionRef->IsPlayerRef()) {
                    if (evt->objectActivated->IsPlayerRef()) {
                        const char* nm = evt->actionRef->GetDisplayFullName();
                        SKSE::log::info("ConvoCam: {} activated the player", nm ? nm : "an actor");
                    }
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* actor = evt->objectActivated->As<RE::Actor>();
                if (!actor) return RE::BSEventNotifyControl::kContinue;
                const auto handle = actor->CreateRefHandle();
                const auto arm    = [handle] {
                    auto  ref = handle.get();
                    auto* a   = ref ? ref->As<RE::Actor>() : nullptr;
                    // The camera starts from the Activate release (BeginPending); this event only logs.
                    SKSE::log::info("ConvoCam: activation event for {}", a ? (a->GetDisplayFullName() ? a->GetDisplayFullName() : "an actor") : "?");
                };
                // Right here when this is the game's own thread: the dialogue opens within a frame of the activation, and a task
                // queued for the next frame came after it for most NPCs (2026-10-24).
                if (FreeCam::OnMainThreadNow()) {
                    arm();
                } else if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask(arm);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        void PublishStatus(const char* block) {
            MenuStatus st;
            std::string state;
            switch (s_s.st) {
            case St::kOff: state = block ? std::string("waiting: ") + block : std::string("no conversation"); break;
            case St::kStarting: state = "starting"; break;
            case St::kRunning: state = s_s.dialogueSeen ? "running" : "waiting for the dialogue to open"; break;
            case St::kReturning: state = "handing the camera back"; break;
            }
            CopyStr(st.state, sizeof(st.state), state);
            if (s_s.st == St::kRunning) CopyStr(st.shot, sizeof(st.shot), ShotName(s_s.shot));
            CopyStr(st.speaker, sizeof(st.speaker), s_s.speakerName);
            CopyStr(st.lastEvent, sizeof(st.lastEvent), s_lastEvent);
            std::lock_guard lock(s_statusLock);
            s_status = st;
        }

        // Passive diagnostics (whatever the setting): after every Activate press on an NPC, log whether the dialogue opens.
        struct PressWatch {
            double      at = 0.0;
            int         stage = 0;  // 0 = idle, 1..3 = next check
            std::string who;
        };
        PressWatch s_watch;

        void RunPressWatch(double a_now) {
            if (s_watch.stage == 0) return;
            static const double k_at[] = { 0.0, 0.3, 1.0, 2.5 };
            if (a_now - s_watch.at < k_at[s_watch.stage]) return;
            auto* mtm = RE::MenuTopicManager::GetSingleton();
            auto* ui  = RE::UI::GetSingleton();
            SKSE::log::info("ConvoCam: watch +{:.1f}s after pressing on {} - dialogue menu {}, manager menuOpen {}, speaker {}, topic info {}, camera session {}",
                k_at[s_watch.stage], s_watch.who, ui && ui->IsMenuOpen("Dialogue Menu") ? "OPEN" : "closed", mtm ? mtm->menuOpen : false,
                mtm && mtm->speaker.get() ? "set" : "none", mtm && mtm->currentTopicInfo ? "set" : "none", s_s.st == St::kOff ? "off" : "active");
            if (++s_watch.stage > 3) s_watch.stage = 0;
        }

        // The game pans the camera when the mouse is pushed to the screen edge in a dialogue (fPCDialogueLookSpeed). It fights the
        // conversation camera, so the setting is held at 0 while the camera films (and always, when noEdgePan is ticked).
        void ApplyEdgePan() {
            static RE::Setting* s_set = nullptr;
            static float        s_orig = 0.0f;
            static bool         s_off  = false;
            if (!s_set) {
                auto* ini = RE::INISettingCollection::GetSingleton();
                s_set     = ini ? ini->GetSetting("fPCDialogueLookSpeed:Controls") : nullptr;
                if (!s_set) return;
            }
            const bool want = s_cfg.noEdgePan || (s_cfg.enabled && s_s.st != St::kOff);
            if (want && !s_off) {
                s_orig       = s_set->data.f;
                s_set->data.f = 0.0f;
                s_off        = true;
                SKSE::log::info("ConvoCam: mouse edge-pan in dialogue off (fPCDialogueLookSpeed {:.2f} -> 0)", s_orig);
            } else if (!want && s_off) {
                s_set->data.f = s_orig;
                s_off         = false;
                SKSE::log::info("ConvoCam: mouse edge-pan in dialogue back (fPCDialogueLookSpeed {:.2f})", s_orig);
            }
        }

        void Tick() {
            s_tickQueued = false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            const double now = Now();
            ApplyEdgePan();
            if (!player || !player->Is3DLoaded()) {
                if (s_s.st != St::kOff) ResetSession();
                return;
            }
            RunPressWatch(now);
            // A held Dialogue Menu never waits long: Drive lets it through once the camera is in, else it goes through here.
            if (const double held = HeldFor(); held >= 0.0) {
                if (held > 2.0) {
                    ReleaseHeld("the camera did not come in time");
                } else if (s_s.st == St::kOff && held > 0.5) {
                    ReleaseHeld("no camera session for it");
                }
            }
            if (s_s.st == St::kOff) {
                RememberControls();
                CheckControlsAfter();
                PublishStatus(s_cfg.enabled ? nullptr : "off");
                return;
            }
            const bool menu     = DialogueOpen();
            const bool lineTalk = OurSpeakerTalking();
            if (menu || lineTalk) s_s.lastTalk = now;
            if (menu && !s_s.seenMenu) {
                s_s.seenMenu = true;
                Event("the dialogue opened");
            }
            if ((menu || lineTalk) && !s_s.dialogueSeen) {
                s_s.dialogueSeen = true;
                if (!menu) Event("the NPC is talking without the dialogue menu (a scene)");
            }
            const bool free = Cam() && Cam()->IsInFreeCameraMode();
            if (s_s.st == St::kStarting && !free && now - s_s.startedAt > 2.0) {
                Event("the free camera never started");
                ResetSession();
                return;
            }
            if (s_s.st != St::kStarting && !free) {
                Event("the free camera was left");
                ResetSession();
                return;
            }
            if (s_s.st == St::kStarting || s_s.st == St::kRunning) {
                if (!s_cfg.enabled) {
                    BeginReturn(s_cfg.blendOut, "turned off");
                } else if (s_s.seenMenu && !menu) {
                    BeginReturn(s_cfg.blendOut, "the conversation ended");
                } else if (s_s.dialogueSeen && !s_s.seenMenu && !lineTalk && now - s_s.lastTalk > 1.5) {
                    BeginReturn(s_cfg.blendOut, "the NPC has finished talking");
                } else if (!s_s.dialogueSeen && now - s_s.armedAt > 2.5) {
                    auto* mtm = RE::MenuTopicManager::GetSingleton();
                    SKSE::log::info("ConvoCam: nothing opened for {} - dialogue manager: menuOpen {}, speaker {}, current topic info {}", s_s.speakerName,
                        mtm ? mtm->menuOpen : false, mtm && mtm->speaker.get() ? "set" : "none", mtm && mtm->currentTopicInfo ? "set" : "none");
                    BeginReturn(0.8f, "no dialogue opened");
                } else if (s_s.dialogueSeen && !s_s.speaker.get()) {
                    BeginReturn(0.6f, "the speaker is gone");
                } else if (SceneTracker::PlayerSceneActive()) {
                    BeginReturn(0.6f, "a scene started");
                }
            }
            PublishStatus(nullptr);
        }

        void QueueTick() {
            if (s_tickQueued.exchange(true)) return;
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([] { Tick(); });
            } else {
                s_tickQueued = false;
            }
        }

        // ---- per frame ------------------------------------------------------------------------------------------------
        void SeedCamera(RE::FreeCameraState* fcs) {
            const Pose cur = ReadPose(fcs);
            const auto m   = Mix(s_s.player, s_s.npc, 0.5f);
            s_s.ang        = std::atan2(cur.pos.x - m.x, cur.pos.y - m.y);
            s_s.rad        = Len2(cur.pos.x - m.x, cur.pos.y - m.y);
            s_s.z          = cur.pos.z;
            s_s.fov        = cur.fov > 1.0f ? cur.fov : 60.0f;
            const float sep = (std::max)(Len2(s_s.npc.x - s_s.player.x, s_s.npc.y - s_s.player.y), 120.0f);
            const RE::NiPoint3 fwd{ std::sin(cur.yaw) * std::cos(cur.pitch), std::cos(cur.yaw) * std::cos(cur.pitch), -std::sin(cur.pitch) };
            s_s.look  = { cur.pos.x + fwd.x * sep, cur.pos.y + fwd.y * sep, cur.pos.z + fwd.z * sep };
            s_s.vAng = s_s.vRad = s_s.vZ = s_s.vFov = 0.0f;
            s_s.vLook                                  = {};
            {
                // The first shot is the one that needs the shorter swing round the pair (an NPC behind the player would
                // otherwise send the camera right round the player).
                auto swing = [&](Shot sh) {
                    const Target t = ShotTarget(sh);
                    return std::fabs(WrapPi(std::atan2(t.pos.x - m.x, t.pos.y - m.y) - s_s.ang));
                };
                s_s.shot = swing(Shot::kReverse) + 0.35f < swing(Shot::kOverPlayer) ? Shot::kReverse : Shot::kOverPlayer;
            }
            s_s.shotSince                              = Now();
            SKSE::log::info("ConvoCam: seed - camera ({:.0f}, {:.0f}, {:.0f}) yaw {:.2f} pitch {:.2f} fov {:.0f}; player ({:.0f}, {:.0f}, {:.0f}); NPC ({:.0f}, {:.0f}, {:.0f}); "
                            "midpoint angle {:.2f} radius {:.0f}; first shot {}",
                cur.pos.x, cur.pos.y, cur.pos.z, cur.yaw, cur.pitch, cur.fov, s_s.player.x, s_s.player.y, s_s.player.z, s_s.npc.x, s_s.npc.y,
                s_s.npc.z, s_s.ang, s_s.rad, s_s.shot == Shot::kReverse ? "reverse" : "over the player");
            s_s.settle                                 = s_cfg.blendIn;
            s_s.clock                                  = 0.0f;
        }

        void DriveReturn(RE::FreeCameraState* fcs, float dt) {
            if (!s_s.retFromSet) {
                s_s.retFrom    = ReadPose(fcs);
                s_s.retFromSet = true;
            }
            s_s.retT += dt;
            const float w = s_s.retDur > 0.0f ? SmoothStep(s_s.retT / s_s.retDur) : 1.0f;
            WritePose(fcs, LerpPose(s_s.retFrom, ChasePose(), w));
            if (auto* player = RE::PlayerCharacter::GetSingleton()) CamCollision::Request(EyePoint(player), "conversation camera, back to the normal camera");
            if (w >= 1.0f) FinishReturn();
        }

        // --Claude 11-22: the camera put on a shot at once (a cut): its cylindrical place round the midpoint m and what it looks at,
        // at rest; the wall push is solved afresh for the new place instead of easing over from the old one.
        void CutTo(const Target& t, const RE::NiPoint3& m) {
            s_s.ang  = std::atan2(t.pos.x - m.x, t.pos.y - m.y);
            s_s.rad  = Len2(t.pos.x - m.x, t.pos.y - m.y);
            s_s.z    = t.pos.z;
            s_s.fov  = t.fov;
            s_s.look = t.look;
            s_s.vAng = s_s.vRad = s_s.vZ = s_s.vFov = 0.0f;
            s_s.vLook = {};
            g_turnPending = false;
            CamCollision::SnapNext();
        }

        // --Claude 11-22: the player turns to face the NPC (about 230 deg/s at most, easing in at the end), from 9 deg off
        void TurnToFace(RE::PlayerCharacter* p, RE::Actor* npc, float dt) {
            if (!p || !npc || dt <= 0.0f) return;
            if (p->IsOnMount() || p->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) return;
            const auto a = p->GetPosition(), b = npc->GetPosition();
            if (Len2(b.x - a.x, b.y - a.y) < 1.0f) return;
            const float cur  = p->GetAngleZ();
            const float diff = WrapPi(std::atan2(b.x - a.x, b.y - a.y) - cur);
            if (!s_s.turning) {
                if (std::fabs(diff) < 0.15f) return;
                s_s.turning = true;
                if (!s_s.turnLogged) {
                    s_s.turnLogged = true;
                    Event(std::format("turning you {:.0f} deg to face {}", diff * 57.2958f, s_s.speakerName));
                } else if (s_s.turnBacks < 12) {
                    // 11-22: something else may turn the player
                    // away after the first turn - logged with what the dialogue is doing then, and turned back
                    ++s_s.turnBacks;
                    auto* mtm = RE::MenuTopicManager::GetSingleton();
                    SKSE::log::info("ConvoCam: something turned you {:.0f} deg away from {} (heading now {:.0f} deg) - turning back; topic info {}, "
                                    "your voice {}, their voice {}, shot {}",
                        -diff * 57.2958f, s_s.speakerName, cur * 57.2958f, mtm && mtm->currentTopicInfo ? "set" : "none",
                        VoicePlaying(p) ? "playing" : "not playing", VoicePlaying(npc) ? "playing" : "not playing", ShotName(s_s.shot));
                }
            }
            if (std::fabs(diff) < 0.03f) {
                s_s.turning = false;
                return;
            }
            const float maxStep = 4.0f * dt;
            float       step    = diff * (std::min)(1.0f, dt * 9.0f);
            if (std::fabs(step) < 0.01f) step = diff > 0.0f ? (std::min)(diff, 0.01f) : (std::max)(diff, -0.01f);
            p->SetHeading(WrapPi(cur + std::clamp(step, -maxStep, maxStep)));
        }

        void DriveRun(RE::FreeCameraState* fcs, float dt) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto  ref    = s_s.speaker.get();
            auto* npc    = ref ? ref->As<RE::Actor>() : nullptr;
            if (!player || !npc) {
                BeginReturn(0.6f, "the speaker is gone");
                return;
            }
            // --Claude 11-21: under a trade menu the paused game gives dt 0; with Keep moving on the shot runs on real time (its
            // sway goes on and it follows the two when another mod keeps their animation stepping under the pause)
            if (s_cfg.tradeLive && TradeMenuOpen()) dt = g_realDt;
            const RE::NiPoint3 pRaw = EyePoint(player);
            const RE::NiPoint3 nRaw = EyePoint(npc);
            if (!s_s.seeded) {
                s_s.player = pRaw;
                s_s.npc    = nRaw;
                s_s.seeded = true;
                SeedCamera(fcs);
            } else {
                const float w = ExpWeight(dt, 0.25f);
                s_s.player    = Mix(s_s.player, pRaw, w);
                s_s.npc       = Mix(s_s.npc, nRaw, w);
            }
            s_s.clock += dt;
            // --Claude 11-22: turn the player to face the NPC - one can talk to someone standing behind; the shots on the player
            // then show their face and Face to face looks past their head. Not under a trade menu (the game is paused there).
            if (s_cfg.faceSpeaker && !TradeMenuOpen()) TurnToFace(player, npc, dt);

            // Which shot: over the player's shoulder while the NPC talks, the reverse while it is the player's turn - never
            // sooner than minShotSec after the last change.
            const double now     = Now();
            // --Claude 11-21: such a menu pauses the game, so the damped swing below stopped wherever it was when the menu opened
            // and every barter looked different. Under it the camera goes to the trade shot and stays there; after the menu the
            // talk carries on from that shot. 11-22: with Keep moving (real time under the menu) it glides there like a change
            // of shot, carrying on from however it was moving; without it the paused game gives no time to glide, so it is set
            // at once as before.
            if (TradeMenuOpen()) {
                if (!s_s.tradeHold) {
                    s_s.tradeHold = true;
                    s_s.shot      = Shot::kOverPlayer;
                    g_turnPending = false;
                    if (!s_cfg.tradeLive) {
                        s_s.player = pRaw;
                        s_s.npc    = nRaw;
                    }
                    // 11-22: the shoulder shot (or the close one) with a wall between it and the NPC - face to face instead, for this
                    // menu (decided once, so it does not flip back and forth)
                    s_s.tradeBlocked = false;
                    if (s_cfg.tradeShot == 0 || s_cfg.tradeShot == 2) {
                        constexpr float    kBlocked = 30.0f;
                        const Target       sh       = TradeTarget();
                        const RE::NiPoint3 clear    = CamCollision::ClearSpot(sh.look, sh.pos, "trade shot");
                        const float        dx = clear.x - sh.pos.x, dy = clear.y - sh.pos.y, dz = clear.z - sh.pos.z;
                        const float        in = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (in > kBlocked) {
                            s_s.tradeBlocked = true;
                            Event(std::format("the shoulder trade shot is {:.0f} units into a wall - face to face for this menu", in));
                        }
                    }
                    const Target t0 = ShotTarget(s_s.shot);  // also measures tradePush for the line below
                    if (s_cfg.tradeShot == 3 || s_s.tradeBlocked) {
                        const float dx = t0.look.x - t0.pos.x, dy = t0.look.y - t0.pos.y, dz = t0.look.z - t0.pos.z;
                        SKSE::log::info("ConvoCam: face to face - the camera {:.0f} units from {}'s face, {:.0f} between you, FOV {:.0f}",
                            std::sqrt(dx * dx + dy * dy + dz * dz), s_s.speakerName, Len2(s_s.npc.x - s_s.player.x, s_s.npc.y - s_s.player.y), t0.fov);
                    }
                    if (s_cfg.tradeLive) {
                        s_s.settle = s_cfg.shotSwitch;
                    } else {
                        const auto m0 = Mix(s_s.player, s_s.npc, 0.5f);
                        s_s.ang  = std::atan2(t0.pos.x - m0.x, t0.pos.y - m0.y);
                        s_s.rad  = Len2(t0.pos.x - m0.x, t0.pos.y - m0.y);
                        s_s.z    = t0.pos.z;
                        s_s.fov  = t0.fov;
                        s_s.look = t0.look;
                        s_s.vAng = s_s.vRad = s_s.vZ = s_s.vFov = 0.0f;
                        s_s.vLook = {};
                    }
                    Event(std::format("a menu opened over the talk - camera {} the trade shot ({}) until it closes{}{}",
                        s_cfg.tradeLive ? std::format("gliding over {:.1f} s to", s_cfg.shotSwitch) : "set at once on", TradeShotName(),
                        s_s.tradePush > 0.5f ? std::format(", moved {:.0f} units out so your head is off the NPC", s_s.tradePush) : "",
                        s_cfg.tradeLive ? ", moving on real time" : ""));
                }
                s_s.shotSince = now;
            } else if (s_s.tradeHold) {
                s_s.tradeHold    = false;
                s_s.tradeBlocked = false;
                s_s.shotSince = now;
            }
            // --Claude 11-22: a picked line is the PLAYER's first - a player-voice mod (DBReV) says it and the NPC's reply waits for
            // it - but the topic info is set from the pick. So the camera stays on the player until the NPC's own voice starts;
            // a reply with no sound at all turns it after kHoldSilent. Logged twice a second for the first 6 s of each line.
            constexpr double         kHoldSilent = 10.0;
            const RE::TESTopicInfo* info        = NpcTalking() ? RE::MenuTopicManager::GetSingleton()->currentTopicInfo : nullptr;
            if (info != s_s.lastInfo) {
                s_s.lastInfo = info;
                if (info) {
                    s_s.pickAt       = now;
                    s_s.npcVoiceSeen = false;
                    s_s.nextVoiceLog = now;
                }
            }
            bool holdOnPlayer = false;
            if (info && !s_s.npcVoiceSeen) {
                const bool npcVoice = VoicePlaying(npc);
                if (now >= s_s.nextVoiceLog && now - s_s.pickAt < 6.0) {
                    s_s.nextVoiceLog = now + 0.5;
                    SKSE::log::info("ConvoCam: line +{:.2f} s - {}'s voice {}, your voice {}", now - s_s.pickAt, s_s.speakerName,
                        npcVoice ? "playing" : "not playing", VoicePlaying(player) ? "playing" : "not playing");
                }
                if (npcVoice) {
                    s_s.npcVoiceSeen = true;
                    Event(std::format("{}'s voice started {:.2f} s after the line began", s_s.speakerName, now - s_s.pickAt));
                } else if (now - s_s.pickAt > kHoldSilent) {
                    s_s.npcVoiceSeen = true;
                    Event(std::format("no voice from {} in {:.0f} s - turning to them anyway", s_s.speakerName, kHoldSilent));
                } else {
                    holdOnPlayer = true;
                }
            }
            const bool   playersTurn = s_s.seenMenu && DialogueOpen() && (!info || holdOnPlayer);
            const Shot   wanted      = (s_cfg.alternate && playersTurn && s_s.clock > 0.5f && !s_s.tradeHold) ? Shot::kReverse : Shot::kOverPlayer;
            // the NPC's voice starting is a cue of its own: the change waits only 0.5 s after the last one, not minShotSec
            const bool   voiceCue    = wanted == Shot::kOverPlayer && info != nullptr && s_s.npcVoiceSeen;
            if (wanted != s_s.shot && (now - s_s.shotSince >= s_cfg.minShotSec || (voiceCue && now - s_s.shotSince >= 0.5))) {
                s_s.shot      = wanted;
                s_s.shotSince = now;
                s_s.settle    = s_cfg.shotSwitch;
                // 11-22: Cut (film) - the camera is put on the new shot at once; Swing - it turns round the two in shotSwitch
                if (s_cfg.cutShots && !s_s.tradeHold && s_s.clock > s_cfg.blendIn) {
                    CutTo(ShotTarget(wanted), Mix(s_s.player, s_s.npc, 0.5f));
                    Event(std::format("shot: {} (cut)", ShotName(wanted)));
                } else {
                    g_turnPending = true;
                    Event(std::format("shot: {}", ShotName(wanted)));
                }
            } else if (s_s.settle != s_cfg.shotSwitch && now - s_s.shotSince > s_cfg.blendIn) {
                s_s.settle = s_cfg.shotSwitch;  // the entry is over: later changes use the shot-change time
            }

            const Target t = ShotTarget(s_s.shot);
            const auto   m = Mix(s_s.player, s_s.npc, 0.5f);

            // The wanted camera in cylindrical coordinates about the midpoint; each coordinate is damped, so a change of shot
            // swings round the pair instead of cutting through them.
            const float tAng = std::atan2(t.pos.x - m.x, t.pos.y - m.y);
            const float tRad = Len2(t.pos.x - m.x, t.pos.y - m.y);
            const float settle = (std::max)(s_s.settle, 0.2f);
            // 11-22: a change of shot is a half turn round the two; at 1.3 rad/s it took about 3 s whatever 'Change of shot
            // takes' said (1.0 s gave 2.7 s), longer than the setting asked for. The speed limits now scale with
            // that time, so the turn ends when the setting says; the entry keeps its old limits.
            const bool  switching = s_s.settle == s_cfg.shotSwitch;
            const float angCap    = switching ? (std::max)(1.3f, 6.0f / settle) : 1.3f;
            const float k         = switching ? (std::max)(1.0f, 2.25f / settle) : 1.0f;
            s_s.ang  = SmoothDampAngle(s_s.ang, tAng, s_s.vAng, settle, angCap, dt);
            s_s.rad  = SmoothDamp(s_s.rad, tRad, s_s.vRad, settle, 240.0f * k, dt);
            s_s.z    = SmoothDamp(s_s.z, t.pos.z, s_s.vZ, settle, 150.0f * k, dt);
            s_s.fov  = SmoothDamp(s_s.fov, t.fov, s_s.vFov, settle, 22.0f * k, dt);
            s_s.look.x = SmoothDamp(s_s.look.x, t.look.x, s_s.vLook.x, settle, 300.0f * k, dt);
            s_s.look.y = SmoothDamp(s_s.look.y, t.look.y, s_s.vLook.y, settle, 300.0f * k, dt);
            s_s.look.z = SmoothDamp(s_s.look.z, t.look.z, s_s.vLook.z, settle, 300.0f * k, dt);
            {
                if (g_turnPending && !s_s.tradeHold && std::fabs(WrapPi(tAng - s_s.ang)) < 0.05f) {
                    g_turnPending = false;
                    SKSE::log::info("ConvoCam: {} - the turn took {:.2f} s (change of shot set to {:.1f} s)", ShotName(s_s.shot),
                        now - s_s.shotSince, s_cfg.shotSwitch);
                }
            }

            static double s_nextTrace = 0.0;
            if (s_s.clock < 0.05f) s_nextTrace = 0.0;
            if (s_s.clock < 7.0f && now >= s_nextTrace) {
                s_nextTrace = now + 0.5;
                SKSE::log::info("ConvoCam: trace t={:.1f} - angle {:.2f} -> {:.2f}, radius {:.0f} -> {:.0f}, height {:.0f} -> {:.0f}, look ({:.0f}, {:.0f}, {:.0f}) -> ({:.0f}, {:.0f}, {:.0f}), shot {}",
                    s_s.clock, s_s.ang, tAng, s_s.rad, tRad, s_s.z, t.pos.z, s_s.look.x, s_s.look.y, s_s.look.z, t.look.x, t.look.y, t.look.z, ShotName(s_s.shot));
            }
            Pose p;
            p.pos = { m.x + std::sin(s_s.ang) * s_s.rad, m.y + std::cos(s_s.ang) * s_s.rad, s_s.z };
            // A slow handheld sway on the point it looks at.
            RE::NiPoint3 look = s_s.look;
            if (s_cfg.drift > 0.0f) {
                const float c = s_s.clock;
                look.x += std::sin(c * 0.37f) * 4.0f * s_cfg.drift;
                look.y += std::cos(c * 0.29f) * 4.0f * s_cfg.drift;
                look.z += std::sin(c * 0.51f + 1.0f) * 2.5f * s_cfg.drift;
            }
            LookAngles({ look.x - p.pos.x, look.y - p.pos.y, look.z - p.pos.z }, p.yaw, p.pitch);
            p.roll = 0.0f;
            p.fov  = s_s.fov;
            WritePose(fcs, p);
            // 11-22: face to face with the camera in or behind the player's head (Distance moved out): the head is hidden, so
            // it does not block the NPC; shown again as soon as the camera is clear of it
            bool hideHead = false;
            if (s_s.tradeHold && (s_cfg.tradeShot == 3 || s_s.tradeBlocked)) {
                const float d  = (std::max)(Len2(s_s.npc.x - s_s.player.x, s_s.npc.y - s_s.player.y), 1.0f);
                const float fx = (s_s.npc.x - s_s.player.x) / d, fy = (s_s.npc.y - s_s.player.y) / d;
                const float rx = p.pos.x - s_s.player.x, ry = p.pos.y - s_s.player.y, rz = p.pos.z - s_s.player.z;
                const float along = rx * fx + ry * fy;            // + = towards the NPC from the player's head
                const float off   = std::fabs(rx * fy - ry * fx);  // to the side of the line between them
                hideHead          = along < 14.0f && off < 26.0f && rz > -30.0f && rz < 40.0f;
            }
            if (hideHead) {
                HidePlayerHead();
            } else {
                ShowPlayerHead("the camera is clear of it");
            }
            // 11-22: under a trade menu the paused game froze the wall push (0 time) while the shot glided on real time, so the
            // last push stayed on as the camera went round the two - near walls the push pointed the wrong way. It runs on
            // real time there now, also when the shot is set at once (its push from the old spot eases off instead of sticking).
            CamCollision::Request(look, "conversation camera", false, TradeMenuOpen());
        }
    }

    Settings& Get() { return s_cfg; }

    void Clamp() {
        auto range = [](float& v, float lo, float hi, float def) { v = std::isfinite(v) ? std::clamp(v, lo, hi) : def; };
        range(s_cfg.blendIn, 0.3f, 6.0f, 1.6f);
        range(s_cfg.blendOut, 0.0f, 6.0f, 1.4f);
        range(s_cfg.shotSwitch, 0.3f, 6.0f, 1.5f);
        range(s_cfg.minShotSec, 0.5f, 15.0f, 2.5f);
        range(s_cfg.distance, 40.0f, 400.0f, 115.0f);
        range(s_cfg.shoulder, -100.0f, 100.0f, 36.0f);
        range(s_cfg.height, -40.0f, 80.0f, 10.0f);
        range(s_cfg.lookShift, -80.0f, 80.0f, 24.0f);
        range(s_cfg.fovNpc, 20.0f, 100.0f, 46.0f);
        range(s_cfg.fovWide, 20.0f, 110.0f, 56.0f);
        range(s_cfg.drift, 0.0f, 3.0f, 1.0f);
        s_cfg.reverseKind = std::clamp(s_cfg.reverseKind, 0, 1);
        s_cfg.tradeShot   = std::clamp(s_cfg.tradeShot, 0, 3);
        range(s_cfg.tradeDistance, 0.4f, 2.5f, 1.0f);
        range(s_cfg.tradeHeight, -60.0f, 80.0f, 0.0f);
        range(s_cfg.tradeSide, -80.0f, 80.0f, 0.0f);
        range(s_cfg.tradeTurn, -90.0f, 90.0f, 0.0f);
        range(s_cfg.tradeFov, 0.0f, 100.0f, 0.0f);
        if (s_cfg.tradeFov > 0.0f && s_cfg.tradeFov < 20.0f) s_cfg.tradeFov = 20.0f;
    }

    void Install() {
        Clamp();
        // A plain thread paces the checks (a task that re-queues itself would spin inside one frame).
        std::thread([] {
            while (true) {
                ::Sleep(100);
                QueueTick();
            }
        }).detach();
        if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) holder->AddEventSink<RE::TESActivateEvent>(ActivateSink::Get());
        SKSE::log::info("ConvoCam: ready - conversation camera {}", s_cfg.enabled ? "on" : "off");
    }

    void Drive(RE::FreeCameraState* fcs, bool driving) {
        const float dt = FrameDt();
        if (!fcs || s_s.st == St::kOff) return;
        if (!driving) {
            Event("another camera took the free camera");
            ResetSession();
            return;
        }
        // The camera from before the session = where the free camera started (the first frame it updates).
        if (!s_s.havePre) {
            s_s.pre     = ToPre(ReadPose(fcs));
            s_s.havePre = true;
        }
        if (s_s.st == St::kReturning) {
            DriveReturn(fcs, dt);
            return;
        }
        if (s_s.st == St::kStarting) {
            s_s.st = St::kRunning;
            Event("conversation camera running");
            ReleaseHeld("the camera is in");  // the Dialogue Menu opens on top of the free camera, never under it
        }
        DriveRun(fcs, dt);
    }

    bool OwnsMotion() { return s_s.st == St::kRunning || s_s.st == St::kReturning || s_s.st == St::kStarting; }

    namespace {
        RE::ObjectRefHandle s_pending;
        double              s_pendingAt = 0.0;
    }

    void NotePress(RE::ButtonEvent* a_event) {
        s_pending = {};
        if (!a_event || !a_event->IsDown()) return;
        auto* pk = RE::CrosshairPickData::GetSingleton();
        if (!pk) return;
        const auto handle = pk->GetActiveTarget();
        auto       r      = handle.get();
        auto*      a      = r ? r->As<RE::Actor>() : nullptr;
        if (!a) return;
        // passive diagnostics, whatever the setting: does the dialogue open after this press
        const char* nm = a->GetDisplayFullName();
        s_watch.who    = nm ? nm : "an actor";
        s_watch.who += a->GetCurrentScene() ? " (in a scene)" : "";
        s_watch.at    = Now();
        s_watch.stage = 1;
        SKSE::log::info("ConvoCam: press on {} - conversation camera {}", s_watch.who, s_cfg.enabled ? "on" : "off");
        if (!s_cfg.enabled) return;
        if (const char* block = ArmBlock(a)) {
            SKSE::log::info("ConvoCam: activate pressed on {} - not armed: {}", s_watch.who, block);
            return;
        }
        s_pending   = handle;
        s_pendingAt = Now();
    }

    bool PendingRelease(RE::ButtonEvent* a_event) {
        if (!s_pending || !a_event || !a_event->IsUp()) return false;
        if (Now() - s_pendingAt > 2.0) {
            s_pending = {};
            return false;
        }
        return true;
    }

    void BeginPending() {
        const auto handle = s_pending;
        s_pending         = {};
        auto  ref         = handle.get();
        auto* a           = ref ? ref->As<RE::Actor>() : nullptr;
        if (!a) return;
        auto* mtm = RE::MenuTopicManager::GetSingleton();
        SKSE::log::info("ConvoCam: after the release ({:.0f} ms after the press) - dialogue manager: menuOpen {}, speaker {}, greeting {}, topic info {}",
            (Now() - s_pendingAt) * 1000.0, mtm ? mtm->menuOpen : false, mtm && mtm->speaker.get() ? "set" : "none",
            mtm ? mtm->isGreetingPlayer : false, mtm && mtm->currentTopicInfo ? "set" : "none");
        if (!mtm || !mtm->menuOpen) {  // a bark, or a scene the game is still stopping: no dialogue to film
            SKSE::log::info("ConvoCam: no dialogue started by the release - not filming");
            return;
        }
        if (const char* block = ArmBlock(a)) {  // the release itself may have changed things (a dialogue already open, a menu)
            SKSE::log::info("ConvoCam: not armed after the release: {}", block);
            return;
        }
        Arm(a);
    }

    // ---- dialogue the NPC starts (force-greet, an NPC walking up to talk) ------------------------------------------
    // No Activate press, so NotePress / BeginPending never run. The game's dialogue manager says a dialogue has begun
    // (menuOpen) a moment before the Dialogue Menu itself opens - the same gap the Activate path starts in. The camera may
    // only begin inside that gap (see Arm): once the menu is open it is too late and the NPC is left to the game's camera.
    namespace {
        bool             s_prevTopicMenu = false;
        bool             s_npcWatch      = false;   // menuOpen came on, waiting for the speaker
        double           s_npcWatchAt    = 0.0;
        std::atomic_bool s_npcQueued{ false };

        void BeginNpcStart(RE::ObjectRefHandle a_handle, double a_seenAt) {
            auto  ref = a_handle.get();
            auto* a   = ref ? ref->As<RE::Actor>() : nullptr;
            if (!a) return;
            const char* nm  = a->GetDisplayFullName();
            const std::string who = nm ? nm : "an NPC";
            const double ms = (Now() - a_seenAt) * 1000.0;
            auto*        mtm = RE::MenuTopicManager::GetSingleton();
            if (!mtm || !mtm->menuOpen) {
                SKSE::log::info("ConvoCam: {} started a dialogue - over again {:.0f} ms later, not filming", who, ms);
                return;
            }
            if (s_s.st != St::kOff) return;  // the dialogue menu hold has already started the camera for this talk
            if (const char* block = ArmBlock(a)) {
                SKSE::log::info("ConvoCam: {} started a dialogue with you - not filming ({:.0f} ms after it began): {}", who, ms, block);
                return;
            }
            SKSE::log::info("ConvoCam: {} started a dialogue with you - camera starting {:.0f} ms after it began, Dialogue Menu not open yet", who, ms);
            Arm(a, "started talking to you");
        }
    }

    // ---- the dialogue menu hold -----------------------------------------------------------------------------------
    // The game can open the Dialogue Menu before TFCam sees the talk at all: an NPC walking up to talk, or a talk the
    // game opens on the Activate PRESS (the release then goes to the open menu and never reaches the handler). The free
    // camera must begin before that menu opens (see Arm), so the menu's open request is held at the menu queue
    // (UIMessageQueue::AddMessage, "Dialogue Menu", kShow) until the camera is in, then sent on. It is never lost: a talk
    // the camera may not film, or a camera that does not come within 2 s, lets it through; a talk that ends while it
    // waits drops it (its own close is let through).
    //
    // The hook only CHAINS onto another mod's jump: in a load order where some mod has already put a jump at
    // AddMessage's first byte (an absolute FF 25 jump, or a near E9 jump), that jump's target is swapped for ours and
    // ours calls the old one (see InstallDialogueHold). The swap is one store inside a cache line, so a thread inside
    // the queue at that moment takes either the old jump or ours, both whole. Anywhere else (the game's own first
    // bytes, or another kind of hook) nothing is written and the NPC-started talk keeps the game's camera, as before.
    namespace {
        using AddMessage_t = void (*)(RE::UIMessageQueue*, const RE::BSFixedString&, RE::UI_MESSAGE_TYPE, RE::IUIMessageData*);
        AddMessage_t s_prevAdd = nullptr;

        struct Held {
            bool                on    = false;
            RE::UIMessageQueue* queue = nullptr;
            RE::IUIMessageData* data  = nullptr;
            double              at    = 0.0;
        };
        std::mutex        s_holdLock;
        Held              s_held;
        std::atomic<bool> s_holding{ false };

        bool IsDialogueMenu(const RE::BSFixedString& a_name) {
            auto* istr = RE::InterfaceStrings::GetSingleton();
            return istr && a_name.data() == istr->dialogueMenu.data();  // pooled strings: one pointer per name
        }

        // Main thread (a task queued by the hook): film this talk, or let its menu through.
        void DecideHeld() {
            if (HeldFor() < 0.0) return;
            if (s_s.st == St::kRunning || s_s.st == St::kReturning) {
                ReleaseHeld("a conversation camera is already running");
                return;
            }
            if (s_s.st == St::kStarting) {
                Event("the Dialogue Menu waits - the camera is starting");
                return;  // Drive lets it through once the camera is in
            }
            auto* mtm = RE::MenuTopicManager::GetSingleton();
            auto  ref = mtm ? mtm->speaker.get() : RE::NiPointer<RE::TESObjectREFR>{};
            auto* a   = ref ? ref->As<RE::Actor>() : nullptr;
            if (!a || a->IsPlayerRef()) {
                ReleaseHeld("no NPC speaking");
                return;
            }
            const char* nm = a->GetDisplayFullName();
            if (const char* block = ArmBlock(a)) {
                ReleaseHeld(std::format("not filming {}: {}", nm ? nm : "the NPC", block));
                return;
            }
            s_pending = {};  // a press whose release went to the open menu: this talk is started here instead
            Arm(a, "is talking to you - the Dialogue Menu waits for the camera");
        }

        bool HoldShow(RE::UIMessageQueue* a_queue, RE::IUIMessageData* a_data) {
            if (!s_cfg.enabled) return false;
            {
                std::scoped_lock l(s_holdLock);
                if (s_held.on) return false;  // one at a time
                s_held = Held{ true, a_queue, a_data, Now() };
            }
            s_holding = true;
            SKSE::log::info("ConvoCam: the Dialogue Menu was asked to open - held until the camera is in");
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([] { DecideHeld(); });
            } else {
                ReleaseHeld("no task interface");
            }
            return true;
        }

        void DropHeld(const char* a_why) {
            {
                std::scoped_lock l(s_holdLock);
                if (!s_held.on) return;
                s_held = Held{};
            }
            s_holding = false;
            SKSE::log::info("ConvoCam: the held Dialogue Menu was dropped - {}", a_why);
        }

        // Any thread: the game's (or the previous hook's) AddMessage, with the Dialogue Menu's open request held.
        void AddMessageThunk(RE::UIMessageQueue* a_queue, const RE::BSFixedString& a_menu, RE::UI_MESSAGE_TYPE a_type, RE::IUIMessageData* a_data) {
            if (s_prevAdd && IsDialogueMenu(a_menu)) {
                if (a_type == RE::UI_MESSAGE_TYPE::kShow) {
                    if (HoldShow(a_queue, a_data)) return;
                } else if (a_type == RE::UI_MESSAGE_TYPE::kHide || a_type == RE::UI_MESSAGE_TYPE::kForceHide) {
                    DropHeld("the dialogue was closed while its menu waited");
                }
            }
            s_prevAdd(a_queue, a_menu, a_type, a_data);
        }

        std::string ModuleOf(std::uintptr_t a_addr) {
            HMODULE mod = nullptr;
            if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(a_addr), &mod) || !mod) {
                return std::format("code outside any DLL at {:X}", a_addr);
            }
            char path[MAX_PATH]{};
            ::GetModuleFileNameA(mod, path, MAX_PATH);
            const char* base = std::strrchr(path, '\\');
            return std::format("{}+{:X}", base ? base + 1 : path, a_addr - reinterpret_cast<std::uintptr_t>(mod));
        }

        // Where a jump at a leads, through relays (FF 25 absolute / E9 relative), named by the DLL it ends in. Logging only.
        std::string DescribeJumpTarget(std::uintptr_t a) {
            for (int hop = 0; hop < 4 && a; ++hop) {
                const auto* q = reinterpret_cast<const std::uint8_t*>(a);
                if (q[0] == 0xFF && q[1] == 0x25 && q[2] == 0 && q[3] == 0 && q[4] == 0 && q[5] == 0) {
                    a = *reinterpret_cast<const std::uint64_t*>(q + 6);
                } else if (q[0] == 0xE9) {
                    a = a + 5 + *reinterpret_cast<const std::int32_t*>(q + 1);
                } else {
                    break;
                }
            }
            return ModuleOf(a);
        }

        // One store inside a cache line is seen whole by a thread running the jump at that moment.
        bool InOneLine(std::uintptr_t a_addr, std::size_t a_size) { return (a_addr & 63) + a_size <= 64; }

        bool WriteWhole(void* a_at, std::uint64_t a_value, std::size_t a_size) {
            DWORD old = 0;
            if (!::VirtualProtect(a_at, a_size, PAGE_EXECUTE_READWRITE, &old)) return false;
            if (a_size == 8) {
                *static_cast<volatile std::uint64_t*>(a_at) = a_value;
            } else {
                *static_cast<volatile std::uint32_t*>(a_at) = static_cast<std::uint32_t>(a_value);
            }
            ::VirtualProtect(a_at, a_size, old, &old);
            return true;
        }

        // Main thread, the first camera frame (every mod's own hooks are in by then). Two kinds of jump are chained:
        //   FF 25 00000000 <8-byte address>  (an absolute jump)  - the address is swapped for AddMessageThunk's
        //   E9 <4-byte distance>             (a near jump, MinHook / SKSE-trampoline style) - an absolute jump to
        //      AddMessageThunk goes in TFCam's trampoline (near the game) and the distance is swapped for it.
        // Either way the old target is what ours calls. Anything else: nothing is written.
        void InstallDialogueHold() {
            REL::Relocation<std::uintptr_t> fn{ RELOCATION_ID(13530, 13631) };
            auto*                         p = reinterpret_cast<std::uint8_t*>(fn.address());
            const auto                    entry = reinterpret_cast<std::uintptr_t>(p);
            static constexpr std::uint8_t kAbsJmp[6]{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
            std::uintptr_t                prev = 0;
            if (std::memcmp(p, kAbsJmp, sizeof(kAbsJmp)) == 0) {
                if (!InOneLine(entry + 6, 8)) {
                    SKSE::log::info("ConvoCam: the jump at UIMessageQueue::AddMessage crosses a cache line - no dialogue menu hold");
                    return;
                }
                prev = static_cast<std::uintptr_t>(*reinterpret_cast<volatile std::uint64_t*>(p + 6));
                if (!prev) return;
                s_prevAdd = reinterpret_cast<AddMessage_t>(prev);
                if (!WriteWhole(p + 6, reinterpret_cast<std::uint64_t>(&AddMessageThunk), 8)) {
                    s_prevAdd = nullptr;
                    SKSE::log::warn("ConvoCam: could not unprotect UIMessageQueue::AddMessage's jump - no dialogue menu hold");
                    return;
                }
            } else if (p[0] == 0xE9) {
                if (!InOneLine(entry + 1, 4)) {
                    SKSE::log::info("ConvoCam: the jump at UIMessageQueue::AddMessage crosses a cache line - no dialogue menu hold");
                    return;
                }
                prev = entry + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
                auto* stub = static_cast<std::uint8_t*>(SKSE::GetTrampoline().allocate(14));
                if (!stub) {
                    SKSE::log::warn("ConvoCam: no trampoline room for the dialogue menu hold");
                    return;
                }
                const auto dist = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(stub)) - static_cast<std::int64_t>(entry + 5);
                if (dist < INT32_MIN || dist > INT32_MAX) {
                    SKSE::log::warn("ConvoCam: TFCam's trampoline is too far from UIMessageQueue::AddMessage - no dialogue menu hold");
                    return;
                }
                std::memcpy(stub, kAbsJmp, sizeof(kAbsJmp));
                const auto thunk = reinterpret_cast<std::uint64_t>(&AddMessageThunk);
                std::memcpy(stub + 6, &thunk, sizeof(thunk));
                s_prevAdd = reinterpret_cast<AddMessage_t>(prev);
                if (!WriteWhole(p + 1, static_cast<std::uint32_t>(static_cast<std::int32_t>(dist)), 4)) {
                    s_prevAdd = nullptr;
                    SKSE::log::warn("ConvoCam: could not unprotect UIMessageQueue::AddMessage's jump - no dialogue menu hold");
                    return;
                }
            } else {
                SKSE::log::info("ConvoCam: UIMessageQueue::AddMessage starts with {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}, not another mod's jump - "
                    "no dialogue menu hold, so a talk the NPC starts keeps the game's camera", p[0], p[1], p[2], p[3], p[4], p[5]);
                return;
            }
            ::FlushInstructionCache(::GetCurrentProcess(), p, 14);
            SKSE::log::info("ConvoCam: dialogue menu hold in - chained ({} jump) after {} (another mod's hook on UIMessageQueue::AddMessage)",
                p[0] == 0xE9 ? "near" : "absolute", DescribeJumpTarget(prev));
        }
    }

    namespace {
        double HeldFor() {
            std::scoped_lock l(s_holdLock);
            return s_held.on ? Now() - s_held.at : -1.0;
        }

        void ReleaseHeld(const std::string& a_why) {
            Held h;
            {
                std::scoped_lock l(s_holdLock);
                if (!s_held.on) return;
                h      = s_held;
                s_held = Held{};
            }
            s_holding = false;
            const double ms  = (Now() - h.at) * 1000.0;
            auto*        mtm = RE::MenuTopicManager::GetSingleton();
            auto*        istr = RE::InterfaceStrings::GetSingleton();
            if (!mtm || !mtm->menuOpen || !istr || !s_prevAdd) {
                SKSE::log::info("ConvoCam: the held Dialogue Menu was dropped after {:.0f} ms - the dialogue is over ({})", ms, a_why);
                return;
            }
            SKSE::log::info("ConvoCam: the Dialogue Menu let through after {:.0f} ms - {}", ms, a_why);
            s_prevAdd(h.queue, istr->dialogueMenu, RE::UI_MESSAGE_TYPE::kShow, h.data);
        }
    }

    void FrameCheck() {
        static bool s_holdTried = false;
        if (!s_holdTried) {
            s_holdTried = true;
            InstallDialogueHold();
        }
        auto*        mtm  = RE::MenuTopicManager::GetSingleton();
        const bool   open = mtm && mtm->menuOpen;
        const double now  = Now();
        if (open && !s_prevTopicMenu) {
            s_npcWatch   = true;
            s_npcWatchAt = now;
        }
        s_prevTopicMenu = open;
        if (!open) s_npcWatch = false;
        if (s_holding.load()) {  // the hold has this talk (DecideHeld)
            s_npcWatch = false;
            return;
        }
        if (!s_npcWatch) return;
        // The Activate path, a session already running, or the camera turned off: nothing to do for this dialogue.
        if (!s_cfg.enabled || s_s.st != St::kOff || s_pending) {
            s_npcWatch = false;
            return;
        }
        auto  ref = mtm->speaker.get();
        auto* a   = ref ? ref->As<RE::Actor>() : nullptr;
        if (!a) {
            if (now - s_npcWatchAt > 0.5) {
                SKSE::log::info("ConvoCam: a dialogue began with no speaker set - not filming");
                s_npcWatch = false;
            }
            return;
        }
        s_npcWatch = false;
        if (a->IsPlayerRef()) return;
        auto* ui = RE::UI::GetSingleton();
        if (ui && ui->IsMenuOpen("Dialogue Menu")) {
            const char* nm = a->GetDisplayFullName();
            SKSE::log::info("ConvoCam: {} started a dialogue - the Dialogue Menu was already open when seen, not filming", nm ? nm : "an NPC");
            return;
        }
        // Not from inside the camera update itself: the free camera begins at the next task point.
        if (s_npcQueued.exchange(true)) return;
        const auto handle = a->CreateRefHandle();
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([handle, now] {
                s_npcQueued = false;
                BeginNpcStart(handle, now);
            });
        } else {
            s_npcQueued = false;
        }
    }

    void OnGameLoad(const char* a_why) {
        DropHeld(a_why);  // a menu held across a load belongs to a dialogue that no longer exists
        if (s_s.st != St::kOff) Event(std::format("{} - session dropped", a_why));
        ResetSession();
    }

    MenuStatus GetMenuStatus() {
        std::lock_guard lock(s_statusLock);
        return s_status;
    }
}
