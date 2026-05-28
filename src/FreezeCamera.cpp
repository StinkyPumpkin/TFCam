#include "FreezeCamera.h"

namespace FreezeCamera {

    static bool         s_frozen = false;
    static RE::NiMatrix3 s_frozenRot;
    static RE::NiPoint3  s_frozenPos;

    bool IsFrozen() { return s_frozen; }

    void Toggle() {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) return;

        if (s_frozen) {
            s_frozen = false;
            SKSE::log::info("FreezeCamera: released");
            return;
        }

        if (!cam->IsInFreeCameraMode()) {
            SKSE::log::info("FreezeCamera: not in TFC, ignoring");
            return;
        }

        auto* root = cam->cameraRoot.get();
        if (!root) return;

        s_frozenPos = root->world.translate;
        s_frozenRot = root->world.rotate;

        cam->ToggleFreeCameraMode(false);
        s_frozen = true;

        SKSE::log::info("FreezeCamera: locked at ({:.0f}, {:.0f}, {:.0f})",
            s_frozenPos.x, s_frozenPos.y, s_frozenPos.z);
    }

    void Release() {
        s_frozen = false;
    }

    struct PlayerCameraUpdateHook {
        static void thunk(RE::TESCamera* a_this) {
            func(a_this);
            if (!s_frozen) return;

            auto* root = a_this->cameraRoot.get();
            if (!root) return;

            root->world.translate = s_frozenPos;
            root->world.rotate    = s_frozenRot;
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void Install() {
        REL::Relocation<std::uintptr_t> vtable(RE::VTABLE_PlayerCamera[0]);
        PlayerCameraUpdateHook::func =
            vtable.write_vfunc(0x2, PlayerCameraUpdateHook::thunk);

        SKSE::log::info("FreezeCamera: PlayerCamera::Update hook installed");
    }
}
