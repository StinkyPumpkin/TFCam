#pragma once

namespace FreeCamMenu {
    void Register();
    void LoadSettings();
    void ApplyGameSettings();  // apply camera speed to game setting (call after DataLoaded)
    float GetCameraSpeed();    // current user-set camera speed
    int  GetFreeFlyKey();

    bool IsCapturingKey();
    void StartKeyCapture();
    void OnKeyCaptured(int dxScanCode);
    void CancelCapture();

    // True while the SKSE Menu Framework overlay (our config window) is open, so the
    // controller can suspend mouse handling and let the user interact with the menu.
    bool IsOverlayOpen();
}
