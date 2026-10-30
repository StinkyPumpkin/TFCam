#pragma once

// 10-31: "the arrows and spells hit the dot". The player's projectiles - arrows, bolts, fire-and-forget spells, flames
// and beams - are aimed at the point the middle of the screen looks at (the dot FrameFx draws), from whatever camera
// renders the picture (the archer camera included). No menu, one switch. Nothing happens while Hit Where You Aim is
// loaded - that mod owns the job then.
//
// Adapted from Hit Where You Aim by tossaponk (https://github.com/tossaponk/HitWhereYouAim, GPL-3.0-or-later):
// the hook sites (the player's launch-angle call, the flame / beam aim calls, the projectile init after TDM), the
// crosshair ray with the projectile's own collision layer + group, the angle maths and the game's own ballistic
// solver for arrows. Its options (prediction windows, target leading, biases) are left out on purpose.
namespace AimDot {

    struct Settings {
        bool enabled = true;  // [AimDot] bHitTheDot
    };

    Settings& Get();

    void Install();  // kDataLoaded, after the trampoline is reserved (6 write_call<5>: 6 * 14 bytes)
    bool Active();   // installed, switched on, and Hit Where You Aim not loaded: the dot is where they go

    // The dot should show: a bow, crossbow, staff or spell is out (in first or third person; menus closed). Main thread.
    bool DotWanted();

    const char* Status();  // for the menu
}
