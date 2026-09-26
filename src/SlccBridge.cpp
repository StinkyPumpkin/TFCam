#include "SlccBridge.h"

#include "FcfwBridge.h"
#include "FreeCamController.h"
#include "FreeCamMenu.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace SlccBridge {

    namespace {

        // ------------------------------------------------------------------------------------
        // State
        // ------------------------------------------------------------------------------------
        enum class State {
            kIdle,
            kEnterWaitGate,     // user asked for free-fly while FCFW owns the camera; waiting for gameplay input
            kEnterKeyUp,        // Director key-down sent, key-up due
            kEnterWaitRelease,  // waiting (<= 1.5 s) for FCFW's active timeline to drop to 0
            kSuspendedFlying,   // TFCam free-fly, SLCC director turned off by us
            kResumeWaitGate,    // free-fly ended; waiting for gameplay input to turn the director back on
            kResumeKeyUp,       // Director key-down sent, key-up due
            kResumeVerify,      // informational wait for SLCC to take the camera back
        };

        const char* Name(State a_state) {
            switch (a_state) {
            case State::kIdle:              return "Idle";
            case State::kEnterWaitGate:     return "EnterWaitGate";
            case State::kEnterKeyUp:        return "EnterKeyUp";
            case State::kEnterWaitRelease:  return "EnterWaitRelease";
            case State::kSuspendedFlying:   return "SuspendedFlying";
            case State::kResumeWaitGate:    return "ResumeWaitGate";
            case State::kResumeKeyUp:       return "ResumeKeyUp";
            case State::kResumeVerify:      return "ResumeVerify";
            }
            return "?";
        }

        struct Binding {
            RE::INPUT_DEVICE device = RE::INPUT_DEVICE::kKeyboard;
            std::uint32_t    code   = 0;
            bool             shift  = false;
            bool             ctrl   = false;
            std::string      text;
        };

        // SLCC's hotkey gate: 250 ms debounce, first press only, and a menu edge (console close)
        // closes input eligibility until SLCC's next main-thread reconciliation.
        constexpr double kGateSettleSec    = 0.20;  // gameplay input continuously open this long
        constexpr double kDebounceSec      = 0.30;  // between two Director presses of ours
        constexpr double kKeyUpDelaySec    = 0.05;  // key-down -> key-up (always a later frame)
        constexpr double kReleaseTimeout   = 1.50;  // Director key -> FCFW idle / FCFW busy again
        constexpr double kFallbackPairSec  = 0.25;  // End -> Begin re-entry window (fallback detector)

        State  s_state          = State::kIdle;
        bool   s_ready          = false;  // Install() ran (Address Library present, TFCam hooks in)
        bool   s_installed      = false;  // ...and FCFW + SLCC are both present: the bridge is armed
        bool   s_suspendedByUs  = false;  // SLCC's director is OFF because we pressed its key
        bool   s_tfcHooked      = false;
        bool   s_injecting      = false;
        double s_gateOpenSince  = -1.0;
        double s_lastInjectDown = -1000.0;
        double s_keyDownAt      = 0.0;
        std::string s_lastWaitWhy;
        Binding     s_binding;
        bool        s_bindingReady = false;  // s_binding read from the TOML for the current state

        bool         s_poseValid = false;  // FCFW camera pose captured just before the Director key
        RE::NiPoint3 s_posePos{};
        float        s_posePitch = 0.0f;
        float        s_poseYaw   = 0.0f;

        std::uintmax_t s_slccLogMark    = 0;
        std::uintmax_t s_slccFlyingMark = 0;  // SLCC log size just before our Director-OFF press

        bool   s_lastEndFcfwOwned = false;  // fallback tfc detector (console hook missing)
        double s_lastEndAt        = -1000.0;

        std::atomic<bool> s_stepQueued{ false };
        std::atomic<bool> s_inStep{ false };
        std::atomic<bool> s_tfcfPending{ false };

        RE::SCRIPT_FUNCTION::Execute_t* s_origTfc = nullptr;

        double Now() {
            static LARGE_INTEGER freq = [] { LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return f; }();
            LARGE_INTEGER t;
            ::QueryPerformanceCounter(&t);
            return static_cast<double>(t.QuadPart) / static_cast<double>(freq.QuadPart);
        }

        void SetState(State a_new, std::string_view a_reason) {
            SKSE::log::info("SLCC bridge: {} -> {} ({}) [suspendedByUs={}, FCFW timeline={}]",
                Name(s_state), Name(a_new), a_reason, s_suspendedByUs, FcfwBridge::ActiveTimelineID());
            s_state         = a_new;
            s_gateOpenSince = -1.0;
            s_bindingReady  = false;
            s_lastWaitWhy.clear();
        }

        void Notify(const char* a_text) {
            RE::DebugNotification(a_text);
        }

        void ConsolePrint(const char* a_text) {
            if (auto* console = RE::ConsoleLog::GetSingleton()) {
                console->Print("%s", a_text);
            }
        }

        bool SlccLoaded() {
            return ::GetModuleHandleA("SLCCNative.dll") != nullptr;
        }

        // FCFW owns the camera and SLCC is the one that can be asked to let go.
        bool SlccHandlingApplies() {
            if (!s_installed || !FcfwBridge::FcfwOwnsCamera()) return false;
            if (!SlccLoaded()) {
                SKSE::log::info("SLCC bridge: FCFW timeline {} is active but SLCCNative.dll is not loaded - "
                                "plain toggle (FCFW keeps its camera)", FcfwBridge::ActiveTimelineID());
                return false;
            }
            return true;
        }

        std::filesystem::path DataPath(const char* a_rel) {
            std::error_code ec;
            return std::filesystem::current_path(ec) / a_rel;
        }

        // ------------------------------------------------------------------------------------
        // SLCC's Director binding, read from its TOML every time (the user can rebind it in
        // SLCC's own menu at any moment). Grammar seen in SLCCNative.dll strings / defaults:
        // "DIK_47", "0", "Up", "Backspace", "MouseWheelUp", "Shift+8", "Ctrl+8".
        // ------------------------------------------------------------------------------------
        std::string Trim(std::string_view a_s) {
            std::size_t b = 0, e = a_s.size();
            while (b < e && std::isspace(static_cast<unsigned char>(a_s[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(a_s[e - 1]))) --e;
            return std::string(a_s.substr(b, e - b));
        }

        std::string Lower(std::string a_s) {
            std::transform(a_s.begin(), a_s.end(), a_s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return a_s;
        }

        std::optional<Binding> ParseBinding(const std::string& a_text, std::string& a_err) {
            Binding     b;
            std::string rest = a_text;
            b.text           = a_text;

            for (;;) {
                const auto plus = rest.find('+');
                if (plus == std::string::npos || plus + 1 >= rest.size()) break;
                const auto mod = Lower(Trim(rest.substr(0, plus)));
                if (mod == "shift") {
                    b.shift = true;
                } else if (mod == "ctrl" || mod == "control") {
                    b.ctrl = true;
                } else {
                    a_err = "unknown modifier '" + mod + "'";
                    return std::nullopt;
                }
                rest = rest.substr(plus + 1);
            }

            const auto key = Lower(Trim(rest));
            if (key.empty()) {
                a_err = "empty key";
                return std::nullopt;
            }

            if (key.rfind("dik_", 0) == 0) {
                char* end = nullptr;
                const auto v = std::strtoul(key.c_str() + 4, &end, 16);
                if (!end || *end != '\0' || v == 0 || v > 0xFF) {
                    a_err = "bad DIK code '" + key + "'";
                    return std::nullopt;
                }
                b.code = static_cast<std::uint32_t>(v);
                return b;
            }

            if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {  // top-row digits
                b.code = key[0] == '0' ? 0x0Bu : static_cast<std::uint32_t>(0x01 + (key[0] - '0'));
                return b;
            }

            if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z') {
                static constexpr std::uint8_t kLetters[26] = {
                    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,  // a-m
                    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C   // n-z
                };
                b.code = kLetters[key[0] - 'a'];
                return b;
            }

            struct Named { std::string_view name; RE::INPUT_DEVICE dev; std::uint32_t code; };
            static constexpr Named kNamed[] = {
                { "up", RE::INPUT_DEVICE::kKeyboard, 0xC8 },        { "down", RE::INPUT_DEVICE::kKeyboard, 0xD0 },
                { "left", RE::INPUT_DEVICE::kKeyboard, 0xCB },      { "right", RE::INPUT_DEVICE::kKeyboard, 0xCD },
                { "backspace", RE::INPUT_DEVICE::kKeyboard, 0x0E }, { "space", RE::INPUT_DEVICE::kKeyboard, 0x39 },
                { "enter", RE::INPUT_DEVICE::kKeyboard, 0x1C },     { "return", RE::INPUT_DEVICE::kKeyboard, 0x1C },
                { "tab", RE::INPUT_DEVICE::kKeyboard, 0x0F },       { "home", RE::INPUT_DEVICE::kKeyboard, 0xC7 },
                { "end", RE::INPUT_DEVICE::kKeyboard, 0xCF },       { "insert", RE::INPUT_DEVICE::kKeyboard, 0xD2 },
                { "delete", RE::INPUT_DEVICE::kKeyboard, 0xD3 },    { "pageup", RE::INPUT_DEVICE::kKeyboard, 0xC9 },
                { "pgup", RE::INPUT_DEVICE::kKeyboard, 0xC9 },      { "pagedown", RE::INPUT_DEVICE::kKeyboard, 0xD1 },
                { "pgdn", RE::INPUT_DEVICE::kKeyboard, 0xD1 },
                { "f1", RE::INPUT_DEVICE::kKeyboard, 0x3B },  { "f2", RE::INPUT_DEVICE::kKeyboard, 0x3C },
                { "f3", RE::INPUT_DEVICE::kKeyboard, 0x3D },  { "f4", RE::INPUT_DEVICE::kKeyboard, 0x3E },
                { "f5", RE::INPUT_DEVICE::kKeyboard, 0x3F },  { "f6", RE::INPUT_DEVICE::kKeyboard, 0x40 },
                { "f7", RE::INPUT_DEVICE::kKeyboard, 0x41 },  { "f8", RE::INPUT_DEVICE::kKeyboard, 0x42 },
                { "f9", RE::INPUT_DEVICE::kKeyboard, 0x43 },  { "f10", RE::INPUT_DEVICE::kKeyboard, 0x44 },
                { "f11", RE::INPUT_DEVICE::kKeyboard, 0x57 }, { "f12", RE::INPUT_DEVICE::kKeyboard, 0x58 },
                // BSWin32MouseDevice::Key: 0 left, 1 right, 2 middle, 3-7 buttons 4-8, 8 wheel up, 9 wheel down
                { "mouseleft", RE::INPUT_DEVICE::kMouse, 0 },    { "mousebutton1", RE::INPUT_DEVICE::kMouse, 0 },
                { "mouseright", RE::INPUT_DEVICE::kMouse, 1 },   { "mousebutton2", RE::INPUT_DEVICE::kMouse, 1 },
                { "mousemiddle", RE::INPUT_DEVICE::kMouse, 2 },  { "mousebutton3", RE::INPUT_DEVICE::kMouse, 2 },
                { "mousebutton4", RE::INPUT_DEVICE::kMouse, 3 }, { "mousebutton5", RE::INPUT_DEVICE::kMouse, 4 },
                { "mousebutton6", RE::INPUT_DEVICE::kMouse, 5 }, { "mousebutton7", RE::INPUT_DEVICE::kMouse, 6 },
                { "mousebutton8", RE::INPUT_DEVICE::kMouse, 7 },
                { "mousewheelup", RE::INPUT_DEVICE::kMouse, 8 },   { "wheelup", RE::INPUT_DEVICE::kMouse, 8 },
                { "mousewheeldown", RE::INPUT_DEVICE::kMouse, 9 }, { "wheeldown", RE::INPUT_DEVICE::kMouse, 9 },
            };
            for (const auto& n : kNamed) {
                if (key == n.name) {
                    b.device = n.dev;
                    b.code   = n.code;
                    return b;
                }
            }
            a_err = "unrecognised key '" + key + "'";
            return std::nullopt;
        }

        std::optional<Binding> ReadDirectorBinding(std::string& a_err) {
            const auto    path = DataPath("Data/SKSE/Plugins/SLCCNative.toml");
            std::ifstream f(path);
            if (!f) {
                a_err = "cannot open " + path.string();
                return std::nullopt;
            }
            bool        inGeneral = false;
            std::string line, value;
            while (std::getline(f, line)) {
                auto t = Trim(line);
                if (t.empty() || t[0] == '#') continue;
                if (t[0] == '[') {
                    inGeneral = (t == "[General]");
                    continue;
                }
                if (!inGeneral) continue;
                const auto eq = t.find('=');
                if (eq == std::string::npos || Trim(t.substr(0, eq)) != "ToggleHotkey") continue;
                auto v = Trim(t.substr(eq + 1));
                if (!v.empty() && v[0] == '"') {
                    const auto close = v.find('"', 1);
                    v = close == std::string::npos ? v.substr(1) : v.substr(1, close - 1);
                } else if (const auto hash = v.find('#'); hash != std::string::npos) {
                    v = Trim(v.substr(0, hash));
                }
                value = v;
                break;
            }
            if (value.empty()) {
                a_err = "no [General] ToggleHotkey in " + path.string();
                return std::nullopt;
            }
            return ParseBinding(value, a_err);
        }

        // ------------------------------------------------------------------------------------
        // Key injection. SLCC reads its hotkeys with a typed BSTEventSink<InputEvent*> on the
        // BSInputDeviceManager, so the event is dispatched through that same source (every sink,
        // PlayerControls included, sees it). userEvent stays empty, so no vanilla control fires.
        // One ButtonEvent (game heap, RE::malloc) is reused for every send and never freed.
        // ------------------------------------------------------------------------------------
        void SendButton(RE::INPUT_DEVICE a_device, std::uint32_t a_code, float a_value, float a_held) {
            auto* idm = RE::BSInputDeviceManager::GetSingleton();
            if (!idm) return;
            static RE::ButtonEvent* evt = nullptr;
            if (!evt) {
                evt = RE::ButtonEvent::Create(a_device, "", a_code, a_value, a_held);
                if (!evt) return;
            }
            evt->device       = a_device;
            evt->eventType    = RE::INPUT_EVENT_TYPE::kButton;
            evt->next         = nullptr;
            evt->userEvent    = "";
            evt->idCode       = a_code;
            evt->value        = a_value;
            evt->heldDownSecs = a_held;

            RE::InputEvent* head = evt;
            s_injecting          = true;
            idm->SendEvent(&head);
            s_injecting = false;
        }

        void InjectBinding(const Binding& a_b, bool a_down) {
            constexpr auto kKbd = RE::INPUT_DEVICE::kKeyboard;
            const bool     wheel = a_b.device == RE::INPUT_DEVICE::kMouse && a_b.code >= 8;
            if (a_down) {
                if (a_b.ctrl) SendButton(kKbd, 0x1D, 1.0f, 0.0f);
                if (a_b.shift) SendButton(kKbd, 0x2A, 1.0f, 0.0f);
                SendButton(a_b.device, a_b.code, 1.0f, 0.0f);  // IsDown(): value > 0, held == 0
            } else {
                // IsUp(): value 0 with held > 0. A wheel notch has no release.
                if (!wheel) SendButton(a_b.device, a_b.code, 0.0f, static_cast<float>(kKeyUpDelaySec));
                if (a_b.shift) SendButton(kKbd, 0x2A, 0.0f, static_cast<float>(kKeyUpDelaySec));
                if (a_b.ctrl) SendButton(kKbd, 0x1D, 0.0f, static_cast<float>(kKeyUpDelaySec));
            }
            SKSE::log::info("SLCC bridge: injected Director key {} '{}' (device {}, code 0x{:X}{}{})",
                a_down ? "DOWN" : "UP", a_b.text, static_cast<int>(a_b.device), a_b.code,
                a_b.shift ? ", +Shift" : "", a_b.ctrl ? ", +Ctrl" : "");
        }

        // ------------------------------------------------------------------------------------
        // Evidence from SLCC's own log (Data/SKSE/Plugins/SLCCNative.log): what did it make of our
        // key? Diagnostic only, plus the one timeout decision below. Nothing here is fatal.
        // ------------------------------------------------------------------------------------
        enum class SlccSaid { kNothing, kDisabled, kEnabled, kIgnored };

        const char* Name(SlccSaid a_s) {
            switch (a_s) {
            case SlccSaid::kDisabled: return "director disabled";
            case SlccSaid::kEnabled:  return "director enabled";
            case SlccSaid::kIgnored:  return "Director key ignored by SLCC";
            default:                  return "nothing about the Director";
            }
        }

        std::filesystem::path SlccLogPath() { return DataPath("Data/SKSE/Plugins/SLCCNative.log"); }

        std::uintmax_t SlccLogSize() {
            std::error_code ec;
            const auto      s = std::filesystem::file_size(SlccLogPath(), ec);
            return ec ? 0 : s;
        }

        SlccSaid ReadSlccLogSince(std::uintmax_t a_mark) {
            std::ifstream f(SlccLogPath(), std::ios::binary);
            if (!f) {
                SKSE::log::info("SLCC bridge: SLCCNative.log not readable - no confirmation from SLCC");
                return SlccSaid::kNothing;
            }
            f.seekg(0, std::ios::end);
            const auto end = static_cast<std::uintmax_t>(f.tellg());
            if (end <= a_mark) {
                SKSE::log::info("SLCC bridge: SLCCNative.log has no new lines since the key");
                return SlccSaid::kNothing;
            }
            const auto len = std::min<std::uintmax_t>(end - a_mark, 256 * 1024);
            std::string buf(static_cast<std::size_t>(len), '\0');
            f.seekg(static_cast<std::streamoff>(end - len));
            f.read(buf.data(), static_cast<std::streamsize>(len));

            SlccSaid said   = SlccSaid::kNothing;
            int      logged = 0;
            std::size_t pos = 0;
            while (pos < buf.size()) {
                auto nl = buf.find('\n', pos);
                if (nl == std::string::npos) nl = buf.size();
                std::string_view ln(buf.data() + pos, nl - pos);
                pos = nl + 1;

                const bool ignored  = ln.find("Toggle hotkey ignored") != std::string_view::npos &&
                                      ln.find("DirectorToggle") != std::string_view::npos;
                const bool disabled = ln.find("Director disabled") != std::string_view::npos ||
                                      ln.find("directorEnabled=false") != std::string_view::npos;
                const bool enabled  = ln.find("Director enabled") != std::string_view::npos ||
                                      ln.find("directorEnabled=true") != std::string_view::npos;
                if (!(ignored || disabled || enabled || ln.find("DirectorToggle") != std::string_view::npos)) {
                    continue;
                }
                if (ignored) said = SlccSaid::kIgnored;
                else if (disabled) said = SlccSaid::kDisabled;
                else if (enabled) said = SlccSaid::kEnabled;
                if (logged++ < 6) {
                    if (!ln.empty() && ln.back() == '\r') ln.remove_suffix(1);
                    SKSE::log::info("SLCC bridge:   SLCC log: {}", ln.substr(0, 320));
                }
            }
            SKSE::log::info("SLCC bridge: SLCC log verdict since the key: {}", Name(said));
            return said;
        }

        // ------------------------------------------------------------------------------------
        // Gameplay gate: SLCC ignores hotkeys while the console / a menu / pause / an SKSE Menu
        // Framework window is up, and for a moment after such a menu closes.
        // ------------------------------------------------------------------------------------
        bool GameplayGateOpen(std::string& a_why) {
            if (FreeCamMenu::IsOverlayOpen()) {
                a_why = "SKSE Menu Framework window open";
                return false;
            }
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                a_why = "no UI";
                return false;
            }
            if (ui->GameIsPaused()) {
                a_why = "game paused";
                return false;
            }
            static constexpr std::string_view kMenus[] = {
                "Console", "Cursor Menu", "Loading Menu", "Main Menu", "Dialogue Menu", "RaceSex Menu"
            };
            for (const auto m : kMenus) {
                if (ui->IsMenuOpen(m)) {
                    a_why = std::string(m) + " open";
                    return false;
                }
            }
            return true;
        }

        void LogWait(const std::string& a_why) {
            if (a_why != s_lastWaitWhy) {
                SKSE::log::info("SLCC bridge: waiting before the Director key: {}", a_why);
                s_lastWaitWhy = a_why;
            }
        }

        enum class Prep { kWait, kGo, kFail };

        // Gate open long enough, our own debounce respected, SLCC's binding read (once per state, from
        // the TOML), and no stray physical modifier that would turn the press into another SLCC action.
        Prep PrepareInjection(double a_now) {
            std::string why;
            if (!GameplayGateOpen(why)) {
                s_gateOpenSince = -1.0;
                LogWait(why);
                return Prep::kWait;
            }
            if (s_gateOpenSince < 0.0) s_gateOpenSince = a_now;
            if (a_now - s_gateOpenSince < kGateSettleSec) return Prep::kWait;
            if (a_now - s_lastInjectDown < kDebounceSec) return Prep::kWait;

            if (!s_bindingReady) {
                std::string err;
                auto        b = ReadDirectorBinding(err);
                if (!b) {
                    SKSE::log::error("SLCC bridge: cannot use SLCC's Director hotkey: {}", err);
                    Notify("TFCam: could not read SLCC's Director hotkey - see TFCam.log");
                    return Prep::kFail;
                }
                if (b->shift || b->ctrl) {
                    SKSE::log::warn("SLCC bridge: Director hotkey '{}' has a modifier. TFCam sends the modifier as an "
                                    "input event; if SLCC reads the physical key state instead, the press won't match.",
                        b->text);
                }
                s_binding      = *b;
                s_bindingReady = true;
            }

            const bool shiftHeld = (::GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrlHeld  = (::GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if ((shiftHeld && !s_binding.shift) || (ctrlHeld && !s_binding.ctrl)) {
                LogWait("Shift/Ctrl physically held");
                return Prep::kWait;
            }
            return Prep::kGo;
        }

        bool EnterFreeCam(bool a_withPose) {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam) return false;
            if (cam->IsInFreeCameraMode()) {
                SKSE::log::info("SLCC bridge: already in free cam after the release - TFCam drives it now");
                return true;
            }
            if (a_withPose && s_poseValid) {
                FreeCam::SetEntryPose(s_posePos, s_posePitch, s_poseYaw);
            }
            cam->ToggleFreeCameraMode(false);
            const bool ok = cam->IsInFreeCameraMode();
            if (!ok) FreeCam::ClearEntryPose();
            return ok;
        }

        // ------------------------------------------------------------------------------------
        // Per-frame stepping. SKSE drains its task queue until empty, so a task that re-adds
        // itself would spin inside one frame. Instead the step hops main task -> UI task -> main
        // task, which lands once per frame (both SKSE queues run on the main thread).
        // ------------------------------------------------------------------------------------
        void Step();

        bool NeedsStepping() {
            return s_tfcfPending.load() ||
                   (s_state != State::kIdle && s_state != State::kSuspendedFlying);
        }

        void Schedule() {
            if (s_inStep) return;  // the running step re-arms itself through the UI hop below
            if (s_stepQueued.exchange(true)) return;
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                s_stepQueued = false;
                return;
            }
            tasks->AddTask([] {
                s_stepQueued = false;
                s_inStep     = true;
                Step();
                s_inStep = false;
                if (NeedsStepping()) {
                    if (auto* t = SKSE::GetTaskInterface()) {
                        t->AddUITask([] { Schedule(); });
                    }
                }
            });
        }

        void Step() {
            if (s_tfcfPending.exchange(false)) {
                RequestToggle("SLUI 'TFCF' message");
            }

            const double now = Now();
            switch (s_state) {
            case State::kIdle:
            case State::kSuspendedFlying:
                return;

            case State::kEnterWaitGate:
                {
                    if (!FcfwBridge::FcfwOwnsCamera()) {
                        // FCFW let go by itself (scene ended?) - no Director press needed.
                        if (EnterFreeCam(false)) {
                            SetState(State::kIdle, "FCFW went idle before the Director key was needed - plain TFCam free cam");
                        } else {
                            SetState(State::kIdle, "FCFW went idle; entering free cam failed");
                        }
                        return;
                    }
                    switch (PrepareInjection(now)) {
                    case Prep::kWait:
                        return;
                    case Prep::kFail:
                        SetState(State::kIdle, "no usable Director hotkey");
                        return;
                    case Prep::kGo:
                        break;
                    }

                    s_poseValid = FreeCam::CaptureFreeCamPose(s_posePos, s_posePitch, s_poseYaw);
                    s_slccLogMark    = SlccLogSize();
                    s_slccFlyingMark = s_slccLogMark;  // resume checks SLCC's director lines from here on
                    InjectBinding(s_binding, true);
                    s_keyDownAt      = now;
                    s_lastInjectDown = now;
                    s_suspendedByUs  = true;
                    SetState(State::kEnterKeyUp, "Director key down sent to turn SLCC's director OFF");
                    return;
                }

            case State::kEnterKeyUp:
                if (now - s_keyDownAt < kKeyUpDelaySec) return;
                InjectBinding(s_binding, false);
                SetState(State::kEnterWaitRelease, "waiting up to 1.5 s for FCFW's timeline to stop");
                return;

            case State::kEnterWaitRelease:
                {
                    if (FcfwBridge::ActiveTimelineID() == 0) {
                        SKSE::log::info("SLCC bridge: FCFW idle {:.0f} ms after the Director key",
                            (now - s_keyDownAt) * 1000.0);
                        ReadSlccLogSince(s_slccLogMark);
                        if (EnterFreeCam(true)) {
                            SetState(State::kSuspendedFlying, "TFCam free-fly active; SLCC director paused by TFCam");
                        } else {
                            SKSE::log::error("SLCC bridge: ToggleFreeCameraMode did not enter free cam");
                            SetState(State::kResumeWaitGate, "free cam did not start - turning SLCC's director back on");
                        }
                        return;
                    }
                    if (now - s_keyDownAt <= kReleaseTimeout) return;

                    const auto said = ReadSlccLogSince(s_slccLogMark);
                    if (said == SlccSaid::kDisabled) {
                        Notify("TFCam: an FCFW camera that isn't SLCC's is running - free cam unavailable");
                        SetState(State::kResumeWaitGate,
                            "SLCC turned its director off but an FCFW timeline is still active - restoring the director");
                    } else {
                        s_suspendedByUs = false;
                        Notify("TFCam: SLCC did not release the camera - see TFCam.log");
                        SetState(State::kIdle, "FCFW still busy 1.5 s after the Director key; SLCC evidently ignored it");
                    }
                    return;
                }

            case State::kResumeWaitGate:
                {
                    auto* cam = RE::PlayerCamera::GetSingleton();
                    if (cam && cam->IsInFreeCameraMode() && !FcfwBridge::FcfwOwnsCamera()) {
                        SetState(State::kSuspendedFlying, "free cam is on again before SLCC was resumed - keep flying");
                        return;
                    }
                    switch (PrepareInjection(now)) {
                    case Prep::kWait:
                        return;
                    case Prep::kFail:
                        s_suspendedByUs = false;
                        Notify("TFCam: press SLCC's Director key yourself to turn its camera back on");
                        SetState(State::kIdle, "no usable Director hotkey - SLCC director left OFF");
                        return;
                    case Prep::kGo:
                        break;
                    }

                    // The Director key is a toggle. If the user pressed it themselves while flying,
                    // SLCC's director is already ON and pressing it again would turn it OFF.
                    if (ReadSlccLogSince(s_slccFlyingMark) == SlccSaid::kEnabled) {
                        s_suspendedByUs = false;
                        SetState(State::kIdle, "SLCC logged its director back ON during free-fly - not pressing the key again");
                        return;
                    }

                    s_slccLogMark = SlccLogSize();
                    InjectBinding(s_binding, true);
                    s_keyDownAt      = now;
                    s_lastInjectDown = now;
                    SetState(State::kResumeKeyUp, "Director key down sent to turn SLCC's director back ON");
                    return;
                }

            case State::kResumeKeyUp:
                if (now - s_keyDownAt < kKeyUpDelaySec) return;
                InjectBinding(s_binding, false);
                s_suspendedByUs = false;
                SetState(State::kResumeVerify, "director toggled back; watching for SLCC to retake the camera (informational)");
                return;

            case State::kResumeVerify:
                if (FcfwBridge::ActiveTimelineID() != 0) {
                    ReadSlccLogSince(s_slccLogMark);
                    SetState(State::kIdle, "SLCC's FCFW camera is back");
                } else if (now - s_keyDownAt > kReleaseTimeout) {
                    ReadSlccLogSince(s_slccLogMark);
                    SetState(State::kIdle, "no FCFW timeline after 1.5 s (normal when no SexLab scene is running)");
                }
                return;
            }
        }

        // ------------------------------------------------------------------------------------
        // Console tfc. The ToggleFreeCamera command's execute pointer is wrapped (data write to the
        // command table, not a code patch). While FCFW owns the camera the call is swallowed and
        // becomes an ENTER request - FCFW would otherwise just re-enter its carrier next frame.
        // ------------------------------------------------------------------------------------
        bool OnConsoleTfc(std::uint16_t a_numParams) {
            switch (s_state) {
            case State::kIdle:
                if (!SlccHandlingApplies()) return false;
                ConsolePrint("TFCam: SLCC owns the camera - pausing SLCC's director; free cam starts when the console closes.");
                SetState(State::kEnterWaitGate,
                    a_numParams ? "console tfc (argument ignored) while FCFW owns the camera"
                                : "console tfc while FCFW owns the camera");
                Schedule();
                return true;
            case State::kEnterWaitGate:
                ConsolePrint("TFCam: free cam request cancelled.");
                SetState(State::kIdle, "console tfc again before the hand-over started - cancelled");
                return true;
            case State::kSuspendedFlying:
                SKSE::log::info("SLCC bridge: console tfc leaves TFCam free-fly (SLCC's director resumes after the console closes)");
                return false;  // vanilla exits the free cam; the End hook schedules the resume
            case State::kResumeWaitGate:
                SetState(State::kSuspendedFlying, "console tfc before SLCC was resumed - back to free-fly");
                return false;  // vanilla enters the free cam (FCFW is idle, nothing re-enters)
            default:
                ConsolePrint("TFCam: busy handing the camera over - try again in a moment.");
                SKSE::log::info("SLCC bridge: console tfc ignored while {}", Name(s_state));
                return true;
            }
        }

        bool TfcExecute(const RE::SCRIPT_PARAMETER* a_paramInfo, RE::SCRIPT_FUNCTION::ScriptData* a_scriptData,
            RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR* a_containingObj, RE::Script* a_scriptObj,
            RE::ScriptLocals* a_locals, double& a_result, std::uint32_t& a_opcodeOffsetPtr) {
            if (s_installed && OnConsoleTfc(a_scriptData ? a_scriptData->numParams : 0)) {
                return true;
            }
            return s_origTfc(a_paramInfo, a_scriptData, a_thisObj, a_containingObj, a_scriptObj, a_locals,
                a_result, a_opcodeOffsetPtr);
        }

        void OnSkseMessage(SKSE::MessagingInterface::Message* a_msg) {
            if (!a_msg || a_msg->type != kMsgToggleFreeFly) return;
            SKSE::log::info("SLCC bridge: 'TFCF' toggle request from {}", a_msg->sender ? a_msg->sender : "?");
            // Only before kDataLoaded: the listener is registered only when the Address Library is
            // present (plugin.cpp), so an inert TFCam never accepts a request it would drop.
            if (!s_ready) return;
            s_tfcfPending = true;  // the sender's thread is unknown - act on the main thread
            Schedule();
        }
    }

    // ----------------------------------------------------------------------------------------
    // Public
    // ----------------------------------------------------------------------------------------

    void OnPostLoad() {
        auto* messaging = SKSE::GetMessagingInterface();
        if (messaging && messaging->RegisterListener(kSluiPluginName, OnSkseMessage)) {
            SKSE::log::info("SLCC bridge: listening for 'TFCF' (0x{:08X}) from {}", kMsgToggleFreeFly, kSluiPluginName);
        } else {
            SKSE::log::info("SLCC bridge: {} not loaded - no 'TFCF' listener", kSluiPluginName);
        }
    }

    void Install() {
        s_ready = true;
        // TFCam ships to users without SLCC: without FCFW + SLCCNative.dll nothing is wrapped and every
        // request stays the plain pre-0.7.7 toggle.
        if (!FcfwBridge::Available() || !SlccLoaded()) {
            SKSE::log::info("SLCC bridge: inactive (FCFW API {}, SLCCNative.dll {}) - plain free cam toggles",
                FcfwBridge::Available() ? "present" : "missing", SlccLoaded() ? "loaded" : "not loaded");
            return;
        }
        s_installed = true;

        // The vanilla command is "ToggleFlyCam" / "tfc" (strings checked in SkyrimSE.exe 1.6.1170:
        // "ToggleFlyCam\0...tfc\0...Toggles the Free Fly camera"). Fall back to the short name.
        auto* cmd = RE::SCRIPT_FUNCTION::LocateConsoleCommand("ToggleFlyCam");
        if (!cmd) {
            if (auto* first = RE::SCRIPT_FUNCTION::GetFirstConsoleCommand()) {
                for (std::uint16_t i = 0; i < RE::SCRIPT_FUNCTION::Commands::kConsoleCommandsEnd; ++i) {
                    if (first[i].shortName && _stricmp(first[i].shortName, "tfc") == 0) {
                        cmd = &first[i];
                        break;
                    }
                }
            }
        }
        if (cmd && cmd->executeFunction) {
            s_origTfc = cmd->executeFunction;
            RE::SCRIPT_FUNCTION::Execute_t* const wrapped = &TfcExecute;
            REL::safe_write(reinterpret_cast<std::uintptr_t>(&cmd->executeFunction), &wrapped, sizeof(wrapped));
            s_tfcHooked = true;
            SKSE::log::info("SLCC bridge: console {} ({}) wrapped", cmd->functionName ? cmd->functionName : "?",
                cmd->shortName ? cmd->shortName : "?");
        } else {
            SKSE::log::warn("SLCC bridge: console ToggleFlyCam (tfc) not found - falling back to the End/Begin "
                            "re-entry detector for tfc during SLCC scenes");
        }
        SKSE::log::info("SLCC bridge: armed (FCFW API present, SLCCNative.dll loaded)");
    }

    void RequestToggle(const char* a_source) {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) return;

        if (!s_installed) {
            cam->ToggleFreeCameraMode(false);
            return;
        }

        switch (s_state) {
        case State::kIdle:
            if (SlccHandlingApplies()) {
                SetState(State::kEnterWaitGate, std::string(a_source) + " while FCFW owns the camera");
                Schedule();
                return;
            }
            SKSE::log::info("SLCC bridge: {} - plain free cam toggle", a_source);
            cam->ToggleFreeCameraMode(false);
            return;

        case State::kEnterWaitGate:
            SetState(State::kIdle, std::string(a_source) + " again before the hand-over started - cancelled");
            return;

        case State::kSuspendedFlying:
            if (cam->IsInFreeCameraMode()) {
                SKSE::log::info("SLCC bridge: {} - leaving TFCam free-fly", a_source);
                cam->ToggleFreeCameraMode(false);  // End hook moves us to ResumeWaitGate
                if (s_state == State::kSuspendedFlying && !cam->IsInFreeCameraMode()) {
                    SetState(State::kResumeWaitGate, "free cam left (End hook not seen)");
                    Schedule();
                }
            } else {
                SetState(State::kResumeWaitGate, std::string(a_source) + ": not in free cam any more");
                Schedule();
            }
            return;

        case State::kResumeWaitGate:
            if (EnterFreeCam(false)) {
                SetState(State::kSuspendedFlying, std::string(a_source) + " before SLCC was resumed - back to free-fly");
            }
            return;

        default:
            SKSE::log::info("SLCC bridge: {} ignored while {}", a_source, Name(s_state));
            return;
        }
    }

    void OnFreeCamBegin(bool a_tfcamDriving) {
        if (!s_installed) return;

        // Fallback only when the console command could not be wrapped: FCFW re-enters its carrier the
        // frame after a tfc exit, so an FCFW-owned End followed within 250 ms by an FCFW-owned Begin is
        // read as the user's tfc. SLCC's scene-start takeover is excluded: that End happens while no
        // timeline is active (SLCC exits the external free cam first, then starts playback).
        if (!s_tfcHooked && !a_tfcamDriving && s_state == State::kIdle && s_lastEndFcfwOwned &&
            Now() - s_lastEndAt <= kFallbackPairSec && SlccHandlingApplies()) {
            SetState(State::kEnterWaitGate, "FCFW re-entered right after an FCFW-owned exit - treating it as console tfc (fallback)");
            Schedule();
            return;
        }
        if (a_tfcamDriving && s_state == State::kResumeWaitGate) {
            SetState(State::kSuspendedFlying, "free cam entered before SLCC was resumed - keep flying");
        }
    }

    void OnFreeCamEnd(bool a_fcfwOwnedAtEnd) {
        s_lastEndFcfwOwned = a_fcfwOwnedAtEnd;
        s_lastEndAt        = Now();
        if (s_state == State::kSuspendedFlying) {
            SetState(State::kResumeWaitGate, "TFCam free-fly ended - SLCC's director goes back on once gameplay input is open");
            Schedule();
        }
    }

    void OnGameLoaded(const char* a_why) {
        FreeCam::ClearEntryPose();
        if (!s_installed) return;
        // SLCC's director on/off is native, process-wide state: SLCC documents it as persistent desired
        // state and documents resets only on plugin/process reload - a save load does not turn it back
        // on. So a director we turned off is turned back on here.
        if (s_state == State::kResumeKeyUp || s_state == State::kResumeVerify) {
            return;  // the director is already being turned back on; let that finish
        }
        if (s_suspendedByUs) {
            SetState(State::kResumeWaitGate, std::string(a_why) + " while SLCC's director was paused by TFCam - restoring it");
            Schedule();
        } else if (s_state != State::kIdle) {
            SetState(State::kIdle, std::string(a_why) + " - pending hand-over dropped");
        }
    }

    // The UI-task hop normally re-arms the step every frame; this only matters if it ever did not.
    void Pump() {
        if (NeedsStepping()) Schedule();
    }

    bool IsInjecting() { return s_injecting; }
}
