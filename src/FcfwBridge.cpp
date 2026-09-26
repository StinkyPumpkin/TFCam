#include "FcfwBridge.h"

#include <Windows.h>

#include "FCFW_API.h"

namespace FcfwBridge {

    static FCFW_API::IVFCFW1* s_api = nullptr;

    void Init() {
        if (s_api) return;

        HMODULE mod = ::GetModuleHandleA("FreeCameraFramework.dll");
        if (!mod) {
            SKSE::log::info("FCFW: FreeCameraFramework.dll not loaded - SLCC/FCFW integration off");
            return;
        }
        auto request = reinterpret_cast<FCFW_API::_RequestPluginAPI>(::GetProcAddress(mod, "RequestPluginAPI"));
        if (!request) {
            SKSE::log::warn("FCFW: FreeCameraFramework.dll has no RequestPluginAPI export - integration off");
            return;
        }
        s_api = static_cast<FCFW_API::IVFCFW1*>(request(FCFW_API::InterfaceVersion::V1));
        if (!s_api) {
            SKSE::log::warn("FCFW: RequestPluginAPI(V1) returned null - integration off");
            return;
        }

        // The vendored header's slot order was checked against the build that reports 10000
        // (see include/FCFW_API.h). Another build still gets used, but say so in the log.
        const int ver = s_api->GetFCFWPluginVersion();
        if (ver == 10000) {
            SKSE::log::info("FCFW: API V1 acquired (plugin version {}, vtable verified for this build)", ver);
        } else {
            SKSE::log::warn("FCFW: API V1 acquired but plugin version is {} - TFCam's IVFCFW1 layout was "
                            "verified against 10000 only", ver);
        }
    }

    bool Available() { return s_api != nullptr; }

    std::size_t ActiveTimelineID() {
        return s_api ? s_api->GetActiveTimelineID() : 0;
    }

    bool FcfwOwnsCamera() {
        return ActiveTimelineID() != 0;
    }
}
