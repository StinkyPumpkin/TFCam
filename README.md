# TFCam

A free camera overhaul for Skyrim SE / AE (SKSE plugin, no ESP): camera roll, an attached camera light, time freeze,
FOV control, screenshot keys, a cell-entry camera, and the Scene Camera below. Settings live on the SKSE Menu Framework
pages under **FreeCam** and in `Data/SKSE/Plugins/TFCam.ini`, which TFCam creates on first launch (it is never part of
the download). The Nexus page text is in `nexus_description.txt`; build steps are in `BUILD.md`.

## Idle camera and Furniture camera (0.11.0, routines reworked in 0.11.1)

Page: **SKSE Menu Framework > FreeCam > Idle & Furniture**. Both drive TFCam's free camera and give it back with a short
blend.

**Idle camera.** After a set time without input (default 25 s; not in menus, combat, SexLab scenes or while another free
camera is on) the camera circles you, playing these routines in turn:

- *Dolly zoom*: the camera moves in while the lens zooms out (distance = framing / (2 tan(FOV / 2))), so you keep your
  size while the world stretches. It sits low near the ground (*Camera height above the ground*, default 25 units) and
  tilts up at your face while it circles you (*Speed round you*, default 10 degrees a second, negative = the other way).
  When it comes too close to stay that low, it rises just enough to keep the zoom exact.
- *Spiral*: it opens on your face from the front, pulls back, drops slowly and comes in again near the floor; it does
  not stop at the feet but swings straight into the spiral, from the floor to your face in two loops round you,
  slowing down on four areas only: the bum (from behind),
  the genitals, the breasts and the face (from the front). It swings out between areas and comes in close on them.
  *Seconds per area* sets the pace of the whole path, opening included (higher = slower); *Go round the other way*
  flips the direction. The blend into it goes round you, not through you. Heights follow the player's own
  skeleton, so it fits any scale.

Any key, click or mouse movement gives the camera back. It works in first and third person. After *Runs for* seconds it
hands over to **Sandbox When Idle** (po3): that mod starts when the engine's idle timer passes `fAutoVanityModeDelay`, so
TFCam sets `PlayerCamera::idleTimer` past it as the camera is given back. Keep *starts after* + *runs for* below
`fAutoVanityModeDelay` (120 s by default). *Distances in interiors* shrinks the radii indoors; walls are handled by
*Walls* (below).

**Furniture camera.** A camera shot saved per furniture (its base form, in the player's frame). Sit on the furniture,
turn on free-fly, fly to where the camera should be and press the Scene Camera **Save camera shot** key (bind it on the
Scene Camera page; **Go to saved shot** and **Delete saved shot** work the same way). From then on, sitting on that
furniture, by hand or through Sandbox When Idle, blends the camera there after a short delay, and standing up gives it
back. Movement, jump and activate keys also give it back (not TFCam's own roll / reset / freeze / screenshot / slow keys).
Shots are kept in `SceneShots.tsv` under `furn:<plugin>|<local id>`.

**Walls.** The automatic cameras stop in front of walls: the idle camera (and its blend back), the Scene Camera orbit
and every blend to a saved shot (scene stage, pose, furniture). Each frame TFCam casts rays from the point the camera
looks at to the camera (one on the camera, one either side of it, level with it). If something solid is in between,
the camera moves in at once to *Distance kept from a wall* in front of it. When the way is clear again it waits a
quarter of a second and then eases back out over *Ease back out*. People are passed through. Your own free-fly and the
eye view are never stopped. A shot that ends behind a wall stays at the safe spot, and you fly on from there. The rays
use the camera-sphere collision layer (`iLayer=36` in `[Collision]`; 41 = line of sight). Each push is logged in
`TFCam.log` ("CamCollision: ... pulled in N of M units by <ref> (layer L)").

Settings: `TFCam.ini` sections `[Idle]`, `[Furniture]` and `[Collision]` (written by the page; an older file uses the
defaults).

## Scene Camera (0.10.0, extended in 0.13.0)

TFCam's free camera can work as a scene camera for SexLab P+ scenes (with SexLab P+'s automatic free camera on, a scene
starts in TFCam free-fly as usual). Page: **SKSE Menu Framework > FreeCam > Scene Camera**.

- **Orbit** circles the actors of your SexLab scene (outside a scene: the actor SLUI posed, else you), always looking at
  the smoothed centre of their torsos. *Orbit round* = **You only** always circles you instead. Radius, tilt (the
  elevation the camera looks down from; `fOrbitHeight` in the ini), speed (negative = the other way) and FOV are
  settings. *Aim height* (-60..60 units) moves the point the orbit circles and looks at up or down, so the camera can
  frame faces or feet while the tilt stays the angle it looks down at. *Sway* (0..15 degrees, default 8 as before) is
  how far the tilt slowly rises and falls; 0 keeps it level. While orbiting, WASD and the mouse do nothing and the mouse
  wheel changes the radius. Turning it off leaves the camera where it is and puts your FOV back.
- **Eye view** (your SexLab scene only) looks out of a partner's eyes. The mouse looks around up to 85 degrees each way,
  *Next partner* cycles through the partners, the mouse wheel changes the eye-view FOV. The partner's head is hidden and
  the near clip pulled in while it lasts; both are put back on every way out (off, partner change, scene end, free camera
  off, the partner unloading, loading a save). Turning it off blends back to the camera you had before.
- **Saved shots**: *Save* stores the free camera (position, direction, roll, FOV) for the current scene stage and your
  position slot in it, or, outside a scene, for the pose SLUI is playing. The position is kept relative to you (stage
  shots) or the posed actor (pose shots), so the same framing comes back wherever the scene plays. *Go to* blends there,
  *Delete* removes it; the page (and SLUI) also deletes every shot of the current scene. With the automatic options on,
  the camera blends to a stage's shot at scene start and on every stage change, and to a pose's shot when SLUI starts
  that pose - only in plain free-fly (orbit and eye view stay on across stages).
- **Automatic free camera for saved shots** (0.13.0, *Turn the free camera on for a saved shot*, `bAutoEnterShots`,
  **on by default**). When a stage that has a saved shot starts and the free camera is off, TFCam turns its free-fly on
  by itself and blends to the shot. It waits while SLUI, the console, a dialogue, a message box, a loading screen or any
  pausing menu is open and does it within 5 s after they close; it never takes the camera from an FCFW timeline / SLCC
  and never runs while another Scene Camera request is waiting. Once per stage change at most; if you leave the free
  camera during a scene it stays off for the rest of that scene. The same goes for a pose SLUI starts on an **NPC** with
  a saved shot (never for your own poses: you end those by moving), and TFCam turns that free camera off again when the
  pose ends (unless you switched it to orbit or eye view). Needs the matching *Go to a stage's / pose's shot*
  option. **This changes what happens for anyone who already has saved shots**: before 0.13.0 a saved shot only played
  when the free camera was already on. Turn the option off to get the old behaviour.

Orbit, eye view and *Go to* turn TFCam free-fly on first when the free camera is off. All six keys (orbit, eye view,
next partner, save, go to, delete) are **unset by default**; bind them on the page. SLUI (SexLab UI) drives the same
functions through `RequestTFCamAPI(3)` (`src/TFCamAPI.h`; version 2 stays available unchanged for older SLUI builds).

Stage keys: TFCam reads the scene and stage ids of your SexLab P+ thread through the Papyrus VM. P+ sets a new scene
before its stage, so since 0.13.0 a new key also has to pass `SexLabRegistry.StageExists(scene, stage)` before it
counts; a shot can no longer be saved under the new scene paired with the previous scene's stage.

Files (TFCam writes both; neither is ever shipped, so with MO2 they appear in `overwrite`):

- `Data/SKSE/Plugins/TFCam.ini` section `[SceneCam]`: `iSceneOrbitKey`, `iSceneEyeKey`, `iSceneEyeNextKey`,
  `iSceneShotSaveKey`, `iSceneShotRecallKey`, `iSceneShotDeleteKey`, `fOrbitRadius`, `fOrbitHeight` (tilt),
  `fOrbitSpeed`, `fOrbitFOV`, `fOrbitAim` (0), `fOrbitSway` (8), `iOrbitTarget` (0 = auto, 1 = you only), `fEyeFOV`,
  `fEyeNearClip`, `fEyeForward`, `bEyeHideHead`, `bAutoStageShots`, `bAutoPoseShots`, `bAutoEnterShots` (1),
  `fShotBlend`. An older TFCam.ini without a key uses its default until a setting is changed.
- `Data/SKSE/Plugins/TFCam/SceneShots.tsv`: one shot per line,
  `key  x  y  z  yawRel  pitch  roll  fov  savedAt  label`, keys `stage:<scene id>|<stage id>|<player slot>` and
  `pose:<animation event>`. Lines TFCam cannot read (or with a value that is not a finite number) are kept as they
  are and not used; a shot's FOV is held inside TFCam's FOV limits.

What it does is logged to `Documents/My Games/Skyrim Special Edition/SKSE/TFCam.log` (mode changes, stage changes with
the scene / stage ids, shots saved / recalled / deleted, refusals, automatic free camera waits and drops).

## Changelog

**0.13.0**
- Scene Camera API v3 (`RequestTFCamAPI(3)`, `ITFCam3`): orbit aim, sway and target, the automatic free camera switch,
  a status with the free-camera state and the scene's shot count, and "delete every shot of this scene". v1 and v2
  are unchanged.
- Orbit: *Aim height*, *Sway* (replaces the fixed +-8 degrees) and *Orbit round: You only*; "Height" is shown as
  "Tilt". New ini keys `fOrbitAim`, `fOrbitSway`, `iOrbitTarget`.
- Automatic free camera for saved shots (`bAutoEnterShots`, on by default; see above).
- Stage keys are checked with `SexLabRegistry.StageExists` before they count (fixes a stage key that paired a new scene
  with the previous scene's stage for about a second).
