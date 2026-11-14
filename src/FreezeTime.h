#pragma once

namespace FreezeTime {
    void Toggle();
    bool IsFrozen();
    void Restore();
    // 0.18.0: every frame (camera hooks): keeps the freeze on - an SKSE Menu Framework window opening / closing clears
    // the engine's freezeTime when FreezeTimeOnMenu is set.
    void Tick();
}
