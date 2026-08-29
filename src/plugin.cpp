#include "FreeCamController.h"
#include "FreeCamMenu.h"

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
    bool AddressLibraryPresent() {
        const auto ver = REL::Module::get().version();
        std::string file;
        if (ver.major() == 1 && ver.minor() < 6) {
            file = std::format("Data/SKSE/Plugins/version-{}-{}-{}-{}.bin",
                               ver.major(), ver.minor(), ver.patch(), ver.build());
        } else {
            file = std::format("Data/SKSE/Plugins/versionlib-{}-{}-{}-{}.bin",
                               ver.major(), ver.minor(), ver.patch(), ver.build());
        }
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::current_path() / file, ec)) {
            return true;
        }
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
        case SKSE::MessagingInterface::kDataLoaded:
            if (!AddressLibraryPresent()) {
                break;
            }
            FreeCam::Install();
            FreeCamMenu::Register();
            FreeCamMenu::ApplyGameSettings();
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    InitializeLogging();

    SKSE::log::info("FreeCam v{} loaded", "0.7.0");

    FreeCamMenu::LoadSettings();

    auto* messaging = SKSE::GetMessagingInterface();
    if (messaging) {
        messaging->RegisterListener(MessageHandler);
    }

    return true;
}
