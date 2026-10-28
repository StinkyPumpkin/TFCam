#include "CamPathWindow.h"
#include "CamPath.h"
#include "FreeCamMenu.h"

#include "SKSEMenuFramework.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace CamPathWindow {

    namespace {
        SKSEMenuFramework::Model::WindowInterface* s_window = nullptr;
        std::atomic<long long>                     s_closedAt{ 0 };  // ms; the key that closed it must not reopen it
        char                                       s_saveName[48] = {};
        bool                                       s_confirmClear = false;

        long long NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Font Awesome 6 (solid) icons
        constexpr unsigned kIconCamera     = 0xf030;
        constexpr unsigned kIconPlay       = 0xf04b;
        constexpr unsigned kIconPause      = 0xf04c;
        constexpr unsigned kIconStop       = 0xf04d;
        constexpr unsigned kIconSnow       = 0xf2dc;
        constexpr unsigned kIconRecord     = 0xf111;
        constexpr unsigned kIconCross      = 0xf05b;
        constexpr unsigned kIconUser       = 0xf007;
        constexpr unsigned kIconTerminal   = 0xf120;
        constexpr unsigned kIconGo         = 0xf124;
        constexpr unsigned kIconPin        = 0xf3c5;
        constexpr unsigned kIconTrash      = 0xf1f8;
        constexpr unsigned kIconSave       = 0xf0c7;
        constexpr unsigned kIconOpen       = 0xf07c;
        constexpr unsigned kIconWalk       = 0xf554;
        constexpr unsigned kIconRepeat     = 0xf363;

        const ImGuiMCP::ImVec4 kGrey{ 0.6f, 0.6f, 0.6f, 1.0f };
        const ImGuiMCP::ImVec4 kGold{ 1.0f, 0.82f, 0.35f, 1.0f };
        const ImGuiMCP::ImVec4 kRed{ 0.85f, 0.3f, 0.3f, 1.0f };

        std::string Icon(unsigned a_code, const char* a_text) { return FontAwesome::UnicodeToUtf8(a_code) + "  " + a_text; }

        // A big icon + words button. a_tint: 0 normal, 1 green (go), 2 red (stop / record), 3 blue (on)
        bool Big(unsigned a_icon, const char* a_text, const char* a_id, int a_tint = 0, bool a_enabled = true) {
            static const ImGuiMCP::ImVec4 tints[] = { { 0.26f, 0.30f, 0.38f, 1.0f }, { 0.20f, 0.50f, 0.28f, 1.0f },
                { 0.62f, 0.20f, 0.20f, 1.0f }, { 0.20f, 0.38f, 0.62f, 1.0f } };
            ImGuiMCP::BeginDisabled(!a_enabled);
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, tints[a_tint]);
            FontAwesome::PushSolid();
            const bool hit = ImGuiMCP::Button((Icon(a_icon, a_text) + "##" + a_id).c_str(), ImGuiMCP::ImVec2(0.0f, 50.0f));
            FontAwesome::Pop();
            ImGuiMCP::PopStyleColor(1);
            ImGuiMCP::EndDisabled();
            return hit;
        }

        bool Small(unsigned a_icon, const char* a_text, const char* a_id) {
            FontAwesome::PushSolid();
            const bool hit = ImGuiMCP::Button((Icon(a_icon, a_text) + "##" + a_id).c_str());
            FontAwesome::Pop();
            return hit;
        }

        void Tip(const char* a_text) {
            if (ImGuiMCP::IsItemHovered(0)) ImGuiMCP::SetTooltip("%s", a_text);
        }

        void Close() {
            if (s_window && s_window->IsOpen.exchange(false)) s_closedAt = NowMs();
        }

        // One camera's card. Returns true when the options changed (the caller hands them to CamPath::EditCam).
        bool CamCard(int a_i, int a_count, bool a_loop, bool a_at, CamPath::Cam& a_c) {
            bool changed = false;
            ImGuiMCP::PushID(a_i);
            const std::string title = std::format("Camera {}{}###cam", a_i + 1, a_at ? "   (the camera is here)" : "");
            if (a_at) ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, kGold);
            const bool open = ImGuiMCP::CollapsingHeader(title.c_str(), 0);
            if (a_at) ImGuiMCP::PopStyleColor(1);
            if (open) {
                if (Small(kIconGo, "Go here", "go")) CamPath::RequestGoToCam(a_i);
                Tip("Puts the free camera on this camera");
                ImGuiMCP::SameLine();
                if (Small(kIconPin, "Move it to where I am", "here")) CamPath::RequestSetCamHere(a_i);
                Tip("This camera takes the free camera's spot and view");
                ImGuiMCP::SameLine();
                if (Small(kIconTrash, "Delete", "del")) CamPath::RequestDeleteCam(a_i);

                ImGuiMCP::Text("Looks at:");
                ImGuiMCP::SameLine();
                changed |= ImGuiMCP::RadioButton("the pivot##look", &a_c.look, CamPath::kLookPivot);
                ImGuiMCP::SameLine();
                changed |= ImGuiMCP::RadioButton("the way I set it##look", &a_c.look, CamPath::kLookSet);
                ImGuiMCP::SameLine();
                changed |= ImGuiMCP::RadioButton("where it is going##look", &a_c.look, CamPath::kLookAhead);

                ImGuiMCP::SetNextItemWidth(220.0f);
                changed |= ImGuiMCP::SliderFloat("Wait here (seconds)##wait", &a_c.wait, 0.0f, 10.0f, "%.1f", 0);
                changed |= ImGuiMCP::Checkbox("Blur the background##blur", &a_c.blur);
                if (a_c.blur) {
                    ImGuiMCP::SameLine();
                    ImGuiMCP::SetNextItemWidth(160.0f);
                    changed |= ImGuiMCP::SliderFloat("How much##blurs", &a_c.blurStrength, 0.1f, 1.0f, "%.1f", 0);
                }

                const bool hasNext = a_i + 1 < a_count || (a_loop && a_count > 1);
                if (hasNext) {
                    const int next = a_i + 1 < a_count ? a_i + 2 : 1;
                    ImGuiMCP::SeparatorText(std::format("Then to camera {}", next).c_str());
                    ImGuiMCP::Text("Path:");
                    ImGuiMCP::SameLine();
                    changed |= ImGuiMCP::RadioButton("straight line##move", &a_c.move, CamPath::kMoveStraight);
                    ImGuiMCP::SameLine();
                    changed |= ImGuiMCP::RadioButton("curve##move", &a_c.move, CamPath::kMoveCurve);
                    ImGuiMCP::SameLine();
                    changed |= ImGuiMCP::RadioButton("around the pivot##move", &a_c.move, CamPath::kMoveAround);
                    ImGuiMCP::SetNextItemWidth(220.0f);
                    changed |= ImGuiMCP::SliderFloat("Seconds to get there##time", &a_c.time, 0.5f, 30.0f, "%.1f", 0);
                    changed |= ImGuiMCP::Checkbox("Slow start and stop##smooth", &a_c.smooth);
                    ImGuiMCP::SetNextItemWidth(220.0f);
                    changed |= ImGuiMCP::SliderInt("Extra full turns##turns", &a_c.turns, 0, 10, "%d", 0);
                    Tip("Around the pivot: circles round it this many more times on the way.\n"
                        "Straight line or curve with both cameras looking the way you set them: spins in place.");
                    ImGuiMCP::Text("Which way round:");
                    ImGuiMCP::SameLine();
                    changed |= ImGuiMCP::RadioButton("shortest##way", &a_c.way, CamPath::kWayShortest);
                    ImGuiMCP::SameLine();
                    changed |= ImGuiMCP::RadioButton("left##way", &a_c.way, CamPath::kWayLeft);
                    ImGuiMCP::SameLine();
                    changed |= ImGuiMCP::RadioButton("right##way", &a_c.way, CamPath::kWayRight);
                    changed |= ImGuiMCP::Checkbox("Zoom trick (the pivot keeps its size while the background stretches)##zoom", &a_c.zoomTrick);
                }
                ImGuiMCP::Spacing();
            }
            ImGuiMCP::PopID();
            return changed;
        }

        void RenderWindow();
        bool s_inWindow = false;  // between Begin and End (an exception in between still ends the window)

        // Nothing may leave an SKSE Menu Framework callback (an exception there takes the game down).
        void __stdcall Render() {
            try {
                RenderWindow();
            } catch (const std::exception& e) {
                if (std::exchange(s_inWindow, false)) ImGuiMCP::End();
                static bool s_logged = false;
                if (!std::exchange(s_logged, true)) SKSE::log::error("CamPathWindow: drawing the window failed: {}", e.what());
            } catch (...) {
                if (std::exchange(s_inWindow, false)) ImGuiMCP::End();
                static bool s_logged = false;
                if (!std::exchange(s_logged, true)) SKSE::log::error("CamPathWindow: drawing the window failed");
            }
        }

        void RenderWindow() {
            // Big enough to read on a 4K screen without squinting (it can be resized and moved; ImGui remembers).
            ImGuiMCP::SetNextWindowSize(ImGuiMCP::ImVec2(1000.0f, 1150.0f), ImGuiMCP::ImGuiCond_FirstUseEver);
            bool open = true;
            s_inWindow = true;
            if (!ImGuiMCP::Begin("Camera Path##tfcamPath", &open, 0)) {
                s_inWindow = false;
                ImGuiMCP::End();
                if (!open) Close();
                return;
            }
            if (!open) {
                s_inWindow = false;
                ImGuiMCP::End();
                Close();
                return;
            }
            ImGuiMCP::SetWindowFontScale(1.5f);  // large, plain words: usable without reading glasses or a manual
            const auto  s   = CamPath::GetSnapshot();
            auto&       set = CamPath::Get();
            const int   key = set.openKey;
            ImGuiMCP::TextColored(kGrey, "%s", key ? std::format("{} closes this window so you can fly the camera.", FreeCamMenu::KeyName(key)).c_str()
                                                   : "The X closes this window. Bind a key to open it: FreeCam > Camera Path.");
            if (!s.freeCam) ImGuiMCP::TextColored(kRed, "The free camera is off - turn it on (tfc) to add cameras.");

            // ---- the big buttons
            if (Big(kIconCamera, "Add camera here", "add", 3, s.freeCam && !s.recording)) CamPath::RequestAddCam();
            Tip("A new camera where the free camera is, looking the way it looks");
            ImGuiMCP::SameLine();
            if (s.playing && !s.paused) {
                if (Big(kIconPause, "Pause", "pause", 0)) CamPath::RequestPause();
            } else {
                if (Big(kIconPlay, s.paused ? "Go on" : "Play", "play", 1, s.cams.size() >= 2)) {
                    CamPath::RequestPlay(1);
                    Close();  // watch it
                }
                Tip("Plays the cameras in order (the window closes so you can watch)");
            }
            ImGuiMCP::SameLine();
            if (Big(kIconStop, "Stop", "stop", 2, s.playing)) CamPath::RequestPlay(0);
            ImGuiMCP::SameLine();
            if (Big(kIconSnow, s.frozen ? "Unfreeze world" : "Freeze world", "freeze", s.frozen ? 3 : 0)) CamPath::RequestFreezeWorld();
            Tip("Stops everyone in the world (the camera still moves)");

            if (s.recording) {
                if (Big(kIconStop, "Stop recording", "rec", 2)) CamPath::RequestRecord(0);
                ImGuiMCP::SameLine();
                ImGuiMCP::TextColored(kRed, "Recording - %d cameras so far", static_cast<int>(s.cams.size()));
            } else {
                if (Big(kIconRecord, "Record manual path", "rec", 2, s.freeCam)) {
                    CamPath::RequestRecord(1);
                    Close();  // fly
                }
                Tip("Fly the camera by hand and it becomes the path.\nThe window closes - fly, then open it again and press Stop recording.\n"
                    "The path you had is kept as \"Before recording\".");
            }
            if (s.playing) {
                ImGuiMCP::ProgressBar(s.progress, ImGuiMCP::ImVec2(-1.0f, 0.0f),
                    std::format("{} - camera {} of {}", s.paused ? "Paused" : "Playing", s.atCam + 1, s.cams.size()).c_str());
            }

            // ---- pivot
            ImGuiMCP::SeparatorText("Pivot - what the path is about");
            FontAwesome::PushSolid();
            ImGuiMCP::TextColored(kGold, "%s", Icon(kIconCross, s.pivotName.c_str()).c_str());
            FontAwesome::Pop();
            if (Small(kIconCross, "Pick what the camera points at", "pick")) CamPath::RequestPickPivot();
            Tip("Point the middle of the screen at a person or a thing first");
            ImGuiMCP::SameLine();
            if (Small(kIconUser, "Me", "me")) CamPath::RequestPivotMe();
            ImGuiMCP::SameLine();
            if (Small(kIconTerminal, "Console pick", "console")) CamPath::RequestPivotConsole();
            Tip("The person or thing you clicked in the console");
            bool follow = s.follow;
            FontAwesome::PushSolid();
            const bool followHit = ImGuiMCP::Checkbox(Icon(kIconWalk, "Follow the pivot (the whole path moves and turns with it)##follow").c_str(), &follow);
            FontAwesome::Pop();
            if (followHit) CamPath::SetFollow(follow);
            ImGuiMCP::SetNextItemWidth(200.0f);
            if (ImGuiMCP::SliderFloat("Look a bit higher / lower##aim", &set.aimHeight, -60.0f, 60.0f, "%.0f", 0)) {
                CamPath::Clamp();
                FreeCamMenu::SaveCamPathSettings();
            }

            // ---- the path
            ImGuiMCP::SeparatorText("The path");
            bool loop = s.loop;
            FontAwesome::PushSolid();
            const bool loopHit = ImGuiMCP::Checkbox(Icon(kIconRepeat, "Loop (start again at camera 1)##loop").c_str(), &loop);
            FontAwesome::Pop();
            if (loopHit) CamPath::SetLoop(loop);
            ImGuiMCP::SetNextItemWidth(200.0f);
            if (ImGuiMCP::SliderFloat("Smoothness##smooth", &set.smoothness, 0.0f, 1.0f, "%.2f", 0)) {
                CamPath::Clamp();
                FreeCamMenu::SaveCamPathSettings();
            }
            Tip("Takes the edge off sudden turns and speed changes (0 = exactly on the path)");
            ImGuiMCP::SetNextItemWidth(200.0f);
            ImGuiMCP::InputText("##savename", s_saveName, sizeof(s_saveName), 0, nullptr, nullptr);
            ImGuiMCP::SameLine();
            if (Small(kIconSave, "Save as", "saveas")) CamPath::RequestSaveAs(s_saveName);
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(180.0f);
            if (ImGuiMCP::BeginCombo("##open", "Open a saved path", 0)) {
                for (const auto& name : s.savedPaths) {
                    if (ImGuiMCP::Selectable(name.c_str(), false, 0, ImGuiMCP::ImVec2(0.0f, 0.0f))) {
                        CamPath::RequestLoad(name);
                        std::snprintf(s_saveName, sizeof(s_saveName), "%s", name.c_str());
                    }
                }
                if (s.savedPaths.empty()) ImGuiMCP::TextColored(kGrey, "Nothing saved yet");
                ImGuiMCP::EndCombo();
            }
            if (!s_confirmClear) {
                if (Small(kIconTrash, "Clear all cameras", "clear")) s_confirmClear = true;
            } else {
                ImGuiMCP::TextColored(kRed, "Clear all %d cameras?", static_cast<int>(s.cams.size()));
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Yes, clear##clearyes")) {
                    CamPath::RequestClear();
                    s_confirmClear = false;
                }
                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("No##clearno")) s_confirmClear = false;
            }

            // ---- the cameras
            ImGuiMCP::SeparatorText(std::format("Cameras ({})", s.cams.size()).c_str());
            if (s.cams.empty()) {
                ImGuiMCP::TextWrapped("No cameras yet. Fly the free camera somewhere, open this window and press Add camera here. "
                                      "Do it again somewhere else. Then press Play.");
            }
            const int n = static_cast<int>(s.cams.size());
            for (int i = 0; i < n; ++i) {
                CamPath::Cam c = s.cams[i];
                if (CamCard(i, n, s.loop, s.playing && s.atCam == i, c)) CamPath::EditCam(i, c);
            }
            if (!s.lastEvent.empty()) {
                ImGuiMCP::Spacing();
                ImGuiMCP::TextColored(kGrey, "Last: %s", s.lastEvent.c_str());
            }
            s_inWindow = false;
            ImGuiMCP::End();
        }
    }

    void Register() {
        if (s_window) return;
        s_window = SKSEMenuFramework::AddWindow(Render, true);
        if (s_window) s_window->IsOpen = false;
        SKSE::log::info("CamPathWindow: {}", s_window ? "registered" : "SKSE Menu Framework window unavailable");
    }

    void Toggle() {
        if (!s_window) return;
        if (s_window->IsOpen) {
            Close();
            return;
        }
        if (NowMs() - s_closedAt.load() < 300) return;  // the key press that just closed it
        s_window->IsOpen = true;
    }

    bool IsOpen() { return s_window && s_window->IsOpen; }

    bool OnMenuInput(RE::InputEvent* a_event) {
        if (!IsOpen()) return false;
        const int key = CamPath::Get().openKey;
        if (key == 0) return false;
        for (auto* evt = a_event; evt; evt = evt->next) {
            if (evt->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
            auto* btn = evt->AsButtonEvent();
            if (!btn || !btn->IsDown() || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
            if (static_cast<int>(btn->GetIDCode()) == key && !FreeCamMenu::IsCapturingKey()) {
                Close();
                return true;
            }
        }
        return false;
    }
}
