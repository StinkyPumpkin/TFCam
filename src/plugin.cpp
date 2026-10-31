#include "FreeCamController.h"
#include "FreeCamMenu.h"
#include "FcfwBridge.h"
#include "SlccBridge.h"
#include "SceneTracker.h"
#include "CellEntryCam.h"
#include "SceneCam.h"
#include "AmbientCam.h"
#include "ConvoCam.h"
#include "CamBlur.h"
#include "CamPath.h"
#include "AimDot.h"
#include "FixedCam.h"
#include "FrameFx.h"
#include "FovFx.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <filesystem>
#include <cstdio>
#include <format>

namespace {
    // 0.13.1: SKSE's own log folder first - it follows the running game (Skyrim Special Edition, GOG, VR). The fixed
    // 'Skyrim Special Edition' folder below is only the fallback, and what 0.13.0 and older always used.
    std::filesystem::path ResolveLogDirectory() {
        if (const auto skseDir = SKSE::log::log_directory(); skseDir && !skseDir->empty()) {
            return *skseDir;
        }
        wchar_t* docs = nullptr;
        std::filesystem::path p;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs))) {
            p = docs;
            ::CoTaskMemFree(docs);
        } else {
            const wchar_t* up = _wgetenv(L"USERPROFILE");
            if (up) p = std::filesystem::path(up) / "Documents";
        }
        p /= "My Games";
        p /= "Skyrim Special Edition";
        p /= "SKSE";
        return p;
    }

    void InitializeLogging() {
        auto logDir = ResolveLogDirectory();
        std::error_code ec;
        std::filesystem::create_directories(logDir, ec);

        auto logPath = logDir / "TFCam.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
        auto log  = std::make_shared<spdlog::logger>("TFCam", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        spdlog::set_default_logger(std::move(log));
    }

    // 2026-08-28 (new game update released): CommonLib fatally terminates the game with a
    // cryptic popup when the Address Library database for the RUNNING runtime is absent
    // (e.g. a user on the new patch without the updated Address Library). Check for the
    // file ourselves before any REL-dependent install; if missing, stay inert and say so
    // plainly — the game keeps running without TFCam.
    std::string AddressLibraryFile() {
        const auto ver = REL::Module::get().version();
        // Same file CommonLib's IDDB::load() opens: VR reads a .csv, AE a versionlib .bin,
        // SE a version .bin. (0.7.0 looked for a .bin on VR, so VR users got the "missing" popup.)
        // 0.8.0 (CommonLibSSE-NG 9.x): NG classifies minor >= 6 as AE, so 1.7.x is AE here too and the file is
        // versionlib-1-7-104-0.bin - the Address Library v5 database (format 5), which NG 9.x parses.
        if (REL::Module::IsVR()) {
            return std::format("Data/SKSE/Plugins/version-{}.csv", ver.string());
        }
        if (REL::Module::IsAE()) {
            return std::format("Data/SKSE/Plugins/versionlib-{}.bin", ver.string());
        }
        return std::format("Data/SKSE/Plugins/version-{}.bin", ver.string());
    }

    // Quiet check (no REL::ID lookup, no popup) - safe at kPostLoad.
    bool AddressLibraryFileExists() {
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::current_path() / AddressLibraryFile(), ec);
    }

    bool AddressLibraryPresent() {
        if (AddressLibraryFileExists()) {
            return true;
        }
        const auto ver = REL::Module::get().version();
        const std::string file = AddressLibraryFile();
        SKSE::log::error("Address Library missing for runtime {}.{}.{}.{} ({}) - TFCam disabled",
                         ver.major(), ver.minor(), ver.patch(), ver.build(), file);
        const std::string text = std::format(
            "TFCam: the Address Library file for your game version ({}.{}.{}.{}) is not "
            "installed, so TFCam has been disabled.\n\nInstall/update \"Address Library for "
            "SKSE Plugins\" for this game version.\n\nThe game will continue to run normally.",
            ver.major(), ver.minor(), ver.patch(), ver.build());
        ::MessageBoxA(nullptr, text.c_str(), "TFCam", MB_OK | MB_ICONWARNING);
        return false;
    }

    // 0.14.1: does this DLL's runtime variant (TFCAM_RUNTIME_NAME) fit the running game? ALL fits every one.
    bool RuntimeMatchesBuild(const REL::Version& a_runtime) {
        const std::string_view build = TFCAM_RUNTIME_NAME;
        if (build == "ALL") return true;
        if (build == "SE") return a_runtime.minor() == 5;   // Skyrim SE 1.5.97
        return a_runtime.minor() >= 6;                      // AE: 1.6.x and 1.7.x
    }

    void MessageHandler(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            // 0.7.7: every plugin is loaded now - FCFW's API and SLUI's 'TFCF' messages.
            FcfwBridge::Init();
            CamBlur::Install();  // 0.17.0: ENB SDK for the background blur (retried at kDataLoaded)
            // Listen for SLUI's 'TFCF' only if TFCam will arm at kDataLoaded. Without the Address
            // Library TFCam stays inert; with no listener SLUI's Dispatch returns false and SLUI does
            // its own plain toggle, instead of the request being accepted and silently dropped.
            if (AddressLibraryFileExists()) {
                SlccBridge::OnPostLoad();
            } else {
                SKSE::log::info("SLCC bridge: Address Library missing - no 'TFCF' listener (SLUI toggles on its own)");
            }
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            if (!AddressLibraryPresent()) {
                break;
            }
            // One reservation for every trampoline hook: CommonLib sets the trampoline up only on the FIRST
            // AllocTrampoline call, so a second call adds nothing. 14 bytes per write_call<5>: the camera-root hook +
            // FrameFx + AimDot's 6 (launch angles, flame yaw/pitch, beam yaw/pitch, projectile init).
            SKSE::AllocTrampoline(8 * 14);
            FreeCam::Install();
            CellEntryCam::Install();  // 0.9.0: zoom + orbit after a loading screen into a new cell
            SlccBridge::Install();
            SceneTracker::Register();  // 0.7.8: player SexLab scenes for the SLCC camera cycle
            SceneCam::Install();       // 0.10.0: orbit / eye view / saved shots
            AmbientCam::Install();     // 0.11.0: idle camera + furniture camera
            ConvoCam::Install();       // 0.16.0: conversation camera
            CamBlur::Install();        // 0.17.0: no-op when already linked at kPostLoad
            FixedCam::Install();       // 0.18.0: the camera-root hook (Action FOV's frame tick + the archer camera)
            FovFx::Install();          // 0.18.0: action FOV + archer camera
            FrameFx::Install();        // 0.18.0: edge blur, archer vignette, aim dot - before the UI
            AimDot::Install();         // 0.18.0: arrows and spells go where the dot is (unless Hit Where You Aim is loaded)
            FreeCamMenu::Register();
            FreeCamMenu::ApplyGameSettings();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            CellEntryCam::OnPreLoadGame();  // the loading screen that follows is a save load, not a door
            FovFx::Reset();                 // 0.18.0
            // 0.10.0: the scene, orbit / eye view (head cull, near clip) and the active pose end with the old game.
            SceneTracker::Reset("save loading");
            SceneCam::OnGameLoad("save loading");
            AmbientCam::OnGameLoad("save loading");
            ConvoCam::OnGameLoad("save loading");
            CamPath::OnGameLoad();       // 0.17.0
            CamBlur::Off("save loading");  // 0.17.0
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            SceneTracker::Reset("save loaded");
            SceneCam::OnGameLoad("save loaded");
            AmbientCam::OnGameLoad("save loaded");
            ConvoCam::OnGameLoad("save loaded");
            CamPath::OnGameLoad();       // 0.17.0
            CamBlur::Off("save loaded");  // 0.17.0
            SlccBridge::OnGameLoaded("save loaded");
            break;
        case SKSE::MessagingInterface::kNewGame:
            SceneTracker::Reset("new game");
            SceneCam::OnGameLoad("new game");
            AmbientCam::OnGameLoad("new game");
            ConvoCam::OnGameLoad("new game");
            CamPath::OnGameLoad();       // 0.17.0
            CamBlur::Off("new game");  // 0.17.0
            SlccBridge::OnGameLoaded("new game");
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    // 0.8.0: CommonLibSSE-NG 9.x's Init opens its own <plugin>.log by default; TFCam keeps its own logger (same
    // file, same format as 0.7.x), so NG's is switched off.
    SKSE::Init(skse, { .log = false });
    InitializeLogging();

    SKSE::log::info("TFCam v{} loaded", TFCAM_VERSION);
    SKSE::log::info("built on CommonLibSSE-NG {}, runtime variant {}", TFCAM_COMMONLIB_VERSION, TFCAM_RUNTIME_NAME);

    // 0.14.1: the installer ships one DLL per runtime (SE 1.5.97, AE 1.6.x / 1.7.x) next to the all-in-one
    // one. A variant built for another runtime would call the wrong game addresses, so it stays out instead.
    if (!RuntimeMatchesBuild(skse->RuntimeVersion())) {
        const auto v = skse->RuntimeVersion();
        SKSE::log::error("This TFCam.dll is the {} build but the game is {}.{}.{}.{} - TFCam disabled. Install the matching "
                         "option of the TFCam installer (or the 'all versions' one).",
            TFCAM_RUNTIME_NAME, v.major(), v.minor(), v.patch(), v.build());
        ::MessageBoxA(nullptr,
            "TFCam: this TFCam.dll was built for a different Skyrim version than the one running, so it has been "
            "disabled.\n\nReinstall TFCam and pick the option that matches your game (or 'All versions').\n\n"
            "The game will continue to run normally.",
            "TFCam", MB_OK | MB_ICONWARNING);
        return true;  // loaded but inert: SKSE keeps going and TFCam registers nothing
    }

    // 0.7.9: SKSEPluginLoad runs on the game's main thread (kDataLoaded runs on the data-loading thread), so this
    // is where the diagnostics learn which thread is "main".
    FreeCam::NoteMainThread();
    SKSE::log::info("Main thread: {}", FreeCam::ThreadTag());

    FreeCamMenu::LoadSettings();

    auto* messaging = SKSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(MessageHandler);
    }

    return true;
}
