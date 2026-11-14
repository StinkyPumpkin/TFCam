#include "CamPath.h"
#include "CamBlur.h"
#include "CamFocus.h"
#include "CamPathPrisma.h"
#include "CamPathView.h"
#include "CamPathWindow.h"
#include "FreeCamController.h"
#include "FreezeTime.h"
#include "SceneCam.h"
#include "SceneTracker.h"
#include "SlccBridge.h"
#include "VideoCapture.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace CamPath {

    namespace {
        constexpr float  kPi             = 3.14159265358979f;
        constexpr float  kTwoPi          = 2.0f * kPi;
        constexpr float  kLn2            = 0.69314718f;
        constexpr float  kPitchLimit     = 1.55f;  // ~89 degrees, no flip
        constexpr float  kStartGlide     = 1.0f;   // seconds from where the camera is to camera 1
        constexpr float  kPivotHalfLife  = 0.2f;   // live pivot position / heading smoothing
        constexpr int    kSamples        = 32;     // per curved move, for the even-speed table
        constexpr double kPendingTimeout = 1.5;    // Play waits this long for TFCam free-fly
        constexpr float  kRecordStep     = 0.4f;   // record manual path: a camera at most this often (seconds)
        constexpr float  kRecordMove     = 3.0f;   // ... once the camera moved this many units
        constexpr float  kRecordTurn     = 0.02f;  // ... or turned this many radians
        constexpr float  kRewindRate     = 2.0f;   // a rewind runs the path backwards at twice its speed
        constexpr int    kSlowSamples    = 64;     // per slow-motion move, for its time table

        Settings s_settings;

        // ---- math --------------------------------------------------------------------------------------------------
        float WrapPi(float a_angle) { return std::remainder(a_angle, kTwoPi); }
        float Clamp01(float a_x) { return std::clamp(a_x, 0.0f, 1.0f); }
        float SmoothStep(float a_x) {
            a_x = Clamp01(a_x);
            return a_x * a_x * (3.0f - 2.0f * a_x);
        }
        float ExpWeight(float a_dt, float a_halfLife) {
            if (a_halfLife <= 0.0001f) return 1.0f;
            return a_dt <= 0.0f ? 0.0f : 1.0f - std::exp(-kLn2 * a_dt / a_halfLife);
        }
        float        Dot(const RE::NiPoint3& a, const RE::NiPoint3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        float        Length(const RE::NiPoint3& a_v) { return std::sqrt(Dot(a_v, a_v)); }
        RE::NiPoint3 Mix(const RE::NiPoint3& a, const RE::NiPoint3& b, float w) {
            return { a.x + (b.x - a.x) * w, a.y + (b.y - a.y) * w, a.z + (b.z - a.z) * w };
        }
        bool Finite(const RE::NiPoint3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

        // Turn a_v by heading a_h (Skyrim's angle Z: clockwise from +Y).
        RE::NiPoint3 RotZ(const RE::NiPoint3& a_v, float a_h) {
            const float c = std::cos(a_h), s = std::sin(a_h);
            return { a_v.x * c + a_v.y * s, -a_v.x * s + a_v.y * c, a_v.z };
        }

        // TFCam's camera convention: forward = (sin yaw cos pitch, cos yaw cos pitch, -sin pitch); + pitch looks down.
        RE::NiPoint3 Forward(float a_yaw, float a_pitch) {
            return { std::sin(a_yaw) * std::cos(a_pitch), std::cos(a_yaw) * std::cos(a_pitch), -std::sin(a_pitch) };
        }

        // Yaw / pitch looking along a_dir; a_yaw is left alone when a_dir is (almost) straight up or down.
        void LookAngles(const RE::NiPoint3& a_dir, float& a_yaw, float& a_pitch) {
            const float flat = std::hypot(a_dir.x, a_dir.y);
            if (flat > 1e-4f) a_yaw = std::atan2(a_dir.x, a_dir.y);
            a_pitch = std::atan2(-a_dir.z, flat);
        }

        RE::NiPoint3 Normalized(const RE::NiPoint3& a_v, const RE::NiPoint3& a_fallback) {
            const float len = Length(a_v);
            return len > 1e-5f && std::isfinite(len) ? a_v * (1.0f / len) : a_fallback;
        }

        // Spherical blend of two unit directions.
        RE::NiPoint3 SlerpDir(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, float a_w) {
            const float d = std::clamp(Dot(a_a, a_b), -1.0f, 1.0f);
            if (d > 0.9995f) return Normalized(Mix(a_a, a_b, a_w), a_b);
            RE::NiPoint3 b = a_b;
            if (d < -0.9995f) {  // opposite: go round through the side
                b = Normalized(RE::NiPoint3{ -a_a.y, a_a.x, 0.0f }, RE::NiPoint3{ 1.0f, 0.0f, 0.0f });
                return a_w < 0.5f ? SlerpDir(a_a, b, a_w * 2.0f) : SlerpDir(b, a_b, (a_w - 0.5f) * 2.0f);
            }
            const float th = std::acos(d);
            const float s  = std::sin(th);
            const float wa = std::sin((1.0f - a_w) * th) / s;
            const float wb = std::sin(a_w * th) / s;
            return Normalized(a_a * wa + b * wb, b);
        }

        // 0..1 progress with a slow start (a_in) and / or a slow stop (a_out) - cubic Hermite, speed 0 or 1 at each end.
        float Ease(float a_f, bool a_in, bool a_out) {
            const float f  = Clamp01(a_f);
            const float v0 = a_in ? 0.0f : 1.0f;
            const float v1 = a_out ? 0.0f : 1.0f;
            const float f2 = f * f, f3 = f2 * f;
            return v0 * (f3 - 2.0f * f2 + f) + (-2.0f * f3 + 3.0f * f2) + v1 * (f3 - f2);
        }

        // A speed curve (CSS cubic-bezier: (0,0), (x1,y1), (x2,y2), (1,1)) at time a_f: the bezier's x is solved for
        // a_f (x1, x2 in 0..1 keep it rising, so one answer), then its y is the progress.
        float CurveEase(float a_f, float a_x1, float a_y1, float a_x2, float a_y2) {
            const float f   = Clamp01(a_f);
            auto        bez = [](float a_s, float a_p1, float a_p2) {
                const float r = 1.0f - a_s;
                return 3.0f * r * r * a_s * a_p1 + 3.0f * r * a_s * a_s * a_p2 + a_s * a_s * a_s;
            };
            float lo = 0.0f, hi = 1.0f, s = f;
            for (int i = 0; i < 24; ++i) {  // bisection: plain and safe at the flat ends
                s = 0.5f * (lo + hi);
                if (bez(s, a_x1, a_x2) < f) {
                    lo = s;
                } else {
                    hi = s;
                }
            }
            return Clamp01(bez(s, a_y1, a_y2));
        }

        double Now() {
            static const double freq = [] {
                LARGE_INTEGER f;
                ::QueryPerformanceFrequency(&f);
                return static_cast<double>(f.QuadPart);
            }();
            LARGE_INTEGER t;
            ::QueryPerformanceCounter(&t);
            return static_cast<double>(t.QuadPart) / freq;
        }

        // Own frame time, capped at 0.1 s, 0 while the game is paused (the path waits behind a pausing menu).
        float FrameDt() {
            static double s_last = 0.0;
            const double  now    = Now();
            float         dt     = s_last > 0.0 ? static_cast<float>(now - s_last) : 0.0f;
            s_last               = now;
            dt                   = std::clamp(dt, 0.0f, 0.1f);
            auto* ui             = RE::UI::GetSingleton();
            if (ui && ui->GameIsPaused()) dt = 0.0f;
            return dt;
        }

        float GetFOV() {
            auto* cam = RE::PlayerCamera::GetSingleton();
            return cam ? cam->GetRuntimeData2().worldFOV : 0.0f;
        }

        void SetFOV(float a_fov) {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (cam && a_fov > 0.0f && std::isfinite(a_fov)) cam->GetRuntimeData2().worldFOV = std::clamp(a_fov, 5.0f, 150.0f);
        }

        RE::FreeCameraState* CurrentFreeCam() {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam || !cam->IsInFreeCameraMode() || !FreeCam::TFCamDriving()) return nullptr;
            auto* state = cam->currentState.get();
            return state ? static_cast<RE::FreeCameraState*>(state) : nullptr;
        }

        // ---- the path (s_lock) ---------------------------------------------------------------------------------------
        std::mutex       s_lock;
        std::vector<Cam> s_cams;
        std::string      s_name      = "Current";
        bool             s_follow    = false;
        bool             s_loop      = false;
        std::string      s_lastEvent;
        std::vector<std::string> s_savedPaths;
        // play state copied for the window (s_lock)
        bool  s_pubPlaying = false, s_pubPaused = false, s_pubRewinding = false, s_pubRecording = false, s_pubFreeCam = false;
        float s_pubRate    = 1.0f;
        int   s_pubAt      = -1;
        float s_pubProgress = 0.0f;
        bool  s_loaded = false;
        std::unordered_map<RE::FormID, std::string> s_refNames;  // pivot names for the window (filled on the main thread)
        // 11-14 round 13: the player's SexLab scene for the window (s_lock; filled on the main thread)
        bool        s_sceneIn    = false;
        bool        s_sceneSaved = false;
        bool        s_pubScene   = false;
        std::string s_sceneLabel;

        void EventLocked(std::string a_text) {
            SKSE::log::info("CamPath: {}", a_text);
            s_lastEvent = std::move(a_text);
        }

        // ---- 11-13: where a camera was made (see Cam::space) -----------------------------------------------------------
        constexpr float kFarUnknown = 12000.0f;  // an older camera (no area saved) this far from you counts as somewhere else

        std::string SpaceOfForm(const RE::TESForm* a_form, char a_tag) {
            if (!a_form) return {};
            const auto* file = a_form->GetFile(0);
            return std::format("{}:{}|{:X}", a_tag, file ? std::string(file->GetFilename()) : std::string(), a_form->GetLocalFormID());
        }

        std::string SpaceHere() {  // main thread
            auto* pc   = RE::PlayerCharacter::GetSingleton();
            auto* cell = pc ? pc->GetParentCell() : nullptr;
            if (!cell) return {};
            if (cell->IsInteriorCell()) return SpaceOfForm(cell, 'I');
            return SpaceOfForm(pc->GetWorldspace(), 'W');
        }

        // its name for a message: the cell's / worldspace's own name, else the plugin and id
        std::string SpaceName(const std::string& a_space) {
            if (a_space.size() < 3) return "another place";
            const auto bar = a_space.find('|');
            if (bar == std::string::npos) return "another place";
            const std::string plugin = a_space.substr(2, bar - 2);
            const auto        local  = static_cast<RE::FormID>(std::strtoul(a_space.c_str() + bar + 1, nullptr, 16));
            auto*             dh     = RE::TESDataHandler::GetSingleton();
            const auto*       form   = dh && !plugin.empty() ? dh->LookupForm(local, plugin) : nullptr;
            const char*       name   = form ? form->GetName() : nullptr;
            if (name && *name) return name;
            return std::format("{} {:X} ({})", a_space[0] == 'I' ? "the interior" : "the worldspace", local, plugin.empty() ? "?" : plugin);
        }

        // true = the camera is somewhere else than you; a_where = where (for the message)
        bool CamAway(const Cam& a_c, const std::string& a_here, std::string& a_where) {
            if (!a_c.space.empty()) {
                if (a_c.space == a_here) return false;
                a_where = SpaceName(a_c.space);
                return true;
            }
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc) return false;
            const auto d = a_c.pos - pc->GetPosition();
            if (std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) <= kFarUnknown) return false;
            a_where = "where it was made (an older camera - no area saved; it is far from you)";
            return true;
        }

        void Event(std::string a_text) {
            std::scoped_lock lock(s_lock);
            EventLocked(std::move(a_text));
        }

        void Notify(const std::string& a_hud) { RE::SendHUDMessage::ShowHUDMessage(a_hud.c_str()); }

        void Queue(std::function<void()> a_fn) {
            if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask(std::move(a_fn));
        }

        // ---- files -------------------------------------------------------------------------------------------------
        std::filesystem::path PathsDir() {
            wchar_t     buf[MAX_PATH] = {};
            const DWORD len = ::GetFullPathNameW(L"Data\\SKSE\\Plugins\\TFCam\\Paths", MAX_PATH, buf, nullptr);
            return len > 0 && len < MAX_PATH ? std::filesystem::path(std::wstring(buf, len)) : std::filesystem::path(L"Data\\SKSE\\Plugins\\TFCam\\Paths");
        }

        std::string CleanName(std::string a_name) {
            std::string out;
            for (char c : a_name) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') out.push_back(c);
            }
            while (!out.empty() && out.back() == ' ') out.pop_back();
            while (!out.empty() && out.front() == ' ') out.erase(out.begin());
            if (out.size() > 40) out.resize(40);
            return out;
        }

        void RefreshSavedLocked() {
            s_savedPaths.clear();
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(PathsDir(), ec)) {
                if (!e.is_regular_file() || e.path().extension() != ".tsv") continue;
                const auto stem = e.path().stem().string();
                if (stem != "Current") s_savedPaths.push_back(stem);
            }
            std::sort(s_savedPaths.begin(), s_savedPaths.end());
        }

        // 11-14 round 13: one writer / reader for both kinds of path file (Paths\ and the SexLab ScenePaths\)
        bool WriteFile(const std::filesystem::path& a_file, const std::vector<Cam>& a_cams, bool a_follow, bool a_loop);
        bool ReadFile(const std::filesystem::path& a_file, std::vector<Cam>& a_cams, bool& a_follow, bool& a_loop);

        bool WriteLocked(const std::string& a_name) { return WriteFile(PathsDir() / (a_name + ".tsv"), s_cams, s_follow, s_loop); }

        bool ReadLocked(const std::string& a_name) {
            std::vector<Cam> cams;
            bool             follow = false, loop = false;
            if (!ReadFile(PathsDir() / (a_name + ".tsv"), cams, follow, loop)) return false;
            s_cams   = std::move(cams);
            s_follow = follow;
            s_loop   = loop;
            return true;
        }

        bool WriteFile(const std::filesystem::path& a_file, const std::vector<Cam>& a_cams, bool a_follow, bool a_loop) {
            std::error_code ec;
            std::filesystem::create_directories(a_file.parent_path(), ec);
            const auto file = a_file;
            auto       tmp  = a_file;
            tmp += ".tmp";
            {
                std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!out) {
                    SKSE::log::error("CamPath: cannot write {}", file.string());
                    return false;
                }
                out << "# TFCam camera path v3 - tab separated. pivot: - - follow loop (no pivot for new cameras since 10-29)\n"
                       "# cam: x y z yaw pitch roll fov | pivot x y z heading known | look wait blur strength | move time "
                       "smooth turns way zoom | pivot kind formID | bend side up | bone shake crash | slow at speed "
                       "length world | elastic sling slowCamera | curve on x1 y1 x2 y2 | dolly | where (11-13: I/W:plugin|id, - = unknown)\n";
                out << std::format("pivot\t-1\t00000000\t{}\t{}\n", a_follow ? 1 : 0, a_loop ? 1 : 0);
                for (const auto& c : a_cams) {
                    out << std::format("cam\t{:.3f}\t{:.3f}\t{:.3f}\t{:.5f}\t{:.5f}\t{:.5f}\t{:.2f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.5f}\t{}"
                                       "\t{}\t{:.2f}\t{}\t{:.2f}\t{}\t{:.2f}\t{}\t{}\t{}\t{}\t{}\t{:08X}\t{:.3f}\t{:.3f}"
                                       "\t{}\t{:.3f}\t{}\t{}\t{:.3f}\t{:.3f}\t{:.3f}\t{}\t{:.3f}\t{:.3f}\t{}"
                                       "\t{}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{}\n",
                        c.pos.x, c.pos.y, c.pos.z, c.yaw, c.pitch, c.roll, c.fov, c.pivotPos.x, c.pivotPos.y, c.pivotPos.z,
                        c.pivotHeading, c.havePivot ? 1 : 0, c.look, c.wait, c.blur ? 1 : 0, c.blurStrength, c.move, c.time,
                        c.smooth ? 1 : 0, c.turns, c.way, c.zoomTrick ? 1 : 0, c.pivotKind, c.pivotRef, c.bendSide, c.bendUp,
                        c.bone, c.shake, c.crashZoom ? 1 : 0, c.slowMo ? 1 : 0, c.slowAt, c.slowSpeed, c.slowLength,
                        c.slowWorld ? 1 : 0, c.elastic, c.sling, c.slowCamera ? 1 : 0, c.curveOn ? 1 : 0, c.curveX1,
                        c.curveY1, c.curveX2, c.curveY2, c.dolly, c.space.empty() ? std::string("-") : c.space);
                }
            }
            std::filesystem::rename(tmp, file, ec);
            if (ec) {
                std::filesystem::remove(file, ec);
                std::filesystem::rename(tmp, file, ec);
            }
            return !ec;
        }

        bool ReadFile(const std::filesystem::path& a_file, std::vector<Cam>& a_out, bool& a_follow, bool& a_loop) {
            std::ifstream in(a_file, std::ios::in | std::ios::binary);
            if (!in) return false;
            std::vector<Cam> cams;
            int              kind = 0, follow = 0, loop = 0;
            unsigned         ref  = 0;
            std::string      line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.starts_with("pivot\t")) {
                    std::sscanf(line.c_str() + 6, "%d %x %d %d", &kind, &ref, &follow, &loop);
                    continue;
                }
                if (!line.starts_with("cam\t")) continue;
                Cam c;
                int      have = 0, blur = 0, smooth = 1, zoom = 0, ckind = 0, crash = 0, slow = 0, sworld = 0, slowcam = 1, curve = 0;
                unsigned cref = 0;
                const int n = std::sscanf(line.c_str() + 4,
                    "%f %f %f %f %f %f %f %f %f %f %f %d %d %f %d %f %d %f %d %d %d %d %d %x %f %f %d %f %d %d %f %f %f %d %f %f %d "
                    "%d %f %f %f %f %f",
                    &c.pos.x, &c.pos.y,
                    &c.pos.z, &c.yaw, &c.pitch, &c.roll, &c.fov, &c.pivotPos.x, &c.pivotPos.y, &c.pivotPos.z,
                    &c.pivotHeading, &have, &c.look, &c.wait, &blur, &c.blurStrength, &c.move, &c.time, &smooth, &c.turns,
                    &c.way, &zoom, &ckind, &cref, &c.bendSide, &c.bendUp, &c.bone, &c.shake, &crash, &slow, &c.slowAt,
                    &c.slowSpeed, &c.slowLength, &sworld, &c.elastic, &c.sling, &slowcam, &curve, &c.curveX1, &c.curveY1,
                    &c.curveX2, &c.curveY2, &c.dolly);
                if (n < 22 || !Finite(c.pos) || !std::isfinite(c.yaw) || !std::isfinite(c.pitch)) continue;
                c.havePivot = have != 0;
                c.blur      = blur != 0;
                c.smooth    = smooth != 0;
                c.zoomTrick = zoom != 0;
                c.move      = std::clamp(c.move, 0, 3);
                // v2 (0.18.0 before the per-camera pivot): every camera has the path's pivot. 11-14 round 13: 2 = a SexLab scene
                // position (scene paths; the reference is the position's index)
                c.pivotKind = n >= 24 ? (ckind == 1 ? 1 : (ckind == 2 ? 2 : (ckind < 0 ? -1 : 0))) : (kind == 1 ? 1 : 0);
                c.pivotRef  = c.pivotKind == 1 ? (n >= 24 ? cref : ref) : (c.pivotKind == 2 ? std::min<RE::FormID>(cref, 15) : 0);
                if (n < 26 || !std::isfinite(c.bendSide) || !std::isfinite(c.bendUp)) c.bendSide = c.bendUp = 0.0f;
                c.bendSide = std::clamp(c.bendSide, -1.0f, 1.0f);
                c.bendUp   = std::clamp(c.bendUp, -1.0f, 1.0f);
                // the effects (26 fields before them)
                c.bone      = std::clamp(c.bone, 0, kBoneCount - 1);
                c.shake     = std::isfinite(c.shake) ? std::clamp(c.shake, 0.0f, 1.0f) : 0.0f;
                c.crashZoom = crash != 0;
                c.slowMo    = slow != 0;
                c.slowWorld = sworld != 0;
                c.slowAt     = std::isfinite(c.slowAt) ? std::clamp(c.slowAt, 0.0f, 1.0f) : 0.66f;
                c.slowSpeed  = std::isfinite(c.slowSpeed) ? std::clamp(c.slowSpeed, 0.05f, 1.0f) : 0.2f;
                c.slowLength = std::isfinite(c.slowLength) ? std::clamp(c.slowLength, 0.05f, 1.0f) : 0.3f;
                c.slowCamera = slowcam != 0;
                c.elastic    = std::isfinite(c.elastic) ? std::clamp(c.elastic, 0.0f, 1.0f) : 0.0f;
                c.sling      = std::isfinite(c.sling) ? std::clamp(c.sling, 0.0f, 1.0f) : 0.0f;
                // the speed curve (37 fields before it)
                const bool curveOk = n >= 42 && std::isfinite(c.curveX1) && std::isfinite(c.curveY1) && std::isfinite(c.curveX2) &&
                                     std::isfinite(c.curveY2);
                if (!curveOk) {
                    const Cam d;
                    c.curveX1 = d.curveX1, c.curveY1 = d.curveY1, c.curveX2 = d.curveX2, c.curveY2 = d.curveY2;
                }
                c.curveOn = curveOk && curve != 0;
                c.curveX1 = std::clamp(c.curveX1, 0.0f, 1.0f);
                c.curveY1 = std::clamp(c.curveY1, 0.0f, 1.0f);
                c.curveX2 = std::clamp(c.curveX2, 0.0f, 1.0f);
                c.curveY2 = std::clamp(c.curveY2, 0.0f, 1.0f);
                // the dolly while waiting (42 fields before it, 10-31)
                c.dolly = n >= 43 && std::isfinite(c.dolly) ? std::clamp(c.dolly, -1.0f, 1.0f) : 0.0f;
                // where it was made (11-13): the 44th field, text; older files have none
                if (n >= 43) {
                    std::size_t at = 0;
                    for (int tabs = 0; tabs < 44 && at != std::string::npos; ++tabs) at = line.find('\t', at == 0 ? 0 : at + 1);
                    if (at != std::string::npos) {
                        std::string sp = line.substr(at + 1);
                        if (const auto t = sp.find('\t'); t != std::string::npos) sp.resize(t);
                        if (sp != "-" && sp.size() > 2 && (sp[0] == 'I' || sp[0] == 'W') && sp[1] == ':') c.space = sp;
                    }
                }
                cams.push_back(c);
            }
            a_out    = std::move(cams);
            a_follow = follow != 0;
            a_loop   = loop != 0;
            return true;
        }

        void LoadOnce() {
            std::scoped_lock lock(s_lock);
            if (s_loaded) return;
            s_loaded = true;
            // 11-14 round 13 (user: "after loading a save the camera path should not show a previous saved path - it should be
            // blank; while playing, a path that is set stays in the UI until removed or the game closes"): each game session starts
            // blank (later loads in the same session keep the path - this runs once). The last session's path is not lost: it is kept
            // as the saved path "Last session" (the editor's Open list), before the first edit writes Current over it
            if (ReadLocked("Current") && !s_cams.empty()) {
                const auto n = s_cams.size();
                WriteLocked("Last session");
                SKSE::log::info("CamPath: the last session's path ({} camera(s)) is kept as \"Last session\" - this session starts blank", n);
            }
            s_cams.clear();
            s_follow = false;
            s_loop   = false;
            WriteLocked("Current");
            RefreshSavedLocked();
        }

        // Names of reference pivots for the window: looked up on the main thread, then kept.
        std::atomic<bool> s_resolving{ false };
        void ResolveNamesSoon() {
            if (s_resolving.exchange(true)) return;
            Queue([] {
                std::vector<RE::FormID> ids;
                {
                    std::scoped_lock lock(s_lock);
                    for (const auto& c : s_cams) {
                        if (c.pivotKind == 1 && c.pivotRef && !s_refNames.contains(c.pivotRef)) ids.push_back(c.pivotRef);
                    }
                }
                std::vector<std::pair<RE::FormID, std::string>> found;
                for (const auto id : ids) {
                    auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
                    found.emplace_back(id, ref ? CamFocus::Describe(ref) : std::format("{:08X} (not here)", id));
                }
                {
                    std::scoped_lock lock(s_lock);
                    for (auto& [id, name] : found) s_refNames[id] = std::move(name);
                }
                s_resolving = false;
            });
        }

        std::atomic<bool> s_dirty{ false };
        std::atomic<int>  s_editGen{ 0 };  // + 1 on every edit of the path
        void SaveCurrentSoon() {
            ++s_editGen;
            if (s_dirty.exchange(true)) return;
            Queue([] {
                s_dirty = false;
                std::scoped_lock lock(s_lock);
                WriteLocked("Current");
            });
        }

        // ---- pivot -------------------------------------------------------------------------------------------------
        constexpr const char* kBoneNames[kBoneCount] = { "middle", "head", "eyes", "chest", "hips", "bum", "groin", "hands", "feet" };

        // A body part of an actor, from the XPMSSE skeleton's nodes (a pair is averaged). False = not an actor / no
        // such node (a_out is left alone).
        bool BonePoint(RE::TESObjectREFR* a_ref, int a_bone, RE::NiPoint3& a_out) {
            auto* actor = a_ref ? a_ref->As<RE::Actor>() : nullptr;
            auto* root  = actor ? actor->Get3D2() : nullptr;
            if (!root) return false;
            auto avg = [&](std::initializer_list<const char*> a_names) {
                RE::NiPoint3 sum{};
                int          found = 0;
                for (const char* n : a_names) {
                    if (auto* o = root->GetObjectByName(n)) {
                        sum += o->world.translate;
                        ++found;
                    }
                }
                if (found) a_out = sum * (1.0f / static_cast<float>(found));
                return found > 0;
            };
            const float        h = actor->GetAngleZ();
            const RE::NiPoint3 fwd{ std::sin(h), std::cos(h), 0.0f };
            const RE::NiPoint3 up{ 0.0f, 0.0f, 1.0f };
            switch (a_bone) {
            case kBoneHead:
                return avg({ "NPC Head [Head]" });
            case kBoneEyes:
                if (!avg({ "NPC Head [Head]" })) return false;
                a_out += fwd * 4.0f + up * 7.0f;
                return true;
            case kBoneChest:
                if (avg({ "NPC L Breast", "NPC R Breast" }) || avg({ "L Breast02", "R Breast02" })) return true;
                if (!avg({ "NPC Spine2 [Spn2]" })) return false;
                a_out += fwd * 8.0f;
                return true;
            case kBoneHips:
                return avg({ "NPC Pelvis [Pelv]" });
            case kBoneBum:
                if (avg({ "NPC L Butt", "NPC R Butt" })) return true;
                if (!avg({ "NPC Pelvis [Pelv]" })) return false;
                a_out -= fwd * 8.0f;
                return true;
            case kBoneGroin:
                if (avg({ "NPC Genitals01 [Gen01]" }) || avg({ "VaginaB1" }) || avg({ "Clitoral1" })) return true;
                if (!avg({ "NPC Pelvis [Pelv]" })) return false;
                a_out += fwd * 6.0f - up * 6.0f;
                return true;
            case kBoneHands:
                return avg({ "NPC L Hand [LHnd]", "NPC R Hand [RHnd]" });
            case kBoneFeet:
                return avg({ "NPC L Foot [Lft ]", "NPC R Foot [Rft ]" });
            default:
                return false;
            }
        }

        // The pivot as it is right now: its middle (a_out: the frame Follow rides in), heading and name; a_aim = the
        // point the camera looks at / goes round (the chosen body part of an actor, else the middle).
        bool PivotRaw(int a_kind, RE::FormID a_ref, RE::NiPoint3& a_out, float& a_heading, std::string* a_name,
            int a_bone = kBoneMiddle, RE::NiPoint3* a_aim = nullptr) {
            if (a_kind == 2) {  // 11-14 round 13: a position in the player's SexLab scene - whoever plays it now
                const auto& sc = SceneTracker::GetPlayerScene();
                if (sc.inScene && a_ref < sc.actors.size()) {
                    auto actor = sc.actors[a_ref].get();
                    if (actor && actor->Get3D() && CamFocus::RefPoint(actor.get(), a_out)) {
                        a_heading = actor->GetAngleZ();
                        if (a_name) *a_name = std::format("scene position {} ({})", a_ref + 1, CamFocus::Describe(actor.get()));
                        if (a_aim) {
                            *a_aim = a_out;
                            if (a_bone != kBoneMiddle) BonePoint(actor.get(), a_bone, *a_aim);
                        }
                        return true;
                    }
                }
            }
            if (a_kind == 1 && a_ref) {
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
                if (ref && !ref->IsDeleted() && ref->Get3D() && CamFocus::RefPoint(ref, a_out)) {
                    a_heading = ref->GetAngleZ();
                    if (a_name) *a_name = CamFocus::Describe(ref);
                    if (a_aim) {
                        *a_aim = a_out;
                        if (a_bone != kBoneMiddle) BonePoint(ref, a_bone, *a_aim);
                    }
                    return true;
                }
            }
            std::string what;
            if (!SceneCam::SubjectPoint(a_out, &what)) return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            a_heading    = player ? player->GetAngleZ() : 0.0f;
            if (a_name) *a_name = a_kind == 1 ? "you (the picked one is not here)" : (a_kind == 2 ? "you (that scene position is not here)" : "you");
            if (a_aim) {
                *a_aim = a_out;
                if (a_bone != kBoneMiddle && player) BonePoint(player, a_bone, *a_aim);
            }
            return true;
        }

        // ---- playback (main thread) --------------------------------------------------------------------------------
        // A pivot the playing cameras use, as it is right now (smoothed).
        struct Piv {
            int          kind    = 0;
            RE::FormID   ref     = 0;
            int          bone    = kBoneMiddle;
            RE::NiPoint3 aim{};  // the body part (looked at, gone round)
            RE::NiPoint3 pos{};  // the middle (Follow's frame)
            float        heading = 0.0f;
            bool         have    = false;
            std::string  name;
        };

        struct Play {
            bool                      on       = false;
            bool                      paused   = false;
            float                     rate     = 1.0f;  // 1 = forward, -kRewindRate = rewinding
            bool                      gliding  = false;
            float                     glideT   = 0.0f;
            RE::NiPoint3              fromPos{};
            RE::NiPoint3              fromFwd{ 0.0f, 1.0f, 0.0f };
            float                     fromRoll = 0.0f;
            float                     fromFov  = 70.0f;
            std::vector<Cam>          cams;
            std::vector<RE::NiPoint3> base;     // per camera: world position, or offset in its pivot's frame (rides)
            std::vector<float>        baseYaw;  // per camera: yaw, or yaw minus its pivot's heading (rides)
            std::vector<bool>         rides;    // per camera: moves and turns with its pivot (follow)
            std::vector<int>          pivOf;    // per camera: its pivot in pivots
            std::vector<Piv>          pivots;   // the different pivots the cameras use
            std::vector<RE::NiPoint3> world;    // per camera: where it is this frame
            std::vector<std::array<float, kSlowSamples + 1>> slowTab;  // per camera: seconds to each step of a slow-motion move
            float                     shakeT   = 0.0f;  // the shake's own clock
            bool                      follow   = false;
            bool                      loop     = false;
            int                       k        = 0;   // camera
            int                       phase    = 0;   // 0 = waiting at camera k, 1 = moving on from it
            float                     t        = 0.0f;
            float                     fovCarry = 70.0f;  // the FOV the camera arrived with (zoom trick)
            float                     segFov   = 70.0f;  // the FOV / pivot distance a move started with
            float                     segDist  = 100.0f;
            bool                      haveOut  = false;
            bool                      snapOut  = false;  // a cut this frame: no motion smoothness
            RE::NiPoint3              outPos{};
            RE::NiPoint3              outFwd{ 0.0f, 1.0f, 0.0f };
            float                     outYaw   = 0.0f;
            float                     outRoll  = 0.0f;
            float                     outFov   = 70.0f;
            float                     blur     = 0.0f;
            bool                      blurOn   = false;
            float                     total    = 0.0f;
            float                     elapsed  = 0.0f;
            int                       editGen  = 0;  // s_editGen when Play started
            bool                      scene    = false;  // 11-14 round 13: a SexLab stage's scene path (loops, ends with the stage)
        };
        Play   s_play;
        bool   s_pendingPlay   = false;
        double s_pendingUntil  = 0.0;
        int    s_pendingFrom   = 0;
        float  s_pendingRate   = 1.0f;

        // Record manual path (main thread).
        struct Record {
            bool         on = false;
            float        sinceLast = 0.0f;
            RE::NiPoint3 lastPos{};
            float        lastYaw = 0.0f, lastPitch = 0.0f;
        };
        Record s_rec;

        int Count() { return static_cast<int>(s_play.cams.size()); }
        int Next(int a_k) {
            const int n = Count();
            if (a_k + 1 < n) return a_k + 1;
            return s_play.loop && n > 1 ? 0 : -1;
        }
        int   Prev(int a_k) { return a_k > 0 ? a_k - 1 : -1; }  // a rewind stops at camera 1, loop or not
        // Slow motion: the speed (0..1 of normal) at normal-time progress a_f of a move - a smooth dip round slowAt.
        float SlowSpeed(const Cam& a_c, float a_f) {
            if (!a_c.slowMo) return 1.0f;
            const float half = (std::max)(a_c.slowLength, 0.05f) * 0.5f;
            const float d    = std::fabs(a_f - a_c.slowAt);
            if (d >= half) return 1.0f;
            const float b = 0.5f * (1.0f + std::cos(kPi * d / half));
            return 1.0f - (1.0f - std::clamp(a_c.slowSpeed, 0.05f, 1.0f)) * b;
        }

        // A move is up to three parts: the slingshot's wind-up (pull back), the move itself, the elastic tail (past the
        // next camera and springing back). Bullet time: the camera keeps its own pace, only the world slows.
        constexpr float kElasticHz = 1.2f;  // the elastic swing's rate
        bool  CamSlows(const Cam& a_c) { return a_c.slowMo && a_c.slowCamera; }
        float WindTime(const Cam& a_c) { return a_c.sling > 0.001f ? 0.4f + 0.5f * a_c.sling : 0.0f; }
        float ElasticDecay(const Cam& a_c) { return 1.5f + 6.0f * (1.0f - std::clamp(a_c.elastic, 0.0f, 1.0f)); }
        float TailTime(const Cam& a_c) { return a_c.elastic > 0.001f ? (std::min)(4.6f / ElasticDecay(a_c), 3.0f) : 0.0f; }

        // Seconds of the move itself: its own at normal speed, more with slow motion.
        float CoreTime(int a_k) {
            const auto& c = s_play.cams[a_k];
            if (CamSlows(c) && a_k < static_cast<int>(s_play.slowTab.size())) return s_play.slowTab[a_k][kSlowSamples];
            return c.time;
        }

        // Seconds a move takes, all parts (none for a cut).
        float MoveTime(int a_k) {
            const auto& c = s_play.cams[a_k];
            if (c.move == kMoveCut) return 0.0f;
            return WindTime(c) + CoreTime(a_k) + TailTime(c);
        }

        void BuildSlowTables() {
            const int n = static_cast<int>(s_play.cams.size());
            s_play.slowTab.assign(n, {});
            for (int k = 0; k < n; ++k) {
                const auto& c = s_play.cams[k];
                if (!CamSlows(c)) continue;
                auto& T = s_play.slowTab[k];
                for (int i = 1; i <= kSlowSamples; ++i) {
                    const float mid = (static_cast<float>(i) - 0.5f) / kSlowSamples;
                    T[i]            = T[i - 1] + (c.time / kSlowSamples) / (std::max)(SlowSpeed(c, mid), 0.05f);
                }
            }
        }

        // Seconds a_t into move a_k -> how far along it is in normal time (0..1).
        float NormalF(int a_k, float a_t) {
            const auto& c = s_play.cams[a_k];
            if (c.time <= 0.0f) return 1.0f;
            if (!CamSlows(c) || a_k >= static_cast<int>(s_play.slowTab.size())) return Clamp01(a_t / c.time);
            const auto& T = s_play.slowTab[a_k];
            if (a_t <= 0.0f) return 0.0f;
            if (a_t >= T[kSlowSamples]) return 1.0f;
            const auto it   = std::upper_bound(T.begin(), T.end(), a_t);
            const int  i    = static_cast<int>(std::distance(T.begin(), it));  // T[i - 1] <= a_t < T[i]
            const float span = T[i] - T[i - 1];
            const float w    = span > 1e-6f ? (a_t - T[i - 1]) / span : 1.0f;
            return (static_cast<float>(i - 1) + w) / kSlowSamples;
        }

        // The world's time: a slow-motion move with "the world slows too" (1 = normal). The engine's CURRENT multiplier
        // is written as well as the target (a target alone eases there - and from 0 never back, see FreezeTime).
        bool  s_worldSlowed = false;
        float s_worldBase   = 1.0f;
        void WorldSpeed(float a_v) {
            if (FreezeTime::IsFrozen()) return;  // Freeze world wins
            REL::Relocation<float*> current{ RELOCATION_ID(511882, 388442) };
            REL::Relocation<float*> target{ RELOCATION_ID(511883, 388443) };
            if (a_v >= 0.999f) {
                if (s_worldSlowed) {
                    *current      = s_worldBase;
                    *target       = s_worldBase;
                    s_worldSlowed = false;
                    SKSE::log::info("CamPath: the world runs at normal speed again ({:.2f})", s_worldBase);
                }
                return;
            }
            if (!s_worldSlowed) {
                s_worldBase   = *target > 0.0f ? *target : 1.0f;
                s_worldSlowed = true;
                SKSE::log::info("CamPath: slow motion - the world slows down too");
            }
            *current = s_worldBase * a_v;
            *target  = s_worldBase * a_v;
        }

        // Camera shake: layered waves at unrelated rates - bumps up and down, sway, small tilts (a cart on a rough road).
        void ShakeAt(float a_t, float a_amount, RE::NiPoint3& a_pos, float& a_yaw, float& a_pitch, float& a_roll) {
            if (a_amount <= 0.001f) return;
            auto wave = [a_t](float a_f1, float a_f2, float a_f3, float a_p) {
                return std::sin(a_t * a_f1 + a_p) * 0.5f + std::sin(a_t * a_f2 + a_p * 1.7f) * 0.3f + std::sin(a_t * a_f3 + a_p * 2.3f) * 0.2f;
            };
            a_pos.z += wave(9.1f, 15.3f, 23.7f, 0.0f) * 10.0f * a_amount;  // bumps
            a_pos.x += wave(2.3f, 5.9f, 11.1f, 1.3f) * 4.0f * a_amount;    // sway
            a_pos.y += wave(2.9f, 6.7f, 12.9f, 2.1f) * 4.0f * a_amount;
            a_pitch += wave(7.7f, 13.1f, 19.3f, 0.7f) * 0.021f * a_amount;  // ~1.2 degrees
            a_yaw += wave(3.1f, 7.3f, 10.7f, 2.9f) * 0.014f * a_amount;
            a_roll += wave(4.3f, 8.9f, 14.1f, 1.9f) * 0.026f * a_amount;
        }

        const Piv&   PivOf(int a_i) { return s_play.pivots[s_play.pivOf[a_i]]; }
        bool         HavePivot(int a_i) { return PivOf(a_i).have; }
        RE::NiPoint3 AimPoint(int a_i) { return PivOf(a_i).aim + RE::NiPoint3{ 0.0f, 0.0f, s_settings.aimHeight }; }

        // Where camera a_i is now: where it was put, or its offset turned with its pivot (follow).
        RE::NiPoint3 WorldOf(int a_i) {
            const auto& p = PivOf(a_i);
            if (s_play.rides[a_i] && p.have) return p.pos + RotZ(s_play.base[a_i], p.heading);
            return s_play.cams[a_i].pos;
        }

        float SetYaw(int a_i) {
            const auto& p = PivOf(a_i);
            return s_play.rides[a_i] && p.have ? s_play.baseYaw[a_i] + p.heading : s_play.cams[a_i].yaw;
        }

        // Camera a_i's spot this frame; one past either end is mirrored (a curve's end tangents).
        RE::NiPoint3 WorldAt(int a_i) {
            const int n = Count();
            if (s_play.loop) return s_play.world[((a_i % n) + n) % n];
            if (a_i < 0) return s_play.world[0] * 2.0f - s_play.world[std::min(1, n - 1)];
            if (a_i >= n) return s_play.world[n - 1] * 2.0f - s_play.world[std::max(n - 2, 0)];
            return s_play.world[a_i];
        }

        // Centripetal Catmull-Rom between cameras a_k and a_next.
        RE::NiPoint3 CurvePos(int a_k, int a_next, float a_u) {
            const RE::NiPoint3 p1 = s_play.world[a_k], p2 = s_play.world[a_next];
            const RE::NiPoint3 p0 = (s_play.loop || a_k > 0) ? WorldAt(a_k - 1) : p1 * 2.0f - p2;
            const RE::NiPoint3 p3 = (s_play.loop || a_next + 1 < Count()) ? WorldAt(a_next + 1) : p2 * 2.0f - p1;
            auto knot = [](const RE::NiPoint3& a, const RE::NiPoint3& b) { return (std::max)(std::sqrt(Length(b - a)), 1e-3f); };
            const float t0 = 0.0f, t1 = t0 + knot(p0, p1), t2 = t1 + knot(p1, p2), t3 = t2 + knot(p2, p3);
            const float t  = t1 + (t2 - t1) * a_u;
            const RE::NiPoint3 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
            const RE::NiPoint3 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
            const RE::NiPoint3 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
            const RE::NiPoint3 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
            const RE::NiPoint3 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
            return b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1));
        }

        // Bend of a straight / curved move: nothing at the cameras, the most half-way (bend 1 = half the distance).
        RE::NiPoint3 BendAt(int a_k, int a_next, float a_p) {
            const auto& c = s_play.cams[a_k];
            if (c.bendSide == 0.0f && c.bendUp == 0.0f) return {};
            const RE::NiPoint3 d   = s_play.world[a_next] - s_play.world[a_k];
            const float        len = Length(d);
            if (len < 1.0f) return {};
            const RE::NiPoint3 right = Normalized(RE::NiPoint3{ d.y, -d.x, 0.0f }, RE::NiPoint3{ 1.0f, 0.0f, 0.0f });
            const float        s     = std::sin(kPi * Clamp01(a_p)) * 0.5f * len;
            return right * (c.bendSide * s) + RE::NiPoint3{ 0.0f, 0.0f, c.bendUp * s };
        }

        // A straight / curved move at its own parameter a_p.
        RE::NiPoint3 LinePos(int a_k, int a_next, float a_p) {
            const RE::NiPoint3 on = s_play.cams[a_k].move == kMoveStraight ? Mix(s_play.world[a_k], s_play.world[a_next], a_p)
                                                                            : CurvePos(a_k, a_next, a_p);
            return on + BendAt(a_k, a_next, a_p);
        }

        // Even speed along a straight / curved move: progress a_u (0..1 of its length) -> its own parameter. Measured
        // each frame, since a followed pivot moves the ends.
        float EvenParam(int a_k, int a_next, float a_u) {
            const auto& c = s_play.cams[a_k];
            if (c.move == kMoveStraight && c.bendSide == 0.0f && c.bendUp == 0.0f) return a_u;  // already even
            std::array<float, kSamples + 1> L{};
            RE::NiPoint3 prev = LinePos(a_k, a_next, 0.0f);
            for (int i = 1; i <= kSamples; ++i) {
                const RE::NiPoint3 p = LinePos(a_k, a_next, static_cast<float>(i) / kSamples);
                L[i] = L[i - 1] + Length(p - prev);
                prev = p;
            }
            if (L[kSamples] <= 1e-3f) return a_u;
            const float s  = Clamp01(a_u) * L[kSamples];
            const auto  it = std::lower_bound(L.begin(), L.end(), s);
            const int   i  = static_cast<int>(std::distance(L.begin(), it));
            if (i == 0) return 0.0f;
            if (i > kSamples) return 1.0f;
            const float span = L[i] - L[i - 1];
            const float w    = span > 1e-4f ? (s - L[i - 1]) / span : 1.0f;
            return (static_cast<float>(i - 1) + w) / kSamples;
        }

        // Signed angle for "which way round" + extra turns (a_d = the shortest signed difference).
        float WayAngle(float a_d, int a_way, int a_turns) {
            float d = a_d;
            if (a_way == kWayRight && d < 0.0f) d += kTwoPi;   // right = clockwise = the angle grows
            if (a_way == kWayLeft && d > 0.0f) d -= kTwoPi;
            const float sign = a_way == kWayLeft ? -1.0f : (a_way == kWayRight ? 1.0f : (d < 0.0f ? -1.0f : 1.0f));
            return d + sign * kTwoPi * static_cast<float>(std::max(a_turns, 0));
        }

        // Position along move a_k -> a_next at eased progress a_u.
        RE::NiPoint3 MovePos(int a_k, int a_next, float a_u) {
            const auto& c = s_play.cams[a_k];
            // Around: one middle for the whole move - where the leaving camera's pivot is NOW, else the next one's
            // (a pivot picked after the cameras were put, or one that walked off, is still the one circled). A middle
            // per end slid the circle across when the two cameras had different pivots: the camera swung far out and
            // back in (user 10-29). With one middle the distance blends smoothly. No pivot at either end: a curve.
            const int ci = c.move == kMoveAround ? (HavePivot(a_k) ? a_k : (HavePivot(a_next) ? a_next : -1)) : -1;
            if (ci >= 0) {
                const RE::NiPoint3 mid = PivOf(ci).aim;
                const RE::NiPoint3 o0 = s_play.world[a_k] - mid, o1 = s_play.world[a_next] - mid;
                const float r0 = std::hypot(o0.x, o0.y), r1 = std::hypot(o1.x, o1.y);
                const float a0 = std::atan2(o0.x, o0.y), a1 = std::atan2(o1.x, o1.y);
                const float da = WayAngle(WrapPi(a1 - a0), c.way, c.turns);
                const float r  = r0 + (r1 - r0) * a_u;
                const float a  = a0 + da * a_u;
                const float h  = o0.z + (o1.z - o0.z) * a_u;
                return mid + RE::NiPoint3{ std::sin(a) * r, std::cos(a) * r, h };
            }
            return LinePos(a_k, a_next, EvenParam(a_k, a_next, a_u));
        }

        // The way camera a_i looks from a_pos (a_motion = the direction of travel for "where it is going").
        RE::NiPoint3 LookDir(int a_i, const RE::NiPoint3& a_pos, const RE::NiPoint3& a_motion) {
            const auto& c = s_play.cams[a_i];
            switch (c.look) {
            case kLookPivot:
                if (HavePivot(a_i)) return Normalized(AimPoint(a_i) - a_pos, Forward(SetYaw(a_i), c.pitch));
                [[fallthrough]];
            case kLookSet:
                return Forward(SetYaw(a_i), c.pitch);
            default:
                return Normalized(a_motion, s_play.haveOut ? s_play.outFwd : Forward(SetYaw(a_i), c.pitch));
            }
        }

        float CamBlurOf(int a_i) {
            const auto& c = s_play.cams[a_i];
            return c.blur ? std::clamp(c.blurStrength, 0.0f, 1.0f) : 0.0f;
        }

        struct Target {
            RE::NiPoint3 pos{};
            RE::NiPoint3 fwd{ 0.0f, 1.0f, 0.0f };
            float        roll = 0.0f;
            float        fov  = 70.0f;
            float        blur = 0.0f;
            RE::NiPoint3 focus{};  // what the blur keeps sharp
            bool         haveFocus = false;
            float        shake     = 0.0f;
            bool         fovDirect = false;  // a crash zoom: no motion smoothing on the field of view
            float        world     = 1.0f;   // the world's speed (slow motion)
            RE::NiPoint3 swing{};            // slingshot pull-back / elastic swing: added after the motion smoothing
        };

        bool EaseIn(int a_k) {
            const auto& c = s_play.cams[a_k];
            if (c.sling > 0.001f) return false;  // a slingshot shoots off at speed
            return c.smooth || c.wait > 0.0f || (!s_play.loop && a_k == 0);
        }
        bool EaseOut(int a_k, int a_next) {
            const auto& c = s_play.cams[a_k];
            if (c.elastic > 0.001f) return false;  // an elastic arrival comes in at speed and flies past
            return c.smooth || s_play.cams[a_next].wait > 0.0f || (!s_play.loop && Next(a_next) < 0);
        }

        // Progress along move a_k at its normal-time fraction a_f: the camera's own speed curve, else automatic.
        float MoveEase(int a_k, int a_next, float a_f) {
            const auto& c = s_play.cams[a_k];
            if (c.curveOn) return CurveEase(a_f, c.curveX1, c.curveY1, c.curveX2, c.curveY2);
            return Ease(a_f, EaseIn(a_k), EaseOut(a_k, a_next));
        }

        // The slingshot's pull back and the elastic swing, on top of the move's own position. a_tc = seconds into the
        // move itself (< 0: winding up, > core: the tail); a_f its normal-time progress.
        RE::NiPoint3 SwingOffset(int a_k, int a_next, float a_tc, float a_core, float a_f) {
            const auto&        c   = s_play.cams[a_k];
            const float        len = Length(s_play.world[a_next] - s_play.world[a_k]);
            RE::NiPoint3       out{};
            if (c.sling > 0.001f) {
                const RE::NiPoint3 d0   = Normalized(MovePos(a_k, a_next, 0.02f) - MovePos(a_k, a_next, 0.0f), RE::NiPoint3{});
                const float        back = c.sling * (std::min)(0.3f * len, 250.0f);
                const float        W    = WindTime(c);
                const float pull = a_tc < 0.0f ? SmoothStep((a_tc + W) / W) : 1.0f - SmoothStep(a_f / 0.35f);
                out = out - d0 * (back * pull);
            }
            if (c.elastic > 0.001f && a_tc > a_core) {
                // swings on past the camera at the speed it arrived with, then springs back and settles
                const RE::NiPoint3 dPu  = (MovePos(a_k, a_next, 1.0f) - MovePos(a_k, a_next, 0.98f)) * 50.0f;
                const float        dfdt = (CamSlows(c) ? SlowSpeed(c, 1.0f) : 1.0f) / (std::max)(c.time, 0.05f);
                const float        w    = 2.0f * kPi * kElasticHz;
                const float        amp  = (std::min)(Length(dPu) * dfdt / w * (0.6f + 0.8f * c.elastic), (std::min)(0.3f * len, 300.0f));
                const float        tau  = a_tc - a_core;
                out = out + Normalized(dPu, RE::NiPoint3{}) * (amp * std::exp(-ElasticDecay(c) * tau) * std::sin(w * tau));
            }
            return out;
        }

        // The zoom trick's field of view: a_fov at pivot distance a_d0 -> at a_d, the pivot the same size on screen.
        float KeepSizeFov(float a_fov, float a_d0, float a_d) {
            const float half = std::atan(std::tan(a_fov * 0.5f * kPi / 180.0f) * a_d0 / (std::max)(a_d, 1.0f));
            return std::clamp(2.0f * half * 180.0f / kPi, 5.0f, 150.0f);
        }

        // Dolly while waiting (zoom trick on, 10-31): the camera pushes in on the pivot (+, up to 70% of the way there)
        // or pulls back (-, up to twice as far) over the wait. a_w = 0..1 of it; the move on blends back onto its path.
        // 11-14 round 12 (user: "Dolly on a waiting camera ... did not seem to work - and blur background also did not seem to
        // work"): both needed the camera's own pivot, and a new camera has none (camera 3 of the user's path: Dolly 100%, a 3.9 s
        // wait, blur on, no pivot - nothing happened, and the blur said nothing). Without a pivot the subject is the point on the
        // camera's own line of sight at your depth (you in front of it), else 300 units out: the dolly pushes along that line and
        // keeps that depth's size, the blur keeps that depth sharp
        RE::NiPoint3 SubjectOf(int a_k, const RE::NiPoint3& a_from, const RE::NiPoint3& a_fwd) {
            if (HavePivot(a_k)) return AimPoint(a_k);
            float        depth = 300.0f;
            RE::NiPoint3 you{}, youAim{};
            float        heading = 0.0f;
            if (PivotRaw(0, 0, you, heading, nullptr, kBoneMiddle, &youAim)) {
                const float d = Dot(youAim - a_from, a_fwd);
                if (d > 50.0f) depth = (std::min)(d, 3000.0f);
            }
            return a_from + a_fwd * depth;
        }
        // the subject of camera a_k from where it was put, looking the way it was set
        RE::NiPoint3 SetSubjectOf(int a_k) {
            return SubjectOf(a_k, s_play.world[a_k], Forward(SetYaw(a_k), s_play.cams[a_k].pitch));
        }

        RE::NiPoint3 DollyOffset(int a_k, float a_w) {
            const auto& c = s_play.cams[a_k];
            if (!c.zoomTrick || c.dolly == 0.0f || c.wait <= 0.0f) return {};
            return (SetSubjectOf(a_k) - s_play.world[a_k]) * ((c.dolly > 0.0f ? 0.7f * c.dolly : c.dolly) * a_w);
        }

        Target Evaluate() {
            Target out;
            const int k = s_play.k;
            const auto& ck = s_play.cams[k];
            const int next = Next(k);
            if (s_play.phase == 0 || next < 0) {
                const RE::NiPoint3 dolly = DollyOffset(k, ck.wait > 0.0f ? SmoothStep(s_play.t / ck.wait) : 0.0f);
                out.pos       = s_play.world[k] + dolly;
                out.fwd       = LookDir(k, out.pos, s_play.haveOut ? s_play.outFwd : Forward(SetYaw(k), ck.pitch));
                out.roll      = ck.roll;
                out.fov       = s_play.fovCarry;
                if (dolly.x != 0.0f || dolly.y != 0.0f || dolly.z != 0.0f) {
                    const RE::NiPoint3 subject = SetSubjectOf(k);
                    out.fov = KeepSizeFov(s_play.fovCarry, (std::max)(Length(subject - s_play.world[k]), 1.0f), Length(subject - out.pos));
                }
                out.blur      = CamBlurOf(k);
                out.focus     = HavePivot(k) ? AimPoint(k) : SubjectOf(k, out.pos, out.fwd);   // 11-14 round 12: no pivot = your depth
                out.haveFocus = true;
                out.shake     = ck.shake;
                return out;
            }
            const auto& cn   = s_play.cams[next];
            const float core = CoreTime(k);
            const float tc   = s_play.t - WindTime(ck);  // seconds into the move itself
            const float f    = tc <= 0.0f ? 0.0f : (tc >= core ? 1.0f : NormalF(k, tc));
            const float u    = MoveEase(k, next, f);
            const RE::NiPoint3 dolly  = DollyOffset(k, 1.0f);  // where a dolly while waiting left the camera
            const RE::NiPoint3 onPath = MovePos(k, next, u) + dolly * (1.0f - u);
            // the path is smoothed; the swing goes on top afterwards (smoothing would iron a quick bounce flat)
            out.pos   = onPath;
            out.swing = SwingOffset(k, next, tc, core, f);
            const RE::NiPoint3 seenFrom = onPath + out.swing;
            const float        uAhead   = MoveEase(k, next, std::min(f + 0.02f, 1.0f));
            const RE::NiPoint3 ahead    = MovePos(k, next, uAhead) + dolly * (1.0f - uAhead) - onPath;
            if (ck.look == kLookSet && cn.look == kLookSet) {
                // both keep their own view: turn from one to the other. Extra turns and which way round are orbit
                // settings (10-31): an orbit without a pivot (it curves) spins in place with them; a real orbit has
                // them already, and other moves take the short way.
                const float y0 = SetYaw(k), y1 = SetYaw(next);
                const bool  orbits = ck.move == kMoveAround && (HavePivot(k) || HavePivot(next));  // no pivot: a curve
                const float d = orbits || ck.move != kMoveAround ? WrapPi(y1 - y0) : WayAngle(WrapPi(y1 - y0), ck.way, ck.turns);
                out.fwd = Forward(y0 + d * u, ck.pitch + (cn.pitch - ck.pitch) * u);
            } else {
                // Leaving a look at the pivot: keep it in view and turn in the last 40% (a spiral stays on the pivot).
                // Arriving into one: lock onto the pivot in the first 60% and keep it there. Otherwise over the whole move.
                float w = SmoothStep(f);
                if (ck.look == kLookPivot && cn.look != kLookPivot) w = SmoothStep((f - 0.6f) / 0.4f);
                if (ck.look != kLookPivot && cn.look == kLookPivot) w = SmoothStep(f / 0.6f);
                out.fwd = SlerpDir(LookDir(k, seenFrom, ahead), LookDir(next, seenFrom, ahead), w);
            }
            out.roll = ck.roll + WrapPi(cn.roll - ck.roll) * u;
            if (ck.zoomTrick && HavePivot(k)) {
                out.fov = KeepSizeFov(s_play.segFov, s_play.segDist, Length(AimPoint(k) - out.pos));
            } else {
                // crash zoom: slow, then slamming in at the end (cubic)
                out.fov       = s_play.segFov + (cn.fov - s_play.segFov) * (ck.crashZoom ? f * f * f : u);
                out.fovDirect = ck.crashZoom;
            }
            out.blur  = CamBlurOf(k) + (CamBlurOf(next) - CamBlurOf(k)) * u;
            out.shake = ck.shake + (cn.shake - ck.shake) * u;
            out.world = ck.slowMo && (ck.slowWorld || !ck.slowCamera) ? SlowSpeed(ck, f) : 1.0f;  // bullet time: always
            if (HavePivot(k) && HavePivot(next)) {
                out.focus     = Mix(AimPoint(k), AimPoint(next), u);
                out.haveFocus = true;
            } else if (HavePivot(k) || HavePivot(next)) {
                out.focus     = HavePivot(k) ? AimPoint(k) : AimPoint(next);
                out.haveFocus = true;
            } else {
                out.focus     = SubjectOf(k, seenFrom, out.fwd);   // 11-14 round 12: no pivot either end = your depth on its line of sight
                out.haveFocus = true;
            }
            return out;
        }

        void StartMove() {
            const int k    = s_play.k;
            s_play.segFov  = s_play.fovCarry;
            s_play.segDist = HavePivot(k) ? (std::max)(Length(AimPoint(k) - s_play.world[k]), 1.0f) : 100.0f;
        }

        // Run the clock by a_dt (negative = rewinding).
        enum Step { kGoing, kAtEnd, kAtStart };
        Step Advance(float a_dt) {
            s_play.t += a_dt;
            s_play.elapsed += a_dt;
            for (int guard = 0; guard < 4 * Count() + 4; ++guard) {
                const int   k = s_play.k;
                const auto& c = s_play.cams[k];
                if (s_play.t >= 0.0f) {
                    if (s_play.phase == 0) {
                        if (s_play.t < c.wait) return kGoing;
                        if (Next(k) < 0) {
                            s_play.t = c.wait;  // the end (a rewind starts from here)
                            return kAtEnd;
                        }
                        s_play.t -= c.wait;
                        s_play.phase = 1;
                        StartMove();
                    } else {
                        const float T = MoveTime(k);
                        if (s_play.t < T) return kGoing;
                        s_play.t -= T;
                        // the FOV the camera arrives with (a zoom trick keeps its result)
                        const int next  = Next(k);
                        s_play.fovCarry = c.zoomTrick && c.move != kMoveCut ? s_play.outFov : s_play.cams[next].fov;
                        if (c.move == kMoveCut) s_play.snapOut = true;
                        s_play.k     = next;
                        s_play.phase = 0;
                        if (next == 0) s_play.elapsed = s_play.t;  // a new lap
                    }
                } else if (s_play.phase == 1) {  // rewinding: back into the wait at camera k
                    s_play.phase    = 0;
                    s_play.t       += c.wait;
                    s_play.fovCarry = c.fov;
                } else {                         // rewinding: back into the move that led to camera k
                    const int prev = Prev(k);
                    if (prev < 0) {
                        s_play.t       = 0.0f;
                        s_play.elapsed = 0.0f;
                        return kAtStart;
                    }
                    s_play.k        = prev;
                    s_play.phase    = 1;
                    s_play.fovCarry = s_play.cams[prev].fov;
                    StartMove();
                    s_play.t += MoveTime(prev);
                    if (s_play.cams[prev].move == kMoveCut) s_play.snapOut = true;
                }
            }
            return kGoing;
        }

        // The path clock put a_sec into the path (one lap): the camera, wait or move there; the camera jumps to it.
        void SeekTo(float a_sec) {
            const int n    = Count();
            float     left = std::clamp(std::isfinite(a_sec) ? a_sec : 0.0f, 0.0f, s_play.total);
            s_play.elapsed = left;
            int k          = 0;
            for (int guard = 0; guard <= n; ++guard) {
                const auto& c = s_play.cams[k];
                if (left <= c.wait || Next(k) < 0) {  // waiting at camera k (the last one holds the end)
                    s_play.k        = k;
                    s_play.phase    = 0;
                    s_play.t        = (std::min)(left, c.wait);
                    s_play.fovCarry = c.fov;
                    break;
                }
                left -= c.wait;
                const float T = MoveTime(k);
                if (left < T || guard == n) {  // on the way to the next camera
                    s_play.k        = k;
                    s_play.phase    = 1;
                    s_play.fovCarry = c.fov;
                    StartMove();
                    s_play.t = (std::min)(left, T);
                    break;
                }
                left -= T;
                k = Next(k);
            }
            s_play.snapOut = true;
        }

        void UpdatePivots(float a_dt, bool a_snap) {
            for (auto& p : s_play.pivots) {
                RE::NiPoint3 raw{}, rawAim{};
                float        heading = 0.0f;
                std::string  name;
                if (p.kind < 0 || !PivotRaw(p.kind, p.ref, raw, heading, &name, p.bone, &rawAim)) {
                    p.have = false;
                    continue;
                }
                if (!p.have || a_snap) {
                    p.pos     = raw;
                    p.aim     = rawAim;
                    p.heading = heading;
                } else {
                    const float w = ExpWeight(a_dt, kPivotHalfLife);
                    p.pos         = Mix(p.pos, raw, w);
                    p.aim         = Mix(p.aim, rawAim, w);
                    p.heading    += WrapPi(heading - p.heading) * w;
                }
                p.have = true;
                p.name = p.bone != kBoneMiddle ? std::format("{} - {}", name, kBoneNames[p.bone]) : std::move(name);
            }
            const int n = Count();
            s_play.world.resize(n);
            for (int i = 0; i < n; ++i) s_play.world[i] = WorldOf(i);
        }

        void WritePose(RE::FreeCameraState* a_fcs, float a_shake, const RE::NiPoint3& a_swing) {
            float yaw = s_play.outYaw, pitch = 0.0f;
            LookAngles(s_play.outFwd, yaw, pitch);
            s_play.outYaw = yaw;
            RE::NiPoint3 pos  = s_play.outPos;
            float        roll = s_play.outRoll;
            pos = pos + a_swing;
            ShakeAt(s_play.shakeT, a_shake, pos, yaw, pitch, roll);  // on top of the path, never into it
            a_fcs->translation = pos;
            a_fcs->rotation.x  = std::clamp(pitch, -kPitchLimit, kPitchLimit);
            a_fcs->rotation.y  = WrapPi(yaw);
            SetFOV(s_play.outFov);
            FreeCam::SetExtraRoll(WrapPi(roll - FreeCam::GetRollRadians()));
        }

        void Publish() {
            std::scoped_lock lock(s_lock);
            s_pubPlaying   = s_play.on;
            s_pubPaused    = s_play.on && s_play.paused;
            s_pubRewinding = s_play.on && !s_play.paused && s_play.rate < 0.0f;
            s_pubRate      = s_play.on ? s_play.rate : 1.0f;
            s_pubAt        = s_play.on ? s_play.k : -1;
            s_pubProgress  = s_play.on && s_play.total > 0.0f ? Clamp01(s_play.elapsed / s_play.total) : 0.0f;
            s_pubRecording = s_rec.on;
            s_pubScene     = s_play.on && s_play.scene;   // 11-14 round 13
        }

        void BlurOff() {
            if (s_play.blurOn) {
                CamBlur::External(false, {}, -1.0f);
                s_play.blurOn = false;
            }
        }

        bool s_videoByPath = false;  // 0.18.0: the path started the video recording (it stops it at the end)

        // The camera stays where the path left it; its roll becomes the free camera's own.
        void EndPlay(std::string_view a_why) {
            if (!s_play.on) return;
            if (std::exchange(s_videoByPath, false)) VideoCapture::RequestStop("the camera path stopped");
            FreeCam::SetRollRadians(WrapPi(FreeCam::GetRollRadians() + FreeCam::GetExtraRoll()));
            FreeCam::SetExtraRoll(0.0f);
            BlurOff();
            WorldSpeed(1.0f);
            s_play.on      = false;
            s_play.paused  = false;
            s_play.gliding = false;
            Event(std::format("camera path stopped ({}) - the camera stays where it is", a_why));
            Publish();
        }

        // The play state for s_play.cams: each camera's pivot, where it is now, the slow-motion tables, the total time.
        void PreparePlay() {
            const int n = Count();
            for (int i = 0; i < n; ++i) {
                const auto&      c    = s_play.cams[i];
                const int        kind = c.pivotKind < 0 ? -1 : (c.pivotKind == 2 ? 2 : (c.pivotKind == 1 && c.pivotRef ? 1 : 0));
                const RE::FormID ref  = kind == 1 || kind == 2 ? c.pivotRef : 0;   // 11-14 round 13: 2 = a scene position
                const int        bone = std::clamp(c.bone, 0, kBoneCount - 1);
                int              idx  = -1;
                for (int j = 0; j < static_cast<int>(s_play.pivots.size()); ++j) {
                    const auto& q = s_play.pivots[j];
                    if (q.kind == kind && q.ref == ref && q.bone == bone) idx = j;
                }
                if (idx < 0) {
                    Piv p;
                    p.kind = kind;
                    p.ref  = ref;
                    p.bone = bone;
                    s_play.pivots.push_back(p);
                    idx = static_cast<int>(s_play.pivots.size()) - 1;
                }
                const bool rides = s_play.follow && c.havePivot;
                s_play.pivOf.push_back(idx);
                s_play.rides.push_back(rides);
                s_play.base.push_back(rides ? RotZ(c.pos - c.pivotPos, -c.pivotHeading) : c.pos);
                s_play.baseYaw.push_back(rides ? WrapPi(c.yaw - c.pivotHeading) : c.yaw);
            }
            UpdatePivots(0.0f, true);
            BuildSlowTables();
            for (int k = 0; k < n; ++k) {
                s_play.total += s_play.cams[k].wait;
                if (Next(k) >= 0) s_play.total += MoveTime(k);
            }
        }

        // ---- the path drawn in the world (10-29) ---------------------------------------------------------------
        // The shape of each move (not its timing) and the cameras, for CamPathView: from the playing path when it
        // plays (its pivots as they are now), else from a preview set up like Play's (the play state is put back).
        std::atomic<bool> s_clean{ false };  // Clean screen is on (ApplyClean below)
        double s_viewNext = 0.0;
        int    s_viewGen  = -1;

        RE::NiPoint3 ShapeAt(int a_k, int a_next, float a_p) {
            if (s_play.cams[a_k].move == kMoveAround) return MovePos(a_k, a_next, a_p);  // no pivot: MovePos curves
            return LinePos(a_k, a_next, a_p);
        }

        void BuildView(CamPathView::View& a_out) {
            const int n = Count();
            for (int k = 0; k < n; ++k) {
                const auto&        c = s_play.cams[k];
                CamPathView::Mark  m;
                m.pos                  = s_play.world[k];
                const RE::NiPoint3 set = Forward(SetYaw(k), c.pitch);
                m.fwd       = c.look == kLookPivot && HavePivot(k) ? Normalized(AimPoint(k) - m.pos, set) : set;
                m.havePivot = HavePivot(k);
                if (m.havePivot) m.pivot = AimPoint(k);
                a_out.cams.push_back(m);
                const int next = Next(k);
                if (next < 0) continue;
                CamPathView::Line line;
                line.cut = c.move == kMoveCut;
                if (line.cut) {
                    line.pts = { s_play.world[k], s_play.world[next] };
                } else {
                    constexpr int kSteps = 32;
                    for (int i = 0; i <= kSteps; ++i) line.pts.push_back(ShapeAt(k, next, static_cast<float>(i) / kSteps));
                }
                a_out.lines.push_back(std::move(line));
            }
        }

        // Main thread, every free-camera frame: shown while TFCam drives the free camera and Clean screen is off.
        void UpdateView(bool a_driving) {
            if (!s_settings.showPath || !a_driving || s_clean) {
                CamPathView::Clear();
                return;
            }
            const double now = Now();
            const int    gen = s_editGen.load();
            if (now < s_viewNext && gen == s_viewGen) return;
            s_viewNext = now + (s_play.on ? 0.1 : 0.5);  // a followed pivot moves the path
            s_viewGen  = gen;
            CamPathView::View v;
            if (s_play.on) {
                BuildView(v);
                v.at = s_play.k;
            } else {
                Play saved = std::move(s_play);
                s_play     = Play{};
                {
                    std::scoped_lock lock(s_lock);
                    s_play.cams   = s_cams;
                    s_play.follow = s_follow;
                    s_play.loop   = s_loop;
                }
                if (!s_play.cams.empty()) {
                    PreparePlay();
                    BuildView(v);
                }
                s_play = std::move(saved);
            }
            CamPathView::Publish(std::move(v));
        }

        // Play from camera a_from (a_glide: 1 s from where the camera is; a_rate < 0: backwards from the end of its wait).
        // a_scene (11-14 round 13): a SexLab stage's scene path, already placed round the scene - it plays in its place, loops,
        // never follows, and the editor's own path is left as it is
        bool StartPlay(RE::FreeCameraState* a_fcs, std::string_view a_source, int a_from = 0, bool a_glide = true,
            float a_rate = 1.0f, const std::vector<Cam>* a_scene = nullptr) {
            LoadOnce();
            const bool blurWas = s_play.blurOn;
            {
                std::scoped_lock lock(s_lock);
                const auto& src = a_scene ? *a_scene : s_cams;
                if (src.size() < 2) {
                    EventLocked(std::format("play refused ({}): {} camera(s)", a_source, src.size()));
                    Notify("Camera path: add at least 2 cameras first");
                    return false;
                }
                // 11-13: never fly to another cell / worldspace (the picture goes black)
                const std::string here = SpaceHere();
                for (std::size_t k = 0; k < src.size(); ++k) {
                    std::string where;
                    if (!CamAway(src[k], here, where)) continue;
                    EventLocked(std::format("play refused: camera {} is in {} - go there first", k + 1, where));
                    Notify(std::format("Camera path: go to {} first", where));
                    return false;
                }
                s_play         = Play{};
                s_play.blurOn  = blurWas;
                s_play.cams    = src;
                s_play.editGen = s_editGen.load();
                s_play.follow = a_scene ? false : s_follow;
                s_play.loop   = a_scene ? true : s_loop;
                s_play.scene  = a_scene != nullptr;
            }
            if (s_rec.on) s_rec.on = false;
            SceneCam::StandDown("camera path plays");
            PreparePlay();
            const int n = Count();
            const int from = std::clamp(a_from, 0, n - 1);
            s_play.k       = from;
            for (int k = 0; k < from; ++k) s_play.elapsed += s_play.cams[k].wait + MoveTime(k);
            if (a_rate < 0.0f) {
                s_play.t        = s_play.cams[from].wait;
                s_play.elapsed += s_play.t;
                s_play.rate     = a_rate;
            }
            s_play.fovCarry = s_play.cams[from].fov;
            s_play.fromPos  = a_fcs->translation;
            s_play.fromFwd  = Forward(a_fcs->rotation.y, WrapPi(a_fcs->rotation.x));
            s_play.fromRoll = FreeCam::GetRollRadians() + FreeCam::GetExtraRoll();
            s_play.fromFov  = GetFOV();
            s_play.outYaw   = a_fcs->rotation.y;
            s_play.gliding  = a_glide;
            if (!a_glide) {  // the camera is already there: carry on from it
                s_play.haveOut = true;
                s_play.outPos  = s_play.fromPos;
                s_play.outFwd  = s_play.fromFwd;
                s_play.outRoll = s_play.fromRoll;
                s_play.outFov  = s_play.fromFov;
            }
            s_play.on = true;
            std::string pivots;
            for (const auto& p : s_play.pivots) {
                pivots += (pivots.empty() ? "" : ", ") + (p.have ? p.name : std::string(p.kind < 0 ? "none" : "not found"));
            }
            Event(std::format("camera path {} ({}) - {} cameras, {:.0f} s{}{}, pivot {}{}",
                a_rate < 0.0f ? (a_rate < -1.5f ? "rewinds" : "plays backwards") : "plays", a_source, n,
                s_play.total, from > 0 ? std::format(", from camera {}", from + 1) : "", s_play.loop ? " (loop)" : "", pivots,
                s_play.follow ? ", following" : ""));
            Publish();
            return true;
        }

        // Play (a_rate < 0: backwards) from camera a_from: right away in TFCam's free camera, else once it has started.
        void Begin(int a_from, float a_rate, std::string_view a_source) {
            if (auto* fcs = CurrentFreeCam()) {
                if (s_play.on) EndPlay(a_source);
                StartPlay(fcs, a_source, a_from, true, a_rate);
                return;
            }
            s_pendingPlay = true;
            s_pendingFrom = a_from;
            s_pendingRate = a_rate;
            s_pendingUntil  = Now() + kPendingTimeout;
            Event(std::format("play ({}): entering TFCam free-fly first", a_source));
            SlccBridge::RequestEnterFreeFly("Camera path");
        }

        // ---- 11-14 round 13: SexLab scene paths (see CamPath.h) -----------------------------------------------------------
        // The scene's own spot = the player's placement in it, as the Scene Camera's stage shots use: P+ places every actor
        // rigidly from the scene's base for each stage, so cameras kept against it frame the same wherever the stage plays
        // (anchored on the bones they would shake with the animation). Main thread.
        struct SceneNow {
            bool                    in = false;  // a stage of the player's scene is known
            std::string             key;         // scene id | stage id | the player's position
            std::string             label;
            RE::NiPoint3            anchor{};
            float                   heading = 0.0f;
            std::vector<RE::FormID> actorIDs;    // the scene's positions in order
        };
        SceneNow ReadScene() {
            SceneNow    s;
            const auto& sc     = SceneTracker::GetPlayerScene();
            auto*       player = RE::PlayerCharacter::GetSingleton();
            if (!sc.inScene || !sc.stageKnown || !player || sc.sceneId.empty()) return s;
            s.in       = true;
            s.key      = std::format("{}|{}|{}", sc.sceneId, sc.stageId, sc.playerSlot);
            s.label    = std::format("{} - stage {}", sc.sceneName.empty() ? sc.sceneId : sc.sceneName, sc.stageId);
            s.anchor   = player->GetPosition();
            s.heading  = player->GetAngleZ();
            s.actorIDs = sc.actorIDs;
            return s;
        }

        std::filesystem::path ScenePathsDir() {
            wchar_t     buf[MAX_PATH] = {};
            const DWORD len = ::GetFullPathNameW(L"Data\\SKSE\\Plugins\\TFCam\\ScenePaths", MAX_PATH, buf, nullptr);
            return len > 0 && len < MAX_PATH ? std::filesystem::path(std::wstring(buf, len)) : std::filesystem::path(L"Data\\SKSE\\Plugins\\TFCam\\ScenePaths");
        }
        std::filesystem::path SceneFile(const std::string& a_key) {
            std::string f;
            for (const char c : a_key) f.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ? c : '_');
            if (f.size() > 150) f.resize(150);
            return ScenePathsDir() / (f + ".tsv");
        }

        // a stage has a saved scene path: checked again only when the stage changes (or after a save / delete)
        std::string s_savedCheckKey;
        bool        s_savedCheck = false;
        bool SceneSaved(const std::string& a_key) {
            if (a_key.empty()) return false;
            if (a_key != s_savedCheckKey) {
                s_savedCheckKey = a_key;
                std::error_code ec;
                s_savedCheck = std::filesystem::exists(SceneFile(a_key), ec);
            }
            return s_savedCheck;
        }
        void PublishScene(const SceneNow& a_s) {
            const bool saved = SceneSaved(a_s.key);
            std::scoped_lock lock(s_lock);
            s_sceneIn    = a_s.in;
            s_sceneLabel = a_s.label;
            s_sceneSaved = saved;
        }

        // world -> against the scene's spot (positions, headings), the partners as scene positions, no area
        std::vector<Cam> ToScene(const std::vector<Cam>& a_cams, const SceneNow& a_s, int& a_positions) {
            std::vector<Cam> out    = a_cams;
            auto*            player = RE::PlayerCharacter::GetSingleton();
            for (auto& c : out) {
                c.pos = RotZ(c.pos - a_s.anchor, -a_s.heading);
                c.yaw = WrapPi(c.yaw - a_s.heading);
                if (c.havePivot) {
                    c.pivotPos     = RotZ(c.pivotPos - a_s.anchor, -a_s.heading);
                    c.pivotHeading = WrapPi(c.pivotHeading - a_s.heading);
                }
                c.space.clear();
                if (c.pivotKind == 1 && c.pivotRef) {
                    if (player && c.pivotRef == player->GetFormID()) {
                        c.pivotKind = 0;
                        c.pivotRef  = 0;
                    } else {
                        for (std::size_t i = 0; i < a_s.actorIDs.size(); ++i) {
                            if (a_s.actorIDs[i] != c.pivotRef) continue;
                            c.pivotKind = 2;
                            c.pivotRef  = static_cast<RE::FormID>(i);
                            ++a_positions;
                            break;
                        }
                    }
                }
            }
            return out;
        }
        // ... and back round where the scene is now
        std::vector<Cam> FromScene(const std::vector<Cam>& a_cams, const SceneNow& a_s) {
            std::vector<Cam> out   = a_cams;
            const auto       space = SpaceHere();
            for (auto& c : out) {
                c.pos = a_s.anchor + RotZ(c.pos, a_s.heading);
                c.yaw = WrapPi(c.yaw + a_s.heading);
                if (c.havePivot) {
                    c.pivotPos     = a_s.anchor + RotZ(c.pivotPos, a_s.heading);
                    c.pivotHeading = WrapPi(c.pivotHeading + a_s.heading);
                }
                c.space = space;
            }
            return out;
        }

        bool PlayScene(RE::FreeCameraState* a_fcs, const SceneNow& a_s, std::string_view a_why) {
            std::vector<Cam> cams;
            bool             follow = false, loop = false;
            if (!a_s.in || !ReadFile(SceneFile(a_s.key), cams, follow, loop) || cams.size() < 2) {
                Event(std::format("scene path ({}): none saved for {}", a_why, a_s.in ? a_s.label : std::string("- no scene stage known")));
                return false;
            }
            const auto world = FromScene(cams, a_s);
            if (s_play.on) EndPlay(a_why);
            if (!StartPlay(a_fcs, a_why, 0, true, 1.0f, &world)) return false;
            Event(std::format("scene path plays for {} - {} cameras, looping until the stage changes", a_s.label, world.size()));
            Notify("Scene path: " + a_s.label);
            return true;
        }

        std::string s_sceneSeenKey;  // the stage Drive saw last
        std::string s_sceneWant;     // a stage whose saved scene path starts once TFCam's free camera drives

        // Drive, every frame: a new stage (or the scene's end) ends a playing scene path, and the new stage's own saved
        // path starts (Scene autoplay) - also later, once the free camera is on, while that stage still runs
        void ServiceScene(RE::FreeCameraState* a_fcs, bool a_driving) {
            const auto sc = ReadScene();
            if (sc.key != s_sceneSeenKey) {
                s_sceneSeenKey = sc.key;
                if (s_play.on && s_play.scene) EndPlay(sc.in ? "the stage changed" : "the scene ended");
                s_sceneWant = sc.in && s_settings.sceneAutoPlay && SceneSaved(sc.key) ? sc.key : std::string();
                PublishScene(sc);
            }
            if (!s_sceneWant.empty() && s_sceneWant == sc.key && a_driving && !s_play.on && !s_pendingPlay && !s_rec.on) {
                s_sceneWant.clear();
                PlayScene(a_fcs, sc, "this stage's scene path");
            }
        }

        // The path slider's work: to a_seconds into the path, paused. Edited since Play: it starts again with the
        // cameras as they are now (the playing copy is the old path).
        bool SeekNow(float a_seconds) {
            auto* fcs = CurrentFreeCam();
            if (!fcs) {
                Notify("Camera path: turn the free camera on to move along the path");
                return false;
            }
            if (s_play.on && s_play.editGen != s_editGen.load()) EndPlay("the path was edited - the slider starts it again");
            if (!s_play.on && !StartPlay(fcs, "the path slider", 0, false, 1.0f)) return false;
            s_play.gliding = false;
            s_play.paused  = true;
            s_play.rate    = 1.0f;
            SeekTo(a_seconds);
            Publish();
            return true;
        }

        void Toggle(int a_state, std::string_view a_source) {
            const bool on = a_state < 0 ? !(s_play.on && !s_play.paused) && !s_pendingPlay : a_state != 0;
            if (!on) {
                if (a_state < 0 && s_play.on) {  // play / pause key: pause
                    s_play.paused = true;
                    Event(std::format("paused ({})", a_source));
                    Publish();
                    return;
                }
                s_pendingPlay = false;
                EndPlay(a_source);
                return;
            }
            if (s_play.on) {
                // the play / pause key goes on the way it was going; the Play button always forwards
                if (s_play.paused || (a_state > 0 && s_play.rate < 0.0f)) {
                    // edited while paused (10-31: the edit only showed after a restart): go on from the same moment
                    // with the path as it is now
                    if (s_play.paused && s_play.editGen != s_editGen.load()) {
                        const float rate = s_play.rate;
                        if (!SeekNow(s_play.elapsed)) return;
                        s_play.rate = rate;
                    }
                    s_play.paused = false;
                    if (a_state > 0) s_play.rate = 1.0f;
                    Event(std::format("going on{} ({})", s_play.rate < 0.0f ? " backwards" : "", a_source));
                    Publish();
                }
                return;
            }
            Begin(0, 1.0f, a_source);
        }

        // Backwards to camera 1 at a_rate (< 0): from where the path is, or from its end when it is not playing.
        void Reverse(float a_rate, const char* a_what) {
            if (s_play.on) {
                if (s_play.paused && s_play.editGen != s_editGen.load() && !SeekNow(s_play.elapsed)) return;  // as Toggle
                s_play.rate   = a_rate;
                s_play.paused = false;
                Event(a_what);
                Publish();
                return;
            }
            int n;
            {
                std::scoped_lock lock(s_lock);
                n = static_cast<int>(s_cams.size());
            }
            if (n < 2) {
                Notify("Camera path: add at least 2 cameras first");
                return;
            }
            Begin(n - 1, a_rate, std::format("{} from the end", a_what));
        }

        // Clean screen (all game UI hidden; s_clean is declared with the drawn path, which it hides). Main thread.
        void ApplyClean(bool a_on, std::string_view a_why) {
            if (auto* ui = RE::UI::GetSingleton()) ui->ShowMenus(!a_on);
            Event(a_on ? std::format("clean screen: all game UI hidden ({})", a_why)
                       : std::format("clean screen off - the game UI is back ({})", a_why));
        }

        // ---- editing (main thread) ---------------------------------------------------------------------------------
        Cam CamFromFreeCam(RE::FreeCameraState* a_fcs, int a_kind, RE::FormID a_ref) {
            Cam c;
            c.pos   = a_fcs->translation;
            c.pitch = WrapPi(a_fcs->rotation.x);  // the vanilla free camera keeps its pitch in 0..2 pi
            c.yaw   = WrapPi(a_fcs->rotation.y);
            c.roll  = WrapPi(FreeCam::GetRollRadians() + FreeCam::GetExtraRoll());
            c.fov   = GetFOV();
            c.pivotKind = a_kind < 0 ? -1 : (a_kind == 1 && a_ref ? 1 : 0);
            c.pivotRef  = c.pivotKind == 1 ? a_ref : 0;
            RE::NiPoint3 p{};
            float        h = 0.0f;
            if (c.pivotKind >= 0 && PivotRaw(c.pivotKind, c.pivotRef, p, h, nullptr)) {
                c.pivotPos     = p;
                c.pivotHeading = h;
                c.havePivot    = true;
            }
            c.look   = kLookSet;  // plays back the view you set
            c.move   = kMoveCurve;
            c.time   = 3.0f;
            c.smooth = false;
            c.space  = SpaceHere();  // 11-13: where it was made
            return c;
        }

        // Add camera while the path plays (or is paused): the new camera goes into the path at this moment - the move
        // it was put into is split there, or (at a camera that waits) it takes over the rest of the wait and the move
        // on. Play carries on from it. False = the path was edited since Play; the caller adds at the end.
        bool InsertWhilePlaying(RE::FreeCameraState* a_fcs) {
            const int   k  = s_play.k;
            const auto& ck = s_play.cams[k];
            Cam         fresh = CamFromFreeCam(a_fcs, ck.pivotKind, ck.pivotRef);
            fresh.look         = ck.look;
            fresh.blur         = ck.blur;
            fresh.blurStrength = ck.blurStrength;
            fresh.move         = ck.move;
            fresh.smooth       = ck.smooth;
            fresh.way          = ck.way;
            fresh.zoomTrick    = ck.zoomTrick;
            fresh.wait         = 0.0f;
            const bool moving  = s_play.phase == 1 && Next(k) >= 0;
            int        at      = k + 1;
            {
                std::scoped_lock lock(s_lock);
                if (s_cams.size() != s_play.cams.size()) return false;
                auto& c = s_cams[k];
                if (moving) {
                    const float T         = (std::max)(c.time, 0.1f);
                    const float f         = NormalF(k, std::clamp(s_play.t - WindTime(s_play.cams[k]), 0.0f, CoreTime(k)));
                    const float done      = std::clamp(f * T, 0.0f, T);
                    fresh.elastic = c.elastic;  // the arrival stays with the last part, the wind-up with the first
                    fresh.sling   = 0.0f;
                    c.elastic     = 0.0f;
                    const int   turnsDone = static_cast<int>(std::floor(static_cast<float>(c.turns) * done / T));
                    fresh.time  = (std::max)(T - done, 0.1f);
                    fresh.turns = c.turns - turnsDone;
                    c.time      = (std::max)(done, 0.1f);
                    c.turns     = turnsDone;
                } else {
                    fresh.wait     = (std::max)(c.wait - s_play.t, 0.0f);
                    fresh.time     = c.time;
                    fresh.turns    = c.turns;
                    fresh.bendSide = c.bendSide;
                    fresh.bendUp   = c.bendUp;
                    c.wait         = (std::max)(s_play.t, 0.0f);
                    c.move         = kMoveCut;  // same spot: nothing to travel
                }
                s_cams.insert(s_cams.begin() + at, fresh);
                EventLocked(std::format("camera {} put into the path {} camera {} ({:.0f}, {:.0f}, {:.0f})", at + 1,
                    moving ? "on the way from" : "at", k + 1, fresh.pos.x, fresh.pos.y, fresh.pos.z));
            }
            SaveCurrentSoon();
            Notify(std::format("Camera {} put into the path here", at + 1));
            const bool  paused = s_play.paused;
            const float rate   = s_play.rate;
            if (StartPlay(a_fcs, "a camera went in", at, false, 1.0f)) {
                s_play.paused = paused;
                s_play.rate   = rate;
                Publish();
            }
            return true;
        }

        void AddCam(RE::FreeCameraState* a_fcs) {
            LoadOnce();
            if (s_play.on && !s_play.gliding && InsertWhilePlaying(a_fcs)) return;
            const Cam   c = CamFromFreeCam(a_fcs, -1, 0);  // no pivot until you give it one
            std::size_t n;
            {
                std::scoped_lock lock(s_lock);
                s_cams.push_back(c);
                n = s_cams.size();
                EventLocked(std::format("camera {} added at ({:.0f}, {:.0f}, {:.0f}), FOV {:.0f}", n, c.pos.x, c.pos.y, c.pos.z, c.fov));
            }
            SaveCurrentSoon();
            Notify(std::format("Camera {} added", n));
        }

        void SetCamHere(int a_i, RE::FreeCameraState* a_fcs) {
            int        kind = 0;
            RE::FormID ref  = 0;
            {
                std::scoped_lock lock(s_lock);
                if (a_i < 0 || a_i >= static_cast<int>(s_cams.size())) return;
                kind = s_cams[a_i].pivotKind;  // it keeps its own pivot
                ref  = s_cams[a_i].pivotRef;
            }
            const Cam fresh = CamFromFreeCam(a_fcs, kind, ref);
            {
                std::scoped_lock lock(s_lock);
                if (a_i < 0 || a_i >= static_cast<int>(s_cams.size())) return;
                auto& c        = s_cams[a_i];
                c.pos          = fresh.pos;
                c.yaw          = fresh.yaw;
                c.pitch        = fresh.pitch;
                c.roll         = fresh.roll;
                c.fov          = fresh.fov;
                c.pivotPos     = fresh.pivotPos;
                c.pivotHeading = fresh.pivotHeading;
                c.havePivot    = fresh.havePivot;
                c.space        = fresh.space;  // 11-13: it is here now
                EventLocked(std::format("camera {} moved to the camera's spot", a_i + 1));
            }
            SaveCurrentSoon();
            Notify(std::format("Camera {} moved here", a_i + 1));
        }

        void GoToCam(int a_i, RE::FreeCameraState* a_fcs) {
            Cam        c;
            bool       follow;
            int        kind;
            RE::FormID ref;
            {
                std::scoped_lock lock(s_lock);
                if (a_i < 0 || a_i >= static_cast<int>(s_cams.size())) return;
                c      = s_cams[a_i];
                follow = s_follow;
                kind   = c.pivotKind;
                ref    = c.pivotRef;
            }
            {   // 11-13 (user: "the saved position was in another cell ... screen went black"): no flight to another place
                std::string where;
                if (CamAway(c, SpaceHere(), where)) {
                    Event(std::format("camera {} is in {} - go there first (the camera stays here)", a_i + 1, where));
                    Notify(std::format("Camera path: go to {} first", where));
                    return;
                }
            }
            if (s_play.on) EndPlay("go to a camera");
            RE::NiPoint3 pos = c.pos;
            float        yaw = c.yaw;
            RE::NiPoint3 p{};
            float        h = 0.0f;
            if (follow && c.havePivot && PivotRaw(kind, ref, p, h, nullptr)) {
                pos = p + RotZ(RotZ(c.pos - c.pivotPos, -c.pivotHeading), h);
                yaw = c.yaw - c.pivotHeading + h;
            }
            a_fcs->translation = pos;
            a_fcs->rotation.x  = std::clamp(c.pitch, -kPitchLimit, kPitchLimit);
            a_fcs->rotation.y  = WrapPi(yaw);
            SetFOV(c.fov);
            FreeCam::SetRollRadians(c.roll);
            FreeCam::SetExtraRoll(0.0f);
            Event(std::format("the camera went to camera {}", a_i + 1));
        }

        // What the free camera points at: an actor within a few degrees of the middle of the screen (nearest to the
        // aim line), else the first object a ray from the camera hits.
        RE::NiPointer<RE::TESObjectREFR> PickAhead(RE::FreeCameraState* a_fcs) {
            const RE::NiPoint3 from = a_fcs->translation;
            const RE::NiPoint3 dir  = Forward(a_fcs->rotation.y, WrapPi(a_fcs->rotation.x));
            RE::NiPointer<RE::TESObjectREFR> best;
            float bestScore = 0.06f;  // radians off the aim line
            auto consider = [&](RE::Actor* a_actor) {
                if (!a_actor || !a_actor->Get3D()) return;
                RE::NiPoint3 p{};
                if (!CamFocus::RefPoint(a_actor, p)) return;
                const RE::NiPoint3 d    = p - from;
                const float        dist = Length(d);
                if (dist < 1.0f || dist > 6000.0f) return;
                const float off = std::acos(std::clamp(Dot(d * (1.0f / dist), dir), -1.0f, 1.0f));
                if (off < bestScore) {
                    bestScore = off;
                    best      = RE::NiPointer<RE::TESObjectREFR>(a_actor);
                }
            };
            if (auto* lists = RE::ProcessLists::GetSingleton()) {
                for (auto& h : lists->highActorHandles) {
                    if (auto a = h.get()) consider(a.get());
                }
            }
            consider(RE::PlayerCharacter::GetSingleton());
            if (best) return best;
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell   = player ? player->GetParentCell() : nullptr;
            auto* world  = cell ? cell->GetbhkWorld() : nullptr;
            if (!world) return nullptr;
            const float     scale = RE::bhkWorld::GetWorldScale();
            RE::bhkPickData pick{};
            pick.rayInput.from                        = RE::hkVector4(from * scale);
            pick.rayInput.to                          = RE::hkVector4((from + dir * 6000.0f) * scale);
            pick.rayInput.enableShapeCollectionFilter = true;
            pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
            world->PickObject(pick);
            if (!pick.rayOutput.HasHit() || !pick.rayOutput.rootCollidable) return nullptr;
            auto* ref = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
            return ref ? RE::NiPointer<RE::TESObjectREFR>(ref) : nullptr;
        }

        // Camera a_cam's pivot (a_kind -1 = none). It measures from where it is now (what Follow keeps).
        void SetPivot(int a_kind, RE::NiPointer<RE::TESObjectREFR> a_ref, std::string_view a_how, int a_cam) {
            auto*       player = RE::PlayerCharacter::GetSingleton();
            int         kind   = a_kind < 0 ? -1 : 0;
            RE::FormID  id     = 0;
            std::string name   = kind < 0 ? "none" : "you";
            if (a_kind == 1 && a_ref && a_ref.get() != player) {
                kind = 1;
                id   = a_ref->GetFormID();
                name = CamFocus::Describe(a_ref.get());
            }
            RE::NiPoint3 p{};
            float        h    = 0.0f;
            const bool   have = kind >= 0 && PivotRaw(kind, id, p, h, nullptr);
            {
                std::scoped_lock lock(s_lock);
                if (a_cam < 0 || a_cam >= static_cast<int>(s_cams.size())) return;
                if (kind == 1) s_refNames[id] = name;
                auto& c     = s_cams[a_cam];
                c.pivotKind = kind;
                c.pivotRef  = id;
                c.havePivot = have;
                if (have) {
                    c.pivotPos     = p;
                    c.pivotHeading = h;
                }
                EventLocked(std::format("pivot for camera {}: {} ({})", a_cam + 1, name, a_how));
            }
            SaveCurrentSoon();
            Notify(std::format("Pivot for camera {}: {}", a_cam + 1, name));
        }

        // ---- record manual path ------------------------------------------------------------------------------------
        void RecordFrame(RE::FreeCameraState* a_fcs, float a_dt) {
            const RE::NiPoint3 pos   = a_fcs->translation;
            const float        yaw   = WrapPi(a_fcs->rotation.y);
            const float        pitch = WrapPi(a_fcs->rotation.x);
            s_rec.sinceLast += a_dt;
            if (s_rec.sinceLast < kRecordStep) return;
            const bool moved = Length(pos - s_rec.lastPos) > kRecordMove || std::fabs(WrapPi(yaw - s_rec.lastYaw)) > kRecordTurn ||
                               std::fabs(pitch - s_rec.lastPitch) > kRecordTurn;
            std::scoped_lock lock(s_lock);
            if (s_cams.empty()) return;
            if (!moved) {  // standing still: the last camera waits
                s_cams.back().wait += s_rec.sinceLast;
                s_rec.sinceLast = 0.0f;
                return;
            }
            s_cams.back().time = s_rec.sinceLast;
            Cam c   = s_cams.back();
            c.pos   = pos;
            c.yaw   = yaw;
            c.pitch = pitch;
            c.roll  = WrapPi(FreeCam::GetRollRadians() + FreeCam::GetExtraRoll());
            c.fov   = GetFOV();
            c.wait  = 0.0f;
            s_cams.push_back(c);
            s_rec.sinceLast = 0.0f;
            s_rec.lastPos   = pos;
            s_rec.lastYaw   = yaw;
            s_rec.lastPitch = pitch;
        }

        void StartRecord(RE::FreeCameraState* a_fcs) {
            LoadOnce();
            if (s_play.on) EndPlay("recording");
            {
                std::scoped_lock lock(s_lock);
                if (!s_cams.empty()) WriteLocked("Before recording");  // the old path is kept
            }
            const Cam first = CamFromFreeCam(a_fcs, -1, 0);
            {
                std::scoped_lock lock(s_lock);
                s_cams.clear();
                s_cams.push_back(first);
                RefreshSavedLocked();
                EventLocked("recording a manual path - fly the camera, then stop the recording");
            }
            s_rec           = Record{};
            s_rec.on        = true;
            s_rec.lastPos   = first.pos;
            s_rec.lastYaw   = first.yaw;
            s_rec.lastPitch = first.pitch;
            Notify("Recording - fly the camera. Press record again to stop.");
            Publish();
        }

        void StopRecord(std::string_view a_why) {
            if (!s_rec.on) return;
            s_rec.on = false;
            std::size_t n;
            {
                std::scoped_lock lock(s_lock);
                n = s_cams.size();
                EventLocked(std::format("recording stopped ({}) - {} camera(s)", a_why, n));
            }
            SaveCurrentSoon();
            Notify(std::format("Recorded {} cameras", n));
            Publish();
        }
    }

    Settings& Get() { return s_settings; }

    void Clamp() {
        auto& s = s_settings;
        if (!std::isfinite(s.smoothness)) s.smoothness = 0.3f;
        if (!std::isfinite(s.aimHeight)) s.aimHeight = 0.0f;
        s.smoothness = std::clamp(s.smoothness, 0.0f, 1.0f);
        s.aimHeight  = std::clamp(s.aimHeight, -60.0f, 60.0f);
        for (int* k : { &s.openKey, &s.addKey, &s.playKey, &s.recordKey }) {
            if (*k < 0 || *k > 0xFF) *k = 0;
        }
    }

    void Drive(RE::FreeCameraState* a_fcs, bool a_driving) {
        const float dt = FrameDt();
        if (!a_fcs) return;
        {
            std::scoped_lock lock(s_lock);
            s_pubFreeCam = a_driving;
        }
        if (s_pendingPlay) {
            if (a_driving) {
                s_pendingPlay = false;
                StartPlay(a_fcs, "after entering free-fly", s_pendingFrom, true, s_pendingRate);
            } else if (Now() > s_pendingUntil) {
                s_pendingPlay = false;
                Event("play: TFCam free-fly did not start in time");
            }
        }
        ServiceScene(a_fcs, a_driving);   // 11-14 round 13: SexLab scene paths
        if (s_rec.on) {
            if (!a_driving) {
                StopRecord("another camera took the free camera");
            } else {
                RecordFrame(a_fcs, dt);
            }
        }
        // clean screen: the window steps aside while the path plays, and comes back when it pauses / ends
        CamPathWindow::AutoHide(s_clean && s_play.on && !s_play.paused);
        UpdateView(a_driving);
        // 0.18.0: "start and stop with Camera Path playback" - once the path really runs (after the glide to camera 1)
        if (s_play.on && !s_play.paused && !s_play.gliding && !s_videoByPath && VideoCapture::Get().withPath && !VideoCapture::Recording()) {
            VideoCapture::RequestStart("the camera path plays");
            s_videoByPath = true;
        }
        if (!s_play.on) return;
        if (!a_driving) {
            EndPlay("another camera (FCFW timeline / SLCC) took the free camera");
            return;
        }
        UpdatePivots(dt, false);
        Step step = kGoing;
        if (s_play.gliding) {
            s_play.glideT += s_play.paused ? 0.0f : dt;
        } else if (!s_play.paused) {
            step = Advance(dt * s_play.rate);
        }
        const Target tg = Evaluate();
        if (!s_play.paused) {
            s_play.shakeT += dt;
            if (!s_play.gliding) WorldSpeed(tg.world);
        }
        if (s_play.gliding) {
            const float w  = SmoothStep(s_play.glideT / kStartGlide);
            s_play.outPos  = Mix(s_play.fromPos, tg.pos, w);
            s_play.outFwd  = SlerpDir(s_play.fromFwd, tg.fwd, w);
            s_play.outRoll = s_play.fromRoll + WrapPi(tg.roll - s_play.fromRoll) * w;
            s_play.outFov  = s_play.fromFov + (tg.fov - s_play.fromFov) * w;
            s_play.blur    = tg.blur * w;
            s_play.haveOut = true;
            if (s_play.glideT >= kStartGlide) s_play.gliding = false;
        } else {
            // motion smoothness: the camera chases the path, taking the edge off sudden turns and speed changes
            if (!s_play.haveOut || s_settings.smoothness <= 0.001f || s_play.snapOut) {  // a cut jumps
                s_play.outPos  = tg.pos;
                s_play.outFwd  = tg.fwd;
                s_play.outRoll = tg.roll;
                s_play.outFov  = tg.fov;
            } else {
                // still chased while paused: the camera settles onto the paused spot (a rewind stops ON camera 1)
                float w = ExpWeight(dt, s_settings.smoothness * 0.5f);
                // A slingshot / elastic move rides its path exactly: the chase's lag (speed x ~0.2 s) would swallow the
                // bounce. Faded out over the move's first 0.3 s, so a lag left from before closes without a jump.
                const auto& cm = s_play.cams[s_play.k];
                if (s_play.phase == 1 && (cm.elastic > 0.001f || cm.sling > 0.001f)) w = (std::max)(w, Clamp01(s_play.t / 0.3f));
                s_play.outPos  = Mix(s_play.outPos, tg.pos, w);
                s_play.outFwd  = SlerpDir(s_play.outFwd, tg.fwd, w);
                s_play.outRoll += WrapPi(tg.roll - s_play.outRoll) * w;
                s_play.outFov = tg.fovDirect ? tg.fov : s_play.outFov + (tg.fov - s_play.outFov) * w;
            }
            s_play.blur    = tg.blur;
            s_play.haveOut = true;
        }
        s_play.snapOut = false;
        WritePose(a_fcs, tg.shake, s_play.gliding ? RE::NiPoint3{} : tg.swing);
        if (s_play.blur > 0.01f && tg.haveFocus) {
            CamBlur::External(true, tg.focus, s_play.blur);
            s_play.blurOn = true;
        } else {
            BlurOff();
        }
        Publish();
        if (step == kAtEnd) {
            EndPlay("the path ended");
        } else if (step == kAtStart) {
            s_play.paused = true;
            s_play.rate   = 1.0f;
            Event("back at camera 1 - paused (Play goes on from here)");
            Notify("Back at camera 1");
            Publish();
        }
    }

    bool OwnsMotion() { return s_play.on; }
    bool Playing() { return s_play.on; }

    void Stop(std::string_view a_why) {
        s_pendingPlay = false;
        EndPlay(a_why);
    }

    void StopForEditorClose(std::string_view a_how) {
        s_pendingPlay = false;
        if (s_play.on && !s_play.scene) EndPlay(std::format("the editor was closed ({})", a_how));
    }

    void ToggleEditor() {
        if (s_settings.prismaEditor && CamPathPrisma::Available()) {
            CamPathPrisma::Toggle();
        } else {
            CamPathWindow::Toggle();
        }
    }

    bool OnHotkey(std::uint32_t a_code) {
        if (a_code == 0) return false;
        auto is = [&](int a_key) { return a_key != 0 && a_code == static_cast<std::uint32_t>(a_key); };
        if (is(s_settings.openKey)) {
            ToggleEditor();
            return true;
        }
        if (is(s_settings.addKey)) {
            if (auto* fcs = CurrentFreeCam()) {
                AddCam(fcs);
            } else {
                Notify("Camera path: turn the free camera on to add cameras");
            }
            return true;
        }
        if (is(s_settings.playKey)) {
            Toggle(-1, "play key");
            return true;
        }
        if (is(VideoCapture::Get().key)) {  // 0.18.0
            VideoCapture::Toggle("video key");
            return true;
        }
        if (is(s_settings.recordKey)) {
            if (s_rec.on) {
                StopRecord("record key");
            } else if (auto* fcs = CurrentFreeCam()) {
                StartRecord(fcs);
            } else {
                Notify("Camera path: turn the free camera on to record");
            }
            return true;
        }
        return false;
    }

    void OnFreeCamEnd() {
        CamPathView::Clear();
        if (s_rec.on) StopRecord("the free camera ended");
        {
            std::scoped_lock lock(s_lock);
            s_pubFreeCam = false;
        }
        if (s_play.on) {
            FreeCam::SetExtraRoll(0.0f);
            BlurOff();
            s_play.on = false;
            Event("camera path stopped (the free camera ended)");
            Publish();
        }
        WorldSpeed(1.0f);
        if (s_clean.exchange(false)) ApplyClean(false, "the free camera ended");
        CamPathWindow::AutoHide(false);
    }

    void OnGameLoad() {
        s_pendingPlay = false;
        s_play.on     = false;
        s_rec.on      = false;
        s_worldSlowed = false;  // a load sets the world's time itself
        if (s_clean.exchange(false)) ApplyClean(false, "a game load");
        {
            std::scoped_lock lock(s_lock);
            s_refNames.clear();  // a reference may be here now, or gone
        }
        LoadOnce();
        Publish();
    }

    bool FocusPoint(RE::NiPoint3&, const char*&, bool& a_on) {
        a_on = false;  // a playing path drives the blur itself (CamBlur::External)
        return false;
    }

    // ---- window / menu ------------------------------------------------------------------------------------------------
    Snapshot GetSnapshot() {
        LoadOnce();
        Snapshot         s;
        std::scoped_lock lock(s_lock);
        s.cams       = s_cams;
        s.name       = s_name;
        bool missing = false;
        s.follow     = s_follow;
        s.loop       = s_loop;
        s.playing    = s_pubPlaying;
        s.paused     = s_pubPaused;
        s.rewinding  = s_pubRewinding;
        s.rate       = s_pubRate;
        s.cleanScreen = s_clean;
        s.recording  = s_pubRecording;
        s.freeCam    = s_pubFreeCam;
        s.frozen     = FreezeTime::IsFrozen();
        s.atCam      = s_pubAt;
        s.progress   = s_pubProgress;
        s.lastEvent  = s_lastEvent;
        s.savedPaths = s_savedPaths;
        s.sceneIn      = s_sceneIn;      // 11-14 round 13
        s.sceneLabel   = s_sceneLabel;
        s.sceneSaved   = s_sceneSaved;
        s.scenePlaying = s_pubScene;
        const int nc = static_cast<int>(s_cams.size());
        for (int k = 0; k < nc; ++k) {
            s.total += s_cams[k].wait;
            if (k + 1 < nc || (s_loop && nc > 1)) s.total += MoveSeconds(s_cams[k]);
        }
        for (const auto& c : s_cams) {
            if (c.pivotKind == 2) {   // 11-14 round 13
                s.camPivotNames.push_back(std::format("scene position {}", c.pivotRef + 1));
                continue;
            }
            if (c.pivotKind != 1 || !c.pivotRef) {
                s.camPivotNames.emplace_back(c.pivotKind < 0 ? "none" : "you");
                continue;
            }
            const auto it = s_refNames.find(c.pivotRef);
            if (it != s_refNames.end()) {
                s.camPivotNames.push_back(it->second);
            } else {
                s.camPivotNames.emplace_back("...");
                missing = true;
            }
        }
        if (missing) ResolveNamesSoon();
        return s;
    }

    bool EditCam(int a_index, const Cam& a_o) {
        {
            std::scoped_lock lock(s_lock);
            if (a_index < 0 || a_index >= static_cast<int>(s_cams.size())) return false;
            auto& c        = s_cams[a_index];
            c.look         = std::clamp(a_o.look, 0, 2);
            c.wait         = std::clamp(std::isfinite(a_o.wait) ? a_o.wait : 0.0f, 0.0f, 600.0f);
            c.blur         = a_o.blur;
            c.blurStrength = std::clamp(std::isfinite(a_o.blurStrength) ? a_o.blurStrength : 0.6f, 0.0f, 1.0f);
            c.move         = std::clamp(a_o.move, 0, 3);
            c.time         = std::clamp(std::isfinite(a_o.time) ? a_o.time : 3.0f, 0.1f, 600.0f);
            c.smooth       = a_o.smooth;
            c.turns        = std::clamp(a_o.turns, 0, 20);
            c.way          = std::clamp(a_o.way, 0, 2);
            c.zoomTrick    = a_o.zoomTrick;
            c.dolly        = std::clamp(std::isfinite(a_o.dolly) ? a_o.dolly : 0.0f, -1.0f, 1.0f);
            c.bendSide     = std::clamp(std::isfinite(a_o.bendSide) ? a_o.bendSide : 0.0f, -1.0f, 1.0f);
            c.bendUp       = std::clamp(std::isfinite(a_o.bendUp) ? a_o.bendUp : 0.0f, -1.0f, 1.0f);
            c.fov          = std::clamp(std::isfinite(a_o.fov) ? a_o.fov : 70.0f, 5.0f, 150.0f);
            c.bone         = std::clamp(a_o.bone, 0, kBoneCount - 1);
            c.shake        = std::clamp(std::isfinite(a_o.shake) ? a_o.shake : 0.0f, 0.0f, 1.0f);
            c.crashZoom    = a_o.crashZoom;
            c.slowMo       = a_o.slowMo;
            c.slowAt       = std::clamp(std::isfinite(a_o.slowAt) ? a_o.slowAt : 0.66f, 0.0f, 1.0f);
            c.slowSpeed    = std::clamp(std::isfinite(a_o.slowSpeed) ? a_o.slowSpeed : 0.2f, 0.05f, 1.0f);
            c.slowLength   = std::clamp(std::isfinite(a_o.slowLength) ? a_o.slowLength : 0.3f, 0.05f, 1.0f);
            c.slowWorld    = a_o.slowWorld;
            c.slowCamera   = a_o.slowCamera;
            c.elastic      = std::clamp(std::isfinite(a_o.elastic) ? a_o.elastic : 0.0f, 0.0f, 1.0f);
            c.sling        = std::clamp(std::isfinite(a_o.sling) ? a_o.sling : 0.0f, 0.0f, 1.0f);
            c.curveOn      = a_o.curveOn;
            c.curveX1      = std::clamp(std::isfinite(a_o.curveX1) ? a_o.curveX1 : 0.42f, 0.0f, 1.0f);
            c.curveY1      = std::clamp(std::isfinite(a_o.curveY1) ? a_o.curveY1 : 0.0f, 0.0f, 1.0f);
            c.curveX2      = std::clamp(std::isfinite(a_o.curveX2) ? a_o.curveX2 : 0.58f, 0.0f, 1.0f);
            c.curveY2      = std::clamp(std::isfinite(a_o.curveY2) ? a_o.curveY2 : 1.0f, 0.0f, 1.0f);
        }
        SaveCurrentSoon();
        return true;
    }

    void SetFollow(bool a_on) {
        {
            std::scoped_lock lock(s_lock);
            s_follow = a_on;
        }
        SaveCurrentSoon();
    }

    void SetLoop(bool a_on) {
        {
            std::scoped_lock lock(s_lock);
            s_loop = a_on;
        }
        SaveCurrentSoon();
    }

    void RequestAddCam() {
        Queue([] {
            if (auto* fcs = CurrentFreeCam()) {
                AddCam(fcs);
            } else {
                Notify("Camera path: turn the free camera on to add cameras");
            }
        });
    }

    void RequestSetCamHere(int a_index) {
        Queue([a_index] {
            if (auto* fcs = CurrentFreeCam()) SetCamHere(a_index, fcs);
        });
    }

    void RequestGoToCam(int a_index) {
        Queue([a_index] {
            if (auto* fcs = CurrentFreeCam()) GoToCam(a_index, fcs);
        });
    }

    void RequestDeleteCam(int a_index) {
        Queue([a_index] {
            if (s_play.on) EndPlay("a camera was deleted");
            {
                std::scoped_lock lock(s_lock);
                if (a_index < 0 || a_index >= static_cast<int>(s_cams.size())) return;
                s_cams.erase(s_cams.begin() + a_index);
                EventLocked(std::format("camera {} deleted", a_index + 1));
            }
            SaveCurrentSoon();
        });
    }

    void RequestClear() {
        Queue([] {
            if (s_play.on) EndPlay("the path was cleared");
            {
                std::scoped_lock lock(s_lock);
                if (!s_cams.empty()) WriteLocked("Before clearing");
                s_cams.clear();
                RefreshSavedLocked();
                EventLocked("all cameras cleared (the old path is saved as \"Before clearing\")");
            }
            SaveCurrentSoon();
        });
    }

    void RequestPlay(int a_state) {
        Queue([a_state] { Toggle(a_state, "window"); });
    }

    void RequestPlayFrom(int a_index) {
        Queue([a_index] { Begin(a_index, 1.0f, std::format("play from camera {}", a_index + 1)); });
    }

    void RequestRewind() {
        Queue([] { Reverse(-kRewindRate, "rewinding"); });
    }

    void RequestPlayBackwards() {
        Queue([] { Reverse(-1.0f, "playing backwards"); });
    }

    void SetCleanScreen(bool a_on) {
        if (s_clean.exchange(a_on) == a_on) return;
        Queue([a_on] { ApplyClean(a_on, "window"); });
    }

    bool CleanScreen() { return s_clean; }

    const char* BoneName(int a_bone) { return kBoneNames[std::clamp(a_bone, 0, kBoneCount - 1)]; }

    void RequestSeek(float a_seconds) {
        Queue([a_seconds] { SeekNow(a_seconds); });
    }

    void RequestSlowHere(int a_cam) {
        Queue([a_cam] {
            const int k = s_play.k;
            if (!s_play.on || !s_play.paused || s_play.phase != 1 || k != a_cam || Next(k) < 0 || s_play.cams[k].move == kMoveCut) {
                Event("slow down here: pause the path during a move first");
                return;
            }
            const float tc = (std::max)(s_play.t - WindTime(s_play.cams[k]), 0.0f);  // seconds into the move itself
            const float f  = tc >= CoreTime(k) ? 1.0f : NormalF(k, tc);
            if (f > 0.95f) {
                Event("slow down here: too close to the next camera - pause a little earlier");
                return;
            }
            const float at = s_play.elapsed - tc + f * s_play.cams[k].time;  // this moment, once the slow down starts here
            {
                std::scoped_lock lock(s_lock);
                if (s_cams.size() != s_play.cams.size()) {
                    EventLocked("slow down here: cameras were added or deleted - play the path again first");
                    return;
                }
                // A dip that starts here. On screen it lasts time x length / sqrt(speed) (the dip's average); a camera
                // that already slows keeps how long and how slow, else 1.5 s at 30%.
                auto&       c     = s_cams[k];
                const bool  had   = c.slowMo && c.slowCamera;
                const float speed = had ? c.slowSpeed : 0.3f;
                const float secs  = had ? c.time * c.slowLength / std::sqrt(speed) : 1.5f;
                const float len   = std::clamp(secs * std::sqrt(speed) / (std::max)(c.time, 0.1f), 0.05f, (std::max)(1.0f - f, 0.05f));
                c.slowMo     = true;
                c.slowCamera = true;
                c.slowWorld  = false;
                c.slowSpeed  = speed;
                c.slowLength = len;
                c.slowAt     = std::clamp(f + 0.5f * len, 0.0f, 1.0f);
            }
            SaveCurrentSoon();
            if (SeekNow(at)) Event(std::format("camera {} slows down from here - set how long and how slow", k + 1));
        });
    }

    void RequestPause() {
        Queue([] {
            if (!s_play.on) return;
            s_play.paused = !s_play.paused;
            Event(s_play.paused ? "paused" : "going on");
            Publish();
        });
    }

    void RequestPickPivot(int a_cam) {
        Queue([a_cam] {
            auto* fcs = CurrentFreeCam();
            if (!fcs) {
                Notify("Camera path: turn the free camera on and point it at something");
                return;
            }
            auto ref = PickAhead(fcs);
            if (!ref) {
                Notify("Camera path: nothing there to pick - point the middle of the screen at it");
                Event("pick pivot: nothing under the middle of the screen");
                return;
            }
            SetPivot(1, ref, "picked with the camera", a_cam);
        });
    }

    void RequestPivotConsole(int a_cam) {
        Queue([a_cam] {
            auto ref = CamFocus::ConsoleRef();
            if (!ref) {
                Notify("Camera path: nothing selected in the console");
                Event("pivot: nothing was clicked in the console");
                return;
            }
            SetPivot(1, ref, "the console's selection", a_cam);
        });
    }

    void RequestPivotMe(int a_cam) {
        Queue([a_cam] { SetPivot(0, nullptr, "you", a_cam); });
    }

    void RequestPivotNone(int a_cam) {
        Queue([a_cam] { SetPivot(-1, nullptr, "cleared", a_cam); });
    }

    float MoveSeconds(const Cam& a_c) {
        if (a_c.move == kMoveCut) return 0.0f;
        float core = a_c.time;
        if (CamSlows(a_c)) {  // as BuildSlowTables adds it up
            core = 0.0f;
            for (int i = 1; i <= kSlowSamples; ++i) {
                const float mid = (static_cast<float>(i) - 0.5f) / kSlowSamples;
                core += (a_c.time / kSlowSamples) / (std::max)(SlowSpeed(a_c, mid), 0.05f);
            }
        }
        return WindTime(a_c) + core + TailTime(a_c);
    }

    void RequestFreezeWorld() {
        Queue([] {
            FreezeTime::Toggle();
            Event(FreezeTime::IsFrozen() ? "the world is frozen" : "the world runs again");
        });
    }

    void RequestRecord(int a_state) {
        Queue([a_state] {
            const bool on = a_state < 0 ? !s_rec.on : a_state != 0;
            if (!on) {
                StopRecord("window");
                return;
            }
            if (s_rec.on) return;
            if (auto* fcs = CurrentFreeCam()) {
                StartRecord(fcs);
            } else {
                Notify("Camera path: turn the free camera on to record");
            }
        });
    }

    void RequestSaveAs(std::string a_name) {
        Queue([name = CleanName(std::move(a_name))] {
            if (name.empty() || name == "Current") {
                Notify("Camera path: type a name first");
                return;
            }
            std::scoped_lock lock(s_lock);
            const bool ok = WriteLocked(name);
            if (ok) s_name = name;
            RefreshSavedLocked();
            EventLocked(ok ? std::format("saved as \"{}\"", name) : std::format("saving \"{}\" failed", name));
            RE::SendHUDMessage::ShowHUDMessage(ok ? std::format("Camera path saved: {}", name).c_str() : "Camera path: saving failed");
        });
    }

    void RequestLoad(std::string a_name) {
        Queue([name = CleanName(std::move(a_name))] {
            if (s_play.on) EndPlay("another path was opened");
            if (s_rec.on) StopRecord("another path was opened");
            {
                std::scoped_lock lock(s_lock);
                if (!ReadLocked(name)) {
                    EventLocked(std::format("opening \"{}\" failed", name));
                    return;
                }
                s_name = name;
                EventLocked(std::format("opened \"{}\" - {} camera(s)", name, s_cams.size()));
            }
            SaveCurrentSoon();
        });
    }

    // ---- 11-14 round 13: SexLab scene paths (see CamPath.h) ----------------------------------------------------------------
    void RefreshScene() { PublishScene(ReadScene()); }

    void RequestSceneSave() {
        Queue([] {
            const auto sc = ReadScene();
            if (!sc.in) {
                Notify("Scene path: start a SexLab scene first (its stage must be known)");
                return;
            }
            std::vector<Cam> cams;
            {
                std::scoped_lock lock(s_lock);
                cams = s_cams;
            }
            if (cams.size() < 2) {
                Notify("Scene path: add at least 2 cameras first");
                return;
            }
            int        positions = 0;
            const auto local     = ToScene(cams, sc, positions);
            if (!WriteFile(SceneFile(sc.key), local, false, true)) {
                Event(std::format("scene path for {}: saving failed", sc.label));
                Notify("Scene path: saving failed");
                return;
            }
            s_savedCheckKey = sc.key;
            s_savedCheck    = true;
            PublishScene(sc);
            Event(std::format("scene path saved for {} ({}) - {} cameras, {} pivot(s) kept as scene positions", sc.label, sc.key, local.size(), positions));
            Notify("Scene path saved: " + sc.label);
        });
    }

    void RequestSceneOpen() {
        Queue([] {
            const auto       sc = ReadScene();
            std::vector<Cam> cams;
            bool             follow = false, loop = false;
            if (!sc.in || !ReadFile(SceneFile(sc.key), cams, follow, loop)) {
                Notify(sc.in ? "Scene path: none saved for this stage" : "Scene path: start a SexLab scene first");
                return;
            }
            if (s_play.on) EndPlay("a scene path was opened");
            if (s_rec.on) StopRecord("a scene path was opened");
            {
                std::scoped_lock lock(s_lock);
                if (!s_cams.empty()) WriteLocked("Before scene path");
                s_cams   = FromScene(cams, sc);
                s_follow = false;
                s_loop   = true;
                RefreshSavedLocked();
                EventLocked(std::format("opened the scene path for {} - {} camera(s) round the scene (your path before it is saved as \"Before scene path\")",
                    sc.label, s_cams.size()));
            }
            SaveCurrentSoon();
        });
    }

    void RequestSceneDelete() {
        Queue([] {
            const auto sc = ReadScene();
            if (!sc.in) return;
            std::error_code ec;
            const bool      gone = std::filesystem::remove(SceneFile(sc.key), ec);
            s_savedCheckKey      = sc.key;
            s_savedCheck         = false;
            if (s_play.on && s_play.scene) EndPlay("its scene path was deleted");
            PublishScene(sc);
            Event(std::format("scene path for {}: {}", sc.label, gone ? "deleted" : "there was none"));
            Notify(gone ? "Scene path deleted: " + sc.label : std::string("Scene path: none saved for this stage"));
        });
    }

    void RequestScenePlay() {
        Queue([] {
            auto* fcs = CurrentFreeCam();
            if (!fcs) {
                Notify("Scene path: turn the free camera on first");
                return;
            }
            PlayScene(fcs, ReadScene(), "the editor");
        });
    }
}