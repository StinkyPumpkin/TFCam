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

        void ResetSession() { s_s = Session{}; }

        // ---- the conversation ---------------------------------------------------------------------------------------
        bool DialogueOpen() {
            auto* mtm = RE::MenuTopicManager::GetSingleton();
            auto* ui  = RE::UI::GetSingleton();
            return mtm && ui && mtm->menuOpen && ui->IsMenuOpen("Dialogue Menu");
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

        Target ShotTarget(Shot shot) {
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
        void Arm(RE::Actor* spk) {
            if (!spk) return;
            s_s             = Session{};
            s_s.st          = St::kStarting;
            s_s.startedAt   = Now();
            s_s.armedAt     = s_s.startedAt;
            s_s.speaker     = spk->CreateRefHandle();
            if (auto* cam = Cam(); cam && cam->currentState) s_s.firstPerson = cam->currentState->id == RE::CameraStates::kFirstPerson;
            const char* nm  = spk->GetDisplayFullName();
            s_s.speakerName = nm ? nm : "";
            Event(std::format("you activated {} - camera starting", s_s.speakerName.empty() ? "an NPC" : s_s.speakerName));
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
                if (!evt || !s_cfg.enabled || !evt->actionRef || !evt->actionRef->IsPlayerRef() || !evt->objectActivated)
                    return RE::BSEventNotifyControl::kContinue;
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

        void DriveRun(RE::FreeCameraState* fcs, float dt) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto  ref    = s_s.speaker.get();
            auto* npc    = ref ? ref->As<RE::Actor>() : nullptr;
            if (!player || !npc) {
                BeginReturn(0.6f, "the speaker is gone");
                return;
            }
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

            // Which shot: over the player's shoulder while the NPC talks, the reverse while it is the player's turn - never
            // sooner than minShotSec after the last change.
            const double now     = Now();
            const Shot   wanted  = (s_cfg.alternate && s_s.seenMenu && DialogueOpen() && !NpcTalking() && s_s.clock > 0.5f) ? Shot::kReverse : Shot::kOverPlayer;
            if (wanted != s_s.shot && now - s_s.shotSince >= s_cfg.minShotSec) {
                s_s.shot      = wanted;
                s_s.shotSince = now;
                s_s.settle    = s_cfg.shotSwitch;
                Event(std::format("shot: {}", ShotName(wanted)));
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
            s_s.ang  = SmoothDampAngle(s_s.ang, tAng, s_s.vAng, settle, 1.3f, dt);
            s_s.rad  = SmoothDamp(s_s.rad, tRad, s_s.vRad, settle, 240.0f, dt);
            s_s.z    = SmoothDamp(s_s.z, t.pos.z, s_s.vZ, settle, 150.0f, dt);
            s_s.fov  = SmoothDamp(s_s.fov, t.fov, s_s.vFov, settle, 22.0f, dt);
            s_s.look.x = SmoothDamp(s_s.look.x, t.look.x, s_s.vLook.x, settle, 300.0f, dt);
            s_s.look.y = SmoothDamp(s_s.look.y, t.look.y, s_s.vLook.y, settle, 300.0f, dt);
            s_s.look.z = SmoothDamp(s_s.look.z, t.look.z, s_s.vLook.z, settle, 300.0f, dt);

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
            CamCollision::Request(look, "conversation camera");
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

    void OnGameLoad(const char* a_why) {
        if (s_s.st != St::kOff) Event(std::format("{} - session dropped", a_why));
        ResetSession();
    }

    MenuStatus GetMenuStatus() {
        std::lock_guard lock(s_statusLock);
        return s_status;
    }
}
