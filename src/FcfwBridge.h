#pragma once

// 0.7.7: read-only view of FreeCamera Framework (FCFW, Nexus 174046). SLCC drives its cinematic
// camera through FCFW timelines, which run inside the vanilla FreeCameraState - so every TFCam
// free-cam hook also fires on SLCC's camera. FcfwOwnsCamera() tells the two apart.
//
// Ordering checked in the installed DLL (TimelineManager at +0xD8 = m_activeTimelineID):
//   StartPlayback writes the active ID (rva 0x52865) BEFORE ToggleFreeCameraNotHooked (0x52A07),
//     so FreeCameraState::Begin already sees FcfwOwnsCamera() == true;
//   StopPlayback toggles free cam off (0x53247) BEFORE clearing the ID (0x532AB),
//     so FreeCameraState::End still sees FcfwOwnsCamera() == true.
namespace FcfwBridge {
    // kPostLoad: GetModuleHandle + RequestPluginAPI(V1). FCFW missing -> stays inert and every
    // query below answers "not owned", which is exactly the pre-0.7.7 behaviour.
    void Init();

    bool        Available();
    std::size_t ActiveTimelineID();   // 0 when FCFW is missing or idle
    bool        FcfwOwnsCamera();     // an FCFW timeline is playing or recording
}
