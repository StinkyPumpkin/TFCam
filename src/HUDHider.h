#pragma once

namespace HUDHider {
    void SetEnabled(bool enabled);   // checkbox state
    bool IsEnabled();

    void OnFreeCamEnter();           // called from FreeCamBeginHook
    void OnFreeCamExit();            // called from FreeCamEndHook
}
