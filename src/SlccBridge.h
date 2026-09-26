#pragma once

// 0.7.7: SLCC (SexLab Cinematic Camera) hand-over.
//
// SLCC drives the camera through FreeCamera Framework timelines. While one runs, FCFW re-enters its
// free-camera carrier the frame after any tfc, so a plain toggle can never give the user a free cam.
// SLCC has no pause API; its only supported release is its Director toggle hotkey (Director OFF
// stops SLCC's FCFW playback without ending the scene, ON re-enters). So when the user asks for
// free-fly while FCFW owns the camera, TFCam presses SLCC's Director key for them, waits for FCFW to
// go idle, then enters the vanilla free cam; leaving free-fly presses the Director key again.
//
// Requests: the TFCam free-fly key, console `tfc` (the ToggleFreeCamera command is wrapped), and the
// SKSE message 'TFCF' from SLUI. Everything runs on the main thread, stepped once per frame.
//
// 0.7.8: camera cycle TFCam -> SLCC -> Off -> TFCam during a player SexLab scene with SLCC loaded.
// Driven by TFCam's free-fly key, and by SexLab P+'s own "Toggle Free Camera" hotkey: P+ toggles the
// free cam through PapyrusUtil's MiscUtil.ToggleFreeCamera, a direct PlayerCamera::ToggleFreeCameraMode
// call (no console command), so TFCam follows that toggle from its FreeCameraState Begin/End hooks,
// gated on a physical press of P+'s key. The first press of a scene goes SLCC -> TFCam.
// With SexLab P+ Prism loaded, a player scene has no Off: Prism re-enters the vanilla free cam within
// 0.5 s whenever it is off (SLP_PrismController.EnsureFreecam), so the cycle there is TFCam <-> SLCC.
namespace SlccBridge {
    // Shared message contract with SLUI (E:\dev\SexLabUI-PrismaUI): SLUI sends
    //   SKSE::GetMessagingInterface()->Dispatch(kMsgToggleFreeFly, nullptr, 0, "TFCam");
    // and TFCam treats it exactly like a press of its free-fly key.
    inline constexpr std::uint32_t kMsgToggleFreeFly = 0x54464346;  // 'TFCF'
    inline constexpr const char*   kSluiPluginName   = "SLUI";  // SLUI's SKSEPluginInfo .Name

    void OnPostLoad();                        // kPostLoad: listen for 'TFCF' from SLUI (only when TFCam will arm)
    void Install();                           // kDataLoaded: wrap console tfc, arm the bridge
    void RequestToggle(const char* a_source); // 'TFCF' (TFCam free-fly <-> SLCC)
    void RequestCycle(const char* a_source);  // 0.7.8: TFCam free-fly key (cycle in a scene, plain toggle outside)
    void OnFreeCamBegin(bool a_tfcamDriving); // FreeCameraState::Begin hook, after vanilla
    void OnFreeCamEnd(bool a_fcfwOwnedAtEnd); // FreeCameraState::End hook
    void OnGameLoaded(const char* a_why);     // kPostLoadGame / kNewGame
    void Pump();                              // cheap per-frame backup kick (input sink / free-cam Update)
    bool IsInjecting();                       // our injected ButtonEvent is being dispatched right now

    // 0.7.8
    void NoteKeyDown(std::uint32_t a_dikCode);    // input sink: keyboard key-down while no menu is open
    bool IsPplusFreeCamKey(std::uint32_t a_dikCode);
    void OnPlayerSceneStart(int a_tid);           // SceneTracker, main thread
    void OnPlayerSceneEnd(int a_tid);             // SceneTracker, main thread: last player thread ended
}
