#pragma once

#include <string>

// 0.18.0: TFCam's own image pass on the finished world picture - after ENB / the game's own post-processing, before the
// HUD, the menus and SKSE Menu Framework's windows, so none of those blur. It draws:
//   the Background blur's "blurry round the edge" style (a focus vignette: the picture's own smaller mips toward the
//   screen's edge, the sharp middle on the subject), the archer camera's darker corners, and the aim dot.
// Nothing is drawn unless one of those is on.
//
// Hooked at the game's menu draw call in Main::Draw (SE 35556 + 0x3ab, AE 36555 + 0x371 - where SKSE Menu Framework
// draws): main thread, the game's D3D11 immediate context, the frame buffer the menus are about to draw on.
namespace FrameFx {
    void        Install();  // kDataLoaded
    std::string Status();   // any thread (menu)
}
