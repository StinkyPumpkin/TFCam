#include "FixedCam.h"
#include "FovFx.h"

#include <cstdint>

namespace FixedCam {

    namespace {
        // PlayerCamera::Update's call that updates the camera root (Cinematic Mode's site, + 0x1A6 on SE and AE).
        // Every frame, every camera state.
        struct CameraUpdateHook {
            static void thunk(RE::TESCamera* a_camera) {
                func(a_camera);
                FovFx::Tick();         // its offset goes on top of everyone's FOV
                FovFx::ApplyAimCam();  // the archer camera (Action FOV)
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };
    }

    void Install() {
        if (REL::Module::IsVR()) {
            SKSE::log::info("FixedCam: not on VR");
            return;
        }
        // The exe's code is only readable at run time (on disk it is wrapped), so the site is checked here: a 5-byte
        // call, or no hook at all.
        REL::Relocation<std::uintptr_t> update{ RELOCATION_ID(49852, 50784) };
        const std::uintptr_t site = update.address() + REL::Relocate(0x1A6, 0x1A6);
        const auto*          code = reinterpret_cast<const std::uint8_t*>(site);
        if (code[0] != 0xE8) {
            SKSE::log::warn("FixedCam: PlayerCamera::Update + 0x1A6 is not a call (byte {:02X}) - no archer camera / Action FOV tick",
                code[0]);
            return;
        }
        const auto target = site + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
        CameraUpdateHook::func = SKSE::GetTrampoline().write_call<5>(site, CameraUpdateHook::thunk);
        SKSE::log::info("FixedCam: PlayerCamera::Update + 0x1A6 hooked (it called SkyrimSE.exe+{:X})",
            target - REL::Module::get().base());
    }
}
