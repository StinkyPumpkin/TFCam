#include "FreezeTime.h"

namespace FreezeTime {

    // 0.18.0: "Freeze world" stops EVERYTHING, at once. Measured 2026-10-29 (wiki gotcha "Time multiplier target 0"):
    // the time multiplier alone eases down over 3-5 s and never stops FSMP hair / cloth; the engine's freezeTime alone
    // (console tfc 1, SKSE Menu Framework's freeze-on-menu) stops people and hair at once but leaves grass and trees
    // swaying; both together stop all of it. The multiplier goes to kStill (not 0: the engine eases the CURRENT value
    // toward the target in log space, so from exactly 0 it could never come back if anything else restored the target).
    static constexpr float kStill = 1e-6f;

    static bool  s_frozen          = false;
    static float s_savedTimeMult   = 1.0f;
    static bool  s_setFreezeTime   = false;  // we turned the engine's freezeTime on (a tfc 1 freeze is left alone)

    static bool& EngineFreeze() { return RE::Main::GetSingleton()->GetRuntimeData().freezeTime; }

    bool IsFrozen() { return s_frozen; }

    void Toggle() {
        if (s_frozen) {
            Restore();
            return;
        }
        // 0.8.0: NG 9.x reads this same global (RELOCATION_ID(511883, 388443)) as QGlobalTimeMultiplierTarget().
        s_savedTimeMult = RE::BSTimer::QGlobalTimeMultiplierTarget();
        if (!(s_savedTimeMult > 0.0001f)) s_savedTimeMult = 1.0f;  // never "restore" a frozen value
        REL::Relocation<float*> current{ RELOCATION_ID(511882, 388442) };
        REL::Relocation<float*> timeMult{ RELOCATION_ID(511883, 388443) };
        *timeMult      = kStill;
        *current       = kStill;  // at once, no easing down
        s_setFreezeTime = !EngineFreeze();
        EngineFreeze()  = true;
        s_frozen        = true;
        SKSE::log::info("FreezeTime: frozen - everything (saved mult={:.2f}{})", s_savedTimeMult,
            s_setFreezeTime ? "" : ", the engine was frozen already");
    }

    void Restore() {
        if (!s_frozen) return;
        // 0.18.0: the CURRENT multiplier (RELOCATION_ID(511882, 388442)) is written too. The engine eases it toward the
        // target; a long freeze takes it down to exactly 0.0 and from there it never eases back up. User 2026-10-28: the
        // world stayed frozen after Unfreeze, through a fast travel - DevBench read target 1.0, current 0.0; "sgtm 1" freed it.
        REL::Relocation<float*> current{ RELOCATION_ID(511882, 388442) };
        REL::Relocation<float*> timeMult{ RELOCATION_ID(511883, 388443) };
        const float was = *current;
        *timeMult = s_savedTimeMult;
        *current  = s_savedTimeMult;
        if (s_setFreezeTime) EngineFreeze() = false;
        s_setFreezeTime = false;
        s_frozen        = false;
        SKSE::log::info("FreezeTime: restored (mult={:.2f}, the current value was {:g})", s_savedTimeMult, was);
    }

    void Tick() {
        if (!s_frozen) return;
        if (!EngineFreeze()) {
            EngineFreeze() = true;
            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                SKSE::log::info("FreezeTime: the engine's freeze was cleared by someone else (an SMF window?) - set again");
            }
        }
    }
}
