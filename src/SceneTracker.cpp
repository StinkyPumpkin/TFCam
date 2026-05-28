#include "SceneTracker.h"
#include "FreeCamController.h"

#include <atomic>

namespace SceneTracker {

    static std::atomic<int> s_activeScenes{0};

    bool IsSceneActive() {
        return s_activeScenes.load(std::memory_order_relaxed) > 0;
    }

    struct SceneEventDef {
        const char* eventName;
        bool        isStart;
    };

    static constexpr SceneEventDef kEvents[] = {
        { "AnimationStart",      true  },
        { "AnimationEnd",        false },
        { "ostim_scene_started", true  },
        { "ostim_scene_ended",   false },
        { "FG_SceneStart",       true  },
        { "FG_SceneEnd",         false },
    };

    class ModEventSink : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
    public:
        static ModEventSink* GetSingleton() {
            static ModEventSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const SKSE::ModCallbackEvent* event,
            RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
        {
            if (!event || event->eventName.empty())
                return RE::BSEventNotifyControl::kContinue;

            const char* name = event->eventName.c_str();

            for (const auto& def : kEvents) {
                if (std::strcmp(name, def.eventName) != 0) continue;

                if (def.isStart) {
                    auto n = s_activeScenes.fetch_add(1, std::memory_order_relaxed) + 1;
                    SKSE::log::info("SceneTracker: START '{}' active={}", name, n);
                } else {
                    auto n = s_activeScenes.fetch_sub(1, std::memory_order_relaxed) - 1;
                    if (n < 0) {
                        s_activeScenes.store(0, std::memory_order_relaxed);
                        n = 0;
                    }
                    SKSE::log::info("SceneTracker: END '{}' active={}", name, n);
                }
                break;
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        ModEventSink() = default;
    };

    void Register() {
        auto* source = SKSE::GetModCallbackEventSource();
        if (!source) {
            SKSE::log::error("SceneTracker: GetModCallbackEventSource() returned null");
            return;
        }
        source->AddEventSink(ModEventSink::GetSingleton());
        SKSE::log::info("SceneTracker: registered");
    }
}
