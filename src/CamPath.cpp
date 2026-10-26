#include "CamPath.h"
#include "CamFocus.h"
#include "FreeCamController.h"
#include "SceneCam.h"
#include "SlccBridge.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace CamPath {

    namespace {
        constexpr float  kPi            = 3.14159265358979f;
        constexpr float  kLn2           = 0.69314718f;
        constexpr float  kPitchLimit    = 1.55f;  // ~89 degrees, no flip
        constexpr float  kStartBlend    = 1.0f;   // glide: seconds from where the camera is to the path's start
        constexpr float  kFocusHalfLife = 0.2f;   // focus point / heading smoothing
        constexpr int    kMaxPoints     = 32;
        constexpr int    kSamples       = 48;     // per curved segment, for the even-speed (arc length) table
        constexpr double kPendingTimeout = 1.5;   // Play waits this long for TFCam free-fly

        Settings s_settings;

        // ---- math --------------------------------------------------------------------------------------------------
        float WrapPi(float a_angle) { return std::remainder(a_angle, 2.0f * kPi); }

        float SmoothStep(float a_x) {
            a_x = std::clamp(a_x, 0.0f, 1.0f);
            return a_x * a_x * (3.0f - 2.0f * a_x);
        }

        float ExpWeight(float a_dt, float a_halfLife) { return a_dt <= 0.0f ? 0.0f : 1.0f - std::exp(-kLn2 * a_dt / a_halfLife); }

        float Length(const RE::NiPoint3& a_v) { return std::sqrt(a_v.x * a_v.x + a_v.y * a_v.y + a_v.z * a_v.z); }

        RE::NiPoint3 Mix(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, float a_w) {
            return { a_a.x + (a_b.x - a_a.x) * a_w, a_a.y + (a_b.y - a_a.y) * a_w, a_a.z + (a_b.z - a_a.z) * a_w };
        }

        // Turn a_v by heading a_h (Skyrim's angle Z: clockwise from +Y, the way an actor's heading turns).
        RE::NiPoint3 RotZ(const RE::NiPoint3& a_v, float a_h) {
            const float c = std::cos(a_h), s = std::sin(a_h);
            return { a_v.x * c + a_v.y * s, -a_v.x * s + a_v.y * c, a_v.z };
        }

        // TFCam's camera convention: forward = (sin yaw * cos pitch, cos yaw * cos pitch, -sin pitch).
        void LookAngles(const RE::NiPoint3& a_dir, float& a_yaw, float& a_pitch) {
            a_yaw   = std::atan2(a_dir.x, a_dir.y);
            a_pitch = std::atan2(-a_dir.z, std::hypot(a_dir.x, a_dir.y));
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
            if (cam && a_fov > 0.0f && std::isfinite(a_fov)) cam->GetRuntimeData2().worldFOV = a_fov;
        }

        RE::FreeCameraState* CurrentFreeCam() {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam || !cam->IsInFreeCameraMode() || !FreeCam::TFCamDriving()) return nullptr;
            auto* state = cam->currentState.get();
            return state ? static_cast<RE::FreeCameraState*>(state) : nullptr;
        }

        // ---- status ------------------------------------------------------------------------------------------------
        std::mutex s_statusLock;
        MenuStatus s_status;

        void Event(std::string a_text) {
            SKSE::log::info("CamPath: {}", a_text);
            std::scoped_lock lock(s_statusLock);
            std::snprintf(s_status.lastEvent, sizeof(s_status.lastEvent), "%s", a_text.c_str());
        }

        void Notify(const char* a_hud, std::string_view a_log) {
            RE::SendHUDMessage::ShowHUDMessage(a_hud);
            Event(std::string(a_log));
        }

        // ---- points ------------------------------------------------------------------------------------------------
        struct Point {
            RE::NiPoint3 pos{};
            float        yaw   = 0.0f;
            float        pitch = 0.0f;
            float        roll  = 0.0f;
            float        fov   = 70.0f;
            bool         haveRel = false;  // the focus was known when it was added
            RE::NiPoint3 rel{};            // offset from the focus, in the focus's own frame
            float        relYaw = 0.0f;    // yaw minus the focus's heading
        };
        std::vector<Point> s_points;  // main thread (SKSE tasks included)
        bool               s_loaded = false;

        std::wstring FilePath() {
            wchar_t     buf[MAX_PATH] = {};
            const DWORD len = ::GetFullPathNameW(L"Data\\SKSE\\Plugins\\TFCam\\CameraPath.tsv", MAX_PATH, buf, nullptr);
            return len > 0 && len < MAX_PATH ? std::wstring(buf, len) : std::wstring(L"Data\\SKSE\\Plugins\\TFCam\\CameraPath.tsv");
        }

        void Save() {
            wchar_t     dir[MAX_PATH] = {};
            const DWORD len = ::GetFullPathNameW(L"Data\\SKSE\\Plugins\\TFCam", MAX_PATH, dir, nullptr);
            if (len > 0 && len < MAX_PATH) ::CreateDirectoryW(dir, nullptr);
            const std::wstring file = FilePath();
            const std::wstring tmp  = file + L".tmp";
            {
                std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!out) {
                    SKSE::log::error("CamPath: cannot write the camera path file (Windows error {})", ::GetLastError());
                    return;
                }
                out << "# TFCam camera path v1: x y z yaw pitch roll fov (radians / degrees) | focus known, offset in the "
                       "focus's frame, yaw minus its heading\n";
                for (const auto& p : s_points) {
                    out << std::format("pt\t{:.3f}\t{:.3f}\t{:.3f}\t{:.5f}\t{:.5f}\t{:.5f}\t{:.2f}\t{}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.5f}\n",
                        p.pos.x, p.pos.y, p.pos.z, p.yaw, p.pitch, p.roll, p.fov, p.haveRel ? 1 : 0, p.rel.x, p.rel.y,
                        p.rel.z, p.relYaw);
                }
            }
            if (!::MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                SKSE::log::error("CamPath: replacing the camera path file failed (Windows error {})", ::GetLastError());
            }
        }

        void Load() {
            if (s_loaded) return;
            s_loaded = true;
            std::ifstream in(FilePath(), std::ios::in | std::ios::binary);
            if (!in) return;
            std::string line;
            while (std::getline(in, line) && static_cast<int>(s_points.size()) < kMaxPoints) {
                if (!line.starts_with("pt\t")) continue;
                Point p;
                int   rel = 0;
                const int n = std::sscanf(line.c_str() + 3, "%f %f %f %f %f %f %f %d %f %f %f %f", &p.pos.x, &p.pos.y, &p.pos.z,
                    &p.yaw, &p.pitch, &p.roll, &p.fov, &rel, &p.rel.x, &p.rel.y, &p.rel.z, &p.relYaw);
                if (n < 7 || !std::isfinite(p.pos.x) || !std::isfinite(p.yaw) || !std::isfinite(p.fov)) continue;
                p.haveRel = n >= 12 && rel != 0;
                s_points.push_back(p);
            }
            SKSE::log::info("CamPath: loaded {} camera path point(s)", s_points.size());
        }

        // ---- focus -------------------------------------------------------------------------------------------------
        // The focus as it is right now: its point and heading. a_what: what it is, for the menu.
        bool FocusRaw(RE::NiPoint3& a_out, float& a_heading, std::string* a_what) {
            if (s_settings.focus == 1) {
                if (auto ref = CamFocus::ConsoleRef(); ref && CamFocus::RefPoint(ref.get(), a_out)) {
                    a_heading = ref->GetAngleZ();
                    if (a_what) *a_what = "console selection: " + CamFocus::Describe(ref.get());
                    return true;
                }
            }
            std::string what;
            if (!SceneCam::SubjectPoint(a_out, &what)) return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            a_heading    = player ? player->GetAngleZ() : 0.0f;
            if (a_what) *a_what = s_settings.focus == 1 ? "nothing selected in the console - " + what : what;
            return true;
        }

        // ---- playback ----------------------------------------------------------------------------------------------
        struct Pose {
            RE::NiPoint3 pos{};
            float        yaw   = 0.0f;
            float        pitch = 0.0f;
            float        roll  = 0.0f;
            float        fov   = 70.0f;
        };

        enum Channel : int { kYaw = 0, kPitch, kRoll, kFov, kChannels };

        struct Play {
            bool                      on       = false;
            bool                      blending = false;
            float                     blendT   = 0.0f;
            Pose                      from;
            float                     t        = 0.0f;
            bool                      closed   = false;
            bool                      follow   = false;
            bool                      snap     = false;
            bool                      curved   = true;
            int                       loop     = 0;
            float                     duration = 12.0f;
            std::vector<RE::NiPoint3> base;  // positions played: world, or offsets in the focus's frame (follow)
            std::vector<float>        ang[kChannels];
            float                     lap[kChannels] = {};  // closed paths: what one lap adds to an unwrapped angle
            // curved: the arc length table (segment, u, cumulative length); straight: cumulative length per point
            std::vector<int>          tabSeg;
            std::vector<float>        tabU;
            std::vector<float>        tabLen;
            float                     length  = 0.0f;
            RE::NiPoint3              focus{};
            float                     heading = 0.0f;
            bool                      haveFocus = false;
            std::string               focusWhat;
            float                     progress = 0.0f;
        };
        Play s_play;

        bool   s_pendingPlay  = false;
        double s_pendingUntil = 0.0;

        // Main thread: what the menu page shows (it runs on the render thread and reads only s_status).
        void PublishCounts() {
            std::scoped_lock lock(s_statusLock);
            s_status.points  = static_cast<int>(s_points.size());
            s_status.playing = s_play.on;
            s_status.length  = s_play.on ? s_play.length : 0.0f;
            if (!s_play.on) s_status.progress = 0.0f;
        }

        int Count() { return static_cast<int>(s_play.base.size()); }
        int Segments() { return s_play.closed ? Count() : Count() - 1; }

        RE::NiPoint3 P(int a_i) {
            const int n = Count();
            if (s_play.closed) return s_play.base[((a_i % n) + n) % n];
            if (a_i < 0) return s_play.base[0] * 2.0f - s_play.base[1];
            if (a_i >= n) return s_play.base[n - 1] * 2.0f - s_play.base[n - 2];
            return s_play.base[a_i];
        }

        float V(int a_ch, int a_i) {
            const auto& a = s_play.ang[a_ch];
            const int   n = Count();
            if (s_play.closed) {
                const int k    = ((a_i % n) + n) % n;
                const int laps = (a_i - k) / n;
                return a[k] + static_cast<float>(laps) * s_play.lap[a_ch];
            }
            if (a_i < 0) return 2.0f * a[0] - a[1];
            if (a_i >= n) return 2.0f * a[n - 1] - a[n - 2];
            return a[a_i];
        }

        // Centripetal Catmull-Rom (Barry-Goldman), segment a_seg from point a_seg to a_seg + 1.
        RE::NiPoint3 Curve(int a_seg, float a_u) {
            const RE::NiPoint3 p0 = P(a_seg - 1), p1 = P(a_seg), p2 = P(a_seg + 1), p3 = P(a_seg + 2);
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

        // Uniform Catmull-Rom on one angle / FOV channel.
        float CurveV(int a_ch, int a_seg, float a_u) {
            const float v0 = V(a_ch, a_seg - 1), v1 = V(a_ch, a_seg), v2 = V(a_ch, a_seg + 1), v3 = V(a_ch, a_seg + 2);
            const float u2 = a_u * a_u, u3 = u2 * a_u;
            return 0.5f * (2.0f * v1 + (-v0 + v2) * a_u + (2.0f * v0 - 5.0f * v1 + 4.0f * v2 - v3) * u2 +
                              (-v0 + 3.0f * v1 - 3.0f * v2 + v3) * u3);
        }

        void BuildTable() {
            s_play.tabSeg.clear();
            s_play.tabU.clear();
            s_play.tabLen.clear();
            float total = 0.0f;
            if (s_play.curved) {
                RE::NiPoint3 prev = Curve(0, 0.0f);
                s_play.tabSeg.push_back(0);
                s_play.tabU.push_back(0.0f);
                s_play.tabLen.push_back(0.0f);
                for (int seg = 0; seg < Segments(); ++seg) {
                    for (int k = 1; k <= kSamples; ++k) {
                        const float        u = static_cast<float>(k) / kSamples;
                        const RE::NiPoint3 p = Curve(seg, u);
                        total += Length(p - prev);
                        prev = p;
                        s_play.tabSeg.push_back(seg);
                        s_play.tabU.push_back(u);
                        s_play.tabLen.push_back(total);
                    }
                }
            } else {
                s_play.tabLen.push_back(0.0f);
                for (int seg = 0; seg < Segments(); ++seg) {
                    total += Length(P(seg + 1) - P(seg));
                    s_play.tabLen.push_back(total);
                }
            }
            s_play.length = total;
        }

        // The pose a_g (0..1) of the way along the path, in the base frame (world, or the focus's frame).
        Pose Along(float a_g) {
            Pose        out;
            const int   segs = Segments();
            const float s    = std::clamp(a_g, 0.0f, 1.0f) * s_play.length;
            int         seg  = 0;
            float       u    = 0.0f;
            if (s_play.curved) {
                const auto& L  = s_play.tabLen;
                auto        it = std::lower_bound(L.begin(), L.end(), s);
                std::size_t i  = static_cast<std::size_t>(std::distance(L.begin(), it));
                if (i == 0) {
                    seg = 0;
                    u   = 0.0f;
                } else if (i >= L.size()) {
                    seg = segs - 1;
                    u   = 1.0f;
                } else {
                    const float span = L[i] - L[i - 1];
                    const float w    = span > 1e-4f ? (s - L[i - 1]) / span : 1.0f;
                    seg              = s_play.tabSeg[i];
                    const float u0   = s_play.tabSeg[i - 1] == seg ? s_play.tabU[i - 1] : 0.0f;
                    u                = u0 + (s_play.tabU[i] - u0) * w;
                }
                if (s_play.length <= 1e-3f) {  // all points in one spot: move through them by time instead
                    const float f = std::clamp(a_g, 0.0f, 1.0f) * static_cast<float>(segs);
                    seg           = std::clamp(static_cast<int>(f), 0, segs - 1);
                    u             = f - static_cast<float>(seg);
                }
                out.pos   = Curve(seg, u);
                out.yaw   = CurveV(kYaw, seg, u);
                out.pitch = CurveV(kPitch, seg, u);
                out.roll  = CurveV(kRoll, seg, u);
                out.fov   = CurveV(kFov, seg, u);
            } else {
                const auto& L = s_play.tabLen;
                if (s_play.length <= 1e-3f) {
                    const float f = std::clamp(a_g, 0.0f, 1.0f) * static_cast<float>(segs);
                    seg           = std::clamp(static_cast<int>(f), 0, segs - 1);
                    u             = SmoothStep(f - static_cast<float>(seg));
                } else {
                    while (seg < segs - 1 && s > L[seg + 1]) ++seg;
                    const float span = L[seg + 1] - L[seg];
                    u                = SmoothStep(span > 1e-4f ? (s - L[seg]) / span : 1.0f);  // eases in and out at each point
                }
                out.pos   = Mix(P(seg), P(seg + 1), u);
                out.yaw   = V(kYaw, seg) + (V(kYaw, seg + 1) - V(kYaw, seg)) * u;
                out.pitch = V(kPitch, seg) + (V(kPitch, seg + 1) - V(kPitch, seg)) * u;
                out.roll  = V(kRoll, seg) + (V(kRoll, seg + 1) - V(kRoll, seg)) * u;
                out.fov   = V(kFov, seg) + (V(kFov, seg + 1) - V(kFov, seg)) * u;
            }
            return out;
        }

        Pose PointPose(int a_i) {
            Pose p;
            p.pos   = s_play.base[a_i];
            p.yaw   = s_play.ang[kYaw][a_i];
            p.pitch = s_play.ang[kPitch][a_i];
            p.roll  = s_play.ang[kRoll][a_i];
            p.fov   = s_play.ang[kFov][a_i];
            return p;
        }

        // Base frame -> world (follow mode), then the look at the focus when the path looks there.
        Pose ToWorld(Pose a_p) {
            if (s_play.follow) {
                a_p.pos = s_play.focus + RotZ(a_p.pos, s_play.heading);
                a_p.yaw += s_play.heading;
            }
            if (s_settings.look == 0 && s_play.haveFocus) {
                const RE::NiPoint3 aim = s_play.focus + RE::NiPoint3{ 0.0f, 0.0f, s_settings.focusHeight };
                LookAngles(aim - a_p.pos, a_p.yaw, a_p.pitch);
            }
            a_p.pitch = std::clamp(a_p.pitch, -kPitchLimit, kPitchLimit);
            a_p.fov   = std::clamp(a_p.fov, 5.0f, 150.0f);
            return a_p;
        }

        // Where the path is at time a_t; a_done = a "once" path is over (the pose is its end).
        Pose AtTime(float a_t, bool& a_done) {
            a_done        = false;
            const float T = (std::max)(s_play.duration, 0.1f);
            const int   n = Count();
            if (s_play.snap) {
                const float hold = T / static_cast<float>(n);
                int         idx  = static_cast<int>(a_t / hold);
                if (s_play.loop == 0) {
                    if (idx >= n) {
                        a_done = true;
                        idx    = n - 1;
                    }
                } else if (s_play.loop == 1) {
                    idx %= n;
                } else {
                    const int period = 2 * n - 2;
                    const int k      = idx % period;
                    idx              = k < n ? k : period - k;
                }
                s_play.progress = std::clamp(static_cast<float>(idx) / static_cast<float>((std::max)(n - 1, 1)), 0.0f, 1.0f);
                return PointPose(std::clamp(idx, 0, n - 1));
            }
            float g = 0.0f;
            if (s_play.loop == 0) {
                g = a_t / T;
                if (g >= 1.0f) {
                    g      = 1.0f;
                    a_done = true;
                }
                if (s_play.curved) g = SmoothStep(g);  // straight lines ease at every point already
            } else if (s_play.loop == 1 && s_play.closed) {
                g = std::fmod(a_t / T, 1.0f);
            } else {  // back and forth (also "loop" with only two points)
                const float legs = a_t / T;
                const int   leg  = static_cast<int>(legs);
                g                = legs - static_cast<float>(leg);
                if (s_play.curved) g = SmoothStep(g);
                if (leg % 2 == 1) g = 1.0f - g;
            }
            s_play.progress = g;
            return Along(g);
        }

        void WritePose(RE::FreeCameraState* a_fcs, const Pose& a_p) {
            a_fcs->translation = a_p.pos;
            a_fcs->rotation.x  = a_p.pitch;
            a_fcs->rotation.y  = WrapPi(a_p.yaw);
            SetFOV(a_p.fov);
            FreeCam::SetExtraRoll(WrapPi(a_p.roll - FreeCam::GetRollRadians()));
        }

        // The camera stays where the path left it; its roll becomes the free camera's own.
        void EndPlay(std::string_view a_why) {
            if (!s_play.on) return;
            FreeCam::SetRollRadians(WrapPi(FreeCam::GetRollRadians() + FreeCam::GetExtraRoll()));
            FreeCam::SetExtraRoll(0.0f);
            s_play.on       = false;
            s_play.blending = false;
            PublishCounts();
            Event(std::format("camera path stopped ({}) - the camera stays where it is", a_why));
        }

        void UpdateFocus(float a_dt, bool a_snap) {
            RE::NiPoint3 raw{};
            float        heading = 0.0f;
            std::string  what;
            if (!FocusRaw(raw, heading, &what)) return;
            if (!s_play.haveFocus || a_snap) {
                s_play.focus   = raw;
                s_play.heading = heading;
            } else {
                const float w = ExpWeight(a_dt, kFocusHalfLife);
                s_play.focus  = Mix(s_play.focus, raw, w);
                s_play.heading += WrapPi(heading - s_play.heading) * w;
            }
            s_play.haveFocus = true;
            s_play.focusWhat = std::move(what);
        }

        bool StartPlay(RE::FreeCameraState* a_fcs, std::string_view a_source) {
            Load();
            const int n = static_cast<int>(s_points.size());
            if (n < 2) {
                Notify("Camera path: add at least 2 points first", std::format("play refused ({}): {} point(s)", a_source, n));
                return false;
            }
            bool follow = s_settings.followFocus;
            if (follow && std::any_of(s_points.begin(), s_points.end(), [](const Point& p) { return !p.haveRel; })) {
                follow = false;
                Event("some points were added without a focus - the path plays fixed in the world");
            }
            SceneCam::StandDown("camera path plays");
            s_play          = Play{};
            s_play.follow   = follow;
            s_play.snap     = s_settings.snap;
            s_play.curved   = s_settings.curved;
            s_play.loop     = s_settings.loop;
            s_play.duration = s_settings.duration;
            s_play.closed   = s_play.loop == 1 && n >= (s_play.curved ? 3 : 2);
            for (const auto& p : s_points) {
                s_play.base.push_back(follow ? p.rel : p.pos);
                const float yaw = follow ? p.relYaw : p.yaw;
                auto&       ya  = s_play.ang[kYaw];
                ya.push_back(ya.empty() ? yaw : ya.back() + WrapPi(yaw - ya.back()));
                auto& ra = s_play.ang[kRoll];
                ra.push_back(ra.empty() ? p.roll : ra.back() + WrapPi(p.roll - ra.back()));
                s_play.ang[kPitch].push_back(p.pitch);
                s_play.ang[kFov].push_back(p.fov);
            }
            for (const int ch : { static_cast<int>(kYaw), static_cast<int>(kRoll) }) {
                const auto& a  = s_play.ang[ch];
                s_play.lap[ch] = (a.back() + WrapPi(a.front() - a.back())) - a.front();
            }
            BuildTable();
            UpdateFocus(0.0f, true);

            Pose cur;
            cur.pos         = a_fcs->translation;
            cur.pitch       = a_fcs->rotation.x;
            cur.yaw         = a_fcs->rotation.y;
            cur.roll        = FreeCam::GetRollRadians() + FreeCam::GetExtraRoll();
            cur.fov         = GetFOV();
            s_play.from     = cur;
            s_play.blending = !s_play.snap;
            s_play.on       = true;
            PublishCounts();
            static const char* loops[] = { "once", "loop", "back and forth" };
            Event(std::format("camera path plays ({}) - {} points, {}, {}, {:.0f} s, {}, looking {}{}, focus {}", a_source, n,
                s_play.snap ? "cut" : "glide", s_play.curved ? "curved" : "straight", s_play.duration,
                loops[std::clamp(s_play.loop, 0, 2)], s_settings.look == 0 ? "at the focus" : "the way each point looked",
                follow ? ", moving with the focus" : "", s_play.haveFocus ? s_play.focusWhat : "not found"));
            return true;
        }

        void AddPoint(RE::FreeCameraState* a_fcs) {
            Load();
            if (static_cast<int>(s_points.size()) >= kMaxPoints) {
                Notify("Camera path: 32 points is the most", "add refused: 32 points already");
                return;
            }
            Point p;
            p.pos   = a_fcs->translation;
            p.pitch = a_fcs->rotation.x;
            p.yaw   = WrapPi(a_fcs->rotation.y);
            p.roll  = WrapPi(FreeCam::GetRollRadians() + FreeCam::GetExtraRoll());
            p.fov   = GetFOV();
            RE::NiPoint3 focus{};
            float        heading = 0.0f;
            std::string  what;
            if (FocusRaw(focus, heading, &what)) {
                p.haveRel = true;
                p.rel     = RotZ(p.pos - focus, -heading);
                p.relYaw  = WrapPi(p.yaw - heading);
            }
            s_points.push_back(p);
            Save();
            PublishCounts();
            const char letter = static_cast<char>('A' + (std::min)(static_cast<int>(s_points.size()) - 1, 25));
            const auto hud    = std::format("Camera path: point {} added ({} in all)", letter, s_points.size());
            Notify(hud.c_str(), std::format("point {} added at ({:.0f}, {:.0f}, {:.0f}), FOV {:.0f}{}", letter, p.pos.x, p.pos.y,
                                    p.pos.z, p.fov, p.haveRel ? std::format(", {:.0f} units from {}", Length(p.rel), what) : ", no focus"));
        }

        void RemoveLast() {
            Load();
            if (s_points.empty()) return;
            if (s_play.on) EndPlay("a point was removed");
            s_points.pop_back();
            Save();
            PublishCounts();
            const auto hud = std::format("Camera path: last point removed ({} left)", s_points.size());
            Notify(hud.c_str(), hud);
        }

        void Clear() {
            Load();
            if (s_play.on) EndPlay("the path was cleared");
            s_points.clear();
            Save();
            PublishCounts();
            Notify("Camera path: cleared", "all points cleared");
        }

        void Toggle(int a_state, std::string_view a_source) {
            const bool on = a_state < 0 ? !(s_play.on || s_pendingPlay) : a_state != 0;
            if (!on) {
                s_pendingPlay = false;
                EndPlay(a_source);
                return;
            }
            if (s_play.on) return;
            if (auto* fcs = CurrentFreeCam()) {
                StartPlay(fcs, a_source);
                return;
            }
            s_pendingPlay  = true;
            s_pendingUntil = Now() + kPendingTimeout;
            Event(std::format("play ({}): entering TFCam free-fly first", a_source));
            SlccBridge::RequestEnterFreeFly("Camera path");
        }

        void Queue(std::function<void()> a_fn) {
            if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask(std::move(a_fn));
        }
    }

    Settings& Get() { return s_settings; }

    void Clamp() {
        auto& s = s_settings;
        if (!std::isfinite(s.duration)) s.duration = 12.0f;
        if (!std::isfinite(s.focusHeight)) s.focusHeight = 0.0f;
        s.duration    = std::clamp(s.duration, 1.0f, 300.0f);
        s.focusHeight = std::clamp(s.focusHeight, -60.0f, 60.0f);
        s.loop        = std::clamp(s.loop, 0, 2);
        s.look        = std::clamp(s.look, 0, 1);
        s.focus       = std::clamp(s.focus, 0, 1);
        if (s.addKey < 0 || s.addKey > 0xFF) s.addKey = 0;
        if (s.playKey < 0 || s.playKey > 0xFF) s.playKey = 0;
    }

    void Drive(RE::FreeCameraState* a_fcs, bool a_driving) {
        const float dt = FrameDt();
        if (!a_fcs) return;
        if (s_pendingPlay) {
            if (a_driving) {
                s_pendingPlay = false;
                StartPlay(a_fcs, "after entering free-fly");
            } else if (Now() > s_pendingUntil) {
                s_pendingPlay = false;
                Event("play: TFCam free-fly did not start in time");
            }
        }
        {
            std::scoped_lock lock(s_statusLock);
            s_status.freeCam = a_driving;
        }
        if (!s_play.on) return;
        if (!a_driving) {
            EndPlay("another camera (FCFW timeline / SLCC) took the free camera");
            return;
        }
        UpdateFocus(dt, false);
        if (s_play.blending) {
            s_play.blendT += dt;
            const float w      = SmoothStep(s_play.blendT / kStartBlend);
            bool        done   = false;
            const Pose  target = ToWorld(AtTime(0.0f, done));
            Pose        p;
            p.pos   = Mix(s_play.from.pos, target.pos, w);
            p.yaw   = s_play.from.yaw + WrapPi(target.yaw - s_play.from.yaw) * w;
            p.pitch = s_play.from.pitch + (target.pitch - s_play.from.pitch) * w;
            p.roll  = s_play.from.roll + WrapPi(target.roll - s_play.from.roll) * w;
            p.fov   = s_play.from.fov + (target.fov - s_play.from.fov) * w;
            WritePose(a_fcs, p);
            if (s_play.blendT >= kStartBlend) s_play.blending = false;
            s_play.progress = 0.0f;
        } else {
            s_play.t += dt;
            bool       done = false;
            const Pose p    = ToWorld(AtTime(s_play.t, done));
            WritePose(a_fcs, p);
            if (done) EndPlay("the path ended");
        }
        std::scoped_lock lock(s_statusLock);
        s_status.progress = s_play.progress;
        std::snprintf(s_status.focus, sizeof(s_status.focus), "%s", s_play.haveFocus ? s_play.focusWhat.c_str() : "not found");
    }

    bool OwnsMotion() { return s_play.on; }
    bool Playing() { return s_play.on; }

    void Stop(std::string_view a_why) {
        s_pendingPlay = false;
        EndPlay(a_why);
    }

    bool OnHotkey(std::uint32_t a_code) {
        if (a_code == 0) return false;
        if (s_settings.addKey != 0 && a_code == static_cast<std::uint32_t>(s_settings.addKey)) {
            if (auto* fcs = CurrentFreeCam()) {
                AddPoint(fcs);
            } else {
                Notify("Camera path: turn the free camera on to add points", "add refused: no TFCam free camera");
            }
            return true;
        }
        if (s_settings.playKey != 0 && a_code == static_cast<std::uint32_t>(s_settings.playKey)) {
            Toggle(-1, "play key");
            return true;
        }
        return false;
    }

    void OnFreeCamEnd() {
        {
            std::scoped_lock lock(s_statusLock);
            s_status.freeCam = false;
        }
        if (s_play.on) {
            FreeCam::SetExtraRoll(0.0f);
            s_play.on = false;
            PublishCounts();
            Event("camera path stopped (the free camera ended)");
        }
    }

    void OnGameLoad() {
        s_pendingPlay = false;
        s_play.on     = false;
        Load();
        PublishCounts();
    }

    bool FocusPoint(RE::NiPoint3& a_out, const char*& a_what, bool& a_on) {
        a_on = s_play.on;
        if (!s_play.on) return false;
        if (s_settings.look != 0 || !s_play.haveFocus) {
            a_what = "camera path: screen centre";
            return false;
        }
        a_out  = s_play.focus + RE::NiPoint3{ 0.0f, 0.0f, s_settings.focusHeight };
        a_what = "camera path: the focus";
        return true;
    }

    void RequestAddPoint() {
        Queue([] {
            if (auto* fcs = CurrentFreeCam()) {
                AddPoint(fcs);
            } else {
                Event("add refused: no TFCam free camera");
            }
        });
    }

    void RequestRemoveLast() { Queue([] { RemoveLast(); }); }
    void RequestClear() { Queue([] { Clear(); }); }
    void RequestPlay(int a_state) { Queue([a_state] { Toggle(a_state, "menu"); }); }

    MenuStatus GetMenuStatus() {
        std::scoped_lock lock(s_statusLock);
        return s_status;
    }
}
