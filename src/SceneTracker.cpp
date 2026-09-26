#include "SceneTracker.h"
#include "SlccBridge.h"

#include <cstdlib>
#include <set>
#include <string>
#include <string_view>

namespace SceneTracker {

    namespace {
        // Main thread only.
        std::set<int> s_threads;         // every running SexLab thread we saw start
        std::set<int> s_playerThreads;   // ...with the player in one of the thread quest's reference aliases
        std::set<int> s_unknownThreads;  // ...whose sender we could not inspect (treated as possibly the player's)

        enum class Kind { kStart, kEnd };

        struct Def {
            std::string_view name;
            Kind             kind;
        };

        // Bare names: P+ Form.SendModEvent(name, tid). "Hook"-prefixed names are P+'s ModEvent.Send
        // custom-argument events, which SKSE hands to Papyrus registrations only; they are listed in
        // case a build ever routes them here too, and are used only when strArg carries a thread id.
        constexpr Def kDefs[] = {
            { "AnimationStart", Kind::kStart },      { "AnimationEnding", Kind::kEnd },
            { "AnimationEnd", Kind::kEnd },          { "HookAnimationStart", Kind::kStart },
            { "HookAnimationEnding", Kind::kEnd },   { "HookAnimationEnd", Kind::kEnd },
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
            SKSE::log::info("SceneTracker: {} thread {} ({}) - {} thread(s) running, player scene {}", a_name, a_tid,
                has == 1 ? "player" : has == 0 ? "NPCs only" : "sender not inspectable - treated as the player's",
                s_threads.size(), AnyPlayerThread() ? "active" : "none");
            if (has != 0 && !wasPlayer) {
                SlccBridge::OnPlayerSceneStart(a_tid);
            }
        }

        void OnEnd(const std::string& a_name, int a_tid) {
            const bool wasPlayer = AnyPlayerThread();
            const bool known     = s_threads.erase(a_tid) > 0;
            s_playerThreads.erase(a_tid);
            s_unknownThreads.erase(a_tid);
            if (!known) return;  // AnimationEnd after AnimationEnding, or a thread started before a load
            SKSE::log::info("SceneTracker: {} thread {} - {} thread(s) running, player scene {}", a_name, a_tid,
                s_threads.size(), AnyPlayerThread() ? "active" : "none");
            if (wasPlayer && !AnyPlayerThread()) {
                SlccBridge::OnPlayerSceneEnd(a_tid);
            }
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
                            } else {
                                OnEnd(copy, tid);
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
        SKSE::log::info("SceneTracker: listening for SexLab AnimationStart / AnimationEnding / AnimationEnd");
    }

    void Reset(const char* a_why) {
        if (!s_threads.empty()) {
            SKSE::log::info("SceneTracker: {} - forgetting {} tracked thread(s)", a_why, s_threads.size());
        }
        s_threads.clear();
        s_playerThreads.clear();
        s_unknownThreads.clear();
    }

    bool IsSceneActive() { return !s_threads.empty(); }

    bool PlayerSceneActive() { return AnyPlayerThread(); }
}
