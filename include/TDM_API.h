#pragma once

// True Directional Movement's modder API, version 1 only (the part TFCam reads: is the target lock on).
// From the API header True Directional Movement by Ershin ships for other mods to copy
// (https://github.com/ersh1/TrueDirectionalMovement, TrueDirectionalMovementAPI.h). The class layout below must
// stay exactly as TDM declares it: the calls go through its vtable.

#include <Windows.h>

namespace TDM_API {

    enum class InterfaceVersion : std::uint8_t { V1, V2, V3, V4, V5 };

    enum class APIResult : std::uint8_t { OK, NotOwner, MustKeep, AlreadyGiven, AlreadyTaken, BadThread };

    class IVTDM1 {
    public:
        [[nodiscard]] virtual unsigned long   GetTDMThreadId() const noexcept = 0;
        [[nodiscard]] virtual bool            GetDirectionalMovementState() const noexcept = 0;
        [[nodiscard]] virtual bool            GetTargetLockState() const noexcept = 0;
        [[nodiscard]] virtual RE::ActorHandle GetCurrentTarget() const noexcept = 0;
        [[nodiscard]] virtual APIResult       RequestDisableDirectionalMovement(SKSE::PluginHandle a_handle) noexcept = 0;
        [[nodiscard]] virtual APIResult       RequestDisableHeadtracking(SKSE::PluginHandle a_handle) noexcept = 0;
        virtual SKSE::PluginHandle            GetDisableDirectionalMovementOwner() const noexcept = 0;
        virtual SKSE::PluginHandle            GetDisableHeadtrackingOwner() const noexcept = 0;
        virtual APIResult                     ReleaseDisableDirectionalMovement(SKSE::PluginHandle a_handle) noexcept = 0;
        virtual APIResult                     ReleaseDisableHeadtracking(SKSE::PluginHandle a_handle) noexcept = 0;
    };

    using _RequestPluginAPI = void* (*)(InterfaceVersion a_version);

    // At kPostLoad or later (TDM's DLL must be loaded). Null when TDM is not installed or has no API.
    [[nodiscard]] inline IVTDM1* RequestV1() {
        HMODULE mod = ::GetModuleHandleA("TrueDirectionalMovement.dll");
        if (!mod) return nullptr;
        auto request = reinterpret_cast<_RequestPluginAPI>(::GetProcAddress(mod, "RequestPluginAPI"));
        return request ? static_cast<IVTDM1*>(request(InterfaceVersion::V1)) : nullptr;
    }
}
