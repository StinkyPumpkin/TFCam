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
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
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
        long long   s_closedAt  = 0;

        long long NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        double Now() { return static_cast<double>(NowMs()) / 1000.0; }

        void Queue(std::function<void()> a_fn) {
            if (auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask(std::move(a_fn));
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
                Bool(j, "zoomTrick", c.zoomTrick);
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
            else if (field == "curve") {  // x1|y1|x2|y2: turns the camera's own curve on
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
                Close();
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
            } else if (cmd == "stop") {
                CamPath::RequestPlay(0);
            } else if (cmd == "rewind") {
                CamPath::RequestRewind();
            } else if (cmd == "backwards") {
                CamPath::RequestPlayBackwards();
            } else if (cmd == "seek") {
                CamPath::RequestSeek(F(a, 1));
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
            } else if (cmd == "console") {
                CamPath::RequestPivotConsole(I(a, 1, 0));
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
            s_dirty = true;
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
                Close();
            } else if (NowMs() - s_closedAt > 300) {  // the key press that just closed it
                Open();
            }
        });
    }

    void Open() {
        if (!Available() || s_open) return;
        CamPathWindow::AutoHide(false);
        s_open      = true;
        s_aside     = false;
        s_lastAside = false;
        s_typing    = false;
        Show();
        SKSE::log::info("CamPathPrisma: editor open");
    }

    void Close() {
        if (!Available() || !s_open) return;
        s_open  = false;
        s_typing = false;
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
            if (code == 1 || (set.openKey && code == set.openKey)) {  // Esc / the editor's key
                Queue([] { Close(); });
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
        Push(s_dirty);
        s_dirty = false;
    }
}
