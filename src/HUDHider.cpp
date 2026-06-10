#include "HUDHider.h"

#include <RE/G/GFxMovieView.h>
#include <RE/G/GFxValue.h>
#include <RE/U/UI.h>

#include <iterator>

namespace HUDHider {

    static bool s_enabled = false;  // checkbox state
    static bool s_hidden  = false;  // currently hidden?

    // ---- GFx alpha helper (same approach as iHUD--Claude) ----
    static bool SetGfxAlpha(RE::GFxMovieView* view, const char* path, double alpha) {
        RE::GFxValue obj;
        if (!view->GetVariable(&obj, path)) return false;
        if (!(obj.IsObject() || obj.IsDisplayObject())) return false;
        RE::GFxValue::DisplayInfo info;
        if (!obj.GetDisplayInfo(&info)) return false;
        info.SetAlpha(alpha);
        info.SetVisible(alpha > 0.0);
        obj.SetDisplayInfo(info);
        return true;
    }

    static void SetHudMenuPathAlpha(const char* path, double alpha) {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        auto view = ui->GetMovieView("HUD Menu");
        if (!view) return;
        SetGfxAlpha(view.get(), path, alpha);
    }

    // ---- STB widget menus (each is a separate registered Scaleform menu) ----
    static constexpr const char* kSTBMenuNames[] = {
        "equipWidget_STB",
        "gametimeWidget",
        "goldWidget",
        "lvlWidget",
        "playtimeWidget",
        "ResistWidget",
        "shoutWidget",
        "weightWidget",
        "STBActiveEffects",
    };

    static bool SetMenuRootAlpha(const char* menuName, double alpha) {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        auto view = ui->GetMovieView(menuName);
        if (!view) return false;

        RE::GFxValue root;
        if (!view->GetVariable(&root, "_root")) return false;
        if (!(root.IsObject() || root.IsDisplayObject())) return false;

        RE::GFxValue::DisplayInfo info;
        if (!root.GetDisplayInfo(&info)) return false;
        info.SetAlpha(alpha);
        info.SetVisible(alpha > 0.0);
        root.SetDisplayInfo(info);
        return true;
    }

    static void SetSTBAlpha(double alpha) {
        for (const char* name : kSTBMenuNames) {
            SetMenuRootAlpha(name, alpha);
        }
    }

    // ---- ArousedWidgetClaude messaging (same contract as iHUD--Claude) ----
    enum ArousedMsg : uint32_t {
        kHideAll    = 1,
        kRestoreAll = 2,
    };

    static void SendArousedMessage(uint32_t type) {
        auto* mi = SKSE::GetMessagingInterface();
        if (!mi) return;
        // Broadcast to all listeners (nullptr = all plugins)
        mi->Dispatch(type, nullptr, 0, nullptr);
    }

    // ---- Hide / Show everything ----
    static void HideHUD() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        auto view = ui->GetMovieView("HUD Menu");
        if (!view) return;

        // Main HUD (compass, bars, crosshair, notifications, subtitles)
        SetGfxAlpha(view.get(), "_root.HUDMovieBaseInstance", 0.0);

        // SkyUI Widget Framework container (PSQ bar, etc.)
        SetGfxAlpha(view.get(), "_root.WidgetContainer", 0.0);

        // TrueHUD player bars (separate Scaleform menu)
        SetMenuRootAlpha("TrueHUD", 0.0);

        // STB widgets (separate menus)
        SetSTBAlpha(0.0);

        // ArousedWidgetClaude (ImGui overlay, SKSE messaging)
        SendArousedMessage(kHideAll);

        s_hidden = true;
        SKSE::log::info("HUDHider: hidden (HUD + TrueHUD + STB + ArousedWidget)");
    }

    static void ShowHUD() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return;
        auto view = ui->GetMovieView("HUD Menu");
        if (!view) return;

        SetGfxAlpha(view.get(), "_root.HUDMovieBaseInstance", 100.0);
        SetGfxAlpha(view.get(), "_root.WidgetContainer", 100.0);

        SetMenuRootAlpha("TrueHUD", 100.0);

        SetSTBAlpha(100.0);

        SendArousedMessage(kRestoreAll);

        s_hidden = false;
        SKSE::log::info("HUDHider: shown (all restored)");
    }

    // ---- Public API ----

    void SetEnabled(bool enabled) { s_enabled = enabled; }
    bool IsEnabled() { return s_enabled; }

    void OnFreeCamEnter() {
        if (s_enabled && !s_hidden) {
            HideHUD();
        }
    }

    void OnFreeCamExit() {
        if (s_hidden) {
            ShowHUD();
        }
    }
}
