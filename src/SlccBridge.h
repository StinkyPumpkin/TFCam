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
// 0.7.9: the console tfc wrapper is installed for every user. A tfc typed in the console drives the SLCC
// hand-over as before; a tfc run by a script (console closed - ConsoleUtil.ExecuteCommand) never does, and one
// that would close a free camera the script did not open within 5 s of the Jump key is refused (Poser Hotkeys
// Plus closes any free camera on Jump).
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
    // FreeCameraState Begin / End. Main thread: since 0.7.10 the hooks queue these there (in order) when the camera
    // was toggled from a Papyrus VM thread, so a_selfToggle is IsSelfToggle() as read inside the hook.
    void OnFreeCamBegin(bool a_tfcamDriving, bool a_selfToggle);
    void OnFreeCamEnd(bool a_fcfwOwnedAtEnd, bool a_selfToggle);
    bool IsSelfToggle();                      // TFCam's own ToggleFreeCameraMode call is running right now
    void OnGameLoaded(const char* a_why);     // kPostLoadGame / kNewGame
    void Pump();                              // cheap per-frame backup kick (input sink / free-cam Update)
    bool IsInjecting();                       // our injected ButtonEvent is being dispatched right now

    // 0.7.8
    void NoteKeyDown(std::uint32_t a_dikCode);    // input sink: keyboard key-down while no menu is open
    bool IsPplusFreeCamKey(std::uint32_t a_dikCode);
    void OnPlayerSceneStart(int a_tid);           // SceneTracker, main thread
    void OnPlayerSceneEnd(int a_tid);             // SceneTracker, main thread: last player thread ended
}
