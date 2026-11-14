#pragma once

#include <string>
#include <string_view>

struct ID3D11ShaderResourceView;

// 0.18.0: record the finished picture (TFCam's image effects included, no HUD / menus / windows) to an MP4 in
// Videos\TFCam: H.264 through Windows' own encoder (the GPU's hardware encoder when there is one), the game's own sound
// as AAC (Windows 10 2004+: only Skyrim's sound, not Discord or music). Constant frame rate (a slow game frame is held,
// extra frames are skipped). Fragmented MP4, so a file stays playable even if the game crashes mid-recording.
//
// The picture comes from FrameFx's hook (main thread): converted to NV12 on the GPU, read back two frames late (no
// stall), encoded and written on a worker thread; the sound is captured on its own thread.
namespace VideoCapture {

    struct Settings {
        int  height   = 1080;   // 0 = the game's own size; else this height (the width follows the screen's shape)
        int  fps      = 30;     // 24 / 30 / 60
        int  quality  = 1;      // 0 good, 1 high, 2 very high (the bit rate)
        bool audio    = true;   // the game's own sound
        bool withPath = false;  // start and stop with Camera Path playback
        int  key      = 0;      // DX scan code: start / stop; 0 = unset
    };

    Settings& Get();
    void      Clamp();

    // 11-14 round 12 (user: "they should go in overwrite/Movies folder - or we ask them for a path in settings"): where the
    // videos go. "" = Data\Movies (under MO2 a new file there lands in overwrite\Movies); a relative path is from the game's
    // folder. Any thread (the menu edits it, the main thread reads it when a recording starts)
    std::string Folder();
    void        SetFolder(std::string a_folder);
    std::string FolderPath();  // the full folder the next video goes to

    bool Recording();    // any thread: a recording runs or is starting
    bool WantsFrames();  // FrameFx's hook: send the picture
    void RequestStart(std::string_view a_why);  // any thread
    void RequestStop(std::string_view a_why);   // any thread
    void Toggle(std::string_view a_why);        // any thread

    // FrameFx's hook only (main thread, inside its saved D3D state): the finished picture.
    void OnFrame(ID3D11ShaderResourceView* a_src, unsigned a_w, unsigned a_h);

    void        OnGameLoad();  // main thread: a load ends the recording
    std::string Status();      // any thread (menu / window)
}
