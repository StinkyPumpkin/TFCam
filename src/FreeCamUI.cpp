#include "FreeCamUI.h"
#include "FreeCamController.h"
#include "FreezeTime.h"
#include "CameraLight.h"
#include "SceneTracker.h"
#include "iHUDBridge.h"

#include "SKSEMenuFramework.h"

#include <atomic>

namespace FreeCamUI {

    static std::atomic<bool> s_visible{false};
    static std::atomic<bool> s_wantShow{false};

    bool IsVisible() { return s_visible.load(std::memory_order_relaxed); }

    void Show() { s_wantShow.store(true, std::memory_order_relaxed); }
    void Hide() { s_wantShow.store(false, std::memory_order_relaxed); }
    void Toggle() {
        bool cur = s_wantShow.load(std::memory_order_relaxed);
        s_wantShow.store(!cur, std::memory_order_relaxed);
    }

    static constexpr int kPanelFlags =
        ImGuiMCP::ImGuiWindowFlags_NoTitleBar
        | ImGuiMCP::ImGuiWindowFlags_NoResize
        | ImGuiMCP::ImGuiWindowFlags_NoScrollbar
        | ImGuiMCP::ImGuiWindowFlags_NoCollapse
        | ImGuiMCP::ImGuiWindowFlags_NoSavedSettings
        | ImGuiMCP::ImGuiWindowFlags_NoFocusOnAppearing
        | ImGuiMCP::ImGuiWindowFlags_NoNav
        | ImGuiMCP::ImGuiWindowFlags_NoDocking
        | ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize;

    static void __stdcall RenderPanel() {
        // Hidden by iHUD
        if (iHUDBridge::IsHiddenByExternal()) {
            s_visible.store(false, std::memory_order_relaxed);
            return;
        }

        // Not requested
        if (!s_wantShow.load(std::memory_order_relaxed)) {
            s_visible.store(false, std::memory_order_relaxed);
            return;
        }

        // Only show when freecam is active
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || !cam->IsInFreeCameraMode()) {
            s_visible.store(false, std::memory_order_relaxed);
            return;
        }

        s_visible.store(true, std::memory_order_relaxed);

        // Position in top-left area
        ImGuiMCP::SetNextWindowPos({ 20.0f, 200.0f }, ImGuiMCP::ImGuiCond_FirstUseEver, { 0.0f, 0.0f });
        ImGuiMCP::SetNextWindowBgAlpha(0.75f);

        bool open = true;
        if (!ImGuiMCP::Begin("##FreeCamClaude", &open, kPanelFlags)) {
            ImGuiMCP::End();
            return;
        }

        if (!open) {
            s_wantShow.store(false, std::memory_order_relaxed);
            ImGuiMCP::End();
            return;
        }

        // Title
        ImGuiMCP::TextColored({ 0.4f, 0.8f, 1.0f, 1.0f }, "FreeCam");
        ImGuiMCP::SameLine();
        ImGuiMCP::TextColored({ 0.6f, 0.6f, 0.6f, 1.0f }, "Claude");
        ImGuiMCP::Separator();

        // --- FOV & Roll ---
        if (cam) {
            ImGuiMCP::Text("FOV: %.0f", cam->worldFOV);
            ImGuiMCP::SameLine();
        }
        ImGuiMCP::Text("  Roll: %.1f", FreeCam::GetRollDegrees());

        // --- Light & Freeze status ---
        if (CameraLight::IsActive()) {
            ImGuiMCP::TextColored({ 1.0f, 0.9f, 0.3f, 1.0f }, "Light: ON");
        } else {
            ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "Light: OFF");
        }
        ImGuiMCP::SameLine();
        if (FreezeTime::IsFrozen()) {
            ImGuiMCP::TextColored({ 0.3f, 0.8f, 1.0f, 1.0f }, "  Freeze: ON");
        } else {
            ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "  Freeze: OFF");
        }

        // --- Scene info ---
        if (SceneTracker::IsSceneActive()) {
            ImGuiMCP::TextColored({ 0.2f, 1.0f, 0.2f, 1.0f }, "Scene Active");
        }

        ImGuiMCP::Separator();

        // --- Controls reference ---
        ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, "Shift+LMB Light  Shift+RMB Freeze");
        ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, "MMB Screenshot    Wheel FOV");
        ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, "Q/E Roll  R Reset  U UI");

        ImGuiMCP::Separator();

        // --- Reset button ---
        if (ImGuiMCP::Button("Reset FOV + Roll")) {
            FreeCam::ResetAll();
        }

        ImGuiMCP::End();
    }

    void Register() {
        if (!SKSEMenuFramework::IsInstalled()) {
            SKSE::log::error("FreeCamUI: SKSE Menu Framework not found -- UI disabled");
            return;
        }

        SKSEMenuFramework::AddHudElement(RenderPanel);
        SKSE::log::info("FreeCamUI: HUD element registered");
    }
}
