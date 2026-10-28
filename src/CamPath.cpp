#include "CamPath.h"
#include "CamBlur.h"
#include "CamFocus.h"
#include "CamPathWindow.h"
#include "FreeCamController.h"
#include "FreezeTime.h"
#include "SceneCam.h"
#include "SlccBridge.h"

#include <Windows.h>

#include <algorithm>
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
        int              s_pivotKind = 0;  // 0 = you / your scene, 1 = a reference
        RE::FormID       s_pivotRef  = 0;
        bool             s_follow    = false;
        bool             s_loop      = false;
        std::string      s_lastEvent;
        std::vector<std::string> s_savedPaths;
        // play state copied for the window (s_lock)
        bool  s_pubPlaying = false, s_pubPaused = false, s_pubRecording = false, s_pubFreeCam = false;
        int   s_pubAt      = -1;
        float s_pubProgress = 0.0f;
        std::string s_pubPivotName = "you";
        bool  s_loaded = false;

        void EventLocked(std::string a_text) {
            SKSE::log::info("CamPath: {}", a_text);
            s_lastEvent = std::move(a_text);
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

        bool WriteLocked(const std::string& a_name) {
            std::error_code ec;
            std::filesystem::create_directories(PathsDir(), ec);
            const auto file = PathsDir() / (a_name + ".tsv");
            const auto tmp  = PathsDir() / (a_name + ".tsv.tmp");
            {
                std::ofstream out(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
                if (!out) {
                    SKSE::log::error("CamPath: cannot write {}", file.string());
                    return false;
                }
                out << "# TFCam camera path v2 - tab separated. pivot: kind (0 you, 1 reference) formID follow loop\n"
                       "# cam: x y z yaw pitch roll fov | pivot x y z heading known | look wait blur strength | move time "
                       "smooth turns way zoom\n";
                out << std::format("pivot\t{}\t{:08X}\t{}\t{}\n", s_pivotKind, s_pivotRef, s_follow ? 1 : 0, s_loop ? 1 : 0);
                for (const auto& c : s_cams) {
                    out << std::format("cam\t{:.3f}\t{:.3f}\t{:.3f}\t{:.5f}\t{:.5f}\t{:.5f}\t{:.2f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.5f}\t{}"
                                       "\t{}\t{:.2f}\t{}\t{:.2f}\t{}\t{:.2f}\t{}\t{}\t{}\t{}\n",
                        c.pos.x, c.pos.y, c.pos.z, c.yaw, c.pitch, c.roll, c.fov, c.pivotPos.x, c.pivotPos.y, c.pivotPos.z,
                        c.pivotHeading, c.havePivot ? 1 : 0, c.look, c.wait, c.blur ? 1 : 0, c.blurStrength, c.move, c.time,
                        c.smooth ? 1 : 0, c.turns, c.way, c.zoomTrick ? 1 : 0);
                }
            }
            std::filesystem::rename(tmp, file, ec);
            if (ec) {
                std::filesystem::remove(file, ec);
                std::filesystem::rename(tmp, file, ec);
            }
            return !ec;
        }

        bool ReadLocked(const std::string& a_name) {
            std::ifstream in(PathsDir() / (a_name + ".tsv"), std::ios::in | std::ios::binary);
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
                int have = 0, blur = 0, smooth = 1, zoom = 0;
                const int n = std::sscanf(line.c_str() + 4, "%f %f %f %f %f %f %f %f %f %f %f %d %d %f %d %f %d %f %d %d %d %d",
                    &c.pos.x, &c.pos.y, &c.pos.z, &c.yaw, &c.pitch, &c.roll, &c.fov, &c.pivotPos.x, &c.pivotPos.y,
                    &c.pivotPos.z, &c.pivotHeading, &have, &c.look, &c.wait, &blur, &c.blurStrength, &c.move, &c.time,
                    &smooth, &c.turns, &c.way, &zoom);
                if (n < 22 || !Finite(c.pos) || !std::isfinite(c.yaw) || !std::isfinite(c.pitch)) continue;
                c.havePivot = have != 0;
                c.blur      = blur != 0;
                c.smooth    = smooth != 0;
                c.zoomTrick = zoom != 0;
                cams.push_back(c);
            }
            s_cams      = std::move(cams);
            s_pivotKind = kind == 1 ? 1 : 0;
            s_pivotRef  = ref;
            s_follow    = follow != 0;
            s_loop      = loop != 0;
            return true;
        }

        void LoadOnce() {
            std::scoped_lock lock(s_lock);
            if (s_loaded) return;
            s_loaded = true;
            if (ReadLocked("Current")) SKSE::log::info("CamPath: loaded the current path - {} camera(s)", s_cams.size());
            RefreshSavedLocked();
        }

        std::atomic<bool> s_dirty{ false };
        void SaveCurrentSoon() {
            if (s_dirty.exchange(true)) return;
            Queue([] {
                s_dirty = false;
                std::scoped_lock lock(s_lock);
                WriteLocked("Current");
            });
        }

        // ---- pivot -------------------------------------------------------------------------------------------------
        // The pivot as it is right now: its middle (where the camera looks), heading and name.
        bool PivotRaw(int a_kind, RE::FormID a_ref, RE::NiPoint3& a_out, float& a_heading, std::string* a_name) {
            if (a_kind == 1 && a_ref) {
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_ref);
                if (ref && !ref->IsDeleted() && ref->Get3D() && CamFocus::RefPoint(ref, a_out)) {
                    a_heading = ref->GetAngleZ();
                    if (a_name) *a_name = CamFocus::Describe(ref);
                    return true;
                }
            }
            std::string what;
            if (!SceneCam::SubjectPoint(a_out, &what)) return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            a_heading    = player ? player->GetAngleZ() : 0.0f;
            if (a_name) *a_name = a_kind == 1 ? "you (the picked one is not here)" : "you";
            return true;
        }

        // ---- playback (main thread) --------------------------------------------------------------------------------
        struct Play {
            bool                      on       = false;
            bool                      paused   = false;
            bool                      gliding  = false;
            float                     glideT   = 0.0f;
            RE::NiPoint3              fromPos{};
            RE::NiPoint3              fromFwd{ 0.0f, 1.0f, 0.0f };
            float                     fromRoll = 0.0f;
            float                     fromFov  = 70.0f;
            std::vector<Cam>          cams;
            std::vector<RE::NiPoint3> base;     // per camera: world position, or offset in the pivot's frame (follow)
            std::vector<float>        baseYaw;  // per camera: yaw, or yaw minus the pivot's heading (follow)
            std::vector<RE::NiPoint3> centre;   // per camera: the pivot then (world), or the origin (follow)
            std::vector<std::vector<float>> arc; // per camera: curved move's arc-length table (kSamples + 1)
            bool                      follow   = false;
            bool                      loop     = false;
            int                       pivotKind = 0;
            RE::FormID                pivotRef  = 0;
            int                       k        = 0;   // camera
            int                       phase    = 0;   // 0 = waiting at camera k, 1 = moving on from it
            float                     t        = 0.0f;
            float                     fovCarry = 70.0f;  // the FOV the camera arrived with (zoom trick)
            float                     segFov   = 70.0f;  // the FOV / pivot distance a move started with
            float                     segDist  = 100.0f;
            RE::NiPoint3              pivot{};
            float                     heading  = 0.0f;
            bool                      havePivot = false;
            std::string               pivotName;
            bool                      haveOut  = false;
            RE::NiPoint3              outPos{};
            RE::NiPoint3              outFwd{ 0.0f, 1.0f, 0.0f };
            float                     outYaw   = 0.0f;
            float                     outRoll  = 0.0f;
            float                     outFov   = 70.0f;
            float                     blur     = 0.0f;
            bool                      blurOn   = false;
            float                     total    = 0.0f;
            float                     elapsed  = 0.0f;
        };
        Play   s_play;
        bool   s_pendingPlay  = false;
        double s_pendingUntil = 0.0;

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

        RE::NiPoint3 BaseAt(int a_i) {
            const int n = Count();
            if (s_play.loop) return s_play.base[((a_i % n) + n) % n];
            if (a_i < 0) return s_play.base[0] * 2.0f - s_play.base[std::min(1, n - 1)];
            if (a_i >= n) return s_play.base[n - 1] * 2.0f - s_play.base[std::max(n - 2, 0)];
            return s_play.base[a_i];
        }

        // Centripetal Catmull-Rom between base positions a_k and a_next.
        RE::NiPoint3 CurvePos(int a_k, int a_next, float a_u) {
            const RE::NiPoint3 p1 = s_play.base[a_k], p2 = s_play.base[a_next];
            const RE::NiPoint3 p0 = (s_play.loop || a_k > 0) ? BaseAt(a_k - 1) : p1 * 2.0f - p2;
            const RE::NiPoint3 p3 = (s_play.loop || a_next + 1 < Count()) ? BaseAt(a_next + 1) : p2 * 2.0f - p1;
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

        // Even speed along a curve: progress a_u (0..1 of the length) -> the curve's own parameter.
        float CurveParam(int a_k, float a_u) {
            const auto& L = s_play.arc[a_k];
            if (L.size() < 2 || L.back() <= 1e-3f) return a_u;
            const float s  = Clamp01(a_u) * L.back();
            auto        it = std::lower_bound(L.begin(), L.end(), s);
            const auto  i  = static_cast<std::size_t>(std::distance(L.begin(), it));
            if (i == 0) return 0.0f;
            if (i >= L.size()) return 1.0f;
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

        // Base-frame position along move a_k -> a_next at eased progress a_u.
        RE::NiPoint3 MovePos(int a_k, int a_next, float a_u) {
            const auto& c = s_play.cams[a_k];
            switch (c.move) {
            case kMoveStraight:
                return Mix(s_play.base[a_k], s_play.base[a_next], a_u);
            case kMoveAround: {
                const RE::NiPoint3 c0 = s_play.centre[a_k], c1 = s_play.centre[a_next];
                const RE::NiPoint3 o0 = s_play.base[a_k] - c0, o1 = s_play.base[a_next] - c1;
                const float r0 = std::hypot(o0.x, o0.y), r1 = std::hypot(o1.x, o1.y);
                const float a0 = std::atan2(o0.x, o0.y), a1 = std::atan2(o1.x, o1.y);
                const float da = WayAngle(WrapPi(a1 - a0), c.way, c.turns);
                const float r  = r0 + (r1 - r0) * a_u;
                const float a  = a0 + da * a_u;
                const float h  = o0.z + (o1.z - o0.z) * a_u;
                return Mix(c0, c1, a_u) + RE::NiPoint3{ std::sin(a) * r, std::cos(a) * r, h };
            }
            default:
                return CurvePos(a_k, a_next, CurveParam(a_k, a_u));
            }
        }

        RE::NiPoint3 ToWorld(const RE::NiPoint3& a_base) {
            return s_play.follow ? s_play.pivot + RotZ(a_base, s_play.heading) : a_base;
        }

        RE::NiPoint3 AimPoint() { return s_play.pivot + RE::NiPoint3{ 0.0f, 0.0f, s_settings.aimHeight }; }

        float SetYaw(int a_i) { return s_play.baseYaw[a_i] + (s_play.follow ? s_play.heading : 0.0f); }

        // The way camera a_i looks from a_pos (a_motion = the direction of travel for "where it is going").
        RE::NiPoint3 LookDir(int a_i, const RE::NiPoint3& a_pos, const RE::NiPoint3& a_motion) {
            const auto& c = s_play.cams[a_i];
            switch (c.look) {
            case kLookPivot:
                if (s_play.havePivot) return Normalized(AimPoint() - a_pos, Forward(SetYaw(a_i), c.pitch));
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
        };

        bool EaseIn(int a_k) {
            const auto& c = s_play.cams[a_k];
            return c.smooth || c.wait > 0.0f || (!s_play.loop && a_k == 0);
        }
        bool EaseOut(int a_k, int a_next) {
            const auto& c = s_play.cams[a_k];
            return c.smooth || s_play.cams[a_next].wait > 0.0f || (!s_play.loop && Next(a_next) < 0);
        }

        Target Evaluate() {
            Target out;
            const int k = s_play.k;
            const auto& ck = s_play.cams[k];
            const int next = Next(k);
            if (s_play.phase == 0 || next < 0) {
                out.pos  = ToWorld(s_play.base[k]);
                out.fwd  = LookDir(k, out.pos, s_play.haveOut ? s_play.outFwd : Forward(SetYaw(k), ck.pitch));
                out.roll = ck.roll;
                out.fov  = s_play.fovCarry;
                out.blur = CamBlurOf(k);
                return out;
            }
            const auto& cn = s_play.cams[next];
            const float f  = Clamp01(ck.time > 0.0f ? s_play.t / ck.time : 1.0f);
            const float u  = Ease(f, EaseIn(k), EaseOut(k, next));
            const RE::NiPoint3 basePos = MovePos(k, next, u);
            out.pos = ToWorld(basePos);
            const RE::NiPoint3 ahead = ToWorld(MovePos(k, next, Ease(std::min(f + 0.02f, 1.0f), EaseIn(k), EaseOut(k, next)))) - out.pos;
            if (ck.look == kLookSet && cn.look == kLookSet) {
                // both keep their own view: turn from one to the other (extra turns spin in place unless the move goes
                // round the pivot, which has them already)
                const float y0 = SetYaw(k), y1 = SetYaw(next);
                const float d  = ck.move == kMoveAround ? WrapPi(y1 - y0) : WayAngle(WrapPi(y1 - y0), ck.way, ck.turns);
                out.fwd = Forward(y0 + d * u, ck.pitch + (cn.pitch - ck.pitch) * u);
            } else {
                // Leaving a look at the pivot: keep it in view and turn in the last 40% (a spiral stays on the pivot).
                // Arriving into one: lock onto the pivot in the first 60% and keep it there. Otherwise over the whole move.
                float w = SmoothStep(f);
                if (ck.look == kLookPivot && cn.look != kLookPivot) w = SmoothStep((f - 0.6f) / 0.4f);
                if (ck.look != kLookPivot && cn.look == kLookPivot) w = SmoothStep(f / 0.6f);
                out.fwd = SlerpDir(LookDir(k, out.pos, ahead), LookDir(next, out.pos, ahead), w);
            }
            out.roll = ck.roll + WrapPi(cn.roll - ck.roll) * u;
            if (ck.zoomTrick && s_play.havePivot) {
                const float d = (std::max)(Length(AimPoint() - out.pos), 1.0f);
                const float half = std::atan(std::tan(s_play.segFov * 0.5f * kPi / 180.0f) * s_play.segDist / d);
                out.fov = std::clamp(2.0f * half * 180.0f / kPi, 5.0f, 150.0f);
            } else {
                out.fov = s_play.segFov + (cn.fov - s_play.segFov) * u;
            }
            out.blur = CamBlurOf(k) + (CamBlurOf(next) - CamBlurOf(k)) * u;
            return out;
        }

        void StartMove() {
            s_play.segFov  = s_play.fovCarry;
            s_play.segDist = s_play.havePivot ? (std::max)(Length(AimPoint() - ToWorld(s_play.base[s_play.k])), 1.0f) : 100.0f;
        }

        // Advance the clock; false = a "once" path is over.
        bool Advance(float a_dt) {
            s_play.t += a_dt;
            s_play.elapsed += a_dt;
            for (int guard = 0; guard < 4 * Count() + 4; ++guard) {
                const auto& c = s_play.cams[s_play.k];
                if (s_play.phase == 0) {
                    if (s_play.t < c.wait) return true;
                    s_play.t -= c.wait;
                    if (Next(s_play.k) < 0) return false;
                    s_play.phase = 1;
                    StartMove();
                } else {
                    if (s_play.t < c.time) return true;
                    s_play.t -= c.time;
                    // the FOV the camera arrives with (a zoom trick keeps its result)
                    const int next = Next(s_play.k);
                    s_play.fovCarry = c.zoomTrick ? s_play.outFov : s_play.cams[next].fov;
                    s_play.k     = next;
                    s_play.phase = 0;
                    if (s_play.k == 0) s_play.elapsed = 0.0f;  // a new lap
                }
            }
            return true;
        }

        void UpdatePivot(float a_dt, bool a_snap) {
            RE::NiPoint3 raw{};
            float        heading = 0.0f;
            std::string  name;
            if (!PivotRaw(s_play.pivotKind, s_play.pivotRef, raw, heading, &name)) {
                s_play.havePivot = false;
                return;
            }
            if (!s_play.havePivot || a_snap) {
                s_play.pivot   = raw;
                s_play.heading = heading;
            } else {
                const float w = ExpWeight(a_dt, kPivotHalfLife);
                s_play.pivot  = Mix(s_play.pivot, raw, w);
                s_play.heading += WrapPi(heading - s_play.heading) * w;
            }
            s_play.havePivot = true;
            s_play.pivotName = std::move(name);
        }

        void WritePose(RE::FreeCameraState* a_fcs) {
            float yaw = s_play.outYaw, pitch = 0.0f;
            LookAngles(s_play.outFwd, yaw, pitch);
            s_play.outYaw     = yaw;
            a_fcs->translation = s_play.outPos;
            a_fcs->rotation.x  = std::clamp(pitch, -kPitchLimit, kPitchLimit);
            a_fcs->rotation.y  = WrapPi(yaw);
            SetFOV(s_play.outFov);
            FreeCam::SetExtraRoll(WrapPi(s_play.outRoll - FreeCam::GetRollRadians()));
        }

        void Publish() {
            std::scoped_lock lock(s_lock);
            s_pubPlaying  = s_play.on;
            s_pubPaused   = s_play.on && s_play.paused;
            s_pubAt       = s_play.on ? s_play.k : -1;
            s_pubProgress = s_play.on && s_play.total > 0.0f ? Clamp01(s_play.elapsed / s_play.total) : 0.0f;
            s_pubRecording = s_rec.on;
            if (s_play.on) s_pubPivotName = s_play.pivotName;
        }

        void BlurOff() {
            if (s_play.blurOn) {
                CamBlur::External(false, {}, -1.0f);
                s_play.blurOn = false;
            }
        }

        // The camera stays where the path left it; its roll becomes the free camera's own.
        void EndPlay(std::string_view a_why) {
            if (!s_play.on) return;
            FreeCam::SetRollRadians(WrapPi(FreeCam::GetRollRadians() + FreeCam::GetExtraRoll()));
            FreeCam::SetExtraRoll(0.0f);
            BlurOff();
            s_play.on      = false;
            s_play.paused  = false;
            s_play.gliding = false;
            Event(std::format("camera path stopped ({}) - the camera stays where it is", a_why));
            Publish();
        }

        bool StartPlay(RE::FreeCameraState* a_fcs, std::string_view a_source) {
            LoadOnce();
            {
                std::scoped_lock lock(s_lock);
                if (s_cams.size() < 2) {
                    EventLocked(std::format("play refused ({}): {} camera(s)", a_source, s_cams.size()));
                    Notify("Camera path: add at least 2 cameras first");
                    return false;
                }
                s_play           = Play{};
                s_play.cams      = s_cams;
                s_play.follow    = s_follow;
                s_play.loop      = s_loop;
                s_play.pivotKind = s_pivotKind;
                s_play.pivotRef  = s_pivotRef;
            }
            if (s_rec.on) s_rec.on = false;
            SceneCam::StandDown("camera path plays");
            UpdatePivot(0.0f, true);
            const int n = Count();
            for (int i = 0; i < n; ++i) {
                const auto& c = s_play.cams[i];
                if (s_play.follow && c.havePivot) {
                    s_play.base.push_back(RotZ(c.pos - c.pivotPos, -c.pivotHeading));
                    s_play.baseYaw.push_back(WrapPi(c.yaw - c.pivotHeading));
                    s_play.centre.push_back({});
                } else {
                    s_play.base.push_back(c.pos);
                    s_play.baseYaw.push_back(c.yaw);
                    s_play.centre.push_back(c.havePivot ? c.pivotPos : (s_play.havePivot ? s_play.pivot : c.pos));
                }
            }
            // even speed along curved moves
            s_play.arc.assign(n, {});
            for (int k = 0; k < n; ++k) {
                const int next = Next(k);
                if (next < 0 || s_play.cams[k].move != kMoveCurve) continue;
                auto& L = s_play.arc[k];
                L.push_back(0.0f);
                RE::NiPoint3 prev = CurvePos(k, next, 0.0f);
                float        acc  = 0.0f;
                for (int i = 1; i <= kSamples; ++i) {
                    const RE::NiPoint3 p = CurvePos(k, next, static_cast<float>(i) / kSamples);
                    acc += Length(p - prev);
                    prev = p;
                    L.push_back(acc);
                }
            }
            for (int k = 0; k < n; ++k) {
                s_play.total += s_play.cams[k].wait;
                if (Next(k) >= 0) s_play.total += s_play.cams[k].time;
            }
            s_play.fovCarry = s_play.cams[0].fov;
            s_play.fromPos  = a_fcs->translation;
            s_play.fromFwd  = Forward(a_fcs->rotation.y, WrapPi(a_fcs->rotation.x));
            s_play.fromRoll = FreeCam::GetRollRadians() + FreeCam::GetExtraRoll();
            s_play.fromFov  = GetFOV();
            s_play.outYaw   = a_fcs->rotation.y;
            s_play.gliding  = true;
            s_play.on       = true;
            Event(std::format("camera path plays ({}) - {} cameras, {:.0f} s{}, pivot {}{}", a_source, n, s_play.total,
                s_play.loop ? " (loop)" : "", s_play.havePivot ? s_play.pivotName : "not found", s_play.follow ? ", following it" : ""));
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
                if (s_play.paused) {
                    s_play.paused = false;
                    Event(std::format("going on ({})", a_source));
                    Publish();
                }
                return;
            }
            if (auto* fcs = CurrentFreeCam()) {
                StartPlay(fcs, a_source);
                return;
            }
            s_pendingPlay  = true;
            s_pendingUntil = Now() + kPendingTimeout;
            Event(std::format("play ({}): entering TFCam free-fly first", a_source));
            SlccBridge::RequestEnterFreeFly("Camera path");
        }

        // ---- editing (main thread) ---------------------------------------------------------------------------------
        Cam CamFromFreeCam(RE::FreeCameraState* a_fcs, int a_kind, RE::FormID a_ref) {
            Cam c;
            c.pos   = a_fcs->translation;
            c.pitch = WrapPi(a_fcs->rotation.x);  // the vanilla free camera keeps its pitch in 0..2 pi
            c.yaw   = WrapPi(a_fcs->rotation.y);
            c.roll  = WrapPi(FreeCam::GetRollRadians() + FreeCam::GetExtraRoll());
            c.fov   = GetFOV();
            RE::NiPoint3 p{};
            float        h = 0.0f;
            if (PivotRaw(a_kind, a_ref, p, h, nullptr)) {
                c.pivotPos     = p;
                c.pivotHeading = h;
                c.havePivot    = true;
            }
            c.look   = kLookSet;  // plays back the view you set
            c.move   = kMoveCurve;
            c.time   = 3.0f;
            c.smooth = false;
            return c;
        }

        void PivotNow(int& a_kind, RE::FormID& a_ref) {
            std::scoped_lock lock(s_lock);
            a_kind = s_pivotKind;
            a_ref  = s_pivotRef;
        }

        void AddCam(RE::FreeCameraState* a_fcs) {
            LoadOnce();
            int        kind;
            RE::FormID ref;
            PivotNow(kind, ref);
            const Cam   c = CamFromFreeCam(a_fcs, kind, ref);
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
            int        kind;
            RE::FormID ref;
            PivotNow(kind, ref);
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
                kind   = s_pivotKind;
                ref    = s_pivotRef;
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

        void SetPivot(int a_kind, RE::NiPointer<RE::TESObjectREFR> a_ref, std::string_view a_how) {
            std::string name = "you";
            {
                std::scoped_lock lock(s_lock);
                auto* player = RE::PlayerCharacter::GetSingleton();
                if (a_kind == 1 && a_ref && a_ref.get() != player) {
                    s_pivotKind = 1;
                    s_pivotRef  = a_ref->GetFormID();
                    name        = CamFocus::Describe(a_ref.get());
                } else {
                    s_pivotKind = 0;
                    s_pivotRef  = 0;
                }
                s_pubPivotName = name;
                EventLocked(std::format("pivot: {} ({})", name, a_how));
            }
            SaveCurrentSoon();
            Notify("Pivot: " + name);
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
            int        kind;
            RE::FormID ref;
            {
                std::scoped_lock lock(s_lock);
                if (!s_cams.empty()) WriteLocked("Before recording");  // the old path is kept
                kind = s_pivotKind;
                ref  = s_pivotRef;
            }
            const Cam first = CamFromFreeCam(a_fcs, kind, ref);
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
                StartPlay(a_fcs, "after entering free-fly");
            } else if (Now() > s_pendingUntil) {
                s_pendingPlay = false;
                Event("play: TFCam free-fly did not start in time");
            }
        }
        if (s_rec.on) {
            if (!a_driving) {
                StopRecord("another camera took the free camera");
            } else {
                RecordFrame(a_fcs, dt);
            }
        }
        if (!s_play.on) return;
        if (!a_driving) {
            EndPlay("another camera (FCFW timeline / SLCC) took the free camera");
            return;
        }
        UpdatePivot(dt, false);
        bool finished = false;
        if (s_play.gliding) {
            s_play.glideT += s_play.paused ? 0.0f : dt;
        } else if (!s_play.paused) {
            finished = !Advance(dt);
        }
        const Target tg = Evaluate();
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
            if (!s_play.haveOut || s_settings.smoothness <= 0.001f) {
                s_play.outPos  = tg.pos;
                s_play.outFwd  = tg.fwd;
                s_play.outRoll = tg.roll;
                s_play.outFov  = tg.fov;
            } else {
                const float w = ExpWeight(s_play.paused ? 0.0f : dt, s_settings.smoothness * 0.5f);
                s_play.outPos  = Mix(s_play.outPos, tg.pos, w);
                s_play.outFwd  = SlerpDir(s_play.outFwd, tg.fwd, w);
                s_play.outRoll += WrapPi(tg.roll - s_play.outRoll) * w;
                s_play.outFov += (tg.fov - s_play.outFov) * w;
            }
            s_play.blur    = tg.blur;
            s_play.haveOut = true;
        }
        WritePose(a_fcs);
        if (s_play.blur > 0.01f && s_play.havePivot) {
            CamBlur::External(true, AimPoint(), s_play.blur);
            s_play.blurOn = true;
        } else {
            BlurOff();
        }
        Publish();
        if (finished) EndPlay("the path ended");
    }

    bool OwnsMotion() { return s_play.on; }
    bool Playing() { return s_play.on; }

    void Stop(std::string_view a_why) {
        s_pendingPlay = false;
        EndPlay(a_why);
    }

    bool OnHotkey(std::uint32_t a_code) {
        if (a_code == 0) return false;
        auto is = [&](int a_key) { return a_key != 0 && a_code == static_cast<std::uint32_t>(a_key); };
        if (is(s_settings.openKey)) {
            CamPathWindow::Toggle();
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
    }

    void OnGameLoad() {
        s_pendingPlay = false;
        s_play.on     = false;
        s_rec.on      = false;
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
        s.pivotIsMe  = s_pivotKind == 0;
        s.pivotName  = s_pubPivotName;
        s.follow     = s_follow;
        s.loop       = s_loop;
        s.playing    = s_pubPlaying;
        s.paused     = s_pubPaused;
        s.recording  = s_pubRecording;
        s.freeCam    = s_pubFreeCam;
        s.frozen     = FreezeTime::IsFrozen();
        s.atCam      = s_pubAt;
        s.progress   = s_pubProgress;
        s.lastEvent  = s_lastEvent;
        s.savedPaths = s_savedPaths;
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
            c.move         = std::clamp(a_o.move, 0, 2);
            c.time         = std::clamp(std::isfinite(a_o.time) ? a_o.time : 3.0f, 0.1f, 600.0f);
            c.smooth       = a_o.smooth;
            c.turns        = std::clamp(a_o.turns, 0, 20);
            c.way          = std::clamp(a_o.way, 0, 2);
            c.zoomTrick    = a_o.zoomTrick;
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

    void RequestPause() {
        Queue([] {
            if (!s_play.on) return;
            s_play.paused = !s_play.paused;
            Event(s_play.paused ? "paused" : "going on");
            Publish();
        });
    }

    void RequestPickPivot() {
        Queue([] {
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
            SetPivot(1, ref, "picked with the camera");
        });
    }

    void RequestPivotConsole() {
        Queue([] {
            auto ref = CamFocus::ConsoleRef();
            if (!ref) {
                Notify("Camera path: nothing selected in the console");
                return;
            }
            SetPivot(1, ref, "the console's selection");
        });
    }

    void RequestPivotMe() {
        Queue([] { SetPivot(0, nullptr, "you"); });
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
                std::string pivot = "you";
                if (s_pivotKind == 1) {
                    auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(s_pivotRef);
                    pivot     = ref ? CamFocus::Describe(ref) : "you (the saved pivot is not loaded)";
                }
                s_pubPivotName = pivot;
                EventLocked(std::format("opened \"{}\" - {} camera(s)", name, s_cams.size()));
            }
            SaveCurrentSoon();
        });
    }
}
