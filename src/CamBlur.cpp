#include "CamBlur.h"
#include "AmbientCam.h"
#include "CamFocus.h"
#include "CamPath.h"
#include "ConvoCam.h"
#include "FreeCamController.h"
#include "SceneCam.h"

#include <Windows.h>
#include <Psapi.h>
#pragma comment(lib, "psapi.lib")

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace CamBlur {

    namespace {
        // ---- the part of the ENB SDK this uses (ENBSeries SDK 1002, Boris Vorontsov; same ABI) --------------------------
        enum EnbType : long { kNone = 0, kFloat = 1, kInt = 2, kHex = 3, kBool = 4 };
        struct EnbParam {
            unsigned char Data[16] = {};
            unsigned long Size     = 0;
            EnbType       Type     = kNone;
        };
        enum EnbCallbackType : long { kEndFrame = 1, kBeginFrame = 2, kPreSave = 3, kPostLoad = 4 };
        using EnbCallback      = void(WINAPI*)(EnbCallbackType);
        using FnGetSDKVersion  = long (*)();
        using FnSetCallback    = void (*)(EnbCallback);
        using FnGetParameter   = BOOL (*)(char*, char*, char*, EnbParam*);
        using FnSetParameter   = BOOL (*)(char*, char*, char*, EnbParam*);
        constexpr long kSdkMajor = 1;  // SDK 1000..1999 are compatible

        FnSetCallback  pSetCallback = nullptr;
        FnGetParameter pGet         = nullptr;
        FnSetParameter pSet         = nullptr;
        bool           s_linked     = false;
        bool           s_callbackOn = false;

        // ---- the values touched --------------------------------------------------------------------------------------
        struct Key {
            const char* file;  // nullptr = shader variable
            const char* section;
            const char* key;
        };
        enum : int { kEffect = 0, kAutofocus, kManual, kFarCurve, kShape, kKeyCount };
        constexpr Key kKeys[kKeyCount] = {
            { "enbseries.ini", "EFFECT", "EnableDepthOfField" },
            { nullptr, "ENBDEPTHOFFIELD.FX", "DOF: Enable Autofocus" },
            { nullptr, "ENBDEPTHOFFIELD.FX", "DOF: Manual focus depth" },
            { nullptr, "ENBDEPTHOFFIELD.FX", "DOF: Far blur curve" },
            { nullptr, "ENBDEPTHOFFIELD.FX", "DOF: Bokeh shape max size" },
        };
        constexpr float kStrongCurve  = 1.3f;   // far blur curve at strength 1 (lower = stronger; 1.3 keeps the subject's own depth sharp)
        constexpr float kShapeMax     = 100.0f; // the shader's UIMax
        constexpr float kDistHalfLife = 0.12f;  // focus distance smoothing, seconds

        Settings s_settings;

        double Now();

        // Main thread -> render thread.
        struct Want {
            bool   active = false;
            bool   manual = false;  // false = the shader's own autofocus (screen centre)
            float  depth  = 0.0f;   // shader units
            double until  = 0.0;    // > 0: another mod's camera (API v5) - off after this time without a new call
        };
        std::mutex s_wantLock;
        Want       s_want;
        float      s_strength = 0.6f;  // copied with s_want

        // Render thread only.
        struct Applied {
            bool     on = false;
            EnbParam orig[kKeyCount];
            bool     have[kKeyCount] = {};
            EnbParam last[kKeyCount];
            bool     haveLast[kKeyCount] = {};
        };
        Applied s_applied;

        // Status (any thread).
        std::mutex s_statusLock;
        Status     s_status;

        // Main thread.
        float  s_dist     = 0.0f;
        bool   s_haveDist = false;
        double s_lastT    = 0.0;
        bool   s_wasOn    = false;
        bool   s_prevEnabled = false;
        double s_extUntil    = 0.0;  // API v5: another mod's camera owns the blur until then
        bool   s_axisLogged  = false;
        char   s_lastWhat[96] = {};
        Edge   s_edge;               // 10-31: for the image pass (Edges style); main thread
        Edge   s_archer;             // 10-31: the archer camera's (FovFx); main thread
        std::atomic<bool> s_noShader{ false };  // set by the render thread, cleared on a fresh tick of the box

        void SetError(std::string_view a_text) {
            std::scoped_lock lock(s_statusLock);
            const auto n = (std::min)(a_text.size(), sizeof(s_status.error) - 1);
            std::memcpy(s_status.error, a_text.data(), n);
            s_status.error[n] = '\0';
        }

        bool RawGet(int a_k, EnbParam& a_out) {
            const auto& k = kKeys[a_k];
            return pGet(const_cast<char*>(k.file), const_cast<char*>(k.section), const_cast<char*>(k.key), &a_out) != FALSE;
        }

        bool RawSet(int a_k, EnbParam a_in) {
            const auto& k = kKeys[a_k];
            return pSet(const_cast<char*>(k.file), const_cast<char*>(k.section), const_cast<char*>(k.key), &a_in) != FALSE;
        }

        float AsFloat(const EnbParam& a_p) {
            float f = 0.0f;
            std::memcpy(&f, a_p.Data, 4);
            return f;
        }

        EnbParam Float(const EnbParam& a_like, float a_v) {
            EnbParam p = a_like;
            std::memcpy(p.Data, &a_v, 4);
            return p;
        }

        EnbParam Bool(const EnbParam& a_like, bool a_v) {
            EnbParam   p = a_like;
            const BOOL b = a_v ? TRUE : FALSE;
            std::memcpy(p.Data, &b, 4);
            return p;
        }

        bool Same(const EnbParam& a_a, const EnbParam& a_b) { return a_a.Type == a_b.Type && std::memcmp(a_a.Data, a_b.Data, 4) == 0; }

        // Render thread.
        void SetIfChanged(int a_k, const EnbParam& a_p) {
            if (s_applied.haveLast[a_k] && Same(s_applied.last[a_k], a_p)) return;
            if (RawSet(a_k, a_p)) {
                s_applied.last[a_k]     = a_p;
                s_applied.haveLast[a_k] = true;
            }
        }

        void Restore(const char* a_why) {
            if (!s_applied.on) return;
            int put = 0, had = 0;
            for (int k = 0; k < kKeyCount; ++k) {
                if (!s_applied.have[k]) continue;
                ++had;
                if (RawSet(k, s_applied.orig[k])) ++put;
            }
            s_applied = Applied{};
            SKSE::log::info("CamBlur: ENB depth of field given back ({}) - {}/{} value(s) restored", a_why, put, had);
        }

        // Render thread: remember ENB's own values the first frame the blur is on.
        bool Capture() {
            s_applied = Applied{};
            for (int k = 0; k < kKeyCount; ++k) {
                EnbParam p;
                s_applied.have[k] = RawGet(k, p);
                if (s_applied.have[k]) s_applied.orig[k] = p;
            }
            for (int k = kAutofocus; k < kKeyCount; ++k) {
                if (!s_applied.have[k]) {
                    SetError(std::format("ENB's depth of field shader has no \"{}\" (needs Marty McFly's ADOF in "
                                         "enbseries\\enbdepthoffield.fx) - no blur", kKeys[k].key));
                    SKSE::log::warn("CamBlur: ENB shader variable [{}] \"{}\" not found - background blur stays off",
                        kKeys[k].section, kKeys[k].key);
                    s_applied = Applied{};
                    return false;
                }
            }
            s_applied.on = true;
            SKSE::log::info("CamBlur: ENB depth of field taken over - its own values: effect {}, autofocus {}, manual depth "
                            "{:.4f}, far blur curve {:.2f}, bokeh size {:.1f}",
                s_applied.have[kEffect] ? (AsFloat(s_applied.orig[kEffect]) != 0.0f ? "on" : "off") : "?",
                AsFloat(s_applied.orig[kAutofocus]) != 0.0f ? "on" : "off", AsFloat(s_applied.orig[kManual]),
                AsFloat(s_applied.orig[kFarCurve]), AsFloat(s_applied.orig[kShape]));
            return true;
        }

        void ApplyFrame() {
            Want  w;
            float strength = 0.0f;
            {
                std::scoped_lock lock(s_wantLock);
                w        = s_want;
                strength = s_strength;
            }
            if (w.active && w.until > 0.0 && Now() > w.until) w.active = false;  // that mod's camera stopped calling
            if (s_settings.style != kStyleEnb) w.active = false;                  // 10-31: the image pass does it
            if (!w.active) {
                Restore("blur off");
                return;
            }
            if (!s_applied.on && !Capture()) {
                s_noShader = true;  // nothing to drive; the main thread stays off until the box is ticked again
                return;
            }
            {
                std::scoped_lock lock(s_statusLock);
                s_status.shader = true;
            }
            if (s_applied.have[kEffect]) SetIfChanged(kEffect, Bool(s_applied.orig[kEffect], true));
            const auto& o = s_applied.orig;
            SetIfChanged(kAutofocus, Bool(o[kAutofocus], w.manual ? false : AsFloat(o[kAutofocus]) != 0.0f));
            SetIfChanged(kManual, Float(o[kManual], w.manual ? std::clamp(w.depth, 0.00001f, 1.0f) : AsFloat(o[kManual])));
            const float curve0 = AsFloat(o[kFarCurve]);
            const float curve  = curve0 > kStrongCurve ? curve0 + (kStrongCurve - curve0) * strength : curve0;
            SetIfChanged(kFarCurve, Float(o[kFarCurve], curve));
            const float shape0 = AsFloat(o[kShape]);
            SetIfChanged(kShape, Float(o[kShape], (std::min)(shape0 * (1.0f + strength), kShapeMax)));
        }

        void WINAPI Callback(EnbCallbackType a_type) {
            switch (a_type) {
            case kEndFrame:
                ApplyFrame();
                break;
            case kPreSave:
                // The user saves the preset in ENB's editor: never let a blur value into it (re-applied next frame).
                Restore("before the ENB editor saves");
                break;
            case kPostLoad:
                // ENB (re)loaded its config: the remembered values no longer describe it.
                if (s_applied.on) SKSE::log::info("CamBlur: ENB reloaded its config - taking its new values next frame");
                s_applied  = Applied{};
                s_noShader = false;
                break;
            default:
                break;
            }
        }

        HMODULE FindEnb() {
            if (auto h = ::GetModuleHandleW(L"d3d11.dll"); h && ::GetProcAddress(h, "ENBGetSDKVersion")) return h;
            HMODULE mods[1024];
            DWORD   needed = 0;
            if (::EnumProcessModules(::GetCurrentProcess(), mods, sizeof(mods), &needed)) {
                const auto count = (std::min)(needed / static_cast<DWORD>(sizeof(HMODULE)), static_cast<DWORD>(1024));
                for (DWORD i = 0; i < count; ++i) {
                    if (mods[i] && ::GetProcAddress(mods[i], "ENBGetSDKVersion")) return mods[i];
                }
            }
            return nullptr;
        }

        double Now() {
            static const double freq = [] {
                LARGE_INTEGER f;
                ::QueryPerformanceFrequency(&f);
                return static_cast<double>(f.QuadPart);
            }();
            LARGE_INTEGER t;
            ::QueryPerformanceCounter(&t);
            return static_cast<double>(t.QuadPart) / freq;
        }

        float Length3(const RE::NiPoint3& a_v) { return std::sqrt(a_v.x * a_v.x + a_v.y * a_v.y + a_v.z * a_v.z); }

        // The game's depth buffer value at view depth a_z (D3D, 0 at the near plane), then the shader's
        // LinearizeDepth(x) = x / (3000 - 2999 x) - the same number the shader compares the scene's depth with.
        float ShaderDepth(float a_z, float a_near, float a_far) {
            const double n = (std::max)(static_cast<double>(a_near), 0.1);
            const double f = (std::max)(static_cast<double>(a_far), n + 1.0);
            const double z = (std::max)(static_cast<double>(a_z), n + 0.5);
            const double d = f * (z - n) / (z * (f - n));
            return static_cast<float>(d / (3000.0 - 2999.0 * d));
        }

        // The point to keep sharp for whatever moves the camera; false = no subject (screen-centre autofocus).
        // a_on: a TFCam camera the blur applies to is running at all.
        bool Subject(RE::NiPoint3& a_out, const char*& a_what, bool& a_on) {
            a_on = false;
            if (CamPath::FocusPoint(a_out, a_what, a_on)) return true;
            if (a_on) return false;  // a path that looks where its points looked: centre autofocus
            if (SceneCam::BlurSubject(a_out, a_what, a_on)) return true;
            if (a_on) return false;
            if (ConvoCam::OwnsMotion()) {
                a_on = true;
                if (auto* mtm = RE::MenuTopicManager::GetSingleton()) {
                    auto speaker = mtm->speaker.get();
                    if (!speaker) speaker = mtm->lastSpeaker.get();
                    auto* actor = speaker ? speaker->As<RE::Actor>() : nullptr;
                    if (actor && CamFocus::HeadPoint(actor, a_out)) {
                        a_what = "conversation camera: the speaker's head";
                        return true;
                    }
                }
                a_what = "conversation camera: screen centre";
                return false;
            }
            if (AmbientCam::OwnsMotion()) {
                a_on = true;
                if (CamFocus::RefPoint(RE::PlayerCharacter::GetSingleton(), a_out)) {
                    a_what = "idle camera: you";
                    return true;
                }
                return false;
            }
            if (s_settings.inFreeFly) {
                a_on   = true;
                a_what = "free-fly: screen centre";
            }
            return false;
        }
    }

    Settings& Get() { return s_settings; }

    void Clamp() {
        if (!std::isfinite(s_settings.strength)) s_settings.strength = 0.6f;
        s_settings.strength = std::clamp(s_settings.strength, 0.0f, 1.0f);
        s_settings.style    = std::clamp(s_settings.style, 0, 1);
        if (!std::isfinite(s_settings.sharp)) s_settings.sharp = 0.4f;
        s_settings.sharp = std::clamp(s_settings.sharp, 0.05f, 0.9f);
    }

    Edge EdgeWant() {
        if (s_extUntil > 0.0 && Now() > s_extUntil) s_edge.on = false;  // that mod's camera stopped calling
        return s_edge.on ? s_edge : s_archer;
    }

    void Archer(bool a_on, const RE::NiPoint3& a_subject) {
        s_archer.on          = a_on;
        s_archer.strength    = s_settings.strength;
        s_archer.haveSubject = a_on;
        s_archer.subject     = a_subject;
    }

    void Install() {
        if (s_linked) return;
        const HMODULE enb = FindEnb();
        if (!enb) {
            SetError("ENB is not installed (no module exports the ENB SDK) - background blur needs ENB's depth of field");
            SKSE::log::info("CamBlur: no ENB SDK in the process - background blur unavailable");
            return;
        }
        auto getSdk  = reinterpret_cast<FnGetSDKVersion>(::GetProcAddress(enb, "ENBGetSDKVersion"));
        pSetCallback = reinterpret_cast<FnSetCallback>(::GetProcAddress(enb, "ENBSetCallbackFunction"));
        pGet         = reinterpret_cast<FnGetParameter>(::GetProcAddress(enb, "ENBGetParameter"));
        pSet         = reinterpret_cast<FnSetParameter>(::GetProcAddress(enb, "ENBSetParameter"));
        if (!getSdk || !pSetCallback || !pGet || !pSet) {
            SetError("ENB found but its SDK functions are incomplete - no blur");
            SKSE::log::error("CamBlur: ENB module found but the SDK exports are incomplete");
            return;
        }
        const long sdk = getSdk();
        if (sdk / 1000 != kSdkMajor) {
            SetError(std::format("ENB SDK version {} is not supported - no blur", sdk));
            SKSE::log::error("CamBlur: ENB SDK version {} is not 1xxx", sdk);
            return;
        }
        if (!s_callbackOn) {
            pSetCallback(Callback);
            s_callbackOn = true;
        }
        s_linked = true;
        SetError("");
        {
            std::scoped_lock lock(s_statusLock);
            s_status.linked = true;
        }
        SKSE::log::info("CamBlur: linked the ENB SDK (version {}) - background blur available", sdk);
    }

    void Update(const RE::FreeCameraState* a_fcs, bool a_driving) {
        const double now = Now();
        const float  dt  = s_lastT > 0.0 ? static_cast<float>(std::clamp(now - s_lastT, 0.0, 0.1)) : 0.0f;
        s_lastT          = now;
        const bool enbStyle = s_settings.style == kStyleEnb;
        if (enbStyle && !s_linked) return;
        if (s_extUntil > now) return;  // another mod's camera (API v5) drives the blur

        RE::NiPoint3 subject{};
        const char*  what = "";
        bool         on   = false;
        bool         have = false;
        if (s_settings.enabled && a_fcs && a_driving && !REL::Module::IsVR()) {
            have = Subject(subject, what, on);
        }
        // The render thread found no ADOF variables: stay off until the box is ticked again (or ENB reloads).
        if (s_settings.enabled && !s_prevEnabled) s_noShader = false;
        s_prevEnabled = s_settings.enabled;
        if (s_noShader && enbStyle) on = false;

        s_edge.on          = on;  // 10-31: the image pass (Edges style) reads this
        s_edge.strength    = s_settings.strength;
        s_edge.haveSubject = on && have;
        s_edge.subject     = subject;

        Want w;
        w.active = on;
        if (on && have) {
            const float pitch = a_fcs->rotation.x;
            const float yaw   = a_fcs->rotation.y;
            const RE::NiPoint3 fwd{ std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch) };
            const RE::NiPoint3 d = subject - a_fcs->translation;
            const float        z = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;  // view depth
            auto*              nc = RE::Main::WorldRootCamera();
            if (nc && !s_axisLogged) {
                // One-time check of the axis External() uses: the world camera's local +X should be this view direction.
                s_axisLogged   = true;
                const auto& m  = nc->world.rotate.entry;
                const float c0 = m[0][0] * fwd.x + m[1][0] * fwd.y + m[2][0] * fwd.z;
                const float r0 = m[0][0] * fwd.x + m[0][1] * fwd.y + m[0][2] * fwd.z;
                SKSE::log::info("CamBlur: view axis check - free camera forward vs world camera column 0: {:.3f}, row 0: "
                                "{:.3f} (1 = the same direction); camera {:.0f} units from the world camera",
                    c0, r0, Length3(nc->world.translate - a_fcs->translation));
            }
            if (z > 1.0f && nc) {
                if (!s_haveDist || !s_wasOn) {
                    s_dist     = z;
                    s_haveDist = true;
                } else {
                    s_dist += (z - s_dist) * (dt <= 0.0f ? 0.0f : 1.0f - std::exp(-0.69314718f * dt / kDistHalfLife));
                }
                const auto& fr = nc->GetRuntimeData2().viewFrustum;
                w.manual       = true;
                w.depth        = ShaderDepth(s_dist, fr.fNear, fr.fFar);
            }
            // Subject behind the camera: the shader's own autofocus until it is in front again.
        }
        if (!w.manual) s_haveDist = false;
        {
            std::scoped_lock lock(s_wantLock);
            s_want     = w;
            s_strength = s_settings.strength;
        }
        if (on != s_wasOn || (on && std::strcmp(what, s_lastWhat) != 0)) {
            SKSE::log::info("CamBlur: background blur {} ({}), strength {:.2f}", on ? "on" : "off", on ? what : "no TFCam camera",
                s_settings.strength);
            std::snprintf(s_lastWhat, sizeof(s_lastWhat), "%s", on ? what : "");
        }
        s_wasOn = on;
        {
            std::scoped_lock lock(s_statusLock);
            s_status.active   = on;
            s_status.distance = w.manual ? s_dist : 0.0f;
            std::snprintf(s_status.what, sizeof(s_status.what), "%s", on ? what : "");
        }
    }

    void External(bool a_on, const RE::NiPoint3& a_subject, float a_strength) {
        const bool enbStyle = s_settings.style == kStyleEnb;
        if (enbStyle && !s_linked) return;
        const double now = Now();
        if (!a_on || !std::isfinite(a_subject.x) || !std::isfinite(a_subject.y) || !std::isfinite(a_subject.z)) {
            if (s_extUntil > 0.0) {
                s_extUntil = 0.0;
                s_edge     = Edge{};
                std::scoped_lock lock(s_wantLock);
                s_want = Want{};
                SKSE::log::info("CamBlur: background blur off (another mod's camera let go)");
            }
            return;
        }
        auto* nc = REL::Module::IsVR() ? nullptr : RE::Main::WorldRootCamera();
        if (!nc || (s_noShader && enbStyle)) return;
        s_edge.on          = true;  // 10-31: the image pass (Edges style)
        s_edge.strength    = a_strength < 0.0f || !std::isfinite(a_strength) ? s_settings.strength : std::clamp(a_strength, 0.0f, 1.0f);
        s_edge.haveSubject = true;
        s_edge.subject     = a_subject;
        const float dt = s_lastT > 0.0 ? static_cast<float>(std::clamp(now - s_lastT, 0.0, 0.1)) : 0.0f;
        s_lastT        = now;
        // The world camera, whoever drives it: NiCamera looks along its local +X (column 0 of its world rotation).
        const auto&        m   = nc->world.rotate.entry;
        const RE::NiPoint3 fwd{ m[0][0], m[1][0], m[2][0] };
        const RE::NiPoint3 d   = a_subject - nc->world.translate;
        const float        z   = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
        const bool         was = s_extUntil > now;
        Want               w;
        w.active = true;
        w.until  = now + 0.5;
        if (z > 1.0f) {
            if (!was || !s_haveDist) {
                s_dist = z;
            } else {
                s_dist += (z - s_dist) * (dt <= 0.0f ? 0.0f : 1.0f - std::exp(-0.69314718f * dt / kDistHalfLife));
            }
            s_haveDist     = true;
            const auto& fr = nc->GetRuntimeData2().viewFrustum;
            w.manual       = true;
            w.depth        = ShaderDepth(s_dist, fr.fNear, fr.fFar);
        } else {
            s_haveDist = false;
        }
        {
            std::scoped_lock lock(s_wantLock);
            s_want     = w;
            s_strength = a_strength < 0.0f || !std::isfinite(a_strength) ? s_settings.strength : std::clamp(a_strength, 0.0f, 1.0f);
        }
        if (!was) SKSE::log::info("CamBlur: background blur on (another mod's camera, {:.0f} units to its subject)", z);
        s_extUntil = w.until;
        s_wasOn    = true;
        std::scoped_lock lock(s_statusLock);
        s_status.active   = true;
        s_status.distance = w.manual ? s_dist : 0.0f;
        std::snprintf(s_status.what, sizeof(s_status.what), "%s", "another mod's camera");
    }

    bool Available() {
        if (s_settings.style != kStyleEnb) return !REL::Module::IsVR();  // 10-31: TFCam's own pass
        return s_linked && !s_noShader;
    }

    void Off(const char* a_why) {
        s_extUntil = 0.0;
        s_edge     = Edge{};
        bool was = false;
        {
            std::scoped_lock lock(s_wantLock);
            was           = s_want.active;
            s_want.active = false;
        }
        if (was) SKSE::log::info("CamBlur: background blur off ({})", a_why);
        s_wasOn    = false;
        s_haveDist = false;
        s_lastWhat[0] = '\0';
        std::scoped_lock lock(s_statusLock);
        s_status.active   = false;
        s_status.distance = 0.0f;
        s_status.what[0]  = '\0';
    }

    Status GetStatus() {
        std::scoped_lock lock(s_statusLock);
        return s_status;
    }
}
