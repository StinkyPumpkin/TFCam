#pragma once

namespace FreeCamMenu {
    void Register();
    void LoadSettings();
    int  GetFreeFlyKey();

    bool IsCapturingKey();
    void StartKeyCapture();
    void OnKeyCaptured(int dxScanCode);
    void CancelCapture();
}
