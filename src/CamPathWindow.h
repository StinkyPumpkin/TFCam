#pragma once

// 0.18.0: the Camera Path window (an SKSE Menu Framework window of its own). Open it with its hotkey while you fly
// the free camera: the mouse comes back so you can click; the same key (or the X) closes it and you fly on.
namespace CamPathWindow {
    void Register();  // after the SKSE Menu Framework page is registered
    void Toggle();    // any thread
    bool IsOpen();
    // SKSE Menu Framework input while it is open: the window's own key closes it; the play / pause and add keys work
    // too (game input is blocked while it is open). True = used.
    bool OnMenuInput(RE::InputEvent* a_event);
    // Clean screen: true while the path plays - the window steps aside, and comes back when it is false again (only if
    // it was this that closed it). Main thread, every frame of the free camera.
    void AutoHide(bool a_hide);
}
