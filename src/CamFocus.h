#pragma once

#include <string>

// 0.17.0: what a TFCam camera looks at / keeps sharp. Shared by the camera path (CamPath.h), the orbit's "console
// selection" target (SceneCam) and the background blur (CamBlur.h).
//
// Main thread only.
namespace CamFocus {

    // The reference picked in the console (click it with the console open, or `prid`), while it is usable: not
    // deleted / disabled and with its 3D loaded. Null otherwise.
    RE::NiPointer<RE::TESObjectREFR> ConsoleRef();

    // A point to look at on a reference: an actor's torso (pelvis to spine2 middle), else the centre of its 3D bound,
    // else its position. a_floor: the reference's own z (an actor's feet). False without a reference.
    bool RefPoint(RE::TESObjectREFR* a_ref, RE::NiPoint3& a_out, float* a_floor = nullptr);

    // An actor's head node (NPC Head [Head]); false when the skeleton has none.
    bool HeadPoint(RE::Actor* a_actor, RE::NiPoint3& a_out);

    // "Lydia (000A2C94)" for log lines and the menu.
    std::string Describe(RE::TESObjectREFR* a_ref);
}
