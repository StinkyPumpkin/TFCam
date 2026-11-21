#include "CamPathWindow.h"
#include "CamPath.h"
#include "CamPathView.h"
#include "FixedCam.h"
#include "FreeCamMenu.h"
#include "VideoCapture.h"

#include "SKSEMenuFramework.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace CamPathWindow {

    namespace {
        SKSEMenuFramework::Model::WindowInterface* s_window = nullptr;
        std::atomic<long long>                     s_closedAt{ 0 };  // ms; the key that closed it must not reopen it
        char                                       s_saveName[48] = {};
        bool                                       s_confirmClear = false;
        // 0.18.0: our SKSE Menu Framework fork can keep the game running behind this window (no freeze, no blur), so a
        // path plays with the window open. Without it (the public SMF) the window closes on Play as before.
        bool                                       s_keepsRunning = false;
        std::atomic<bool>                          s_autoHidden{ false };
        bool                                       s_lastAutoHide = false;

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
        constexpr unsigned kIconBack       = 0xf04a;
        constexpr unsigned kIconReverse    = 0xf0d9;
        constexpr unsigned kIconEyeSlash   = 0xf070;
        constexpr unsigned kIconThumbtack  = 0xf08d;
        constexpr unsigned kIconVideo      = 0xf03d;

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

        ImGuiMCP::ImU32 Col(float a_r, float a_g, float a_b, float a_a = 1.0f) {
            return ImGuiMCP::GetColorU32(ImGuiMCP::ImVec4{ a_r, a_g, a_b, a_a });
        }

        // When the camera reaches each camera, seconds from the start of the path (the slider's marks).
        std::vector<float> ArrivalTimes(const CamPath::Snapshot& a_s) {
            std::vector<float> out;
            const int          n = static_cast<int>(a_s.cams.size());
            float              t = 0.0f;
            for (int k = 0; k < n; ++k) {
                out.push_back(t);
                t += a_s.cams[k].wait;
                if (k + 1 < n || (a_s.loop && n > 1)) t += CamPath::MoveSeconds(a_s.cams[k]);
            }
            return out;
        }

        // The camera a moment of the path belongs to: waiting at it, or (a_moving) on the way on from it.
        int CamAtTime(const CamPath::Snapshot& a_s, float a_t, bool& a_moving) {
            const int n    = static_cast<int>(a_s.cams.size());
            float     left = a_t;
            for (int k = 0; k < n; ++k) {
                a_moving = false;
                if (left <= a_s.cams[k].wait) return k;
                left -= a_s.cams[k].wait;
                if (k + 1 >= n && !(a_s.loop && n > 1)) return k;
                a_moving      = true;
                const float T = CamPath::MoveSeconds(a_s.cams[k]);
                if (left < T) return k;
                left -= T;
            }
            a_moving = false;
            return n - 1;
        }

        // The move's speed curve (10-29, Movie Maker's idea): time goes across, how far along the camera is goes up.
        // Steep = fast, flat = slow. Drag a round handle (it turns the curve on); the buttons are ready-made curves.
        bool CurveEditor(CamPath::Cam& a_c) {
            bool changed = false;
            struct Preset {
                const char* name;
                float       x1, y1, x2, y2;
            };
            static const Preset kPresets[] = { { "Steady", 0.33f, 0.33f, 0.67f, 0.67f }, { "Slow start and stop", 0.42f, 0.0f, 0.58f, 1.0f },
                { "Slow start", 0.42f, 0.0f, 1.0f, 1.0f }, { "Slow stop", 0.0f, 0.0f, 0.58f, 1.0f },
                { "Fast start, long glide", 0.1f, 0.7f, 0.3f, 1.0f }, { "Hold, then rush", 0.8f, 0.0f, 0.9f, 0.3f } };
            if (ImGuiMCP::RadioButton("Automatic##cvauto", !a_c.curveOn)) {
                a_c.curveOn = false;
                changed     = true;
            }
            Tip("As before: a slow start and stop where the camera waits or the path ends, else straight through");
            for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i) {
                const auto& p  = kPresets[i];
                const bool  on = a_c.curveOn && a_c.curveX1 == p.x1 && a_c.curveY1 == p.y1 && a_c.curveX2 == p.x2 && a_c.curveY2 == p.y2;
                ImGuiMCP::SameLine();
                if (ImGuiMCP::RadioButton(std::format("{}##cv{}", p.name, i).c_str(), on)) {
                    a_c.curveOn = true;
                    a_c.curveX1 = p.x1, a_c.curveY1 = p.y1, a_c.curveX2 = p.x2, a_c.curveY2 = p.y2;
                    changed     = true;
                }
            }
            if (!a_c.curveOn) {
                changed |= ImGuiMCP::Checkbox("Slow start and stop##smooth", &a_c.smooth);
                Tip("Automatic only: the move eases in and out even when the camera does not wait");
            }

            constexpr float  kBox = 220.0f;
            ImGuiMCP::ImVec2 o{};
            ImGuiMCP::GetCursorScreenPos(&o);
            ImGuiMCP::InvisibleButton("##curvebox", ImGuiMCP::ImVec2{ kBox, kBox }, 0);
            const bool       active = ImGuiMCP::IsItemActive();
            static int       s_drag = -1;  // the handle being dragged (one item is active at a time)
            ImGuiMCP::ImVec2 m{};
            ImGuiMCP::GetMousePos(&m);
            auto  at   = [&](float a_x, float a_y) { return ImGuiMCP::ImVec2{ o.x + a_x * kBox, o.y + (1.0f - a_y) * kBox }; };
            float x1   = a_c.curveOn ? a_c.curveX1 : (a_c.smooth ? 0.42f : 0.33f);
            float y1   = a_c.curveOn ? a_c.curveY1 : (a_c.smooth ? 0.0f : 0.33f);
            float x2   = a_c.curveOn ? a_c.curveX2 : (a_c.smooth ? 0.58f : 0.67f);
            float y2   = a_c.curveOn ? a_c.curveY2 : (a_c.smooth ? 1.0f : 0.67f);
            float* hx[2] = { &x1, &x2 };
            float* hy[2] = { &y1, &y2 };
            if (ImGuiMCP::IsItemActivated()) {  // the nearer handle
                float best = 1e9f;
                for (int i = 0; i < 2; ++i) {
                    const auto  p = at(*hx[i], *hy[i]);
                    const float d = std::hypot(p.x - m.x, p.y - m.y);
                    if (d < best) {
                        best   = d;
                        s_drag = i;
                    }
                }
            }
            if (!active) s_drag = -1;
            if (active && s_drag >= 0) {
                const float x = std::clamp((m.x - o.x) / kBox, 0.0f, 1.0f);
                const float y = std::clamp(1.0f - (m.y - o.y) / kBox, 0.0f, 1.0f);
                if (!a_c.curveOn || x != *hx[s_drag] || y != *hy[s_drag]) {
                    *hx[s_drag] = x;
                    *hy[s_drag] = y;
                    a_c.curveOn = true;
                    a_c.curveX1 = x1, a_c.curveY1 = y1, a_c.curveX2 = x2, a_c.curveY2 = y2;
                    changed     = true;
                }
            }
            namespace D = ImGuiMCP::ImDrawListManager;
            auto*       dl    = ImGuiMCP::GetWindowDrawList();
            const auto  ink   = a_c.curveOn ? Col(1.0f, 0.82f, 0.35f) : Col(0.6f, 0.6f, 0.6f);
            D::AddRectFilled(dl, o, ImGuiMCP::ImVec2{ o.x + kBox, o.y + kBox }, Col(0.10f, 0.13f, 0.20f), 4.0f, 0);
            for (int i = 1; i < 4; ++i) {
                const float g = kBox * static_cast<float>(i) / 4.0f;
                D::AddLine(dl, ImGuiMCP::ImVec2{ o.x + g, o.y }, ImGuiMCP::ImVec2{ o.x + g, o.y + kBox }, Col(1, 1, 1, 0.08f), 1.0f);
                D::AddLine(dl, ImGuiMCP::ImVec2{ o.x, o.y + g }, ImGuiMCP::ImVec2{ o.x + kBox, o.y + g }, Col(1, 1, 1, 0.08f), 1.0f);
            }
            D::AddLine(dl, at(0, 0), at(1, 1), Col(1, 1, 1, 0.15f), 1.0f);  // steady, for comparison
            D::AddLine(dl, at(0, 0), at(x1, y1), Col(1, 1, 1, 0.45f), 1.5f);
            D::AddLine(dl, at(1, 1), at(x2, y2), Col(1, 1, 1, 0.45f), 1.5f);
            D::AddBezierCubic(dl, at(0, 0), at(x1, y1), at(x2, y2), at(1, 1), ink, 3.0f, 48);
            for (int i = 0; i < 2; ++i) {
                const bool hot = s_drag == i;
                D::AddCircleFilled(dl, at(*hx[i], *hy[i]), hot ? 9.0f : 7.0f, hot ? Col(1, 1, 1) : ink, 16);
            }
            ImGuiMCP::TextColored(kGrey, "Across: time. Up: how far along. Steep = fast, flat = slow.%s",
                a_c.curveOn ? "" : "  (Automatic - drag a handle to make your own)");
            return changed;
        }

        // One camera's card: the everyday options open, the rest folded away (More, Effects, More about the move).
        // Returns true when the options changed (the caller hands them to CamPath::EditCam).
        bool CamCard(int a_i, int a_count, bool a_loop, bool a_at, const std::string& a_pivot, bool a_nextHasPivot,
            CamPath::Cam& a_c) {
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
                if (Small(kIconPlay, "Play from here", "playfrom")) {
                    CamPath::RequestPlayFrom(a_i);
                    if (!s_keepsRunning) Close();  // watch it (a window that froze the world would hide it)
                }
                Tip(s_keepsRunning ? "Plays the path starting at this camera"
                                   : "Plays the path starting at this camera (the window closes so you can watch)");
                ImGuiMCP::SameLine();
                if (Small(kIconPin, "Move it to where I am", "here")) CamPath::RequestSetCamHere(a_i);
                Tip("This camera takes the free camera's spot and view");
                ImGuiMCP::SameLine();
                if (Small(kIconTrash, "Delete", "del")) CamPath::RequestDeleteCam(a_i);

                FontAwesome::PushSolid();
                ImGuiMCP::TextColored(kGold, "%s", Icon(kIconCross, ("Pivot: " + a_pivot).c_str()).c_str());
                FontAwesome::Pop();
                ImGuiMCP::SameLine();
                if (Small(kIconCross, "Pick", "cpick")) CamPath::RequestPickPivot(a_i);
                Tip("This camera's pivot: what the middle of the screen points at");
                ImGuiMCP::SameLine();
                if (Small(kIconUser, "Me", "cme")) CamPath::RequestPivotMe(a_i);
                ImGuiMCP::SameLine();
                if (Small(kIconTerminal, "Console", "cconsole")) CamPath::RequestPivotConsole(a_i);
                Tip("This camera's pivot: the person or thing you clicked in the console");
                const bool hasPivot = a_c.pivotKind >= 0;
                if (hasPivot) {
                    ImGuiMCP::SameLine();
                    if (Small(kIconCross, "None", "cnone")) CamPath::RequestPivotNone(a_i);
                    Tip("This camera has no pivot (it looks the way you set it)");
                    static const char* const kBones[] = { CamPath::BoneName(0), CamPath::BoneName(1), CamPath::BoneName(2),
                        CamPath::BoneName(3), CamPath::BoneName(4), CamPath::BoneName(5), CamPath::BoneName(6),
                        CamPath::BoneName(7), CamPath::BoneName(8) };
                    ImGuiMCP::SetNextItemWidth(180.0f);
                    changed |= ImGuiMCP::Combo("Aim at its##bone", &a_c.bone, kBones, CamPath::kBoneCount, -1);
                    Tip("A person's head, eyes, chest, hips, bum, groin, hands or feet (things only have a middle)");
                }

                ImGuiMCP::Text("Looks at:");
                ImGuiMCP::SameLine();
                changed |= ImGuiMCP::RadioButton("the pivot##look", &a_c.look, CamPath::kLookPivot);
                ImGuiMCP::SameLine();
                changed |= ImGuiMCP::RadioButton("the way I set it##look", &a_c.look, CamPath::kLookSet);
                ImGuiMCP::SameLine();
                changed |= ImGuiMCP::RadioButton("where it is going##look", &a_c.look, CamPath::kLookAhead);
                if (!hasPivot && a_c.look == CamPath::kLookPivot) {
                    ImGuiMCP::TextColored(kGrey, "No pivot yet - it looks the way you set it. Pick one above.");
                }

                if (ImGuiMCP::TreeNode("More (wait, zoom, blur)##more")) {
                    ImGuiMCP::SetNextItemWidth(220.0f);
                    changed |= ImGuiMCP::SliderFloat("Wait here (seconds)##wait", &a_c.wait, 0.0f, 10.0f, "%.1f", 0);
                    ImGuiMCP::SetNextItemWidth(220.0f);
                    changed |= ImGuiMCP::SliderFloat("Zoom (field of view)##fov", &a_c.fov, 5.0f, 150.0f, "%.0f", 0);
                    Tip("Smaller = zoomed in. Two cameras on the same spot with different zoom + Crash zoom = a crash zoom.");
                    changed |= ImGuiMCP::Checkbox("Blur the background##blur", &a_c.blur);
                    if (a_c.blur) {
                        ImGuiMCP::SameLine();
                        ImGuiMCP::SetNextItemWidth(160.0f);
                        changed |= ImGuiMCP::SliderFloat("How much##blurs", &a_c.blurStrength, 0.1f, 1.0f, "%.1f", 0);
                    }
                    ImGuiMCP::TreePop();
                }

                const bool hasNext = a_i + 1 < a_count || (a_loop && a_count > 1);
                if (ImGuiMCP::TreeNode("Effects##fx")) {
                    int shake = static_cast<int>(std::lround(a_c.shake * 100.0f));
                    ImGuiMCP::SetNextItemWidth(220.0f);
                    if (ImGuiMCP::SliderInt("Camera shake##shake", &shake, 0, 100, "%d", 0)) {
                        a_c.shake = static_cast<float>(shake) / 100.0f;
                        changed   = true;
                    }
                    Tip("Bumps and sways like a camera on a cart (100 = a rough road). Blends into the next camera's shake.");
                    if (hasNext && a_c.move != CamPath::kMoveCut) {
                        int el = static_cast<int>(std::lround(a_c.elastic * 100.0f));
                        int sl = static_cast<int>(std::lround(a_c.sling * 100.0f));
                        ImGuiMCP::SetNextItemWidth(220.0f);
                        if (ImGuiMCP::SliderInt("Elastic arrival##elastic", &el, 0, 100, "%d", 0)) {
                            a_c.elastic = static_cast<float>(el) / 100.0f;
                            changed     = true;
                        }
                        Tip("The camera flies past the next camera and springs back (more = bouncier). It arrives at speed.");
                        ImGuiMCP::SetNextItemWidth(220.0f);
                        if (ImGuiMCP::SliderInt("Slingshot##sling", &sl, 0, 100, "%d", 0)) {
                            a_c.sling = static_cast<float>(sl) / 100.0f;
                            changed   = true;
                        }
                        Tip("Before leaving, the camera pulls back the other way, then shoots off to the next camera.");
                        changed |= ImGuiMCP::Checkbox("Crash zoom to the next camera##crash", &a_c.crashZoom);
                        Tip("The zoom to the next camera's zoom comes slow, then slams in at the end");
                        changed |= ImGuiMCP::Checkbox("Slow motion on the way##slow", &a_c.slowMo);
                        if (a_c.slowMo) {
                            int at = static_cast<int>(std::lround(a_c.slowAt * 100.0f));
                            int sd = static_cast<int>(std::lround((1.0f - a_c.slowSpeed) * 100.0f));
                            int ln = static_cast<int>(std::lround(a_c.slowLength * 100.0f));
                            ImGuiMCP::SetNextItemWidth(220.0f);
                            if (ImGuiMCP::SliderInt("Where (% of the way)##slowat", &at, 0, 100, "%d", 0)) {
                                a_c.slowAt = static_cast<float>(at) / 100.0f;
                                changed    = true;
                            }
                            ImGuiMCP::SetNextItemWidth(220.0f);
                            if (ImGuiMCP::SliderInt("Slow down by (%)##slowsd", &sd, 0, 95, "%d", 0)) {
                                a_c.slowSpeed = 1.0f - static_cast<float>(sd) / 100.0f;
                                changed       = true;
                            }
                            Tip("80 = a crawl at a fifth of normal speed. The slow part adds time: the move takes longer\n"
                                "than its seconds (the total shows under Seconds to get there).");
                            ImGuiMCP::SetNextItemWidth(220.0f);
                            if (ImGuiMCP::SliderInt("How much of the way (%)##slowln", &ln, 5, 100, "%d", 0)) {
                                a_c.slowLength = static_cast<float>(ln) / 100.0f;
                                changed        = true;
                            }
                            bool bullet = !a_c.slowCamera;
                            if (ImGuiMCP::Checkbox("Bullet time - only the world slows##bullet", &bullet)) {
                                a_c.slowCamera = !bullet;
                                changed        = true;
                            }
                            Tip("The camera keeps its speed while everyone else crawls (catch an arrow in flight)");
                            if (a_c.slowCamera) {
                                changed |= ImGuiMCP::Checkbox("The world slows down too##sloww", &a_c.slowWorld);
                                Tip("Everyone moves in slow motion while the camera crawls (not while the world is frozen)");
                            }
                        }
                    }
                    ImGuiMCP::TreePop();
                }
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
                    ImGuiMCP::SameLine();
                    changed |= ImGuiMCP::RadioButton("cut (jump there)##move", &a_c.move, CamPath::kMoveCut);
                    Tip("Cut: the camera jumps to the next camera at once, like a cut in a film");
                    if (a_c.move == CamPath::kMoveAround && !hasPivot && !a_nextHasPivot) {
                        ImGuiMCP::TextColored(kGrey, "No pivot here or at camera %d to go round - it curves there instead.", next);
                    }
                    if (a_c.move == CamPath::kMoveCut) {
                        ImGuiMCP::TextColored(kGrey, "The camera jumps straight to camera %d.", next);
                    } else {
                        ImGuiMCP::SetNextItemWidth(220.0f);
                        changed |= ImGuiMCP::SliderFloat("Seconds to get there##time", &a_c.time, 0.5f, 30.0f, "%.1f", 0);
                        const float total = CamPath::MoveSeconds(a_c);
                        if (total > a_c.time + 0.05f) {
                            ImGuiMCP::TextColored(kGrey, "With its effects this move takes %.1f s in all (%.1f s more).", total,
                                total - a_c.time);
                        }
                        if (ImGuiMCP::TreeNode(a_c.curveOn ? "Speed curve (your own)##curve" : "Speed curve (automatic)##curve")) {
                            changed |= CurveEditor(a_c);
                            ImGuiMCP::TreePop();
                        }
                    }
                    if (a_c.move != CamPath::kMoveCut && ImGuiMCP::TreeNode("More about the move##moremove")) {
                        if (a_c.move != CamPath::kMoveAround) {
                            int side = static_cast<int>(std::lround(a_c.bendSide * 100.0f));
                            int up   = static_cast<int>(std::lround(a_c.bendUp * 100.0f));
                            ImGuiMCP::SetNextItemWidth(220.0f);
                            if (ImGuiMCP::SliderInt("Bend left (-) / right (+)##bendside", &side, -100, 100, "%d", 0)) {
                                a_c.bendSide = static_cast<float>(side) / 100.0f;
                                changed      = true;
                            }
                            Tip("Bows the path out to one side - round a person or a tree.\n100 = the middle of the path is half the distance off the line.");
                            ImGuiMCP::SetNextItemWidth(220.0f);
                            if (ImGuiMCP::SliderInt("Bend down (-) / up (+)##bendup", &up, -100, 100, "%d", 0)) {
                                a_c.bendUp = static_cast<float>(up) / 100.0f;
                                changed    = true;
                            }
                            Tip("Bows the path up over something, or down under it");
                        }
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
                        ImGuiMCP::TreePop();
                    }
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
                                                   : "The X closes this window. Bind a key to open it: TFCam > Camera path.");
            if (!s.freeCam) ImGuiMCP::TextColored(kRed, "The free camera is off - turn it on (tfc) to add cameras.");

            // ---- the big buttons
            if (Big(kIconCamera, s.playing ? "Put a camera in here" : "Add camera here", "add", 3, s.freeCam && !s.recording)) {
                CamPath::RequestAddCam();
            }
            Tip(s.playing ? "A new camera goes into the path right here, at this moment (the path carries on from it)"
                          : "A new camera where the free camera is, looking the way it looks");
            ImGuiMCP::SameLine();
            if (s.playing && !s.paused && !s.rewinding) {
                if (Big(kIconPause, "Pause", "pause", 0)) CamPath::RequestPause();
            } else {
                if (Big(kIconPlay, s.paused ? "Go on" : "Play", "play", 1, s.cams.size() >= 2)) {
                    CamPath::RequestPlay(1);
                    if (!s_keepsRunning) Close();  // watch it
                }
                Tip(s_keepsRunning ? "Plays the cameras in order" : "Plays the cameras in order (the window closes so you can watch)");
            }
            ImGuiMCP::SameLine();
            if (Big(kIconStop, "Stop", "stop", 2, s.playing)) CamPath::RequestPlay(0);
            ImGuiMCP::SameLine();
            if (Big(kIconBack, "Rewind", "rewind", s.rewinding && s.rate < -1.5f ? 3 : 0, s.cams.size() >= 2)) {
                CamPath::RequestRewind();
                if (!s_keepsRunning) Close();  // watch it
            }
            Tip("Runs the path backwards to camera 1 and waits there (from the end if it is not playing)");
            ImGuiMCP::SameLine();
            if (Big(kIconSnow, s.frozen ? "Unfreeze world" : "Freeze world", "freeze", s.frozen ? 3 : 0)) CamPath::RequestFreezeWorld();
            Tip("Stops everyone in the world (the camera still moves)");

            if (Big(kIconReverse, "Play backwards", "backwards", s.rewinding && s.rate > -1.5f ? 3 : 0, s.cams.size() >= 2)) {
                CamPath::RequestPlayBackwards();
                if (!s_keepsRunning) Close();
            }
            Tip("Plays the path backwards at normal speed, to camera 1 (from the end if it is not playing)");
            ImGuiMCP::SameLine();
            if (Big(kIconEyeSlash, s.cleanScreen ? "Show the game UI" : "Clean screen", "clean", s.cleanScreen ? 3 : 0)) {
                CamPath::SetCleanScreen(!s.cleanScreen);
            }
            Tip("Hides ALL game UI (HUD, widgets, messages) for recording and screenshots.\n"
                "While the path plays this window steps aside too, and comes back when it pauses or ends.");
            ImGuiMCP::SameLine();
            const bool recVideo = VideoCapture::Recording();
            if (Big(kIconVideo, recVideo ? "Stop video" : "Record video", "video", recVideo ? 2 : 0)) VideoCapture::Toggle("window");
            Tip("Records an MP4 in Videos\\TFCam - the picture with TFCam's image effects, never the HUD, menus or this window.\n"
                "Size, frame rate, sound and auto-start with the path: TFCam > Image and video.");
            ImGuiMCP::SameLine();
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
            // ---- the path slider (10-29): drag to any moment of the path - the camera goes there and waits
            if (s.cams.size() >= 2 && s.total > 0.01f) {
                static float s_scrub = 0.0f;  // where the slider was left while the path is not playing
                const auto   arrive  = ArrivalTimes(s);
                float        at      = s.playing ? s.progress * s.total : (std::min)(s_scrub, s.total);
                bool         moving  = false;
                const int    ci      = CamAtTime(s, at, moving);
                const int    cn      = ci + 1 < static_cast<int>(s.cams.size()) ? ci + 2 : 1;
                const char*  state   = !s.playing ? "Drag to move along the path"
                                     : s.paused   ? "Paused"
                                     : s.rewinding ? (s.rate < -1.5f ? "Rewinding" : "Playing backwards")
                                                   : "Playing";
                const std::string fmt = std::format("{} - {} - %.1f s of {:.1f} s", state,
                    moving ? std::format("camera {} to {}", ci + 1, cn) : std::format("at camera {}", ci + 1), s.total);
                ImGuiMCP::SetNextItemWidth(-1.0f);
                if (ImGuiMCP::SliderFloat("##scrub", &at, 0.0f, s.total, fmt.c_str(), 0)) {
                    for (const float t : arrive) {  // close to a camera: right on it
                        if (std::fabs(at - t) < s.total * 0.012f) {
                            at = t;
                            break;
                        }
                    }
                    s_scrub = at;
                    CamPath::RequestSeek(at);
                }
                Tip("Drag to any moment of the path: the camera goes there and waits. It snaps onto a camera when close\n"
                    "(the gold marks). Play goes on from there. Needs the free camera on.");
                ImGuiMCP::ImVec2 a{}, b{};
                ImGuiMCP::GetItemRectMin(&a);
                ImGuiMCP::GetItemRectMax(&b);
                auto*       dl    = ImGuiMCP::GetWindowDrawList();
                const float inset = 8.0f;  // the grab's half width: where 0 and the end sit
                for (const float t : arrive) {
                    const float x = a.x + inset + (b.x - a.x - 2.0f * inset) * (t / s.total);
                    ImGuiMCP::ImDrawListManager::AddLine(dl, ImGuiMCP::ImVec2{ x, b.y - 7.0f }, ImGuiMCP::ImVec2{ x, b.y },
                        Col(1.0f, 0.82f, 0.35f), 3.0f);
                }
            }

            // ---- fixed camera (0.18.0)
            if (ImGuiMCP::CollapsingHeader("Fixed camera - you play on, the camera stays##secfixed", 0)) {
                if (FixedCam::IsOn()) {
                    if (Big(kIconThumbtack, "Let the camera go", "unfix", 3)) FixedCam::RequestRelease("window");
                    Tip("The camera follows you again");
                } else {
                    if (Big(kIconThumbtack, "Fix camera here", "fix", 0, s.freeCam)) {
                        FixedCam::RequestFixHere();
                        Close();  // play
                    }
                    Tip("Keeps this free-camera view and gives you your character back: walk, fight, run through the shot.\n"
                        "Open this window again to let the camera go (or turn the free camera on to move it).");
                }
                bool watch = FixedCam::Watching();
                if (ImGuiMCP::Checkbox("Turn to watch me##fixwatch", &watch)) FixedCam::SetWatch(watch);
                Tip("The fixed camera stays where it is but turns to follow you");
                ImGuiMCP::TextColored(kGrey, "Walking goes by the view: forward = away from the camera.");
            }

            // ---- the path
            if (ImGuiMCP::CollapsingHeader("The path - follow, loop, smoothness, save##secpath", ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
                bool follow = s.follow;
                FontAwesome::PushSolid();
                const bool followHit = ImGuiMCP::Checkbox(Icon(kIconWalk, "Follow (camera follows selected pivot)##follow").c_str(), &follow);
                FontAwesome::Pop();
                if (followHit) CamPath::SetFollow(follow);
                Tip("Each camera moves and turns with its own pivot (a walking NPC). Off: the cameras stay where you put them.");
                if (ImGuiMCP::Checkbox("Show the path in the world##showpath", &set.showPath)) FreeCamMenu::SaveCamPathSettings();
                Tip("A gold line along every move (dashed for a cut), a numbered marker at every camera with a tick the way it\n"
                    "looks, and a cross at its pivot - while the free camera is on. Clean screen hides it for recording.");
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
                if (ImGuiMCP::SliderFloat("Aim point higher / lower##aim", &set.aimHeight, -60.0f, 60.0f, "%.0f", 0)) {
                    CamPath::Clamp();
                    FreeCamMenu::SaveCamPathSettings();
                }
                Tip("Every camera that looks at or goes round a pivot aims this many units above (+) or below (-) the\n"
                    "pivot's middle or chosen body part. 0 = exactly on it.");
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
                static const std::string kNone = "none";
                const std::string& pivot = i < static_cast<int>(s.camPivotNames.size()) ? s.camPivotNames[i] : kNone;
                const int          nx    = i + 1 < n ? i + 1 : (s.loop ? 0 : -1);
                const bool nextHasPivot  = nx >= 0 && s.cams[nx].pivotKind >= 0;
                if (CamCard(i, n, s.loop, s.playing && s.atCam == i, pivot, nextHasPivot, c)) CamPath::EditCam(i, c);
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
        CamPathView::Register();
        s_window = SKSEMenuFramework::AddWindow(Render, true);
        if (s_window) s_window->IsOpen = false;
        using KeepFn = void (*)(SKSEMenuFramework::Model::WindowInterface*, bool);
        const HMODULE smf  = ::GetModuleHandleW(L"SKSEMenuFramework");
        const auto    keep = smf ? reinterpret_cast<KeepFn>(::GetProcAddress(smf, "SetWindowKeepGameRunning")) : nullptr;
        if (s_window && keep) {
            keep(s_window, true);
            s_keepsRunning = true;
        }
        SKSE::log::info("CamPathWindow: {}", s_keepsRunning ? "the game keeps running behind the window (SMF fork)"
                                                            : "a window that pauses the game (closes on Play)");
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
        const auto& set = CamPath::Get();
        if (FreeCamMenu::IsCapturingKey()) return false;
        if (auto* io = ImGuiMCP::GetIO(); io && io->WantTextInput) return false;  // typing a path name
        bool used = false;
        for (auto* evt = a_event; evt; evt = evt->next) {
            if (evt->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) continue;
            auto* btn = evt->AsButtonEvent();
            if (!btn || !btn->IsDown() || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
            const int code = static_cast<int>(btn->GetIDCode());
            if (set.openKey && code == set.openKey) {
                Close();
                return true;
            }
            // the game gets no keys while this window is open, so its path keys work here
            if (set.playKey && code == set.playKey) {
                CamPath::RequestPlay(-1);
                used = true;
            } else if (set.addKey && code == set.addKey) {
                CamPath::RequestAddCam();
                used = true;
            } else if (const int vk = VideoCapture::Get().key; vk && code == vk) {  // 0.18.0
                VideoCapture::Toggle("video key");
                used = true;
            }
        }
        return used;
    }

    void AutoHide(bool a_hide) {
        if (!s_window || a_hide == s_lastAutoHide) return;  // edges only: never fights the window key
        s_lastAutoHide = a_hide;
        if (a_hide) {
            if (s_window->IsOpen.exchange(false)) s_autoHidden = true;
        } else if (s_autoHidden.exchange(false)) {
            s_window->IsOpen = true;
        }
    }
}
