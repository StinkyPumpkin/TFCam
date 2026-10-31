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

**Walls.** The automatic cameras keep out of walls: the idle camera (and its blend back), the Scene Camera orbit
and every blend to a saved shot (scene stage, pose, furniture). Each frame TFCam casts rays from the point the camera
looks at to the camera (one on the camera, one either side of it, level with it, as wide apart as the corners of the
camera's near plane). If a wall is in between, the camera is moved out along the wall's own direction to *Distance kept
from a wall* in front of it, so a camera that passes a wall slides along it and keeps looking at the same point. It
eases in over *Ease in to a wall*; when the way is clear again it waits a quarter of a second and then eases back out
over *Ease back out*. People are passed through, and so are loose clutter and objects smaller than *Ignore objects
smaller than* (sconces, things on a table). Two tick boxes let the camera pass through *large items* (furniture and
other big props) and through *walls* (the upright surfaces of buildings; floors and ceilings still stop it). Your own
free-fly and the eye view are never stopped. A shot that ends behind a wall stays at the safe spot, and you fly on from
there. The rays use the camera-sphere collision layer (`iLayer=36` in `[Collision]`; 41 = line of sight). Each push is
logged in `TFCam.log` ("CamCollision: ... moved N units off the wall of <ref> (layer L ...)"), and the first one logs the
camera's frustum.

Settings: `TFCam.ini` sections `[Idle]`, `[Furniture]` and `[Collision]` (written by the page; an older file uses the
defaults).

## Conversation camera (0.16.0)

Page: **SKSE Menu Framework > FreeCam > Idle & Furniture > Conversation**. Off by default (tick *Conversation camera on*).
When you talk to an NPC in third person, TFCam's free camera takes over and films the conversation like a film scene:

- *Over your shoulder* while the NPC speaks (the NPC sits off to one side of the frame), and a *reverse shot* while it is your turn:
  over the NPC's shoulder, or a wide two-shot from the side (*Wide two-shot*). A shot stays at least *A shot stays at least* seconds.
- Every move is smooth: the camera is held in cylindrical coordinates (angle, radius, height) round the midpoint of the two speakers and each
  is damped, so a change of shot swings round the pair instead of cutting through them. *Handheld sway* adds a slow drift.
- The camera starts on the **release** of the Activate tap, once the game has started the dialogue (the game acts on the release; starting the
  free camera before it kept the dialogue from opening), and ends after the Dialogue Menu closes, easing back behind the player. It is
  not started for a bark with no dialogue menu, for dead / hostile / in-combat NPCs, while sneaking, in a SexLab scene, with a menu open or
  while another free camera is on. First person keeps the game's view (*Third person only*).
- Walls: the framed look point goes through the same wall handling as the other automatic cameras.
- The game's mouse edge-pan in dialogue (`fPCDialogueLookSpeed`) is held at 0 while the camera films, and put back after. *No mouse edge-pan
  in any dialogue* (`bNoEdgePan`) holds it at 0 for every dialogue.

Not done yet: a dialogue the NPC starts by itself (a quest scene that walks up to you and talks) is not filmed, because the camera has no
Activate press to start from; the game's own view is used there.

Settings: `TFCam.ini` section `[Conversation]`: `bEnabled`, `bThirdPersonOnly`, `fBlendIn`, `fBlendOut`, `fShotSwitch`, `fMinShotSeconds`,
`fDistance`, `fShoulder`, `fHeight`, `fLookShift`, `fFovNpc`, `fFovWide`, `bAlternate`, `iReverseKind`, `bSwapSide`, `fDrift`, `bNoEdgePan`.

## Action FOV, archer camera and aim dot (0.18.0)

Page: **SKSE Menu Framework > FreeCam > Action FOV**. Action FOV is off by default (tick *On*).

- **Action FOV**: drawing a bow pulls the view in, letting the arrow go snaps it out, power attacks punch in, big spells
  (from a chosen skill level) and shouts warp it out, and moving fast stretches it wide. It is an offset on the render
  camera's view, on top of whatever FOV you or another mod use; the game's own FOV value is never written.
- **Archer camera** (part of Action FOV, *Archer camera* tick box): while the string is drawn the third-person camera
  glides on an arc to a spot close round your head, so your face, the drawn bow and where the arrow flies are all in the
  shot. It moves in as the string is pulled (not when the arrow is nocked) and back out after the shot. The offset turns
  with your aim, so looking up or down keeps you in frame, and it holds steady against the bow-hold animation's sway.
  Walls pull it in front of anything solid. *Round to your face* (0 behind, 90 side-on), *Distance from your head*
  (10..400) and *Up / down* (-80..80) place it.
- **Aim dot**: a tiny white dot with a dark rim where your arrows and spells go. With TFCam's own aim on (below), or
  with Hit Where You Aim installed and Action FOV on, it sits in the middle of the screen whenever a bow, crossbow,
  staff or spell is out, in first and third person. Otherwise it only shows in the archer camera, where the arrow's
  own line meets the world. Never in the free camera. *Dot size* sets it.
- **Arrows and spells go where the dot is** (`[AimDot] bHitTheDot`, **off by default**): arrows, bolts,
  fire-and-forget spells, flames and beams are aimed at what the middle of the screen looks at, from whatever camera
  renders the picture (the archer camera included); arrows still drop, the game's own ballistic solver is used. Adapted
  from Hit Where You Aim by tossaponk (GPL-3.0-or-later, see `NOTICE.txt`), without its options. `TFCam.log` says how
  many of its 4 hooks went in ("AimDot: ...").
- While the string is drawn, optionally: the Background blur's edge blur (*Blur round the edge*) and darker corners
  (*Vignette*, 0..1).
- **Background blur** (Scene Camera page) has a new default style, *Blurry round the edge, sharp in the middle*: TFCam's
  own image pass blurs the picture more and more toward the screen's edge, the sharp middle on the subject, before the
  HUD and without ENB. The ENB depth of field stays as the other style.

The dot, the edge blur and the vignette are drawn by TFCam's own pass at the game's menu draw (after ENB, before the
HUD and menus). Not on VR.

**With Hit Where You Aim** (Nexus 193960): keep it, both work together. TFCam's own aim switches itself off whenever
`HitWhereYouAim.dll` is loaded (the menu and `TFCam.log` say "Hit Where You Aim is loaded - it aims the projectiles,
TFCam stays out"), so the two never aim the same shot. Hit Where You Aim aims through the middle of the picture the game
renders, which is the archer camera's view while the string is drawn, and the dot sits there. TFCam's box is for players
without Hit Where You Aim.

**With SmoothCam**: its arrow prediction, arc line, 3D crosshair and world crosshair options move the crosshair to
SmoothCam's own guess of the vanilla arrow, which is not where either aim sends it. Turn those off in SmoothCam when you
use the dot.

Settings: `TFCam.ini` section `[ActionFOV]`: `bEnabled`, `fStrength`, `bBow`, `bPowerAttacks`, `bSpells`, `iSpellLevel`,
`bShouts`, `bSpeed`, `bCameraSpeed`, `bArcherCamera`, `fArcherTurn`, `fArcherDistance`, `fArcherUp`, `bArcherDot`,
`fArcherDotSize`, `bArcherBlur`, `fArcherVignette`; `[AimDot]` `bHitTheDot` (0); `[Blur]` `iStyle` (0 edges, 1 ENB),
`fSharpMiddle`.

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
  pose ends (unless you switched it to orbit or eye view, or a SexLab scene of yours started: it is that scene's camera
  then). Needs the matching *Go to a stage's / pose's shot*
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

What it does is logged to `TFCam.log` in SKSE's log folder (`Documents/My Games/Skyrim Special Edition/SKSE/` on Special Edition / Anniversary Edition) (mode changes, stage changes with
the scene / stage ids, shots saved / recalled / deleted, refusals, automatic free camera waits and drops).

## Changelog

**0.18.0**
- *Action FOV* (`[ActionFOV]`, off by default): the view reacts to bow draws and releases, power attacks, big spells,
  shouts and speed.
- *Archer camera*: close round your head while the bow string is drawn, gliding in on an arc as the string is pulled;
  keeps you in frame when aiming up or down, steady against the hold sway, walls respected.
- *Aim dot* and *arrows and spells go where the dot is* (off by default; adapted from Hit Where You Aim, stays out of
  its way when it is loaded); optional edge blur and vignette while the string is drawn.
- *Background blur*: a new default style, blurry round the edge and sharp in the middle, drawn by TFCam itself (no ENB
  needed); the ENB depth of field is the other style.
- Walls: the game's own camera collision sphere no longer counts as a wall (it sits where the camera was last frame,
  so the archer camera kept bumping into it).
- Settings: changes made on TFCam's pages are kept again under Mod Organizer 2. `TFCam.ini` is now written with plain
  file writes; the Windows INI call TFCam used before never reached the file under MO2, so changes were lost at the
  next start.

**0.16.0** (fork by CageTV)
- *Conversation camera* (`[Conversation]`, off by default): a film-style camera for dialogue in third person. Over-the-shoulder shot while the
  NPC speaks, reverse shot (over the NPC's shoulder or a wide two-shot) on your turn, smooth swings round the pair, optional sway, back behind
  the player when the conversation ends. It starts on the release of the Activate tap once the game has opened the dialogue, and works for
  NPCs that are in a scene. Not filmed yet: dialogue an NPC starts by itself.
- The game's mouse edge-pan in dialogue is held off while the camera films; `bNoEdgePan` turns it off for every dialogue.
- A conversation camera page on *Idle & Furniture* with a sliders / tick box for every setting.

**0.15.1** (fork by CageTV)
- *Walls* rework: the camera slides along a wall instead of being pulled toward the player, so passing a wall no longer
  dips in and swings back out; it keeps looking at the same point. The pull eases in (`[Collision] fApproachSeconds`, 0.3 s; 0 = at
  once). The side probes run parallel to the camera line and reach as far as the near plane's corners (from the live near clip, FOV and
  aspect), and the camera keeps at least the near clip plus 4 units off a wall.
- Walls ignore loose clutter and objects whose bounding sphere is under `fIgnoreSmallerThan` (50): sconces and things on tables no
  longer bump the camera. New tick boxes `bPassLargeItems` (bounding sphere up to `fLargeItemRadius`, 200) and `bPassWalls` (upright
  surfaces of anything bigger; floors and ceilings still count), both off by default. All of them are on the Walls page.
- Idle camera, dolly zoom: the close end stops at `[Idle] fDollyMinDistance` (90 units, scaled in interiors) and stays at least 0.7 of that
  to the side of the body, so the near plane and the body's edge are not in view; the wide FOV is capped to keep the framing.

**0.14.1** (fork by CageTV)
- The download is now an installer (FOMOD) with one `TFCam.dll` per game version: **AE 1.6.x / 1.7.x**, **SE 1.5.97**, and **All versions**
  (SE + AE + VR in one DLL, the build of 0.14.0 and older; it is the one for Skyrim VR). The installer picks the matching one from your game's
  version, so pressing Next installs the right one. The SE and AE DLLs are the same code built for one runtime only (`TFCAM_RUNTIME=SE|AE`,
  see `BUILD.md`); a DLL that finds itself on another runtime stays inert, logs it and shows a message instead of calling wrong addresses.
- `package_release.py` builds the installer zip from the three builds; the `fomod/` folder is its source.

**0.14.0** (fork by CageTV)
- *Smooth FOV changes* (`[FOV] bSmooth`, default on, `fSmoothTime` 0.12 s): the mouse wheel and the FOV in / out click actions ease the FOV toward
  a target instead of jumping, and scrolling fast adds up on the target. The easing is frame-rate independent, also runs with time frozen,
  and lets go as soon as anything else changes the FOV (the Scene Camera, a menu restore, reset, leaving the free camera). Turn it off to get
  the old instant steps.
- *Fast movement* key (`[Hotkeys] iFastKey`, unset by default; `[Camera] fFastMultiplier`, 4, range 1.5 to 10): hold it to fly at that many times the
  speed. The slow key (1/5) wins when both are held.
- **Ctrl + mouse wheel** in free-fly changes the base fly speed by 15% a notch (0.5 to 50, the slider's range) and saves it to TFCam.ini; the slow /
  fast factor stays applied on top. Each change is logged.

**0.13.1** (fork by CageTV)
- A TFCam.ini with values outside what the menu allows (or text that is not a number) no longer reaches the camera: speed, FOV step,
  FOV limits, roll speed, light radius / fade / colour, the click actions and the cell-entry camera values are brought back inside the
  slider ranges at load, and the change is logged. A FOV minimum above the maximum no longer feeds `std::clamp` (undefined behaviour);
  both limits go back to 10 / 150 then.
- `TFCam.log` goes to SKSE's own log folder, so on Skyrim VR and the GOG edition it is next to the other SKSE logs instead of
  in a "Skyrim Special Edition" folder that does not exist there. Special Edition and Anniversary Edition are unchanged.
- `build_tfcam.bat` also finds Visual Studio 18 (and asks for its 14.44 toolset, as BUILD.md pins); `NOTICE.txt` added.

**0.13.0**
- Scene Camera API v3 (`RequestTFCamAPI(3)`, `ITFCam3`): orbit aim, sway and target, the automatic free camera switch,
  a status with the free-camera state and the scene's shot count, and "delete every shot of this scene". v1 and v2
  are unchanged.
- Orbit: *Aim height*, *Sway* (replaces the fixed +-8 degrees) and *Orbit round: You only*; "Height" is shown as
  "Tilt". New ini keys `fOrbitAim`, `fOrbitSway`, `iOrbitTarget`.
- Automatic free camera for saved shots (`bAutoEnterShots`, on by default; see above).
- Stage keys are checked with `SexLabRegistry.StageExists` before they count (fixes a stage key that paired a new scene
  with the previous scene's stage for about a second).

## License
GPL-3.0, as the original TFCam by StinkyPumpkin (`LICENSE`, unchanged). `NOTICE.txt` has the copyright statements of the original and of this fork, the GPL notice and the source links. TFCam.dll links alandtse's CommonLibSSE-NG (GPL-3.0 with a modding exception).
