#pragma once

#include <string>

struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;

// 0.18.0: TFCam's own image pass on the finished world picture - after ENB / the game's own post-processing, before the
// HUD, the menus and SKSE Menu Framework's windows, so none of those blur and videos have no UI:
//   motion blur from the camera's turn (film shutter), zoom blur from moving forward and from FOV changes (crash zoom,
//   Action FOV), lens distortion, colour fringes, vignette, anamorphic streaks from bright lights, cinema bars.
// The same spot feeds the video recorder (VideoCapture). Off unless something is turned on (TFCam > Image and video).
//
// Hooked at the game's menu draw call in Main::Draw (SE 35556 + 0x3ab, AE 36555 + 0x371 - where SKSE Menu Framework
// draws): main thread, the game's D3D11 immediate context, the render target the menus are about to draw on.
namespace FrameFx {

    enum When : int { kWhenOff = 0, kWhenCamera = 1, kWhenAlways = 2 };  // camera = the free camera or the fixed camera

    struct Settings {
        int   motionBlur  = kWhenCamera;  // when turning the camera smears the picture
        float shutter     = 0.5f;         // 0.05..1: how much of a frame's movement is smeared (0.5 = film's 180 degrees)
        float maxBlur     = 0.05f;        // 0.01..0.15: the longest smear, part of the screen width
        bool  zoomBlur    = true;         // moving forward and FOV changes smear outward from the middle
        int   lens        = kWhenCamera;  // when the lens effects below show
        float distortion  = 0.0f;         // -1..1: barrel (+) / pincushion (-)
        float fringe      = 0.0f;         // 0..1: colour fringes toward the edges
        float vignette    = 0.0f;         // 0..1: darker corners
        float streaks     = 0.0f;         // 0..1: anamorphic (blue, sideways) streaks from bright lights
        float streakFrom  = 0.85f;        // 0.3..1: how bright a light has to be to streak
        bool  bars        = false;        // cinema bars (2.39:1)
    };

    Settings&   Get();
    void        Clamp();
    void        Install();  // kDataLoaded
    std::string Status();   // any thread (menu)
    // 11-13: whether the edge blur can read the game's depth buffer to keep the subject sharp (any thread, menu)
    std::string DepthStatus();

    // For VideoCapture, only inside the hook: draw a_src onto a_rtv (a_w x a_h, stretched) with one of these shaders.
    enum Shader : int { kShaderCopy = 0, kShaderLuma = 1, kShaderChroma = 2 };
    void Blit(ID3D11RenderTargetView* a_rtv, ID3D11ShaderResourceView* a_src, unsigned a_w, unsigned a_h, int a_shader);
}
