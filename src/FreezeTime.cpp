#include "FreezeTime.h"

namespace FreezeTime {

    static bool  s_frozen       = false;
    static float s_savedTimeMult = 1.0f;

    bool IsFrozen() { return s_frozen; }

    void Toggle() {
        if (s_frozen) {
            Restore();
        } else {
            s_savedTimeMult = RE::BSTimer::GetCurrentGlobalTimeMult();
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
        REL::Relocation<float*> timeMult{ RELOCATION_ID(511883, 388443) };
        *timeMult = s_savedTimeMult;
        s_frozen = false;
        SKSE::log::info("FreezeTime: restored (mult={:.2f})", s_savedTimeMult);
    }
}
