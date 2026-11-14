#pragma once

namespace HUDHider {
    void SetEnabled(bool enabled);   // checkbox state
    bool IsEnabled();

    void OnFreeCamEnter();           // called from FreeCamBeginHook
    void OnFreeCamExit();            // called from FreeCamEndHook
    void OnGameLoaded(bool a_freeCam);  // a load rebuilt the HUD: hide it again if the free camera is still on
}
