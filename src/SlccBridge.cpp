#include "SlccBridge.h"

#include "FcfwBridge.h"
#include "FreeCamController.h"
#include "FreeCamMenu.h"
#include "SceneTracker.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
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
            kSuspendedOff,      // 0.7.8 cycle "Off": SLCC director turned off by us, no free cam
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
            case State::kSuspendedOff:      return "SuspendedOff";
            case State::kResumeWaitGate:    return "ResumeWaitGate";
            case State::kResumeKeyUp:       return "ResumeKeyUp";
            case State::kResumeVerify:      return "ResumeVerify";
            }
            return "?";
        }

        // 0.7.8 camera cycle: TFCam -> SLCC -> Off -> TFCam.
        enum class Mode { kNone, kTFCam, kSLCC, kOff };

        const char* Name(Mode a_mode) {
            switch (a_mode) {
            case Mode::kTFCam: return "TFCam";
            case Mode::kSLCC:  return "SLCC";
            case Mode::kOff:   return "Off";
            default:           return "none";
            }
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
        constexpr double kReentryPairSec   = 0.50;  // 0.7.8: FCFW-owned End -> FCFW re-entry Begin (P+ key)
        // P+ key press -> P+'s Papyrus OnKeyDown toggles free cam. The toggle rides the Papyrus VM, which
        // can lag seconds behind during P+ scenes (SLCC's spec records 15-20 s event backlogs on AE + P+).
        //  - short window: Begins that SexLab P+ Prism's own 0.5 s free-cam re-entry could also explain
        //  - late window:  the FCFW re-entry pair (only a free cam toggle under SLCC's running timeline makes
        //                  it) and the exit from TFCam free-fly (scene end is handled by OnPlayerSceneEnd)
        constexpr double kPplusPressWindow = 2.00;
        constexpr double kPplusLateWindow  = 10.00;
        constexpr double kPrismReentrySec  = 1.50;  // P+ key turned free cam off -> Prism's poll turns it back on

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

        bool   s_lastEndFcfwOwned = false;  // fallback tfc detector (console hook missing) + P+ key detector
        double s_lastEndAt        = -1000.0;

        // 0.7.8 camera cycle
        Mode   s_cycleMode          = Mode::kNone;   // mode the cycle last switched to (kNone: first press = TFCam)
        Mode   s_enterTarget        = Mode::kTFCam;  // kEnter* states: TFCam free-fly or Off once SLCC let go
        bool   s_resumeKeepFreeCam  = false;  // kResume*: scene end / load restore - leave any free cam alone
        bool   s_resumeForeignFcfw  = false;  // kResume*: an FCFW timeline that is not SLCC's is running
        bool   s_announceSlcc       = false;  // kResume*: user-initiated TFCam -> SLCC, show "Camera: SLCC"
        bool   s_restoreAfterEnter  = false;  // the player's scene ended while our Director-OFF press was in flight
        bool   s_selfToggle         = false;  // our own ToggleFreeCameraMode call is running
        std::uint32_t s_pplusKey     = 0;      // SexLab P+ iToggleFreeCamera (DIK code), 0 = none / not keyboard
        double s_pplusPressAt       = -1000.0;  // last physical press of that key with no menu open
        bool   s_pplusPressInCycle  = false;    // ...made during a scene / SLCC camera / hand-over (expiry is logged)
        bool   s_prismPresent       = false;    // SexLab P+ Prism loaded (SexLabPPrism.dll + SexLabPPrism.esp)
        double s_pplusOffAt         = -1000.0;  // a cycle key (P+'s or TFCam's) turned free cam off, SLCC not driving

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

        // Hand-over steps in flight: a press of P+'s key here is not a cycle step (NoteKeyDown).
        bool IsBusy(State a_state) {
            return a_state == State::kEnterKeyUp || a_state == State::kEnterWaitRelease ||
                   a_state == State::kResumeKeyUp || a_state == State::kResumeVerify;
        }

        bool PplusPressArmed() { return s_pplusPressAt > -500.0; }

        void SetState(State a_new, std::string_view a_reason) {
            SKSE::log::info("SLCC bridge: {} -> {} ({}) [suspendedByUs={}, FCFW timeline={}]",
                Name(s_state), Name(a_new), a_reason, s_suspendedByUs, FcfwBridge::ActiveTimelineID());
            if (IsBusy(s_state) && !IsBusy(a_new) && PplusPressArmed()) {
                // A press still armed when a hand-over ends would otherwise be matched to an unrelated
                // free cam exit or entry later (Prism, P+ at scene end).
                SKSE::log::info("SLCC bridge: dropping a SexLab P+ free camera key press left over from the hand-over");
                s_pplusPressAt = -1000.0;
            }
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

        // 0.7.8: the cycle's HUD line. Main thread only (every caller is: hooks, input sink, SKSE tasks).
        void Announce(Mode a_mode, std::string_view a_why, std::string_view a_suffix = {}) {
            s_cycleMode = a_mode;
            std::string text = a_mode == Mode::kTFCam ? "Camera: TFCam" :
                               a_mode == Mode::kSLCC  ? "Camera: SLCC" :
                                                        "Camera: Off";
            text += a_suffix;
            SKSE::log::info("Camera cycle: {} ({})", text, a_why);
            RE::DebugNotification(text.c_str());
        }

        // SexLab P+ Prism (SLP_PrismController) owns "free camera on" for every player scene it tracks:
        // BeginTracking sets FreecamOwned and its 0.5 s poll (EnsureFreecam) calls
        // SexLabUtil.ToggleFreeCamera(1) whenever PlayerCamera is not in free cam. Only Cleanup (scene end)
        // and its First Person button release that hold. So during a player scene with Prism, "free cam
        // off" (the cycle's Off) lasts at most 0.5 s before Prism puts a vanilla free cam - which TFCam
        // drives - back on. The cycle then skips Off: TFCam <-> SLCC.
        bool PrismHoldsFreeCam() {
            return s_prismPresent && SceneTracker::PlayerSceneActive();
        }

        // Our own free cam toggles. FCFW's detour on ToggleFreeCameraMode still sees them; the Begin/End
        // callbacks below use s_selfToggle to tell them from P+ / another mod.
        void ToggleFreeCam(RE::PlayerCamera* a_cam) {
            s_selfToggle = true;
            a_cam->ToggleFreeCameraMode(false);
            s_selfToggle = false;
        }

        bool ConsoleOpen() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen("Console");
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
        // 0.7.8: SexLab P+'s "Toggle Free Camera" hotkey. P+ (SexLabUtil.dll) reads it from
        // Data/SKSE/SexLab/Settings.yaml as `iToggleFreeCamera: <SKSE key code>` (default 0x51,
        // Numpad 3, src/UserData/mcm.def) and sslSystemConfig.OnKeyDown calls
        // SexLabUtil.ToggleFreeCamera() -> PapyrusUtil MiscUtil.ToggleFreeCamera(), which calls
        // PlayerCamera::ToggleFreeCameraMode directly (Address Library ID 50809, no console command).
        // Re-read at data load, each player scene start and each game load (P+'s MCM rewrites the file).
        // ------------------------------------------------------------------------------------
        void RefreshPplusKey(const char* a_when) {
            std::uint32_t key = 0;
            std::string   how;
            if (!::GetModuleHandleA("SexLabUtil.dll")) {
                how = "SexLabUtil.dll (SexLab P+) not loaded";
            } else {
                const auto    path = DataPath("Data/SKSE/SexLab/Settings.yaml");
                std::ifstream f(path);
                key = 0x51;
                how = "P+ default (no " + path.string() + ")";
                if (f) {
                    how = "P+ default (no iToggleFreeCamera in " + path.string() + ")";
                    std::string line;
                    while (std::getline(f, line)) {
                        const auto t = Trim(line);
                        if (t.rfind("iToggleFreeCamera:", 0) != 0) continue;
                        const auto v   = Trim(std::string_view(t).substr(18));
                        char*      end = nullptr;
                        const long n   = std::strtol(v.c_str(), &end, 10);
                        if (end && end != v.c_str()) {
                            if (n > 0 && n <= 0xFF) {
                                key = static_cast<std::uint32_t>(n);
                                how = path.string();
                            } else {
                                key = 0;
                                how = "iToggleFreeCamera=" + v + " is unbound or not a keyboard key";
                            }
                        }
                        break;
                    }
                }
            }
            static bool s_logged = false;
            if (!s_logged || key != s_pplusKey) {
                SKSE::log::info("SLCC bridge: SexLab P+ Toggle Free Camera key = 0x{:X} ({}) [{}]", key, how, a_when);
                s_logged = true;
            }
            s_pplusKey = key;
        }

        bool PplusPressFresh(double a_now) { return a_now - s_pplusPressAt <= kPplusPressWindow; }
        bool PplusPressWithinLate(double a_now) { return a_now - s_pplusPressAt <= kPplusLateWindow; }
        void ConsumePplusPress() { s_pplusPressAt = -1000.0; }

        void LogPplusMatch(double a_now, std::string_view a_what) {
            SKSE::log::info("SLCC bridge: SexLab P+ free camera key press matched {} after {:.0f} ms", a_what,
                (a_now - s_pplusPressAt) * 1000.0);
        }

        // Only during a scene / an SLCC camera / a hand-over of ours; otherwise the key is a plain toggle.
        bool CycleContext() {
            return s_installed && (SceneTracker::PlayerSceneActive() || FcfwBridge::FcfwOwnsCamera() ||
                                   s_state != State::kIdle);
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
            ToggleFreeCam(cam);
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
        void Schedule();

        bool NeedsStepping() {
            return s_tfcfPending.load() ||
                   (s_state != State::kIdle && s_state != State::kSuspendedFlying &&
                    s_state != State::kSuspendedOff);
        }

        // Start pausing SLCC's director; once FCFW lets go, a_target decides: TFCam free-fly or Off.
        void BeginEnter(Mode a_target, std::string_view a_reason) {
            s_enterTarget       = a_target;
            s_restoreAfterEnter = false;
            SetState(State::kEnterWaitGate, std::string(a_reason) + " [then " + Name(a_target) + "]");
            Schedule();
        }

        // Turn SLCC's director back on. a_keepFreeCam: scene end / load restore - no scene for SLCC to
        // film, so a free cam the user is still in stays. a_userCycle: the user asked for SLCC.
        // Finding (0.7.8 review): a late P+ toggle made the TFCam -> SLCC step look like "another mod", so the
        // cycle position stayed TFCam and the next press skipped Off. Any resume inside a live player scene is
        // now that step: the position is recorded here and "Camera: SLCC" is decided at the Director key.
        void BeginResume(bool a_keepFreeCam, bool a_userCycle, std::string_view a_reason) {
            s_resumeKeepFreeCam = a_keepFreeCam;
            s_resumeForeignFcfw = false;
            s_announceSlcc      = a_userCycle;
            if (!a_keepFreeCam && (a_userCycle || SceneTracker::PlayerSceneActive())) s_cycleMode = Mode::kSLCC;
            SetState(State::kResumeWaitGate, a_reason);
            Schedule();
        }

        // At the Director key: a user step, or any resume while the player's scene still runs (SLCC gets the
        // camera back either way). Not for scene-end / load restores or a foreign FCFW timeline.
        bool ResumeShowsSlcc() {
            return s_announceSlcc ||
                   (!s_resumeKeepFreeCam && !s_resumeForeignFcfw && SceneTracker::PlayerSceneActive());
        }

        void CycleStep(const char* a_source);

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
            case State::kSuspendedOff:
                return;

            case State::kEnterWaitGate:
                {
                    if (!FcfwBridge::FcfwOwnsCamera()) {
                        if (s_enterTarget == Mode::kTFCam) {
                            // FCFW let go by itself (scene ended?) - no Director press needed.
                            if (EnterFreeCam(false)) {
                                SetState(State::kIdle, "FCFW went idle before the Director key was needed - plain TFCam free cam");
                                Announce(Mode::kTFCam, "SLCC let go by itself");
                            } else {
                                SetState(State::kIdle, "FCFW went idle; entering free cam failed");
                            }
                            return;
                        }
                        if (!SceneTracker::PlayerSceneActive()) {
                            SetState(State::kIdle, "FCFW went idle and no player scene is running - nothing to turn off");
                            Announce(Mode::kOff, "SLCC let go by itself");
                            return;
                        }
                        // Off with the scene still running: SLCC's director is still on and would take the
                        // camera back at the next stage - turn it off anyway.
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

                    s_poseValid = s_enterTarget == Mode::kTFCam &&
                                  FreeCam::CaptureFreeCamPose(s_posePos, s_posePitch, s_poseYaw);
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
                        if (s_restoreAfterEnter) {
                            s_restoreAfterEnter = false;
                            BeginResume(true, false, "the player's SexLab scene ended during the hand-over - turning SLCC's director back on");
                            return;
                        }
                        if (s_enterTarget == Mode::kOff) {
                            // FCFW's StopPlayback leaves free cam itself; a free cam here is someone else's.
                            auto* cam = RE::PlayerCamera::GetSingleton();
                            if (cam && cam->IsInFreeCameraMode()) {
                                SKSE::log::info("SLCC bridge: a free cam is still on after SLCC let go - leaving it for Off");
                                ToggleFreeCam(cam);
                            }
                            SetState(State::kSuspendedOff, "Off: SLCC director paused by TFCam, no free cam");
                            Announce(Mode::kOff, "SLCC director paused, normal camera");
                            return;
                        }
                        if (EnterFreeCam(true)) {
                            SetState(State::kSuspendedFlying, "TFCam free-fly active; SLCC director paused by TFCam");
                            Announce(Mode::kTFCam, "SLCC director paused, TFCam free-fly");
                        } else {
                            SKSE::log::error("SLCC bridge: ToggleFreeCameraMode did not enter free cam");
                            BeginResume(false, false, "free cam did not start - turning SLCC's director back on");
                        }
                        return;
                    }
                    if (now - s_keyDownAt <= kReleaseTimeout) return;

                    const auto said = ReadSlccLogSince(s_slccLogMark);
                    if (said == SlccSaid::kDisabled) {
                        Notify("TFCam: an FCFW camera that isn't SLCC's is running - free cam unavailable");
                        BeginResume(false, false,
                            "SLCC turned its director off but an FCFW timeline is still active - restoring the director");
                        s_resumeForeignFcfw = true;
                    } else {
                        s_suspendedByUs = false;
                        Notify("TFCam: SLCC did not release the camera - see TFCam.log");
                        SetState(State::kIdle, "FCFW still busy 1.5 s after the Director key; SLCC evidently ignored it");
                    }
                    return;
                }

            case State::kResumeWaitGate:
                {
                    // 0.7.8: 0.7.7 read a vanilla free cam here as "the user is flying again" and went back to
                    // SuspendedFlying. SexLab P+ Prism re-enters free cam within 0.5 s whenever it is off during
                    // a scene it tracks (SLP_PrismController.EnsureFreecam), so that aborted most resumes.
                    // User re-entries are now caught in OnFreeCamBegin / the request paths instead, and a
                    // vanilla free cam still on at the Director key is left (below) so SLCC's FCFW Start
                    // (which refuses while free cam is on) can run.
                    auto* cam = RE::PlayerCamera::GetSingleton();
                    if (!s_resumeForeignFcfw && FcfwBridge::FcfwOwnsCamera()) {
                        s_suspendedByUs = false;
                        SetState(State::kIdle, "an FCFW timeline owns the camera before TFCam pressed the Director key - "
                                               "SLCC's director was turned back on outside TFCam");
                        if (ResumeShowsSlcc()) Announce(Mode::kSLCC, "SLCC took the camera back itself");
                        s_announceSlcc = false;
                        return;
                    }
                    switch (PrepareInjection(now)) {
                    case Prep::kWait:
                        return;
                    case Prep::kFail:
                        s_suspendedByUs = false;
                        s_announceSlcc  = false;
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
                        s_announceSlcc  = false;
                        SetState(State::kIdle, "SLCC logged its director back ON during free-fly - not pressing the key again");
                        return;
                    }

                    if (!s_resumeKeepFreeCam && cam && cam->IsInFreeCameraMode() && !FcfwBridge::FcfwOwnsCamera()) {
                        SKSE::log::info("SLCC bridge: a vanilla free cam is on at the Director key (SexLab P+ / Prism "
                                        "re-enter free cam during their scenes) - leaving it so SLCC's FCFW camera can start");
                        ToggleFreeCam(cam);
                    }

                    s_slccLogMark = SlccLogSize();
                    InjectBinding(s_binding, true);
                    s_keyDownAt      = now;
                    s_lastInjectDown = now;
                    SetState(State::kResumeKeyUp, "Director key down sent to turn SLCC's director back ON");
                    s_announceSlcc = ResumeShowsSlcc();
                    if (s_announceSlcc) Announce(Mode::kSLCC, "SLCC director turned back on");
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
                    s_announceSlcc = false;
                    SetState(State::kIdle, "SLCC's FCFW camera is back");
                } else if (now - s_keyDownAt > kReleaseTimeout) {
                    ReadSlccLogSince(s_slccLogMark);
                    if (s_announceSlcc && SceneTracker::PlayerSceneActive()) {
                        Notify("TFCam: SLCC has not taken the camera back yet - see TFCam.log");
                    }
                    s_announceSlcc = false;
                    SetState(State::kIdle, "no FCFW timeline after 1.5 s (normal when no SexLab scene is running)");
                }
                return;
            }
        }

        // ------------------------------------------------------------------------------------
        // 0.7.8 camera cycle: one press = one step of TFCam -> SLCC -> Off -> TFCam. Main thread.
        // Called for TFCam's free-fly key, and for SexLab P+'s free camera key after P+'s own toggle
        // was seen hitting SLCC's camera (OnFreeCamBegin). The next mode comes from the live state:
        //   SLCC camera (FCFW owns it) -> Off if the cycle put SLCC there, else TFCam (scene start)
        //   TFCam free-fly             -> SLCC
        //   Off                        -> TFCam
        // With SexLab P+ Prism in a player scene there is no Off (Prism turns free cam back on within
        // 0.5 s, see PrismHoldsFreeCam): SLCC -> TFCam -> SLCC.
        // ------------------------------------------------------------------------------------
        void CycleStep(const char* a_source) {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam) return;

            switch (s_state) {
            case State::kIdle:
                if (FcfwBridge::FcfwOwnsCamera() && SlccHandlingApplies()) {
                    Mode target = s_cycleMode == Mode::kSLCC ? Mode::kOff : Mode::kTFCam;
                    if (target == Mode::kOff && PrismHoldsFreeCam()) {
                        SKSE::log::info("SLCC bridge: SexLab P+ Prism holds the free cam during this scene - Off would "
                                        "last 0.5 s, so the cycle goes SLCC -> TFCam");
                        target = Mode::kTFCam;
                    }
                    BeginEnter(target, std::string(a_source) + ": SLCC -> " + Name(target) + ", pausing SLCC's director");
                    return;
                }
                {
                    // SLCC is not driving (no timeline: its director is off, or it has not taken the scene).
                    const bool wasOn = cam->IsInFreeCameraMode();
                    ToggleFreeCam(cam);
                    const bool on = cam->IsInFreeCameraMode();
                    SKSE::log::info("SLCC bridge: {} - no SLCC camera to hand over; plain free cam toggle ({} -> {})",
                        a_source, wasOn ? "on" : "off", on ? "on" : "off");
                    if (on != wasOn) {
                        if (!on && PrismHoldsFreeCam()) {
                            // Prism's next poll turns it back on; OnFreeCamBegin says so when it does.
                            s_pplusOffAt = Now();
                            SKSE::log::info("SLCC bridge: SexLab P+ Prism holds the free cam during this scene - "
                                            "expecting it back within 0.5 s, 'Camera: Off' not shown");
                        } else {
                            Announce(on ? Mode::kTFCam : Mode::kOff, a_source);
                        }
                    }
                }
                return;

            case State::kEnterWaitGate:
                if (s_enterTarget == Mode::kTFCam) {
                    SetState(State::kIdle, std::string(a_source) + " again before the Director key - SLCC keeps the camera");
                    Announce(Mode::kSLCC, a_source);
                } else {
                    s_enterTarget = Mode::kTFCam;
                    SKSE::log::info("SLCC bridge: {} again before the Director key - Off -> TFCam", a_source);
                }
                return;

            case State::kSuspendedFlying:
                SKSE::log::info("SLCC bridge: {} - TFCam -> SLCC, leaving TFCam free-fly", a_source);
                if (cam->IsInFreeCameraMode()) {
                    ToggleFreeCam(cam);  // End hook starts the resume
                }
                if (s_state == State::kSuspendedFlying) {
                    BeginResume(false, true, std::string(a_source) + ": free cam already off / End hook not seen");
                }
                return;

            case State::kSuspendedOff:
                if (EnterFreeCam(false)) {
                    SetState(State::kSuspendedFlying, std::string(a_source) + ": Off -> TFCam free-fly");
                    Announce(Mode::kTFCam, a_source);
                } else {
                    SKSE::log::error("SLCC bridge: {} - ToggleFreeCameraMode did not enter free cam from Off", a_source);
                }
                return;

            case State::kResumeWaitGate:
                if (!s_resumeKeepFreeCam && PrismHoldsFreeCam()) {
                    // SLCC -> next before the director went back on. With Prism the next step is TFCam (see
                    // PrismHoldsFreeCam): keep the director paused and fly (Prism may have the free cam on already).
                    if (EnterFreeCam(false)) {
                        s_announceSlcc = false;
                        SetState(State::kSuspendedFlying, std::string(a_source) +
                                 " before SLCC was resumed: SLCC -> TFCam (Prism holds the free cam, no Off)");
                        Announce(Mode::kTFCam, a_source);
                    } else {
                        SKSE::log::error("SLCC bridge: {} - ToggleFreeCameraMode did not enter free cam; SLCC's "
                                         "director still goes back on", a_source);
                    }
                    return;
                }
                if (!s_resumeKeepFreeCam) {
                    // SLCC -> Off before the director went back on: simply do not turn it on.
                    if (cam->IsInFreeCameraMode() && !FcfwBridge::FcfwOwnsCamera()) {
                        ToggleFreeCam(cam);  // e.g. Prism put its free cam back meanwhile
                    }
                    s_announceSlcc = false;
                    SetState(State::kSuspendedOff, std::string(a_source) + " before SLCC was resumed: SLCC -> Off");
                    Announce(Mode::kOff, a_source);
                    return;
                }
                [[fallthrough]];

            default:
                SKSE::log::info("SLCC bridge: {} ignored while {}", a_source, Name(s_state));
                Notify("Camera: switching - press again in a moment");
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
                BeginEnter(Mode::kTFCam,
                    a_numParams ? "console tfc (argument ignored) while FCFW owns the camera"
                                : "console tfc while FCFW owns the camera");
                return true;
            case State::kEnterWaitGate:
                ConsolePrint("TFCam: free cam request cancelled.");
                SetState(State::kIdle, "console tfc again before the hand-over started - cancelled");
                return true;
            case State::kSuspendedFlying:
                SKSE::log::info("SLCC bridge: console tfc leaves TFCam free-fly (SLCC's director resumes after the console closes)");
                return false;  // vanilla exits the free cam; the End hook schedules the resume
            case State::kSuspendedOff:
                SetState(State::kSuspendedFlying, "console tfc in Off - TFCam free-fly, SLCC's director stays paused");
                Announce(Mode::kTFCam, "console tfc");
                return false;  // vanilla enters the free cam (FCFW is idle)
            case State::kResumeWaitGate:
                if (s_resumeKeepFreeCam) {
                    return false;  // scene-end / load restore: plain tfc, the director still goes back on
                }
                s_announceSlcc = false;
                SetState(State::kSuspendedFlying, "console tfc before SLCC was resumed - back to free-fly");
                Announce(Mode::kTFCam, "console tfc");
                return false;  // vanilla enters the free cam (FCFW is idle, nothing re-enters)
            default:
                ConsolePrint("TFCam: busy handing the camera over - try again in a moment.");
                SKSE::log::info("SLCC bridge: console tfc ignored while {}", Name(s_state));
                return true;
            }
        }

        // 0.7.9: a script's `tfc` (ConsoleUtil.ExecuteCommand and friends: the console is closed, and it runs on a
        // Papyrus VM thread) that would close a free camera it did not open, this soon after the Jump key, is refused.
        //
        // Found 2026-09-26: Poser Hotkeys Plus (mzinPoserHotKeysMainQuest) registers the Jump key and, with its
        // "Free Camera" option on, runs DisableFreeCam() on EVERY Jump press - posing or not - which is
        // `if Game.GetCameraState() == 3: MiscUtil.SetFreeCameraSpeed(10); ConsoleUtil.ExecuteCommand("tfc")`.
        // So Space closed any free camera (TFCam, SexLab P+ / Prism, SLCC's FCFW camera), and during an SLCC scene
        // the same `tfc` read as the user's console tfc and started a TFCam <-> SLCC hand-over on each Space press.
        // Papyrus latency: SexLab P+'s own key toggles landed 44-65 ms after the press in the same session; 5 s
        // leaves room for a busy VM and is logged, so a miss shows up in TFCam.log.
        constexpr double kScriptTfcJumpWindowSec = 5.0;

        bool TfcExecute(const RE::SCRIPT_PARAMETER* a_paramInfo, RE::SCRIPT_FUNCTION::ScriptData* a_scriptData,
            RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR* a_containingObj, RE::Script* a_scriptObj,
            RE::ScriptLocals* a_locals, double& a_result, std::uint32_t& a_opcodeOffsetPtr) {
            if (!ConsoleOpen()) {
                auto*      cam   = RE::PlayerCamera::GetSingleton();
                const bool wasOn = cam && cam->IsInFreeCameraMode();
                double     msAgo = -1.0;
                const bool jump  = FreeCam::JumpPressedWithin(kScriptTfcJumpWindowSec, msAgo);
                const std::string jumpText = msAgo < 0.0 ? std::string("never") : std::format("{:.0f} ms ago", msAgo);
                if (wasOn && jump && !FreeCam::SessionOpenedByScriptTfc()) {
                    FreeCam::ReapplyCameraSpeed();  // Poser set the free camera speed to 10 just before this tfc
                    SKSE::log::info("Scripted tfc REFUSED: a script tried to close a free camera it did not open, {} after the "
                                    "Jump key (Poser Hotkeys Plus closes any free camera on Jump) - free cam stays [FCFW "
                                    "timeline {} | {}]", jumpText, FcfwBridge::ActiveTimelineID(), FreeCam::ThreadTag());
                    return true;
                }
                SKSE::log::info("Scripted tfc (console closed) - free cam {} [last Jump press {}{} | FCFW timeline {} | {}]",
                    wasOn ? "on, this turns it off" : "off, this turns it on", jumpText,
                    wasOn && jump ? ", but this free cam was opened by a script's tfc" : "",
                    FcfwBridge::ActiveTimelineID(), FreeCam::ThreadTag());
                const bool result = s_origTfc(a_paramInfo, a_scriptData, a_thisObj, a_containingObj, a_scriptObj,
                    a_locals, a_result, a_opcodeOffsetPtr);
                if (!wasOn && cam && cam->IsInFreeCameraMode()) {
                    FreeCam::NoteScriptTfcOpenedSession();  // e.g. Poser's pose camera: its own Jump exit stays allowed
                }
                return result;
            }
            // Typed in the console: 0.7.7's SLCC hand-over (a scripted tfc never starts one any more).
            if (s_installed && OnConsoleTfc(a_scriptData ? a_scriptData->numParams : 0)) {
                return true;
            }
            return s_origTfc(a_paramInfo, a_scriptData, a_thisObj, a_containingObj, a_scriptObj, a_locals,
                a_result, a_opcodeOffsetPtr);
        }

        // The vanilla command is "ToggleFlyCam" / "tfc" (strings checked in SkyrimSE.exe 1.6.1170:
        // "ToggleFlyCam\0...tfc\0...Toggles the Free Fly camera"). Fall back to the short name.
        // Data write to the command table (the execute pointer), not a code patch.
        void WrapConsoleTfc() {
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
                SKSE::log::info("Console {} ({}) wrapped (scripted-tfc guard; SLCC hand-over when SLCC is armed)",
                    cmd->functionName ? cmd->functionName : "?", cmd->shortName ? cmd->shortName : "?");
            } else {
                SKSE::log::warn("Console ToggleFlyCam (tfc) not found - no scripted-tfc guard, and the End/Begin re-entry "
                                "detector stands in for tfc during SLCC scenes");
            }
        }

        // CommonLib-NG 3.7.0's LookupLoadedLightModByName counts light plugins in a uint8_t, so with more than 255
        // light plugins it only searches the first (count % 256): SexLabPPrism.esp (ESL-flagged) read as "not active"
        // on 2026-09-26 while Prism's quest scripts were running. LookupModByName walks every known file instead;
        // compileIndex 0xFF = not loaded (0xFE = loaded light plugin).
        bool PluginLoaded(std::string_view a_name) {
            auto*       dh   = RE::TESDataHandler::GetSingleton();
            const auto* file = dh ? dh->LookupModByName(a_name) : nullptr;
            return file && file->GetCompileIndex() != 0xFF;
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
        // 0.7.9: the console tfc wrapper is installed for everyone - it carries the scripted-tfc guard (Poser
        // Hotkeys Plus closing free cam on Jump). Its SLCC hand-over part only runs once the bridge is armed below.
        WrapConsoleTfc();

        // TFCam ships to users without SLCC: without FCFW + SLCCNative.dll every request stays the plain
        // pre-0.7.7 toggle.
        if (!FcfwBridge::Available() || !SlccLoaded()) {
            SKSE::log::info("SLCC bridge: inactive (FCFW API {}, SLCCNative.dll {}) - plain free cam toggles",
                FcfwBridge::Available() ? "present" : "missing", SlccLoaded() ? "loaded" : "not loaded");
            return;
        }
        s_installed = true;
        SKSE::log::info("SLCC bridge: armed (FCFW API present, SLCCNative.dll loaded)");
        RefreshPplusKey("data loaded");

        // SexLab P+ Prism: its Papyrus controller (SexLabPPrism.esp) keeps free cam on during player scenes.
        {
            const bool dll    = ::GetModuleHandleA("SexLabPPrism.dll") != nullptr;
            const bool plugin = PluginLoaded("SexLabPPrism.esp");
            s_prismPresent = dll && plugin;
            SKSE::log::info("SLCC bridge: SexLab P+ Prism {} (SexLabPPrism.dll {}, SexLabPPrism.esp {}){}",
                s_prismPresent ? "present" : "not present", dll ? "loaded" : "not loaded",
                plugin ? "active" : "not active",
                s_prismPresent ? " - it holds the free cam in player scenes, so the camera cycle there is TFCam <-> SLCC" : "");
        }
    }

    // SLUI 'TFCF': TFCam free-fly <-> SLCC, as in 0.7.7 (plus the Off state the cycle can leave behind).
    void RequestToggle(const char* a_source) {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) return;

        if (!s_installed) {
            ToggleFreeCam(cam);
            return;
        }

        switch (s_state) {
        case State::kIdle:
            if (SlccHandlingApplies()) {
                BeginEnter(Mode::kTFCam, std::string(a_source) + " while FCFW owns the camera");
                return;
            }
            SKSE::log::info("SLCC bridge: {} - plain free cam toggle", a_source);
            ToggleFreeCam(cam);
            return;

        case State::kEnterWaitGate:
            SetState(State::kIdle, std::string(a_source) + " again before the hand-over started - cancelled");
            return;

        case State::kSuspendedFlying:
            if (cam->IsInFreeCameraMode()) {
                SKSE::log::info("SLCC bridge: {} - leaving TFCam free-fly", a_source);
                ToggleFreeCam(cam);  // End hook moves us to ResumeWaitGate
            }
            if (s_state == State::kSuspendedFlying) {
                BeginResume(false, true, std::string(a_source) + ": not in free cam any more / End hook not seen");
            }
            return;

        case State::kSuspendedOff:
        case State::kResumeWaitGate:
            if (s_state == State::kResumeWaitGate && s_resumeKeepFreeCam) {
                SKSE::log::info("SLCC bridge: {} ignored - SLCC's director is being restored after the scene", a_source);
                return;
            }
            if (EnterFreeCam(false)) {
                s_announceSlcc = false;
                SetState(State::kSuspendedFlying, std::string(a_source) + " - TFCam free-fly, SLCC's director stays paused");
                Announce(Mode::kTFCam, a_source);
            }
            return;

        default:
            SKSE::log::info("SLCC bridge: {} ignored while {}", a_source, Name(s_state));
            return;
        }
    }

    // 0.7.8: TFCam's free-fly key. Camera cycle while a player SexLab scene / SLCC camera / hand-over is
    // live, the plain toggle it always was otherwise (no SLCC, or outside scenes).
    void RequestCycle(const char* a_source) {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) return;
        if (!CycleContext()) {
            if (s_installed) {
                SKSE::log::info("SLCC bridge: {} - no player SexLab scene or SLCC camera - plain free cam toggle", a_source);
            }
            ToggleFreeCam(cam);
            return;
        }
        CycleStep(a_source);
    }

    void OnFreeCamBegin(bool a_tfcamDriving) {
        if (!s_installed || s_selfToggle) return;  // our own toggles: the caller sets the state
        const double now = Now();

        if (!a_tfcamDriving) {
            // An FCFW-owned Begin right after an FCFW-owned End = FCFW re-entering its carrier the frame
            // after someone toggled free cam off under a running timeline (FCFW's ToggleFreeCamera detour).
            const bool reentry = s_lastEndFcfwOwned && now - s_lastEndAt <= kReentryPairSec;

            // 0.7.8: SexLab P+'s free camera key. P+'s own toggle already ran (and did nothing visible: FCFW
            // put SLCC's camera straight back), so this is the moment to step the cycle. Gated on a physical
            // press of P+'s key: P+ also toggles free cam off by itself (UnlockActor / Prism Cleanup at scene
            // end, DisableHotkeys), which makes exactly the same End/Begin pair while SLCC still plays.
            // The late window applies: only a free cam toggle under SLCC's running timeline makes this pair,
            // so a press whose Papyrus toggle arrived seconds later (VM backlog) still counts.
            if (reentry && PplusPressWithinLate(now) &&
                (s_state == State::kIdle || s_state == State::kEnterWaitGate) && SlccHandlingApplies()) {
                LogPplusMatch(now, "FCFW's re-entry of SLCC's camera");
                ConsumePplusPress();
                SKSE::log::info("SLCC bridge: SexLab P+'s free camera key toggled SLCC's camera (FCFW re-entered it) "
                                "- stepping the camera cycle");
                CycleStep("SexLab P+ free camera key");
                return;
            }

            // Fallback only when the console command could not be wrapped: an FCFW-owned End followed within
            // 250 ms by an FCFW-owned Begin is read as the user's tfc. SLCC's scene-start takeover is excluded:
            // that End happens while no timeline is active (SLCC exits the external free cam first, then starts
            // playback). 0.7.8: the console must be open, so P+'s scene-end toggle is never read as tfc.
            if (!s_tfcHooked && s_state == State::kIdle && reentry && now - s_lastEndAt <= kFallbackPairSec &&
                ConsoleOpen() && SlccHandlingApplies()) {
                BeginEnter(Mode::kTFCam, "FCFW re-entered right after an FCFW-owned exit with the console open - "
                                         "treating it as console tfc (fallback)");
                return;
            }
            if (reentry && s_state == State::kIdle) {
                SKSE::log::info("SLCC bridge: FCFW re-entered its camera after a free cam toggle with no P+ key press "
                                "in the last {:.0f} s (P+ / Prism at scene end, Prism's First Person button, or a mod) - "
                                "not a cycle step", kPplusLateWindow);
            }

            // SLCC took the camera during Off: the user pressed SLCC's own Director key.
            if (s_state == State::kSuspendedOff && ReadSlccLogSince(s_slccFlyingMark) == SlccSaid::kEnabled) {
                s_suspendedByUs = false;
                SetState(State::kIdle, "SLCC's director was turned back on outside TFCam - SLCC has the camera");
                Announce(Mode::kSLCC, "SLCC's own Director key");
            }
            return;
        }

        // A TFCam-driven free cam that TFCam did not start itself: P+'s key, P+ / Prism auto free cam, console.
        const bool pplus = PplusPressFresh(now);
        switch (s_state) {
        case State::kSuspendedOff:
            if (pplus) ConsumePplusPress();
            SetState(State::kSuspendedFlying,
                pplus ? "SexLab P+'s free camera key entered free cam: Off -> TFCam"
                      : "another mod entered free cam during Off (SexLab P+ Prism re-enters free cam while it tracks a "
                        "scene) - TFCam drives it");
            Announce(Mode::kTFCam, pplus ? "SexLab P+ free camera key" : "free cam re-entered by another mod");
            return;

        case State::kResumeWaitGate:
            if (!s_resumeKeepFreeCam && (pplus || ConsoleOpen())) {
                if (pplus) ConsumePplusPress();
                s_announceSlcc = false;
                SetState(State::kSuspendedFlying, "free cam entered by the user before SLCC was resumed - keep flying");
                Announce(Mode::kTFCam, pplus ? "SexLab P+ free camera key" : "console");
            } else {
                SKSE::log::info("SLCC bridge: free cam entered by another mod while SLCC's director is being turned "
                                "back on (SexLab P+ Prism re-enters free cam during its scenes) - resume continues");
            }
            return;

        case State::kIdle:
            if (pplus && SceneTracker::PlayerSceneActive()) {
                ConsumePplusPress();
                s_pplusOffAt = -1000.0;
                Announce(Mode::kTFCam, "SexLab P+'s free camera key; SLCC is not driving this scene");
            } else if (now - s_pplusOffAt <= kPrismReentrySec && PrismHoldsFreeCam()) {
                // The press turned free cam off (OnFreeCamEnd / CycleStep) and Prism's poll put it back.
                s_pplusOffAt = -1000.0;
                Announce(Mode::kTFCam, "SexLab P+ Prism turned the free cam back on after the key turned it off",
                    " (Prism keeps free cam on in scenes)");
            }
            return;

        default:
            return;
        }
    }

    void OnFreeCamEnd(bool a_fcfwOwnedAtEnd) {
        const double now   = Now();
        s_lastEndFcfwOwned = a_fcfwOwnedAtEnd;
        s_lastEndAt        = now;
        if (!s_installed) return;

        if (s_state == State::kSuspendedFlying) {
            // User exits (TFCam key / 'TFCF' = our own toggle, P+'s key, console tfc) step the cycle to SLCC.
            // P+ / Prism turning free cam off at scene end is not announced: there is no scene left to film.
            // Late window: a P+ toggle that took seconds through the Papyrus VM still counts as the press.
            // (Unmatched, the step still happens: BeginResume records SLCC for any in-scene resume.)
            const bool pplus = !s_selfToggle && PplusPressWithinLate(now);
            if (pplus) {
                LogPplusMatch(now, "the exit from TFCam free-fly");
                ConsumePplusPress();
            }
            const bool user = s_selfToggle || pplus || ConsoleOpen();
            BeginResume(false, user,
                pplus ? "SexLab P+'s free camera key left TFCam free-fly - SLCC's director goes back on once gameplay input is open"
                : user ? "TFCam free-fly ended - SLCC's director goes back on once gameplay input is open"
                       : "free cam turned off by another mod (SexLab P+ / Prism, e.g. at scene end) - SLCC's director goes back on");
            return;
        }

        if (s_state == State::kIdle && !a_fcfwOwnedAtEnd && !s_selfToggle && PplusPressFresh(now) &&
            SceneTracker::PlayerSceneActive()) {
            ConsumePplusPress();
            if (PrismHoldsFreeCam()) {
                // Prism's poll turns free cam back on within 0.5 s; OnFreeCamBegin announces it when it does.
                s_pplusOffAt = now;
                SKSE::log::info("SLCC bridge: SexLab P+'s free camera key turned free cam off while SLCC is not driving; "
                                "SexLab P+ Prism holds the free cam during this scene - 'Camera: Off' not shown");
            } else {
                Announce(Mode::kOff, "SexLab P+'s free camera key; SLCC is not driving this scene");
            }
        }
    }

    void OnGameLoaded(const char* a_why) {
        FreeCam::ClearEntryPose();
        s_cycleMode         = Mode::kNone;
        s_announceSlcc      = false;
        s_restoreAfterEnter = false;
        s_pplusOffAt        = -1000.0;
        ConsumePplusPress();
        RefreshPplusKey(a_why);
        if (!s_installed) return;
        // SLCC's director on/off is native, process-wide state: SLCC documents it as persistent desired
        // state and documents resets only on plugin/process reload - a save load does not turn it back
        // on. So a director we turned off is turned back on here.
        if (s_state == State::kResumeKeyUp || s_state == State::kResumeVerify) {
            return;  // the director is already being turned back on; let that finish
        }
        if (s_state == State::kEnterKeyUp) {
            InjectBinding(s_binding, false);  // never leave SLCC's key held down
        }
        if (s_suspendedByUs) {
            BeginResume(true, false, std::string(a_why) + " while SLCC's director was paused by TFCam - restoring it");
        } else if (s_state != State::kIdle) {
            SetState(State::kIdle, std::string(a_why) + " - pending hand-over dropped");
        }
    }

    // The UI-task hop normally re-arms the step every frame; this only matters if it ever did not.
    void Pump() {
        if (NeedsStepping()) Schedule();
        // A press of P+'s key during a scene that no free cam toggle ever matched: say so, so a lost press
        // can be told apart from one TFCam misread (TFCam.log).
        if (PplusPressArmed()) {
            const double now = Now();
            if (now - s_pplusPressAt > kPplusLateWindow) {
                if (s_pplusPressInCycle) {
                    SKSE::log::info("SLCC bridge: SexLab P+ free camera key press expired - no free cam toggle "
                                    "matched it within {:.0f} s [{} | cycle {} | FCFW timeline {}]",
                        kPplusLateWindow, Name(s_state), Name(s_cycleMode), FcfwBridge::ActiveTimelineID());
                }
                ConsumePplusPress();
            }
        }
    }

    bool IsInjecting() { return s_injecting; }

    // ----------------------------------------------------------------------------------------
    // 0.7.8
    // ----------------------------------------------------------------------------------------

    void NoteKeyDown(std::uint32_t a_dikCode) {
        if (!s_installed || s_pplusKey == 0 || a_dikCode != s_pplusKey) return;
        if (IsBusy(s_state)) {
            // Mid hand-over the press is not a cycle step, and arming it would let it match an unrelated
            // exit later. P+ still toggles the vanilla free cam on it (its own Papyrus key handler).
            ConsumePplusPress();
            SKSE::log::info("SLCC bridge: SexLab P+ free camera key (0x{:X}) pressed during the hand-over [{}] - "
                            "not a cycle step", a_dikCode, Name(s_state));
            Notify("Camera: switching - press again in a moment");
            return;
        }
        s_pplusPressAt      = Now();
        s_pplusPressInCycle = CycleContext();
        SKSE::log::info("SLCC bridge: SexLab P+ free camera key (0x{:X}) pressed [{} | cycle {} | FCFW timeline {}]",
            a_dikCode, Name(s_state), Name(s_cycleMode), FcfwBridge::ActiveTimelineID());
    }

    bool IsPplusFreeCamKey(std::uint32_t a_dikCode) {
        return s_pplusKey != 0 && a_dikCode == s_pplusKey;
    }

    void OnPlayerSceneStart(int a_tid) {
        RefreshPplusKey("player scene start");
        s_cycleMode = Mode::kNone;  // SLCC starts on: the first press of the scene goes to TFCam
        if (!s_installed) return;
        SKSE::log::info("SLCC bridge: player SexLab scene (thread {}) started - camera cycle reset [{}]", a_tid, Name(s_state));
        if (s_suspendedByUs && s_state == State::kSuspendedOff) {
            // Only if the end-of-scene restore was missed.
            BeginResume(true, false, "a player scene started while SLCC's director was still paused by TFCam - restoring it");
        }
    }

    // STEP 3: Off / free-fly must not leave SLCC's director off for the next scene.
    void OnPlayerSceneEnd(int a_tid) {
        s_cycleMode  = Mode::kNone;
        s_pplusOffAt = -1000.0;
        ConsumePplusPress();  // a press right at scene end must not be matched to P+'s scene-end toggle
        if (!s_installed) return;
        SKSE::log::info("SLCC bridge: player SexLab scene (thread {}) ended [{}, director paused by TFCam: {}]", a_tid,
            Name(s_state), s_suspendedByUs);
        switch (s_state) {
        case State::kSuspendedOff:
        case State::kSuspendedFlying:
            BeginResume(true, false, "the player's SexLab scene ended while SLCC's director was paused by TFCam - "
                                     "turning it back on for the next scene");
            break;
        case State::kResumeWaitGate:
            s_resumeKeepFreeCam = true;  // no scene left for SLCC: leave whatever camera the user has
            s_announceSlcc      = false;
            break;
        case State::kResumeKeyUp:
        case State::kResumeVerify:
            s_announceSlcc = false;
            break;
        case State::kEnterWaitGate:
            SetState(State::kIdle, "the player's SexLab scene ended before the Director key was pressed - hand-over cancelled");
            break;
        case State::kEnterKeyUp:
        case State::kEnterWaitRelease:
            s_restoreAfterEnter = true;
            SKSE::log::info("SLCC bridge: the Director-OFF press is in flight - SLCC's director goes back on right after it");
            break;
        default:
            break;
        }
    }
}
