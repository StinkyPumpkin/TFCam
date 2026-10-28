#include "FreezeTime.h"

namespace FreezeTime {

    static bool  s_frozen       = false;
    static float s_savedTimeMult = 1.0f;

    bool IsFrozen() { return s_frozen; }

    void Toggle() {
        if (s_frozen) {
            Restore();
        } else {
            // 0.8.0: NG 9.x reads this same global (RELOCATION_ID(511883, 388443)) as QGlobalTimeMultiplierTarget();
            // 3.7.0 called it GetCurrentGlobalTimeMult.
            s_savedTimeMult = RE::BSTimer::QGlobalTimeMultiplierTarget();
            // Set global time multiplier to 0 via the console command approach
            // The game global g_fGlobalTimeMultiplier is at RELOCATION_ID(511883, 388443)
            REL::Relocation<float*> timeMult{ RELOCATION_ID(511883, 388443) };
            *timeMult = 0.0f;
            s_frozen = true;
            SKSE::log::info("FreezeTime: frozen (saved mult={:.2f})", s_savedTimeMult);
        }
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
        s_frozen = false;
        SKSE::log::info("FreezeTime: restored (mult={:.2f}, the current value was {:g})", s_savedTimeMult, was);
    }
}
