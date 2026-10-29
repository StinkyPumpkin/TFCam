#include "CamPathView.h"

#include "SKSEMenuFramework.h"

#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace CamPathView {

    namespace {
        std::mutex s_lock;
        View       s_view;
        bool       s_on = false;

        ImGuiMCP::ImU32 Col(float a_r, float a_g, float a_b, float a_a) {
            return ImGuiMCP::GetColorU32(ImGuiMCP::ImVec4{ a_r, a_g, a_b, a_a });
        }

        struct Projector {
            float               m[4][4]{};
            RE::NiRect<float>   port{};
            float               w = 0.0f, h = 0.0f;

            // Screen pixels of a world point; false behind the camera (or off by more than a screen).
            bool operator()(const RE::NiPoint3& a_p, ImGuiMCP::ImVec2& a_out) const {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                if (!RE::NiCamera::WorldPtToScreenPt3(m, port, a_p, x, y, z, 1e-5f)) return false;
                if (!(z > 0.0f) || !std::isfinite(x) || !std::isfinite(y) || x < -1.0f || x > 2.0f || y < -1.0f || y > 2.0f) return false;
                a_out = { x * w, (1.0f - y) * h };
                return true;
            }
        };

        void __stdcall Render() {
            View v;
            {
                std::scoped_lock lock(s_lock);
                if (!s_on) return;
                v = s_view;
            }
            auto* pc  = RE::PlayerCamera::GetSingleton();
            auto* cam = RE::Main::WorldRootCamera();
            auto* io  = ImGuiMCP::GetIO();
            if (!pc || !pc->IsInFreeCameraMode() || !cam || !io) return;
            Projector P;
            std::memcpy(P.m, cam->GetRuntimeData().worldToCam, sizeof(P.m));
            P.port = cam->GetRuntimeData2().port;
            P.w    = io->DisplaySize.x;
            P.h    = io->DisplaySize.y;
            if (!(P.w > 0.0f) || !(P.h > 0.0f)) return;
            namespace D = ImGuiMCP::ImDrawListManager;
            auto* dl = ImGuiMCP::GetBackgroundDrawList();
            if (!dl) return;

            const auto gold  = Col(1.0f, 0.82f, 0.35f, 0.85f);
            const auto shade = Col(0.0f, 0.0f, 0.0f, 0.45f);
            const auto grey  = Col(0.85f, 0.85f, 0.85f, 0.6f);

            // the moves
            for (const auto& line : v.lines) {
                if (line.pts.size() < 2) continue;
                if (line.cut) {  // dashed straight jump
                    const auto& a = line.pts.front();
                    const auto& b = line.pts.back();
                    constexpr int kDashes = 24;
                    for (int i = 0; i < kDashes; i += 2) {
                        ImGuiMCP::ImVec2 s0{}, s1{};
                        const float      t0 = static_cast<float>(i) / kDashes, t1 = static_cast<float>(i + 1) / kDashes;
                        if (P(a + (b - a) * t0, s0) && P(a + (b - a) * t1, s1)) D::AddLine(dl, s0, s1, grey, 2.0f);
                    }
                    continue;
                }
                ImGuiMCP::ImVec2 prev{};
                bool             havePrev = P(line.pts[0], prev);
                for (std::size_t i = 1; i < line.pts.size(); ++i) {
                    ImGuiMCP::ImVec2 cur{};
                    const bool       ok = P(line.pts[i], cur);
                    if (ok && havePrev) {
                        D::AddLine(dl, prev, cur, shade, 5.0f);
                        D::AddLine(dl, prev, cur, gold, 2.5f);
                    }
                    prev     = cur;
                    havePrev = ok;
                }
            }

            // the cameras
            auto*       font = ImGuiMCP::GetFont();
            const float fs   = 30.0f;
            for (int i = 0; i < static_cast<int>(v.cams.size()); ++i) {
                const auto&      c  = v.cams[i];
                const bool       at = i == v.at;
                ImGuiMCP::ImVec2 sp{};
                if (!P(c.pos, sp)) continue;
                const auto ink = at ? Col(1.0f, 1.0f, 1.0f, 0.95f) : gold;
                ImGuiMCP::ImVec2 tip{};
                if (P(c.pos + c.fwd * 45.0f, tip)) {
                    D::AddLine(dl, sp, tip, shade, 5.0f);
                    D::AddLine(dl, sp, tip, ink, 2.5f);
                }
                const float r = at ? 13.0f : 10.0f;
                D::AddCircleFilled(dl, sp, r + 2.0f, shade, 20);
                D::AddCircle(dl, sp, r, ink, 20, 2.5f);
                const std::string label = std::to_string(i + 1);
                if (font) {
                    D::AddText(dl, font, fs, ImGuiMCP::ImVec2{ sp.x + r + 5.0f, sp.y - fs * 0.55f }, shade, label.c_str(), nullptr, 0.0f, nullptr);
                    D::AddText(dl, font, fs, ImGuiMCP::ImVec2{ sp.x + r + 4.0f, sp.y - fs * 0.6f }, ink, label.c_str(), nullptr, 0.0f, nullptr);
                }
                if (c.havePivot) {
                    ImGuiMCP::ImVec2 pv{};
                    if (P(c.pivot, pv)) {
                        constexpr float k = 7.0f;
                        D::AddLine(dl, ImGuiMCP::ImVec2{ pv.x - k, pv.y - k }, ImGuiMCP::ImVec2{ pv.x + k, pv.y + k }, ink, 2.0f);
                        D::AddLine(dl, ImGuiMCP::ImVec2{ pv.x - k, pv.y + k }, ImGuiMCP::ImVec2{ pv.x + k, pv.y - k }, ink, 2.0f);
                    }
                }
            }
        }
    }

    void Register() {
        static bool s_done = false;
        if (std::exchange(s_done, true)) return;
        SKSEMenuFramework::AddHudElement(Render);
        SKSE::log::info("CamPathView: the path is drawn in the world while the free camera is on");
    }

    void Publish(View&& a_view) {
        std::scoped_lock lock(s_lock);
        s_view = std::move(a_view);
        s_on   = true;
    }

    void Clear() {
        std::scoped_lock lock(s_lock);
        if (!s_on) return;
        s_on   = false;
        s_view = {};
    }
}
