#include "TFCamAPI.h"
#include "FreeCamController.h"
#include "SceneCam.h"
#include "CamBlur.h"

// 0.9.2: cross-plugin API (TFCamAPI.h). PEM and Whore Horde's photo studio take over the
// screenshot key while the player flies the camera for a photo.
// 0.10.0: v2 = Scene Camera control for SLUI. One object serves both versions (ITFCam2 extends ITFCam1), so
// RequestTFCamAPI(1) keeps returning the same pointer PEM / Whore Horde always got. Every v2 call copies its
// arguments into an SKSE task (SceneCam does the engine work on the main thread); GetStatus reads a snapshot.
// 0.13.0: v3 = ITFCam3 (orbit aim / sway / target, automatic free camera for saved shots, status with the free-camera
// state, delete the scene's shots). Still one object: RequestTFCamAPI(1) / (2) return the same pointer as before.
// 0.15.0: v4 = ITFCam4 (preset views, set cam pos). Same object again.
// 0.17.0: v5 = ITFCam5 (background blur and an extra roll for another mod's camera). Same object again; these run in
// the caller's (game) thread - no task: the blur must follow the camera the same frame.
namespace
{
    class TFCamAPIImpl final : public TFCAM_API::ITFCam5
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

        void SetSettings3(const TFCAM_API::SceneCamSettings3& a_settings) noexcept override
        {
            Guard([&] {
                SceneCam::ApiSetSettings3(a_settings);
                return true;
            });
        }

        bool GetStatus3(TFCAM_API::SceneCamStatus3* a_out) noexcept override
        {
            return Guard([&] { return SceneCam::ApiGetStatus3(a_out); });
        }

        bool DeleteSceneShots() noexcept override
        {
            return Guard([] { return SceneCam::ApiDeleteSceneShots(); });
        }

        bool SetPresetView(std::int32_t a_state) noexcept override
        {
            return Guard([&] { return SceneCam::ApiSetPresetView(a_state); });
        }

        bool StepPreset(std::int32_t a_step) noexcept override
        {
            return Guard([&] { return SceneCam::ApiStepPreset(a_step); });
        }

        bool SetShotPlacing(std::int32_t a_state) noexcept override
        {
            return Guard([&] { return SceneCam::ApiSetShotPlacing(a_state); });
        }

        bool GetPreset4(TFCAM_API::SceneCamPreset4* a_out) noexcept override
        {
            return Guard([&] { return SceneCam::ApiGetPreset4(a_out); });
        }

        void SetBlurSubject(bool a_on, float a_x, float a_y, float a_z, float a_strength) noexcept override
        {
            Guard([&] {
                CamBlur::External(a_on, RE::NiPoint3{ a_x, a_y, a_z }, a_strength);
                return true;
            });
        }

        bool BlurAvailable() noexcept override
        {
            return Guard([] { return CamBlur::Available(); });
        }

        void SetCameraRoll(float a_radians) noexcept override
        {
            FreeCam::SetApiRoll(a_radians);
        }

    private:
        // Nothing may leave a noexcept call into another DLL (a failed task allocation would otherwise terminate).
        template <class F>
        static bool Guard(F&& a_fn) noexcept
        {
            try {
                return a_fn();
            } catch (...) {
                SKSE::log::error("TFCamAPI: an API call failed with an exception - ignored");
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
    case TFCAM_API::kVersion3:
        return static_cast<TFCAM_API::ITFCam3*>(&g_api);
    case TFCAM_API::kVersion4:
        return static_cast<TFCAM_API::ITFCam4*>(&g_api);
    case TFCAM_API::kVersion5:
        return static_cast<TFCAM_API::ITFCam5*>(&g_api);
    default:
        return nullptr;
    }
}
