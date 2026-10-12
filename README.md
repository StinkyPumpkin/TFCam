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
- *Spiral*: it opens on your face from the front, pulls back, drops slowly to the floor and comes in again; then it
  spirals from the floor to your face in two loops round you, slowing down on four areas only: the bum (from behind),
  the genitals, the breasts and the face (from the front). It swings out between areas and comes in close on them.
  *Seconds per area* sets the pace of the whole path, opening included (higher = slower); *Go round the other way*
  flips the direction. The blend into it goes round you, not through you. Heights follow the player's own
  skeleton, so it fits any scale.

Any key, click or mouse movement gives the camera back. It works in first and third person. After *Runs for* seconds it
hands over to **Sandbox When Idle** (po3): that mod starts when the engine's idle timer passes `fAutoVanityModeDelay`, so
TFCam sets `PlayerCamera::idleTimer` past it as the camera is given back. Keep *starts after* + *runs for* below
`fAutoVanityModeDelay` (120 s by default). The free camera has no collision; *Distances in interiors* shrinks the radii.

**Furniture camera.** A camera shot saved per furniture (its base form, in the player's frame). Sit on the furniture,
turn on free-fly, fly to where the camera should be and press the Scene Camera **Save camera shot** key (bind it on the
Scene Camera page; **Go to saved shot** and **Delete saved shot** work the same way). From then on, sitting on that
furniture, by hand or through Sandbox When Idle, blends the camera there after a short delay, and standing up gives it
back. Movement, jump and activate keys also give it back (not TFCam's own roll / reset / freeze / screenshot / slow keys).
Shots are kept in `SceneShots.tsv` under `furn:<plugin>|<local id>`.

Settings: `TFCam.ini` sections `[Idle]` and `[Furniture]` (written by the page; an older file uses the defaults).

## Scene Camera (0.10.0)

TFCam's free camera can work as a scene camera for SexLab P+ scenes (with SexLab P+'s automatic free camera on, a scene
starts in TFCam free-fly as usual). Page: **SKSE Menu Framework > FreeCam > Scene Camera**.

- **Orbit** circles the actors of your SexLab scene (outside a scene: the actor SLUI posed, else you), always looking at
  the smoothed centre of their torsos. Radius, height, speed (negative = the other way) and FOV are settings; the height
  breathes +-8 degrees slowly. While orbiting, WASD and the mouse do nothing and the mouse wheel changes the radius.
  Turning it off leaves the camera where it is and puts your FOV back.
- **Eye view** (your SexLab scene only) looks out of a partner's eyes. The mouse looks around up to 85 degrees each way,
  *Next partner* cycles through the partners, the mouse wheel changes the eye-view FOV. The partner's head is hidden and
  the near clip pulled in while it lasts; both are put back on every way out (off, partner change, scene end, free camera
  off, the partner unloading, loading a save). Turning it off blends back to the camera you had before.
- **Saved shots**: *Save* stores the free camera (position, direction, roll, FOV) for the current scene stage and your
  position slot in it, or, outside a scene, for the pose SLUI is playing. The position is kept relative to you (stage
  shots) or the posed actor (pose shots), so the same framing comes back wherever the scene plays. *Go to* blends there,
  *Delete* removes it; the page also deletes every shot of the current scene. With the automatic options on, the camera
  blends to a stage's shot at scene start and on every stage change, and to a pose's shot when SLUI starts that pose -
  only in plain free-fly (orbit and eye view stay on across stages) and never by turning the free camera on.

Orbit, eye view and *Go to* turn TFCam free-fly on first when the free camera is off. All six keys (orbit, eye view,
next partner, save, go to, delete) are **unset by default**; bind them on the page. SLUI (SexLab UI) drives the same
functions through `RequestTFCamAPI(2)` (`src/TFCamAPI.h`).

Files (TFCam writes both; neither is ever shipped, so with MO2 they appear in `overwrite`):

- `Data/SKSE/Plugins/TFCam.ini` section `[SceneCam]`: `iSceneOrbitKey`, `iSceneEyeKey`, `iSceneEyeNextKey`,
  `iSceneShotSaveKey`, `iSceneShotRecallKey`, `iSceneShotDeleteKey`, `fOrbitRadius`, `fOrbitHeight`, `fOrbitSpeed`,
  `fOrbitFOV`, `fEyeFOV`, `fEyeNearClip`, `fEyeForward`, `bEyeHideHead`, `bAutoStageShots`, `bAutoPoseShots`,
  `fShotBlend`. An older TFCam.ini without the section uses the defaults until a setting is changed.
- `Data/SKSE/Plugins/TFCam/SceneShots.tsv`: one shot per line,
  `key  x  y  z  yawRel  pitch  roll  fov  savedAt  label`, keys `stage:<scene id>|<stage id>|<player slot>` and
  `pose:<animation event>`. Lines TFCam cannot read (or with a value that is not a finite number) are kept as they
  are and not used; a shot's FOV is held inside TFCam's FOV limits.

What it does is logged to `Documents/My Games/Skyrim Special Edition/SKSE/TFCam.log` (mode changes, stage changes with
the scene / stage ids, shots saved / recalled / deleted, refusals).
