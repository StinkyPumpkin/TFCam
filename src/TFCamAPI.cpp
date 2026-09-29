#include "TFCamAPI.h"
#include "FreeCamController.h"

// 0.9.2: cross-plugin API (TFCamAPI.h). PEM and Whore Horde's photo studio take over the
// screenshot key while the player flies the camera for a photo.
namespace
{
    class TFCamAPIImpl final : public TFCAM_API::ITFCam1
    {
    public:
        void SetSnapHandler(TFCAM_API::SnapHandler a_handler) noexcept override
        {
            FreeCam::SetSnapHandler(a_handler);
        }
    };

    TFCamAPIImpl g_api;
}

extern "C" __declspec(dllexport) void* RequestTFCamAPI(std::uint32_t a_version)
{
    if (a_version != TFCAM_API::kVersion1) {
        return nullptr;
    }
    return static_cast<TFCAM_API::ITFCam1*>(&g_api);
}
