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
// 11-20 (Hit Where You Aim 1.4): while True Directional Movement's target lock is on, arrows and flames / beams keep
// TDM's aim at the locked target.
namespace AimDot {

    struct Settings {
        bool enabled = false;  // [AimDot] bHitTheDot - off by default: not every player wants it (or runs Hit Where You Aim)
        bool hideGameCrosshair = true;  // [AimDot] bHideGameCrosshair - the game's crosshair steps aside while the dot is in charge
    };

    Settings& Get();

    void Install();  // kDataLoaded, after the trampoline is reserved (6 write_call<5>: 6 * 14 bytes)
    bool Active();   // installed, switched on, and Hit Where You Aim not loaded: the dot is where they go

    // The dot should show: a bow, crossbow, staff or spell is out (in first or third person; menus closed). Main thread.
    bool DotWanted();

    // The dot is the crosshair: it is switched on and the projectiles go through the middle of the screen (TFCam's own
    // aim, or Hit Where You Aim with Action FOV on).
    bool DotInCharge();

    // 10-31: "the crosshair should be disabled unless a bow or spell is equipped". While the dot is in charge the game's
    // own crosshair is hidden (the HUD's Crosshair clip), so the only crosshair is the dot, which shows with
    // something ranged out. Every frame, main thread.
    void CrosshairTick();

    const char* Status();  // for the menu
}
