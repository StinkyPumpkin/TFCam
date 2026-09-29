#pragma once

#include <cstdint>

// --Claude: TFCam cross-plugin API (E:\dev\FreeCamClaude, TFCam.dll). Lets a photo tool
// (PEM / Whore Horde PhotoStudio) take over TFCam's screenshot key while the player flies
// the camera for a photo.
//
//   auto* dll = GetModuleHandleW(L"TFCam.dll");
//   auto  fn  = dll ? reinterpret_cast<TFCAM_API::RequestFn>(GetProcAddress(dll, "RequestTFCamAPI")) : nullptr;
//   auto* api = fn ? static_cast<TFCAM_API::ITFCam1*>(fn(TFCAM_API::kVersion1)) : nullptr;
//
// Older TFCam builds have no export; a version mismatch returns nullptr. Plain types only.
namespace TFCAM_API
{
    inline constexpr std::uint32_t kVersion1 = 1;

    using SnapHandler = void (*)();

    struct ITFCam1
    {
        // While set, TFCam's screenshot key / button calls a_handler (game thread) instead of
        // pressing PrintScreen. nullptr restores the normal screenshot.
        virtual void SetSnapHandler(SnapHandler a_handler) noexcept = 0;
    };

    using RequestFn = void* (*)(std::uint32_t a_version);
}
