#include "CameraLight.h"

#include <cstring>

namespace CameraLight {

    // --- State ---
    static bool  s_active    = false;
    static float s_intensity = 1.0f;
    static float s_radius    = 1000.0f;
    static float s_fade      = 1.7f;   // renderer fade multiplier (matches CustomLight default)
    static float s_colorR    = 1.0f;
    static float s_colorG    = 1.0f;
    static float s_colorB    = 1.0f;

    // Scroll behavior checkboxes (persisted in INI)
    static bool  s_scrollBrightness = true;
    static bool  s_scrollRadius     = true;

    // Scroll step sizes
    static constexpr float kIntensityStep = 0.15f;
    static constexpr float kRadiusStep    = 100.0f;
    static constexpr float kIntensityMin  = 0.1f;
    static constexpr float kIntensityMax  = 10.0f;
    static constexpr float kRadiusMin     = 100.0f;
    static constexpr float kRadiusMax     = 5000.0f;

    static RE::NiPointer<RE::NiPointLight> s_light;
    static RE::NiPointer<RE::BSLight>      s_bsLight;

    // --- NiPointLight creation (calloc + vtable, matching CustomLight) ---
    static constexpr std::size_t kNiPointLightSizeSE = 0x150;
    static constexpr std::size_t kNiPointLightSizeVR = 0x178;

    static RE::NiPointLight* CreateNiPointLight() {
        std::size_t allocSize = REL::Module::IsVR() ? kNiPointLightSizeVR : kNiPointLightSizeSE;
        auto* light = static_cast<RE::NiPointLight*>(RE::calloc(1, allocSize));
        if (!light) return nullptr;

        // Set vtable manually (same approach as working CustomLight)
        REL::Relocation<std::uintptr_t> vtable(RE::VTABLE_NiPointLight[0]);
        *reinterpret_cast<std::uintptr_t*>(light) = vtable.address();

        // Initialize transforms to identity
        auto initTransform = [](RE::NiTransform& t) {
            t.rotate.entry[0][0] = 1.0f;
            t.rotate.entry[1][1] = 1.0f;
            t.rotate.entry[2][2] = 1.0f;
            t.scale = 1.0f;
        };
        initTransform(light->local);
        initTransform(light->world);
        initTransform(light->previousWorld);

        return light;
    }

    // --- Apply current settings to the NiPointLight ---
    static void ApplyLightSettings() {
        if (!s_light) return;

        auto& ld = s_light->GetLightRuntimeData();
        ld.ambient.red   = 0.0f;
        ld.ambient.green = 0.0f;
        ld.ambient.blue  = 0.0f;
        ld.diffuse.red   = s_intensity * s_colorR;
        ld.diffuse.green = s_intensity * s_colorG;
        ld.diffuse.blue  = s_intensity * s_colorB;
        ld.radius.x      = s_radius;
        ld.radius.y      = s_radius;
        ld.radius.z      = s_radius;
        ld.fade           = s_fade;

        // Zero attenuation coefficients (let radius control falloff)
        auto& pd = s_light->GetPointLightRuntimeData();
        pd.constAttenuation     = 0.0f;
        pd.linearAttenuation    = 0.0f;
        pd.quadraticAttenuation = 0.0f;
    }

    // --- ShadowSceneNode BSLight registration (from CustomLight) ---

    struct LightCreateParams {
        bool          dynamic;         // 00
        bool          shadowLight;     // 01
        bool          portalStrict;    // 02
        bool          affectLand;      // 03
        bool          affectWater;     // 04
        bool          neverFades;      // 05
        std::uint16_t pad06;           // 06
        float         fov;             // 08
        float         falloff;         // 0C
        float         nearDistance;    // 10
        float         depthBias;       // 14
        std::int32_t  sceneGraphIndex; // 18
        std::uint32_t pad1C;           // 1C
        void*         restrictedNode;  // 20
        void*         lensFlareData;   // 28
    };
    static_assert(sizeof(LightCreateParams) == 0x30);

    static RE::ShadowSceneNode* GetWorldSSN() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return nullptr;
        auto* obj = player->Get3D();
        if (!obj) return nullptr;

        auto* node = obj->parent;
        while (node) {
            auto* rtti = node->GetRTTI();
            if (rtti && std::strcmp(rtti->GetName(), "ShadowSceneNode") == 0) {
                return reinterpret_cast<RE::ShadowSceneNode*>(node);
            }
            node = node->parent;
        }
        return nullptr;
    }

    static void RegisterWithRenderer() {
        auto* ssn = GetWorldSSN();
        if (!ssn || !s_light) {
            SKSE::log::warn("CameraLight: cannot register BSLight — SSN={} light={}",
                ssn != nullptr, s_light != nullptr);
            return;
        }

        using AddLight_fn = RE::BSLight*(*)(RE::ShadowSceneNode*, RE::NiLight*, LightCreateParams*);
        REL::Relocation<AddLight_fn> addLight{ RELOCATION_ID(99692, 106326) };

        LightCreateParams params{};
        params.dynamic      = true;
        params.affectLand   = true;
        params.affectWater  = true;
        params.neverFades   = true;
        params.falloff      = 1.0f;
        params.nearDistance  = 5.0f;
        params.depthBias    = 1.0f;

        s_bsLight.reset(addLight(ssn, s_light.get(), &params));

        if (s_bsLight) {
            SKSE::log::info("CameraLight: BSLight registered OK");
        } else {
            SKSE::log::error("CameraLight: AddLight returned null");
        }
    }

    static void UnregisterFromRenderer() {
        if (!s_light) return;

        auto* ssn = GetWorldSSN();
        if (ssn) {
            using RemoveLight_fn = void(*)(RE::ShadowSceneNode*, RE::NiLight*);
            REL::Relocation<RemoveLight_fn> removeLight{ RELOCATION_ID(99697, 106331) };
            removeLight(ssn, s_light.get());
        }
        s_bsLight.reset();
    }

    // --- Public API ---

    bool  IsActive()     { return s_active; }
    float GetIntensity() { return s_intensity; }
    float GetRadius()    { return s_radius; }

    bool  GetScrollBrightness() { return s_scrollBrightness; }
    bool  GetScrollRadius()     { return s_scrollRadius; }
    void  SetScrollBrightness(bool v) { s_scrollBrightness = v; }
    void  SetScrollRadius(bool v)     { s_scrollRadius = v; }

    void SetIntensity(float v) {
        s_intensity = std::clamp(v, kIntensityMin, kIntensityMax);
        if (s_light) {
            auto& ld = s_light->GetLightRuntimeData();
            ld.diffuse.red   = s_intensity * s_colorR;
            ld.diffuse.green = s_intensity * s_colorG;
            ld.diffuse.blue  = s_intensity * s_colorB;
        }
    }

    void SetRadius(float v) {
        s_radius = std::clamp(v, kRadiusMin, kRadiusMax);
        if (s_light) {
            auto& ld = s_light->GetLightRuntimeData();
            ld.radius.x = s_radius;
            ld.radius.y = s_radius;
            ld.radius.z = s_radius;
        }
    }

    void SetColor(float r, float g, float b) {
        s_colorR = std::clamp(r, 0.0f, 1.0f);
        s_colorG = std::clamp(g, 0.0f, 1.0f);
        s_colorB = std::clamp(b, 0.0f, 1.0f);
        if (s_light) {
            auto& ld = s_light->GetLightRuntimeData();
            ld.diffuse.red   = s_intensity * s_colorR;
            ld.diffuse.green = s_intensity * s_colorG;
            ld.diffuse.blue  = s_intensity * s_colorB;
        }
    }

    void GetColor(float& r, float& g, float& b) {
        r = s_colorR;
        g = s_colorG;
        b = s_colorB;
    }

    float GetFade() { return s_fade; }

    void SetFade(float v) {
        s_fade = std::clamp(v, 0.1f, 10.0f);
        if (s_light) {
            s_light->GetLightRuntimeData().fade = s_fade;
        }
    }

    // --- Turn light ON ---
    static void TurnOn() {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam || !cam->cameraRoot) {
            SKSE::log::warn("CameraLight: no camera root available");
            return;
        }

        auto* light = CreateNiPointLight();
        if (!light) {
            SKSE::log::error("CameraLight: failed to allocate NiPointLight");
            return;
        }

        s_light.reset(light);
        s_light->name = "FreeCamLight";

        // Apply all light properties (diffuse, radius, fade, attenuation)
        ApplyLightSettings();

        // Small forward offset so light isn't exactly at camera origin
        s_light->local.translate.x = 0.0f;
        s_light->local.translate.y = 50.0f;  // slightly forward
        s_light->local.translate.z = 0.0f;

        // Attach to camera root — follows camera automatically
        cam->cameraRoot->AttachChild(s_light.get());

        // Force scene graph update
        RE::NiUpdateData updateData;
        cam->cameraRoot->Update(updateData);

        s_active = true;

        // Register with ShadowSceneNode for proper rendering
        RegisterWithRenderer();

        SKSE::log::info("CameraLight: ON (intensity={:.2f}, radius={:.0f}, fade={:.1f})",
            s_intensity, s_radius, s_fade);
    }

    // --- Turn light OFF ---
    static void TurnOff() {
        UnregisterFromRenderer();

        if (s_light) {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (cam && cam->cameraRoot) {
                cam->cameraRoot->DetachChild(s_light.get());
            }
            s_light.reset();
        }
        s_active = false;
        SKSE::log::info("CameraLight: OFF");
    }

    void Toggle() {
        if (s_active) {
            TurnOff();
        } else {
            TurnOn();
        }
    }

    // --- Shift+Scroll handlers ---

    void ScrollUp() {
        if (!s_active) {
            TurnOn();
            return;
        }

        bool changed = false;
        if (s_scrollBrightness) {
            float newInt = std::clamp(s_intensity + kIntensityStep, kIntensityMin, kIntensityMax);
            if (newInt != s_intensity) {
                s_intensity = newInt;
                changed = true;
            }
        }
        if (s_scrollRadius) {
            float newRad = std::clamp(s_radius + kRadiusStep, kRadiusMin, kRadiusMax);
            if (newRad != s_radius) {
                s_radius = newRad;
                changed = true;
            }
        }
        if (changed) {
            ApplyLightSettings();
            SKSE::log::trace("CameraLight: scroll up — intensity={:.2f} radius={:.0f}",
                s_intensity, s_radius);
        }
    }

    void ScrollDown() {
        if (!s_active) return;

        bool changed = false;
        if (s_scrollBrightness) {
            float newInt = std::clamp(s_intensity - kIntensityStep, kIntensityMin, kIntensityMax);
            if (newInt != s_intensity) {
                s_intensity = newInt;
                changed = true;
            }
        }
        if (s_scrollRadius) {
            float newRad = std::clamp(s_radius - kRadiusStep, kRadiusMin, kRadiusMax);
            if (newRad != s_radius) {
                s_radius = newRad;
                changed = true;
            }
        }

        if (changed) {
            ApplyLightSettings();
            SKSE::log::trace("CameraLight: scroll down — intensity={:.2f} radius={:.0f}",
                s_intensity, s_radius);
        }

        // Turn off when active scroll axes all at minimum
        bool atMinBright = (s_intensity <= kIntensityMin + 0.001f);
        bool atMinRadius = (s_radius <= kRadiusMin + 0.1f);

        bool shouldOff = true;
        if (s_scrollBrightness && !atMinBright) shouldOff = false;
        if (s_scrollRadius && !atMinRadius) shouldOff = false;
        if (shouldOff) {
            TurnOff();
        }
    }

    void Update() {
        // Light is a child of cameraRoot — moves automatically. No per-frame work.
    }

    void Cleanup() {
        TurnOff();
    }
}
