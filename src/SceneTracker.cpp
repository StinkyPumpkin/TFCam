#include "SceneTracker.h"
#include "SceneCam.h"
#include "SlccBridge.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace SceneTracker {

    namespace {
        // Main thread only.
        std::set<int> s_threads;         // every running SexLab thread we saw start
        std::set<int> s_playerThreads;   // ...with the player in one of the thread quest's reference aliases
        std::set<int> s_unknownThreads;  // ...whose sender we could not inspect (treated as possibly the player's)
        std::map<int, RE::FormID> s_threadQuest;  // 0.10.0: sender quest of the player's / uninspectable threads

        enum class Kind { kStart, kEnd, kChange };

        struct Def {
            std::string_view name;
            Kind             kind;
        };

        // Bare names: P+ Form.SendModEvent(name, tid). "Hook"-prefixed names are P+'s ModEvent.Send
        // custom-argument events, which SKSE hands to Papyrus registrations only; they are listed in
        // case a build ever routes them here too, and are used only when strArg carries a thread id.
        // 0.10.0: the stage / position events (bare only) re-read the player's scene + stage.
        constexpr Def kDefs[] = {
            { "AnimationStart", Kind::kStart },      { "AnimationEnding", Kind::kEnd },
            { "AnimationEnd", Kind::kEnd },          { "HookAnimationStart", Kind::kStart },
            { "HookAnimationEnding", Kind::kEnd },   { "HookAnimationEnd", Kind::kEnd },
            { "StageStart", Kind::kChange },         { "StageEnd", Kind::kChange },
            { "ActorsRelocated", Kind::kChange },    { "PositionChange", Kind::kChange },
            { "AnimationChange", Kind::kChange },
        };

        bool ParseTid(std::string_view a_str, int& a_tid) {
            if (a_str.empty() || a_str.size() > 6) return false;
            int v = 0;
            for (const char c : a_str) {
                if (c < '0' || c > '9') return false;
                v = v * 10 + (c - '0');
            }
            a_tid = v;
            return true;
        }

        // 1 = the player fills one of the thread quest's reference aliases, 0 = the quest has filled
        // reference aliases and the player is not among them, -1 = cannot tell (no quest / no filled alias).
        int ThreadHasPlayer(RE::FormID a_senderID) {
            auto* form  = a_senderID ? RE::TESForm::LookupByID(a_senderID) : nullptr;
            auto* quest = form ? form->As<RE::TESQuest>() : nullptr;
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!quest || !player) return -1;
            int filled = 0;
            for (auto* alias : quest->aliases) {
                if (!alias || alias->GetVMTypeID() != RE::BGSRefAlias::VMTYPEID) continue;
                auto* ref = static_cast<RE::BGSRefAlias*>(alias)->GetReference();
                if (!ref) continue;
                ++filled;
                if (ref == player) return 1;
            }
            return filled > 0 ? 0 : -1;
        }

        bool AnyPlayerThread() { return !s_playerThreads.empty() || !s_unknownThreads.empty(); }

        // ---- 0.10.0: the player's scene through the Papyrus VM ----------------------------------------------------
        PlayerScene s_scene;

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

        constexpr double kDirtyWindow   = 1.5;   // re-read this long after a stage / position event
        constexpr double kDirtyInterval = 0.15;  // ...this often
        constexpr double kIdleInterval  = 2.0;   // a running scene is re-read this often anyway
        constexpr double kAnswerTimeout = 4.0;   // no answer from the VM in time: ask again

        enum Part : int { kPartScene = 1, kPartStage = 2, kPartPositions = 4, kPartsAll = 7 };

        // Filled by the VM callbacks (any thread), read by Tick (main thread).
        struct Mailbox {
            std::mutex              lock;
            std::uint32_t           gen = 0;
            int                     got = 0;
            std::string             scene;
            std::string             stage;
            std::vector<RE::FormID> actors;
            bool                    positionsOk = false;
            std::string             nameFor;    // scene id whose name was asked for
            std::string             name;
            bool                    nameReady = false;
        };
        Mailbox s_mail;

        int           s_queryTid   = -1;    // thread the reads are for
        std::uint32_t s_gen        = 0;     // current read generation (older answers are dropped)
        bool          s_inFlight   = false;
        double        s_sentAt     = 0.0;
        double        s_nextAt     = 0.0;
        double        s_dirtyUntil = 0.0;
        bool          s_loggedNoObject = false;

        // A changed key counts once two reads in a row agree (P+'s ResetScene first sets the new scene and only then
        // the new stage, so one read can pair the new scene with the old scene's stage).
        bool                    s_candValid = false;
        std::string             s_candScene;
        std::string             s_candStage;
        int                     s_candSlot = -1;

        class ReadCallback final : public RE::BSScript::IStackCallbackFunctor {
        public:
            ReadCallback(std::uint32_t a_gen, Part a_part) : _gen(a_gen), _part(a_part) {}

            // VM thread: copy the answer into the mailbox, nothing else.
            void operator()(RE::BSScript::Variable a_result) override {
                std::string             str;
                std::vector<RE::FormID> ids;
                bool                    ok = false;
                if (_part == kPartPositions) {
                    if (a_result.IsArray()) {
                        if (auto arr = a_result.GetArray()) {
                            for (auto& v : *arr) {
                                auto* actor = v.Unpack<RE::Actor*>();
                                ids.push_back(actor ? actor->GetFormID() : 0);
                            }
                        }
                        ok = true;
                    }
                } else if (a_result.IsString()) {
                    str = std::string(a_result.GetString());
                    ok  = true;
                }
                std::lock_guard lock(s_mail.lock);
                if (s_mail.gen != _gen) return;
                s_mail.got |= _part;
                if (_part == kPartScene) {
                    s_mail.scene = std::move(str);
                } else if (_part == kPartStage) {
                    s_mail.stage = std::move(str);
                } else {
                    s_mail.actors      = std::move(ids);
                    s_mail.positionsOk = ok;
                }
            }

            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::uint32_t _gen;
            Part          _part;
        };

        class NameCallback final : public RE::BSScript::IStackCallbackFunctor {
        public:
            explicit NameCallback(std::string a_for) : _for(std::move(a_for)) {}

            void operator()(RE::BSScript::Variable a_result) override {
                if (!a_result.IsString()) return;
                std::lock_guard lock(s_mail.lock);
                if (s_mail.nameFor != _for) return;
                s_mail.name      = std::string(a_result.GetString());
                s_mail.nameReady = true;
            }

            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::string _for;
        };

        std::string Lower(std::string a_s) {
            for (auto& c : a_s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return a_s;
        }

        int PlayerTid() {
            if (!s_playerThreads.empty()) return *s_playerThreads.begin();
            if (!s_unknownThreads.empty()) return *s_unknownThreads.begin();
            return -1;
        }

        void OpenDirtyWindow(double a_now) {
            s_dirtyUntil = (std::max)(s_dirtyUntil, a_now + kDirtyWindow);
            s_nextAt     = (std::min)(s_nextAt, a_now);
        }

        void ClearScene(const char* a_why) {
            const bool had = s_scene.inScene || s_scene.stageKnown;
            const auto serial = s_scene.keySerial;
            s_scene           = PlayerScene{};
            s_scene.keySerial = serial + 1;
            s_queryTid        = -1;
            s_inFlight        = false;
            s_dirtyUntil      = 0.0;
            s_nextAt          = 0.0;
            s_candValid       = false;
            {
                std::lock_guard lock(s_mail.lock);
                s_mail.gen       = ++s_gen;  // answers still on their way are dropped
                s_mail.got       = 0;
                s_mail.nameFor.clear();
                s_mail.nameReady = false;
            }
            if (had) SKSE::log::info("SceneTracker: player scene info cleared ({})", a_why);
        }

        void Dispatch(double a_now) {
            const int tid = PlayerTid();
            const auto it = s_threadQuest.find(tid);
            auto*      form  = it != s_threadQuest.end() ? RE::TESForm::LookupByID(it->second) : nullptr;
            auto*      quest = form ? form->As<RE::TESQuest>() : nullptr;
            auto*      vm    = static_cast<RE::BSScript::IVirtualMachine*>(RE::BSScript::Internal::VirtualMachine::GetSingleton());
            auto*      policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
            RE::BSTSmartPointer<RE::BSScript::Object> obj;
            if (quest && policy) {
                const auto handle = policy->GetHandleForObject(quest->GetFormType(), quest);
                if (handle != policy->EmptyHandle()) {
                    vm->FindBoundObject(handle, "sslThreadController", obj);
                }
            }
            if (!obj) {
                if (!s_loggedNoObject) {
                    s_loggedNoObject = true;
                    SKSE::log::warn("SceneTracker: thread {} - no sslThreadController script on its quest {:08X}; scene and "
                                    "stage stay unknown", tid, it != s_threadQuest.end() ? it->second : 0);
                }
                s_nextAt = a_now + kIdleInterval;
                return;
            }
            s_queryTid = tid;
            ++s_gen;
            {
                std::lock_guard lock(s_mail.lock);
                s_mail.gen         = s_gen;
                s_mail.got         = 0;
                s_mail.scene.clear();
                s_mail.stage.clear();
                s_mail.actors.clear();
                s_mail.positionsOk = false;
            }
            static const RE::BSFixedString kScene("GetActiveScene");
            static const RE::BSFixedString kStage("GetActiveStage");
            static const RE::BSFixedString kPositions("GetPositions");
            const std::pair<const RE::BSFixedString*, Part> calls[] = {
                { &kScene, kPartScene }, { &kStage, kPartStage }, { &kPositions, kPartPositions }
            };
            for (const auto& [fn, part] : calls) {
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb(new ReadCallback(s_gen, part));
                if (!vm->DispatchMethodCall(obj, *fn, RE::MakeFunctionArguments(), cb)) {
                    std::lock_guard lock(s_mail.lock);
                    if (s_mail.gen == s_gen) s_mail.got |= part;  // counts as answered (empty)
                    SKSE::log::warn("SceneTracker: dispatching {} on thread {} failed", fn->c_str(), tid);
                }
            }
            s_inFlight = true;
            s_sentAt   = a_now;
        }

        void RequestSceneName(const std::string& a_sceneId) {
            {
                std::lock_guard lock(s_mail.lock);
                s_mail.nameFor   = a_sceneId;
                s_mail.nameReady = false;
            }
            auto* vm = static_cast<RE::BSScript::IVirtualMachine*>(RE::BSScript::Internal::VirtualMachine::GetSingleton());
            if (!vm) return;
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb(new NameCallback(a_sceneId));
            vm->DispatchStaticCall("SexLabRegistry", "GetSceneName",
                RE::MakeFunctionArguments(RE::BSFixedString(a_sceneId.c_str())), cb);
        }

        std::vector<RE::ActorHandle> ToHandles(const std::vector<RE::FormID>& a_ids) {
            std::vector<RE::ActorHandle> out;
            out.reserve(a_ids.size());
            for (const auto id : a_ids) {
                auto* actor = id ? RE::TESForm::LookupByID<RE::Actor>(id) : nullptr;
                out.push_back(actor ? actor->GetHandle() : RE::ActorHandle{});
            }
            return out;
        }

        void Apply(std::string a_scene, std::string a_stage, std::vector<RE::FormID> a_actors, bool a_positionsOk,
            double a_now) {
            a_scene = Lower(std::move(a_scene));
            a_stage = Lower(std::move(a_stage));
            if (a_scene.empty() || a_stage.empty()) {
                static double s_lastLog = -100.0;
                if (a_now - s_lastLog > 10.0) {
                    s_lastLog = a_now;
                    SKSE::log::info("SceneTracker: thread {} read scene '{}' stage '{}' - an empty id is ignored", s_queryTid,
                        a_scene, a_stage);
                }
                return;
            }
            int slot = -1;
            if (a_positionsOk) {
                for (std::size_t i = 0; i < a_actors.size(); ++i) {
                    if (a_actors[i] == 0x14) {
                        slot = static_cast<int>(i);
                        break;
                    }
                }
            } else {
                a_actors = s_scene.actorIDs;  // keep what we had
                slot     = s_scene.playerSlot;
            }

            const bool sameKey = s_scene.stageKnown && a_scene == s_scene.sceneId && a_stage == s_scene.stageId &&
                                 slot == s_scene.playerSlot;
            if (sameKey) {
                s_candValid = false;
                if (a_actors != s_scene.actorIDs) {
                    s_scene.actorIDs = a_actors;
                    s_scene.actors   = ToHandles(a_actors);
                    ++s_scene.keySerial;
                    SKSE::log::info("SceneTracker: player scene actors changed ({} actor(s)) - same stage", a_actors.size());
                    SceneCam::OnSceneChanged(false);
                }
                return;
            }

            if (!(s_candValid && s_candScene == a_scene && s_candStage == a_stage && s_candSlot == slot)) {
                // First sighting: confirm with one more read before it counts.
                s_candValid = true;
                s_candScene = a_scene;
                s_candStage = a_stage;
                s_candSlot  = slot;
                s_dirtyUntil = (std::max)(s_dirtyUntil, a_now + 0.6);
                return;
            }

            s_candValid = false;
            const bool sceneChanged = a_scene != s_scene.sceneId;
            SKSE::log::info("SceneTracker: player scene key -> scene '{}' stage '{}' player slot {} ({} actor(s), thread {}){}",
                a_scene, a_stage, slot, a_actors.size(), s_queryTid,
                s_scene.stageKnown ? std::format(" - was scene '{}' stage '{}' slot {}", s_scene.sceneId, s_scene.stageId,
                                         s_scene.playerSlot)
                                   : std::string());
            s_scene.stageKnown = true;
            s_scene.sceneId    = a_scene;
            s_scene.stageId    = a_stage;
            s_scene.playerSlot = slot;
            s_scene.actorIDs   = a_actors;
            s_scene.actors     = ToHandles(a_actors);
            ++s_scene.keySerial;
            if (sceneChanged) {
                s_scene.sceneName.clear();
                RequestSceneName(a_scene);
            }
            SceneCam::OnSceneChanged(true);
        }

        // Main thread: the player's thread started (or was re-announced).
        void BeginPlayerScene(int a_tid) {
            if (s_scene.inScene && s_scene.tid == a_tid) {
                OpenDirtyWindow(Now());
                return;
            }
            const auto serial = s_scene.keySerial;
            s_scene           = PlayerScene{};
            s_scene.keySerial = serial + 1;
            s_scene.inScene   = true;
            s_scene.tid       = a_tid;
            s_candValid       = false;
            s_inFlight        = false;
            s_loggedNoObject  = false;
            s_dirtyUntil      = 0.0;
            s_nextAt          = Now();
            OpenDirtyWindow(s_nextAt);
            SceneCam::OnPlayerSceneStart();
        }

        void OnStart(const std::string& a_name, int a_tid, RE::FormID a_sender) {
            const bool wasPlayer = AnyPlayerThread();
            s_threads.insert(a_tid);
            const int has = ThreadHasPlayer(a_sender);
            if (has == 1) {
                s_playerThreads.insert(a_tid);
                s_unknownThreads.erase(a_tid);
            } else if (has == 0) {
                s_playerThreads.erase(a_tid);
                s_unknownThreads.erase(a_tid);
            } else {
                s_unknownThreads.insert(a_tid);
            }
            if (has != 0 && a_sender) {
                s_threadQuest[a_tid] = a_sender;
            } else {
                s_threadQuest.erase(a_tid);
            }
            SKSE::log::info("SceneTracker: {} thread {} ({}) - {} thread(s) running, player scene {}", a_name, a_tid,
                has == 1 ? "player" : has == 0 ? "NPCs only" : "sender not inspectable - treated as the player's",
                s_threads.size(), AnyPlayerThread() ? "active" : "none");
            if (has != 0 && PlayerTid() == a_tid) {
                BeginPlayerScene(a_tid);
            }
            if (has != 0 && !wasPlayer) {
                SlccBridge::OnPlayerSceneStart(a_tid);
            }
        }

        void OnEnd(const std::string& a_name, int a_tid) {
            const bool wasPlayer = AnyPlayerThread();
            const bool known     = s_threads.erase(a_tid) > 0;
            s_playerThreads.erase(a_tid);
            s_unknownThreads.erase(a_tid);
            s_threadQuest.erase(a_tid);
            if (!known) return;  // AnimationEnd after AnimationEnding, or a thread started before a load
            SKSE::log::info("SceneTracker: {} thread {} - {} thread(s) running, player scene {}", a_name, a_tid,
                s_threads.size(), AnyPlayerThread() ? "active" : "none");
            if (wasPlayer && !AnyPlayerThread()) {
                ClearScene("the player's scene ended");
                SceneCam::OnPlayerSceneEnd();  // 0.10.0: orbit / eye view / pending shot end with the scene
                SlccBridge::OnPlayerSceneEnd(a_tid);
            } else if (s_scene.inScene && s_scene.tid == a_tid) {
                // Another thread with the player still runs: read that one from now on.
                ClearScene("the player's thread ended, another one runs");
                BeginPlayerScene(PlayerTid());
            }
        }

        // 0.10.0: a stage / position event on the player's thread - the stage id changes shortly after it.
        void OnChange(const std::string& a_name, int a_tid) {
            if (!s_scene.inScene || a_tid != s_scene.tid) return;
            SKSE::log::info("SceneTracker: {} on the player's thread {} - re-reading scene and stage", a_name, a_tid);
            OpenDirtyWindow(Now());
            SceneCam::ScheduleTick();
        }

        class ModEventSink : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
        public:
            static ModEventSink* GetSingleton() {
                static ModEventSink instance;
                return &instance;
            }

            // Runs on whatever thread sent the event (Papyrus); only copies it into a main-thread task.
            RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event,
                RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
                if (!a_event || a_event->eventName.empty()) return RE::BSEventNotifyControl::kContinue;
                const std::string_view name(a_event->eventName.c_str());
                for (const auto& def : kDefs) {
                    if (name != def.name) continue;
                    int tid = -1;
                    const std::string_view str(a_event->strArg.empty() ? "" : a_event->strArg.c_str());
                    if (!ParseTid(str, tid)) {
                        if (name.starts_with("Hook")) {
                            SKSE::log::info("SceneTracker: '{}' without a thread id in strArg - ignored", name);
                            break;
                        }
                        tid = static_cast<int>(a_event->numArg);
                    }
                    const RE::FormID sender = a_event->sender ? a_event->sender->GetFormID() : 0;
                    const Kind       kind   = def.kind;
                    std::string      copy(name);
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([copy, tid, sender, kind] {
                            if (kind == Kind::kStart) {
                                OnStart(copy, tid, sender);
                            } else if (kind == Kind::kEnd) {
                                OnEnd(copy, tid);
                            } else {
                                OnChange(copy, tid);
                            }
                        });
                    }
                    break;
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            ModEventSink() = default;
        };
    }

    void Register() {
        auto* source = SKSE::GetModCallbackEventSource();
        if (!source) {
            SKSE::log::error("SceneTracker: GetModCallbackEventSource() returned null - no SexLab scene tracking");
            return;
        }
        source->AddEventSink(ModEventSink::GetSingleton());
        SKSE::log::info("SceneTracker: listening for SexLab AnimationStart / AnimationEnding / AnimationEnd and the stage "
                        "events (StageStart / StageEnd / ActorsRelocated / PositionChange / AnimationChange)");
    }

    void Reset(const char* a_why) {
        if (!s_threads.empty()) {
            SKSE::log::info("SceneTracker: {} - forgetting {} tracked thread(s)", a_why, s_threads.size());
        }
        s_threads.clear();
        s_playerThreads.clear();
        s_unknownThreads.clear();
        s_threadQuest.clear();
        ClearScene(a_why);
    }

    bool IsSceneActive() { return !s_threads.empty(); }

    bool PlayerSceneActive() { return AnyPlayerThread(); }

    const PlayerScene& GetPlayerScene() { return s_scene; }

    bool NeedsTick() { return s_scene.inScene; }

    void Tick() {
        if (!s_scene.inScene) return;
        const double now = Now();

        // Scene name (one static call per new scene id).
        {
            std::string name;
            bool        ready = false;
            {
                std::lock_guard lock(s_mail.lock);
                if (s_mail.nameReady && s_mail.nameFor == s_scene.sceneId) {
                    name             = s_mail.name;
                    ready            = true;
                    s_mail.nameReady = false;
                }
            }
            if (ready) {
                s_scene.sceneName = std::move(name);
                SKSE::log::info("SceneTracker: scene '{}' is \"{}\"", s_scene.sceneId, s_scene.sceneName);
            }
        }

        if (s_inFlight) {
            bool                    complete = false;
            std::string             scene, stage;
            std::vector<RE::FormID> actors;
            bool                    positionsOk = false;
            {
                std::lock_guard lock(s_mail.lock);
                if (s_mail.gen == s_gen && s_mail.got == kPartsAll) {
                    complete    = true;
                    scene       = std::move(s_mail.scene);
                    stage       = std::move(s_mail.stage);
                    actors      = std::move(s_mail.actors);
                    positionsOk = s_mail.positionsOk;
                    s_mail.got  = 0;
                }
            }
            if (complete) {
                s_inFlight = false;
                if (s_queryTid == s_scene.tid) {
                    Apply(std::move(scene), std::move(stage), std::move(actors), positionsOk, now);
                }
                s_nextAt = now + (now < s_dirtyUntil ? kDirtyInterval : kIdleInterval);
            } else if (now - s_sentAt > kAnswerTimeout) {
                SKSE::log::info("SceneTracker: no answer from the Papyrus VM for thread {} in {:.0f} s - asking again",
                    s_queryTid, kAnswerTimeout);
                s_inFlight = false;
                s_nextAt   = now;
            }
        }

        if (!s_inFlight && now >= s_nextAt) {
            Dispatch(now);
        }
    }
}
