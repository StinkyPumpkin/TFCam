#pragma once

// 0.18.0: the Camera Path window (an SKSE Menu Framework window of its own). Open it with its hotkey while you fly
// the free camera: the mouse comes back so you can click; the same key (or the X) closes it and you fly on.
namespace CamPathWindow {
    void Register();  // after the SKSE Menu Framework page is registered
    void Toggle();    // any thread
    bool IsOpen();
    // SKSE Menu Framework input while it is open: the window's own key closes it. True = used.
    bool OnMenuInput(RE::InputEvent* a_event);
}
