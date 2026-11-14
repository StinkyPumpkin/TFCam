#pragma once

#include <vector>

// 10-29: the camera path drawn over the world while the free camera is on - a gold line along every move (dashed for
// a cut), a numbered marker at every camera with a tick the way it looks, and a cross at its pivot. The camera the path
// is at shows brighter. Off with Camera Path > "Show the path in the world", and hidden by Clean screen.
//
// Drawn by an SKSE Menu Framework HUD element on the render thread (projected through the world camera each frame);
// CamPath publishes what to draw from the main thread.
namespace CamPathView {

    struct Mark {
        RE::NiPoint3 pos{};
        RE::NiPoint3 fwd{ 0.0f, 1.0f, 0.0f };  // the way it looks (unit)
        bool         havePivot = false;
        RE::NiPoint3 pivot{};                  // the point it looks at / goes round
    };

    struct Line {
        std::vector<RE::NiPoint3> pts;
        bool                      cut = false;  // a cut: the camera jumps, drawn dashed
    };

    struct View {
        std::vector<Mark> cams;
        std::vector<Line> lines;
        int               at = -1;  // the camera the playing path is at / leaving
    };

    void Register();                // with the Camera Path window (SKSE Menu Framework ready)
    void Publish(View&& a_view);    // main thread
    void Clear();                   // main thread: nothing drawn
}
