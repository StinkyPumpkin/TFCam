#pragma once

// 0.7.7: read-only view of FreeCamera Framework (FCFW, Nexus 174046). SLCC drives its cinematic
// camera through FCFW timelines, which run inside the vanilla FreeCameraState - so every TFCam
// free-cam hook also fires on SLCC's camera. FcfwOwnsCamera() tells the two apart.
namespace FcfwBridge {
    // kPostLoad: GetModuleHandle + RequestPluginAPI(V1). FCFW missing -> stays inert and every
    // query below answers "not owned", which is exactly the pre-0.7.7 behaviour.
    void Init();

    bool        Available();
    std::size_t ActiveTimelineID();   // 0 when FCFW is missing or idle
    bool        FcfwOwnsCamera();     // an FCFW timeline is playing or recording
}
