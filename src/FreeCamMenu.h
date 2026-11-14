#pragma once

namespace FreeCamMenu {
    void Register();
    void LoadSettings();
    void ApplyGameSettings();  // apply camera speed to game setting (call after DataLoaded)
    float GetCameraSpeed();    // current user-set camera speed
    void  SetCameraSpeed(float a_speed);  // 0.14.0: Ctrl + mouse wheel in free cam; clamps, applies and saves it
    int  GetFreeFlyKey();

    bool IsCapturingKey();
    void StartKeyCapture();
    void OnKeyCaptured(int dxScanCode);
    void CancelCapture();

    // True while the SKSE Menu Framework overlay (our config window) is open, so the
    // controller can suspend mouse handling and let the user interact with the menu.
    bool IsOverlayOpen();

    // 0.10.0: writes TFCam.ini [SceneCam] only (Scene Camera changes made outside the page: SLUI, the orbit wheel).
    void SaveSceneCamSettings();

    // 0.18.0: the Camera Path window saves its settings through these; key names for its hints.
    void        SaveCamPathSettings();
    const char* KeyName(int a_dxScanCode);
}
