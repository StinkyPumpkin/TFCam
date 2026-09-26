#include "FreeCamController.h"
#include "FreeCamMenu.h"
#include "FcfwBridge.h"
#include "SlccBridge.h"
#include "SceneTracker.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <filesystem>
#include <cstdio>
#include <format>

namespace {
    std::filesystem::path ResolveLogDirectory() {
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

    void MessageHandler(SKSE::MessagingInterface::Message* a_msg) {
        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            // 0.7.7: every plugin is loaded now - FCFW's API and SLUI's 'TFCF' messages.
            FcfwBridge::Init();
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
            FreeCam::Install();
            SlccBridge::Install();
            SceneTracker::Register();  // 0.7.8: player SexLab scenes for the SLCC camera cycle
            FreeCamMenu::Register();
            FreeCamMenu::ApplyGameSettings();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            SceneTracker::Reset("save loaded");
            SlccBridge::OnGameLoaded("save loaded");
            break;
        case SKSE::MessagingInterface::kNewGame:
            SceneTracker::Reset("new game");
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
    SKSE::log::info("built on CommonLibSSE-NG {}", TFCAM_COMMONLIB_VERSION);

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
