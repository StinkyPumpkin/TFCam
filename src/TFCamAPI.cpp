#include "TFCamAPI.h"
#include "FreeCamController.h"
#include "SceneCam.h"

// 0.9.2: cross-plugin API (TFCamAPI.h). PEM and Whore Horde's photo studio take over the
// screenshot key while the player flies the camera for a photo.
// 0.10.0: v2 = Scene Camera control for SLUI. One object serves both versions (ITFCam2 extends ITFCam1), so
// RequestTFCamAPI(1) keeps returning the same pointer PEM / Whore Horde always got. Every v2 call copies its
// arguments into an SKSE task (SceneCam does the engine work on the main thread); GetStatus reads a snapshot.
namespace
{
    class TFCamAPIImpl final : public TFCAM_API::ITFCam2
    {
    public:
        void SetSnapHandler(TFCAM_API::SnapHandler a_handler) noexcept override
        {
            FreeCam::SetSnapHandler(a_handler);
        }

        bool SetOrbit(std::int32_t a_state) noexcept override
        {
            return Guard([&] { return SceneCam::ApiSetOrbit(a_state); });
        }

        bool SetEyeView(std::int32_t a_state) noexcept override
        {
            return Guard([&] { return SceneCam::ApiSetEyeView(a_state); });
        }

        bool NextEyeTarget() noexcept override
        {
            return Guard([] { return SceneCam::ApiNextEyeTarget(); });
        }

        void SetSettings(const TFCAM_API::SceneCamSettings& a_settings) noexcept override
        {
            Guard([&] {
                SceneCam::ApiSetSettings(a_settings);
                return true;
            });
        }

        bool SaveShot() noexcept override
        {
            return Guard([] { return SceneCam::ApiSaveShot(); });
        }

        bool RecallShot() noexcept override
        {
            return Guard([] { return SceneCam::ApiRecallShot(); });
        }

        bool DeleteShot() noexcept override
        {
            return Guard([] { return SceneCam::ApiDeleteShot(); });
        }

        void SetActivePose(std::uint32_t a_actor, const char* a_animEvent) noexcept override
        {
            Guard([&] {
                SceneCam::ApiSetActivePose(a_actor, a_animEvent);
                return true;
            });
        }

        void GetStatus(TFCAM_API::SceneCamStatus* a_out) noexcept override
        {
            Guard([&] {
                SceneCam::ApiGetStatus(a_out);
                return true;
            });
        }

    private:
        // Nothing may leave a noexcept call into another DLL (a failed task allocation would otherwise terminate).
        template <class F>
        static bool Guard(F&& a_fn) noexcept
        {
            try {
                return a_fn();
            } catch (...) {
                SKSE::log::error("TFCamAPI: a v2 call failed with an exception - ignored");
                return false;
            }
        }
    };

    TFCamAPIImpl g_api;
}

extern "C" __declspec(dllexport) void* RequestTFCamAPI(std::uint32_t a_version)
{
    switch (a_version) {
    case TFCAM_API::kVersion1:
        return static_cast<TFCAM_API::ITFCam1*>(&g_api);
    case TFCAM_API::kVersion2:
        return static_cast<TFCAM_API::ITFCam2*>(&g_api);
    default:
        return nullptr;
    }
}
