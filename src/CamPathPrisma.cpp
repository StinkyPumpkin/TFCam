#include "CamPathPrisma.h"
#include "CamPath.h"
#include "CamPathWindow.h"
#include "FixedCam.h"
#include "FreeCamMenu.h"
#include "FrameFx.h"
#include "PrismaUI_API.h"
#include "VideoCapture.h"

#include <Windows.h>

#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace CamPathPrisma {

    namespace {
        PRISMA_UI_API::IVPrismaUI1* s_api   = nullptr;
        PRISMA_UI_API::IVPrismaUI2* s_api2  = nullptr;
        PrismaView                  s_view  = 0;
        std::atomic<bool>           s_dom{ false };     // the page is up and its listener registered
        std::atomic<bool>           s_open{ false };    // the editor is open (shown, or put aside by a clean-screen path)
        std::atomic<bool>           s_typing{ false };  // a text box in the page has the keyboard
        // main thread
        bool        s_aside     = false;  // hidden while a clean-screen path plays
        bool        s_lastAside = false;
        double      s_nextPush  = 0.0;
        bool        s_dirty     = true;
        std::string s_lastJson;
        // 11-13 (user: "I tried to clear all cameras - it did not seem to work until I closed and reopened the UI; same when I
        // add a camera"): the change itself runs a frame later (CamPath's task), so the page is sent its state for a while
        // after every command, and the camera count each push carries is logged when it changes
        double      s_forceUntil = 0.0;
        std::size_t s_pushedCams = static_cast<std::size_t>(-1);
        long long   s_closedAt  = 0;
        // Console button: the editor steps aside and the console opens; when it closes, what was clicked in it becomes
        // camera s_consoleCam's pivot (the editor takes every key, so the console could not be opened beside it)
        int    s_consoleCam   = -1;
        bool   s_consoleSeen  = false;
        double s_consoleSince = 0.0;

        void Hide();
        void Show();

        long long NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        double Now() { return static_cast<double>(NowMs()) / 1000.0; }

        void Queue(std::function<void()> a_fn) {
            if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask(std::move(a_fn));
        }

        // 11-14 (user: "the UI must be closed then opened to see ANY change"; TFCam.log 11-14: "the page was sent N camera(s)" only
        // ever next to "editor open"): the camera hook that calls Tick() does not run while the editor is up, so the page only got
        // its state when it opened. A helper thread now asks for one Tick (one queued main-thread task) every 0.1 s while the editor
        // is open - never re-queued from inside a task
        std::atomic<bool> s_pumpOn{ false };
        void StartPump() {
            if (s_pumpOn.exchange(true)) return;
            try {
                std::thread([] {
                    for (;;) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        if (!s_open.load()) break;
                        if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([] { CamPathPrisma::Tick(); });
                    }
                    s_pumpOn = false;
                }).detach();
            } catch (...) {
                s_pumpOn = false;
            }
        }

        // ---- the state, as JSON -------------------------------------------------------------------------------------
        void Str(std::string& a_out, std::string_view a_s) {
            a_out += '"';
            for (const char c : a_s) {
                switch (c) {
                case '"': a_out += "\\\""; break;
                case '\\': a_out += "\\\\"; break;
                case '\n': a_out += "\\n"; break;
                case '\r': break;
                case '\t': a_out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        a_out += ' ';
                    } else {
                        a_out += c;
                    }
                }
            }
            a_out += '"';
        }
        void Key(std::string& a_out, const char* a_k) {
            if (a_out.back() != '{' && a_out.back() != '[') a_out += ',';
            Str(a_out, a_k);
            a_out += ':';
        }
        void Num(std::string& a_out, const char* a_k, double a_v) {
            Key(a_out, a_k);
            a_out += std::isfinite(a_v) ? std::format("{:.4g}", a_v) : std::string("0");
        }
        void Bool(std::string& a_out, const char* a_k, bool a_v) {
            Key(a_out, a_k);
            a_out += a_v ? "true" : "false";
        }
        void Text(std::string& a_out, const char* a_k, std::string_view a_v) {
            Key(a_out, a_k);
            Str(a_out, a_v);
        }

        std::string KeyText(int a_code) { return a_code ? std::string(FreeCamMenu::KeyName(a_code)) : std::string(); }

        // Which way camera a_i's orbit goes, for the editor (it offers Left / Right only, user 10-31): the camera's own
        // choice, else the short way round as WayAngle plays it. 0 = not an orbit, or no pivot at either end.
        int OrbitWay(const CamPath::Snapshot& a_s, std::size_t a_i) {
            const auto& c = a_s.cams[a_i];
            if (c.move != CamPath::kMoveAround) return 0;
            if (c.way != CamPath::kWayShortest) return c.way;
            const std::size_t n = a_s.cams.size();
            const std::size_t k = a_i + 1 < n ? a_i + 1 : (a_s.loop && n > 1 ? 0 : n);
            if (k >= n) return 0;
            const auto&         d   = a_s.cams[k];
            const RE::NiPoint3* mid = c.havePivot ? &c.pivotPos : (d.havePivot ? &d.pivotPos : nullptr);
            if (!mid) return 0;
            const float a0 = std::atan2(c.pos.x - mid->x, c.pos.y - mid->y);
            const float a1 = std::atan2(d.pos.x - mid->x, d.pos.y - mid->y);
            return std::remainder(a1 - a0, 2.0f * std::numbers::pi_v<float>) < 0.0f ? CamPath::kWayLeft : CamPath::kWayRight;
        }

        std::string StateJson() {
            const auto  s   = CamPath::GetSnapshot();
            const auto& set = CamPath::Get();
            std::string j   = "{";
            Text(j, "name", s.name);
            Bool(j, "follow", s.follow);
            Bool(j, "loop", s.loop);
            Bool(j, "showPath", set.showPath);
            Num(j, "smoothness", set.smoothness);
            Num(j, "aimHeight", set.aimHeight);
            Bool(j, "playing", s.playing);
            Bool(j, "paused", s.paused);
            Bool(j, "rewinding", s.rewinding);
            Num(j, "rate", s.rate);
            Num(j, "atCam", s.atCam);
            Num(j, "progress", s.progress);
            Num(j, "total", s.total);
            Bool(j, "freeCam", s.freeCam);
            Bool(j, "frozen", s.frozen);
            Bool(j, "clean", s.cleanScreen);
            Bool(j, "recordingPath", s.recording);
            Bool(j, "video", VideoCapture::Recording());
            Text(j, "videoStatus", VideoCapture::Status());
            Text(j, "fxStatus", FrameFx::Status());
            Bool(j, "fixed", FixedCam::IsOn());
            Bool(j, "watching", FixedCam::Watching());
            Text(j, "lastEvent", s.lastEvent);
            // 11-14 round 13: SexLab scene paths
            Bool(j, "sceneIn", s.sceneIn);
            Text(j, "sceneLabel", s.sceneLabel);
            Bool(j, "sceneSaved", s.sceneSaved);
            Bool(j, "scenePlaying", s.scenePlaying);
            Bool(j, "sceneAuto", set.sceneAutoPlay);
            Text(j, "openKey", KeyText(set.openKey));
            Text(j, "playKey", KeyText(set.playKey));
            Text(j, "addKey", KeyText(set.addKey));
            Text(j, "videoKey", KeyText(VideoCapture::Get().key));
            Key(j, "saved");
            j += '[';
            for (const auto& name : s.savedPaths) {
                if (j.back() != '[') j += ',';
                Str(j, name);
            }
            j += ']';
            Key(j, "cams");
            j += '[';
            for (std::size_t i = 0; i < s.cams.size(); ++i) {
                const auto& c = s.cams[i];
                if (j.back() != '[') j += ',';
                j += '{';
                Text(j, "pivot", i < s.camPivotNames.size() ? s.camPivotNames[i] : std::string("none"));
                Num(j, "pivotKind", c.pivotKind);
                Num(j, "bone", c.bone);
                Num(j, "look", c.look);
                Num(j, "wait", c.wait);
                Num(j, "fov", c.fov);
                Num(j, "move", c.move);
                Num(j, "time", c.time);
                Num(j, "seconds", CamPath::MoveSeconds(c));
                Bool(j, "smooth", c.smooth);
                Num(j, "turns", c.turns);
                Num(j, "way", c.way);
                Num(j, "wayNow", OrbitWay(s, i));
                Bool(j, "zoomTrick", c.zoomTrick);
                Num(j, "dolly", c.dolly);
                Num(j, "bendSide", c.bendSide);
                Num(j, "bendUp", c.bendUp);
                Bool(j, "blur", c.blur);
                Num(j, "blurStrength", c.blurStrength);
                Num(j, "shake", c.shake);
                Bool(j, "crashZoom", c.crashZoom);
                Bool(j, "slowMo", c.slowMo);
                Num(j, "slowAt", c.slowAt);
                Num(j, "slowSpeed", c.slowSpeed);
                Num(j, "slowLength", c.slowLength);
                Bool(j, "slowWorld", c.slowWorld);
                Bool(j, "slowCamera", c.slowCamera);
                Num(j, "elastic", c.elastic);
                Num(j, "sling", c.sling);
                Bool(j, "curveOn", c.curveOn);
                Key(j, "curve");
                j += std::format("[{:.3f},{:.3f},{:.3f},{:.3f}]", c.curveX1, c.curveY1, c.curveX2, c.curveY2);
                j += '}';
            }
            j += "]}";
            return j;
        }

        void Push(bool a_force) {
            if (!s_api || !s_view || !s_dom || !s_open) return;
            std::string j = StateJson();
            if (!a_force && j == s_lastJson) return;
            s_lastJson = j;
            s_api->InteropCall(s_view, "tfcamState", j.c_str());
            if (const auto n = CamPath::GetSnapshot().cams.size(); n != s_pushedCams) {
                s_pushedCams = n;
                SKSE::log::info("CamPathPrisma: the page was sent {} camera(s)", n);
            }
        }

        // ---- the page's commands ("cmd|arg|arg...") - main thread ------------------------------------------------
        std::vector<std::string> Split(const std::string& a_s) {
            std::vector<std::string> out;
            std::size_t              start = 0;
            for (;;) {
                const std::size_t bar = a_s.find('|', start);
                out.push_back(a_s.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            return out;
        }

        float F(const std::vector<std::string>& a_a, std::size_t a_i, float a_def = 0.0f) {
            if (a_i >= a_a.size()) return a_def;
            char*       end = nullptr;
            const float v   = std::strtof(a_a[a_i].c_str(), &end);
            return end != a_a[a_i].c_str() && std::isfinite(v) ? v : a_def;
        }
        int I(const std::vector<std::string>& a_a, std::size_t a_i, int a_def = -1) {
            return static_cast<int>(std::lround(F(a_a, a_i, static_cast<float>(a_def))));
        }

        // One option of one camera: "edit|<camera>|<field>|<value>[|...]".
        void Edit(const std::vector<std::string>& a_a) {
            const int  i = I(a_a, 1);
            const auto s = CamPath::GetSnapshot();
            if (i < 0 || i >= static_cast<int>(s.cams.size()) || a_a.size() < 4) return;
            CamPath::Cam c     = s.cams[static_cast<std::size_t>(i)];
            const auto&  field = a_a[2];
            const float  v     = F(a_a, 3);
            const bool   b     = v != 0.0f;
            if (field == "look") c.look = static_cast<int>(v);
            else if (field == "wait") c.wait = v;
            else if (field == "fov") c.fov = v;
            else if (field == "move") c.move = static_cast<int>(v);
            else if (field == "time") c.time = v;
            else if (field == "smooth") c.smooth = b;
            else if (field == "turns") c.turns = static_cast<int>(v);
            else if (field == "way") c.way = static_cast<int>(v);
            else if (field == "zoomTrick") c.zoomTrick = b;
            else if (field == "dolly") c.dolly = v;
            else if (field == "bendSide") c.bendSide = v;
            else if (field == "bendUp") c.bendUp = v;
            else if (field == "bone") c.bone = static_cast<int>(v);
            else if (field == "blur") c.blur = b;
            else if (field == "blurStrength") c.blurStrength = v;
            else if (field == "shake") c.shake = v;
            else if (field == "crashZoom") c.crashZoom = b;
            else if (field == "slowMo") c.slowMo = b;
            else if (field == "slowAt") c.slowAt = v;
            else if (field == "slowSpeed") c.slowSpeed = v;
            else if (field == "slowLength") c.slowLength = v;
            else if (field == "slowWorld") c.slowWorld = b;
            else if (field == "slowCamera") c.slowCamera = b;
            else if (field == "elastic") c.elastic = v;
            else if (field == "sling") c.sling = v;
            else if (field == "curveOn") c.curveOn = b;
            else if (field == "slow") {  // at|speed|length: the camera slows down on part of the way
                c.slowMo     = true;
                c.slowCamera = true;
                c.slowAt     = F(a_a, 3, c.slowAt);
                c.slowSpeed  = F(a_a, 4, c.slowSpeed);
                c.slowLength = F(a_a, 5, c.slowLength);
            } else if (field == "curve") {  // x1|y1|x2|y2: turns the camera's own curve on
                c.curveOn = true;
                c.curveX1 = F(a_a, 3, c.curveX1);
                c.curveY1 = F(a_a, 4, c.curveY1);
                c.curveX2 = F(a_a, 5, c.curveX2);
                c.curveY2 = F(a_a, 6, c.curveY2);
            } else {
                SKSE::log::warn("CamPathPrisma: unknown camera option '{}'", field);
                return;
            }
            CamPath::EditCam(i, c);
        }

        void Handle(const std::string& a_cmd) {
            const auto a = Split(a_cmd);
            if (a.empty()) return;
            const auto& cmd = a[0];
            auto&       set = CamPath::Get();
            if (cmd == "ready") {
                s_dom = true;
            } else if (cmd == "close") {
                CloseByUser("the page");
            } else if (cmd == "typing") {
                s_typing = I(a, 1, 0) != 0;
            } else if (cmd == "edit") {
                Edit(a);
            } else if (cmd == "add") {
                CamPath::RequestAddCam();
            } else if (cmd == "play") {  // play / pause
                const auto s = CamPath::GetSnapshot();
                if (s.playing && !s.paused && !s.rewinding) {
                    CamPath::RequestPause();
                } else {
                    CamPath::RequestPlay(1);
                }
            } else if (cmd == "pause") {  // the backwards button while it plays backwards
                const auto s = CamPath::GetSnapshot();
                if (s.playing && !s.paused) CamPath::RequestPause();
            } else if (cmd == "stop") {
                CamPath::RequestPlay(0);
            } else if (cmd == "rewind") {
                CamPath::RequestRewind();
            } else if (cmd == "backwards") {
                CamPath::RequestPlayBackwards();
            } else if (cmd == "seek") {
                CamPath::RequestSeek(F(a, 1));
            } else if (cmd == "slowHere") {
                CamPath::RequestSlowHere(I(a, 1, -1));
            } else if (cmd == "playFrom") {
                CamPath::RequestPlayFrom(I(a, 1, 0));
            } else if (cmd == "goTo") {
                CamPath::RequestGoToCam(I(a, 1, 0));
            } else if (cmd == "here") {
                CamPath::RequestSetCamHere(I(a, 1, 0));
            } else if (cmd == "delete") {
                CamPath::RequestDeleteCam(I(a, 1, -1));
            } else if (cmd == "pick") {
                CamPath::RequestPickPivot(I(a, 1, 0));
            } else if (cmd == "me") {
                CamPath::RequestPivotMe(I(a, 1, 0));
            } else if (cmd == "console") {  // Tick finishes it when the console closes
                s_consoleCam   = I(a, 1, 0);
                s_consoleSeen  = false;
                s_consoleSince = Now();
                if (!s_aside) {
                    s_aside = true;
                    Hide();
                }
                if (auto* q = RE::UIMessageQueue::GetSingleton()) q->AddMessage(RE::Console::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
                RE::SendHUDMessage::ShowHUDMessage("Click who or what to use as the pivot, then close the console");
                SKSE::log::info("CamPathPrisma: console pick for camera {} - the console opens", s_consoleCam + 1);
            } else if (cmd == "none") {
                CamPath::RequestPivotNone(I(a, 1, 0));
            } else if (cmd == "follow") {
                CamPath::SetFollow(I(a, 1, 0) != 0);
            } else if (cmd == "loop") {
                CamPath::SetLoop(I(a, 1, 0) != 0);
            } else if (cmd == "showPath") {
                set.showPath = I(a, 1, 1) != 0;
                FreeCamMenu::SaveCamPathSettings();
            } else if (cmd == "smoothness") {
                set.smoothness = F(a, 1, set.smoothness);
                CamPath::Clamp();
                FreeCamMenu::SaveCamPathSettings();
            } else if (cmd == "aimHeight") {
                set.aimHeight = F(a, 1, set.aimHeight);
                CamPath::Clamp();
                FreeCamMenu::SaveCamPathSettings();
            } else if (cmd == "freeze") {
                CamPath::RequestFreezeWorld();
            } else if (cmd == "clean") {
                CamPath::SetCleanScreen(I(a, 1, 0) != 0);
            } else if (cmd == "video") {
                VideoCapture::Toggle("the editor");
            } else if (cmd == "record") {  // manual path: fly by hand (the editor closes so you can fly)
                const bool starting = !CamPath::GetSnapshot().recording;
                CamPath::RequestRecord(starting ? 1 : 0);
                if (starting) Close();
            } else if (cmd == "saveAs") {
                CamPath::RequestSaveAs(a.size() > 1 ? a[1] : std::string());
            } else if (cmd == "open") {
                if (a.size() > 1) CamPath::RequestLoad(a[1]);
            } else if (cmd == "clear") {
                CamPath::RequestClear();
            } else if (cmd == "sceneSave") {   // 11-14 round 13: SexLab scene paths
                CamPath::RequestSceneSave();
            } else if (cmd == "sceneOpen") {
                CamPath::RequestSceneOpen();
            } else if (cmd == "sceneDelete") {
                CamPath::RequestSceneDelete();
            } else if (cmd == "scenePlay") {
                CamPath::RequestScenePlay();
            } else if (cmd == "sceneAuto") {
                set.sceneAutoPlay = I(a, 1, 1) != 0;
                FreeCamMenu::SaveCamPathSettings();
            } else if (cmd == "fix") {
                FixedCam::RequestFixHere();
                Close();  // play on
            } else if (cmd == "release") {
                FixedCam::RequestRelease("the editor");
            } else if (cmd == "watch") {
                FixedCam::SetWatch(I(a, 1, 0) != 0);
            } else if (cmd == "smf") {  // the old window instead
                Close();
                CamPathWindow::Toggle();
            } else {
                SKSE::log::warn("CamPathPrisma: unknown command '{}'", a_cmd);
                return;
            }
            s_dirty      = true;
            s_forceUntil = Now() + 0.6;  // 11-13: the change lands a frame later - keep sending
        }

        void OnCmd(const char* a_arg) {
            if (!a_arg) return;
            Queue([cmd = std::string(a_arg)] { Handle(cmd); });
        }

        void OnConsole(PrismaView, PRISMA_UI_API::ConsoleMessageLevel a_level, const char* a_msg) {
            if (!a_msg) return;
            if (a_level == PRISMA_UI_API::ConsoleMessageLevel::Error || a_level == PRISMA_UI_API::ConsoleMessageLevel::Warning) {
                SKSE::log::warn("CamPathPrisma page: {}", a_msg);
            } else {
                SKSE::log::info("CamPathPrisma page: {}", a_msg);
            }
        }

        void Hide() {
            s_api->Unfocus(s_view);
            s_api->Hide(s_view);
        }

        void Show() {
            s_api->Show(s_view);
            s_api->Focus(s_view, false);  // the game keeps running; Focus(pause = true) deadlocks a JS close
            s_dirty = true;
            Push(true);
        }
    }

    void Init() {
        if (s_api) return;
        if (!::GetModuleHandleW(L"PrismaUI.dll")) {
            SKSE::log::info("CamPathPrisma: PrismaUI is not installed - the Camera Path editor is the SKSE Menu Framework window");
            return;
        }
        s_api2 = PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI2>();
        s_api  = s_api2 ? s_api2 : PRISMA_UI_API::RequestPluginAPI<PRISMA_UI_API::IVPrismaUI1>();
        if (!s_api) {
            SKSE::log::warn("CamPathPrisma: PrismaUI.dll is loaded but gave no API - using the SKSE Menu Framework window");
            return;
        }
        s_view = s_api->CreateView("TFCam/index.html", [](PrismaView a_view) {
            s_api->RegisterJSListener(a_view, "tfcamCmd", OnCmd);
            if (s_api2) s_api2->RegisterConsoleCallback(a_view, OnConsole);
            s_dom = true;
            SKSE::log::info("CamPathPrisma: the editor page is ready");
        });
        if (!s_view) {
            SKSE::log::warn("CamPathPrisma: PrismaUI could not make the view (TFCam/index.html) - using the SKSE Menu Framework window");
            s_api = nullptr;
            return;
        }
        s_api->Hide(s_view);
        SKSE::log::info("CamPathPrisma: PrismaUI {} - the Camera Path editor view is made (hidden)", s_api2 ? "v2" : "v1");
    }

    bool Available() { return s_api && s_view; }
    bool IsOpen() { return s_open; }

    void Toggle() {
        Queue([] {
            if (s_open) {
                CloseByUser("the editor key");
            } else if (NowMs() - s_closedAt > 300) {  // the key press that just closed it
                Open();
            }
        });
    }

    void Open() {
        if (!Available() || s_open) return;
        CamPathWindow::AutoHide(false);
        s_open       = true;
        s_aside      = false;
        s_lastAside  = false;
        s_typing     = false;
        s_consoleCam = -1;
        Show();
        StartPump();  // 11-14: the page's state every 0.1 s while open
        SKSE::log::info("CamPathPrisma: editor open");
    }

    // 11-14 round 14 (user: "while i was playing a camera path and paused it then closed the cam path ui the view is locked - i can
    // not look around, it just shakes the view; same for if it is playing"): closing the editor yourself (Esc, Tab, its key, the page)
    // ends a playing / paused path - the camera stays where it is and the mouse looks round again. A SexLab scene path plays on
    void CloseByUser(const char* a_how) {
        if (!Available() || !s_open) return;
        CamPath::StopForEditorClose(a_how);
        Close();
    }

    void Close() {
        if (!Available() || !s_open) return;
        s_open       = false;
        s_typing     = false;
        s_consoleCam = -1;
        if (!s_aside) Hide();
        s_aside    = false;
        s_closedAt = NowMs();
        SKSE::log::info("CamPathPrisma: editor closed");
    }

    bool OnInput(RE::InputEvent* const* a_events) {
        if (!s_open || s_aside || !a_events) return false;
        const auto& set = CamPath::Get();
        for (auto* e = *a_events; e; e = e->next) {
            auto* btn = e->AsButtonEvent();
            if (!btn || !btn->IsDown() || btn->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
            const int code = static_cast<int>(btn->GetIDCode());
            if (s_typing) continue;  // typing a path name: the keys are the page's
            // 11-14 round 14 (user: "tab should close the cam path ui - it just flashes some other view for a second"): Tab too
            if (code == 1 || code == 0x0F || (set.openKey && code == set.openKey)) {  // Esc / Tab / the editor's key
                Queue([code] { CloseByUser(code == 1 ? "Esc" : (code == 0x0F ? "Tab" : "the editor key")); });
            } else if (set.playKey && code == set.playKey) {
                Queue([] { Handle("play"); });
            } else if (set.addKey && code == set.addKey) {
                Queue([] { Handle("add"); });
            } else if (const int vk = VideoCapture::Get().key; vk && code == vk) {
                Queue([] { Handle("video"); });
            }
        }
        return true;  // everything else stops here: the game gets no keys while the editor has focus
    }

    void Tick() {
        if (!Available() || !s_open) return;
        CamPath::RefreshScene();   // 11-14 round 13: the SexLab scene line (main thread)
        if (s_consoleCam >= 0) {  // Console button: back when the console closes (or never opened)
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->IsMenuOpen(RE::Console::MENU_NAME)) {
                s_consoleSeen = true;
                return;
            }
            if (!s_consoleSeen && Now() - s_consoleSince < 3.0) return;
            const int cam = std::exchange(s_consoleCam, -1);
            if (s_consoleSeen) {
                CamPath::RequestPivotConsole(cam);
            } else {
                SKSE::log::warn("CamPathPrisma: the console did not open - no pivot picked");
            }
            s_aside     = false;
            s_lastAside = false;
            Show();
            return;
        }
        // a playing path with Clean screen: the editor steps aside, and comes back when it pauses or ends
        const auto s     = CamPath::GetSnapshot();
        const bool aside = s.cleanScreen && s.playing && !s.paused;
        if (aside != s_lastAside) {
            s_lastAside = aside;
            if (aside && !s_aside) {
                s_aside = true;
                Hide();
            } else if (!aside && s_aside) {
                s_aside = false;
                Show();
            }
        }
        if (s_aside) return;
        const double now = Now();
        if (!s_dirty && now < s_nextPush) return;
        s_nextPush = now + 0.1;
        Push(s_dirty || now < s_forceUntil);
        s_dirty = false;
    }
}
