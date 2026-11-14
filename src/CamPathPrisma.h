#pragma once

// 0.18.0: the Camera Path editor as a PrismaUI view (Data/PrismaUI/views/TFCam/index.html) - the same work as the SKSE
// Menu Framework window (CamPathWindow), laid out as an editor: cameras on the left, the selected camera on the right,
// a timeline along the bottom with the path's moves, waits and slow motion, and a scrub. Used when PrismaUI is installed
// and Camera Path > "Use the PrismaUI editor" is on; the SMF window stays as the fallback.
//
// The game keeps running behind it (no pause), so a path plays with the editor open; while it has focus TFCam's input
// sink eats the keys (the editor's own keys - open, play, add, video - still work), and a playing path with Clean screen
// on puts it aside until the path pauses or ends. The page talks back through one JS listener, tfcamCmd("cmd|arg|..."),
// handled on the main thread; TFCam pushes the state with tfcamState(json) about ten times a second while it is open.
namespace CamPathPrisma {

    void Init();       // kPostLoad: PrismaUI found -> the view, hidden
    bool Available();  // PrismaUI is there and the view exists
    bool IsOpen();     // shown (or only put aside while a clean-screen path plays)

    void Toggle();  // any thread
    void Open();    // main thread
    void Close();   // main thread
    void CloseByUser(const char* a_how);  // main thread: the user closed it (Esc, Tab, its key, the page) - a playing path ends too

    // TFCam's input sink, first: true = the editor has focus, eat the input (its own keys are handled here).
    bool OnInput(RE::InputEvent* const* a_events);

    void Tick();  // main thread, every frame (FixedCam's camera hook)
}
